#include "core/Archive.h"

#include <windows.h>
#include <shlwapi.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <bit7z/bit7zlibrary.hpp>
#include <bit7z/bitarchiveitem.hpp>
#include <bit7z/bitarchivereader.hpp>
#include <bit7z/biterror.hpp>
#include <bit7z/bitexception.hpp>
#include <bit7z/bitformat.hpp>

#include "core/FolderNav.h"
#include "core/TempDir.h"

namespace mp {

namespace {

std::unique_ptr<bit7z::Bit7zLibrary> g_lib;
std::mutex g_libMutex;

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::wstring joinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

void listRecursive(const std::wstring& dir, std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(joinPath(dir, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring p = joinPath(dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) listRecursive(p, out);
        else if (isSupportedExtension(extensionOf(fd.cFileName))) out.push_back(p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// 沒有副檔名的項目只在這麼大以內才解出來看檔頭，免得把一個幾 GB 的不明檔也解了。
constexpr uint64_t kNoExtMaxBytes = 64ull << 20;
// 第一趟解壓中斷後，剩下的項目逐一解到記憶體補寫；超過這個大小的不補（當作解不出來）。
constexpr uint64_t kRetryMaxBytes = 256ull << 20;

bool isPasswordError(const bit7z::BitException& e) {
    const auto& c = e.code();
    return c == bit7z::BitFailureSource::WrongPassword || c == bit7z::BitError::InvalidZipPassword ||
           c == bit7z::BitFailureSource::DataError || c == bit7z::BitFailureSource::CRCError;
}

} // namespace

bool loadArchiveLibrary(const std::wstring& dllPath) {
    std::lock_guard<std::mutex> lock(g_libMutex);
    if (g_lib) return true;
    try {
        g_lib = std::make_unique<bit7z::Bit7zLibrary>(bit7z::to_tstring(dllPath));
        return true;
    } catch (const bit7z::BitException&) {
        return false;
    }
}

bool archiveLibraryLoaded() {
    std::lock_guard<std::mutex> lock(g_libMutex);
    return g_lib != nullptr;
}

bool looksLikeArchive(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    unsigned char b[8] = {};
    DWORD got = 0;
    bool ok = ReadFile(h, b, sizeof(b), &got, nullptr) && got >= 6;
    CloseHandle(h);
    if (!ok) return false;
    // zip：PK\3\4（一般）、PK\5\6（空檔）、PK\7\8（分割）
    if (b[0] == 'P' && b[1] == 'K' && ((b[2] == 3 && b[3] == 4) || (b[2] == 5 && b[3] == 6) || (b[2] == 7 && b[3] == 8)))
        return true;
    // 7z：'7z' BC AF 27 1C
    if (b[0] == '7' && b[1] == 'z' && b[2] == 0xBC && b[3] == 0xAF && b[4] == 0x27 && b[5] == 0x1C) return true;
    // rar：Rar! 1A 07 00（v4）/ Rar! 1A 07 01 00（v5）
    if (b[0] == 'R' && b[1] == 'a' && b[2] == 'r' && b[3] == '!' && b[4] == 0x1A && b[5] == 0x07) return true;
    return false;
}

bool isArchiveEntrySkipped(const std::wstring& relPath) {
    size_t pos = 0;
    while (pos <= relPath.size()) {
        size_t end = relPath.find_first_of(L"/\\", pos);
        std::wstring part = relPath.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
        if (!part.empty()) {
            if (part[0] == L'.') return true;
            if (_wcsicmp(part.c_str(), L"__MACOSX") == 0) return true;
            if (_wcsicmp(part.c_str(), L"Thumbs.db") == 0) return true;
            if (_wcsicmp(part.c_str(), L"desktop.ini") == 0) return true;
        }
        if (end == std::wstring::npos) break;
        pos = end + 1;
    }
    return false;
}

std::vector<std::wstring> listImageFilesRecursive(const std::wstring& dir) {
    std::vector<std::wstring> out;
    listRecursive(dir, out);
    sortLogical(out);
    return out;
}

std::wstring sniffImageExtension(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    unsigned char b[16] = {};
    DWORD got = 0;
    bool ok = ReadFile(h, b, sizeof(b), &got, nullptr);
    CloseHandle(h);
    if (!ok) return L"";
    if (got >= 8 && std::memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0) return L".png";
    if (got >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return L".jpg";
    if (got >= 12 && std::memcmp(b, "RIFF", 4) == 0 && std::memcmp(b + 8, "WEBP", 4) == 0) return L".webp";
    if (got >= 4 && (std::memcmp(b, "II*\0", 4) == 0 || std::memcmp(b, "MM\0*", 4) == 0)) return L".tif";
    if (got >= 2 && b[0] == 'B' && b[1] == 'M') return L".bmp";
    return L"";
}

bool extractAll(const std::wstring& archivePath, const std::wstring& destDir, std::wstring& error) {
    error.clear();
    bit7z::Bit7zLibrary* lib = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_libMutex);
        lib = g_lib.get();
    }
    if (!lib) { error = L"7z.dll is not loaded"; return false; }
    if (!createDirectories(destDir)) { error = L"Cannot create folder: " + destDir; return false; }
    try {
        bit7z::BitArchiveReader reader(*lib, bit7z::to_tstring(archivePath), bit7z::BitFormat::Auto);
        reader.extractTo(bit7z::to_tstring(destDir));
        return true;
    } catch (const bit7z::BitException& e) {
        error = utf8ToWide(e.what());
        return false;
    }
}

ArchiveStatus testPassword(const std::wstring& archivePath, const std::wstring& password) {
    bit7z::Bit7zLibrary* lib = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_libMutex);
        lib = g_lib.get();
    }
    if (!lib) return ArchiveStatus::NoLibrary;
    bit7z::tstring arcPath = bit7z::to_tstring(archivePath);
    std::unique_ptr<bit7z::BitArchiveReader> reader;
    try {
        reader = std::make_unique<bit7z::BitArchiveReader>(*lib, arcPath, bit7z::BitFormat::Auto, bit7z::to_tstring(password));
    } catch (const bit7z::BitException&) {
        if (bit7z::BitArchiveReader::isHeaderEncrypted(*lib, arcPath))
            return password.empty() ? ArchiveStatus::NeedPassword : ArchiveStatus::WrongPassword;
        return ArchiveStatus::NotArchive;
    }
    try {
        bool solid = reader->isSolid();
        bool found = false;
        uint32_t pick = 0;
        uint64_t pickSize = 0;
        for (const auto& item : reader->items()) {
            if (item.isDir() || item.isSymLink() || !item.isEncrypted()) continue;
            if (!found || (!solid && item.size() < pickSize)) {
                pick = item.index();
                pickSize = item.size();
                found = true;
            }
            if (solid) break; // solid 區塊從頭解，第一個最便宜
        }
        if (!found) return ArchiveStatus::Ok;
        if (password.empty()) return ArchiveStatus::NeedPassword;
        std::vector<bit7z::byte_t> buf;
        reader->extractTo(buf, pick);
        return ArchiveStatus::Ok;
    } catch (const bit7z::BitException& e) {
        return isPasswordError(e) ? ArchiveStatus::WrongPassword : ArchiveStatus::Failed;
    }
}

ArchiveResult extractImages(const std::wstring& archivePath, const std::wstring& destDir, const ExtractOptions& opt) {
    ArchiveResult r;
    bit7z::Bit7zLibrary* lib = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_libMutex);
        lib = g_lib.get();
    }
    if (!lib) { r.status = ArchiveStatus::NoLibrary; return r; }

    bit7z::tstring arcPath = bit7z::to_tstring(archivePath);
    bit7z::tstring password = bit7z::to_tstring(opt.password);

    // 1. 開檔（7z 加密檔頭在這裡就會失敗）
    std::unique_ptr<bit7z::BitArchiveReader> reader;
    try {
        reader = std::make_unique<bit7z::BitArchiveReader>(*lib, arcPath, bit7z::BitFormat::Auto, password);
    } catch (const bit7z::BitException& e) {
        if (bit7z::BitArchiveReader::isHeaderEncrypted(*lib, arcPath)) {
            r.status = opt.password.empty() ? ArchiveStatus::NeedPassword : ArchiveStatus::WrongPassword;
            return r;
        }
        r.status = ArchiveStatus::NotArchive;
        r.error = utf8ToWide(e.what());
        return r;
    }

    // 2. 挑項目：支援的副檔名，或完全沒有副檔名的小檔（解出來再看檔頭）。
    struct Item { uint32_t index; std::wstring name; bool noExt; uint64_t size; bool encrypted; };
    std::vector<Item> items;
    bool anyEncrypted = false;
    try {
        r.solid = reader->isSolid();
        for (const auto& item : reader->items()) {
            if (item.isDir() || item.isSymLink()) continue;
            std::wstring rel = item.nativePath();
            for (auto& ch : rel) if (ch == L'\\') ch = L'/';
            if (isArchiveEntrySkipped(rel)) continue;
            std::wstring ext = extensionOf(rel);
            bool noExt = ext.empty();
            if (noExt ? item.size() > kNoExtMaxBytes : !isSupportedExtension(ext)) continue;
            items.push_back({item.index(), rel, noExt, item.size(), item.isEncrypted()});
            r.totalBytes += item.size();
            if (item.isEncrypted()) anyEncrypted = true;
        }
    } catch (const bit7z::BitException& e) {
        r.status = ArchiveStatus::Failed;
        r.error = utf8ToWide(e.what());
        return r;
    }
    if (items.empty()) { r.status = ArchiveStatus::NoImages; return r; }
    if (anyEncrypted && opt.password.empty()) { r.status = ArchiveStatus::NeedPassword; return r; }
    if (r.totalBytes > opt.maxTotalBytes) { r.status = ArchiveStatus::TooLarge; return r; }
    // 依壓縮檔內完整路徑自然排序，序號照這個順序給：磁碟上的順序就是閱讀順序。
    std::stable_sort(items.begin(), items.end(),
                     [](const Item& a, const Item& b) { return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0; });
    std::unordered_map<uint32_t, std::wstring> diskName;
    for (size_t i = 0; i < items.size(); ++i) {
        wchar_t buf[16];
        swprintf(buf, 16, L"%05u", static_cast<unsigned>(i + 1));
        diskName[items[i].index] = std::wstring(buf) + (items[i].noExt ? L"" : extensionOf(items[i].name));
    }

    // 3. 目標資料夾與剩餘空間
    if (!createDirectories(destDir)) { r.status = ArchiveStatus::Failed; r.error = L"Cannot create temp folder"; return r; }
    ULARGE_INTEGER freeBytes{};
    if (GetDiskFreeSpaceExW(destDir.c_str(), &freeBytes, nullptr, nullptr) && freeBytes.QuadPart < r.totalBytes + (64ull << 20)) {
        r.status = ArchiveStatus::NoSpace;
        return r;
    }

    // 4. 解壓：一趟解完（solid 也只解一次），每個項目的磁碟檔名由 RenameCallback 給，不要的回空字串略過。
    // bit7z 4.1 遇到第一個解不出來的項目（CRC 錯、資料損毀、目標寫不進去）就整趟停下來丟例外，
    // 後面的項目都還沒解。所以失敗後不直接放棄：先留下已經解好的，再把剩下的逐一解到記憶體補寫（第 5 步）。
    bool cancelled = false;
    uint64_t total = r.totalBytes;
    reader->setOverwriteMode(bit7z::OverwriteMode::Overwrite);
    if (opt.progress) {
        reader->setTotalCallback([&](uint64_t t) { total = t; });
        reader->setProgressCallback([&](uint64_t done) {
            if (!opt.progress(done, total)) { cancelled = true; return false; }
            return true;
        });
    }
    std::wstring lastStarted; // 最近一個開始寫的磁碟檔名（序號名）：第一趟失敗時就是出事的那個，可能寫了一半
    reader->setFileCallback([&](const bit7z::tstring& f) { lastStarted = utf8ToWide(f); });
    bool firstPassOk = true, firstPassPwError = false;
    try {
        reader->extractTo(bit7z::to_tstring(destDir), [&](const bit7z::BitArchiveItem& item) -> bit7z::tstring {
            auto it = diskName.find(item.index());
            return it == diskName.end() ? bit7z::tstring{} : bit7z::to_tstring(it->second);
        });
    } catch (const bit7z::BitException& e) {
        if (cancelled) { r.status = ArchiveStatus::Cancelled; return r; }
        firstPassOk = false;
        firstPassPwError = anyEncrypted && isPasswordError(e);
        r.error = utf8ToWide(e.what());
    }
    if (cancelled) { r.status = ArchiveStatus::Cancelled; return r; }

    // 5. 落地檢查與補救。
    // 第一趟成功：每個項目都該在磁碟上。第一趟失敗：出事的那個（lastStarted）先刪掉，它和後面沒解到的逐一解到記憶體再寫檔，
    // 解不出來的（真的壞了）略過。密碼錯的判定：第一趟就在第一個加密項目失敗、前面沒有任何加密項目解成功 → WrongPassword；
    // 已經有加密項目解成功過，之後再出的資料錯誤就是損毀不是密碼。
    auto onDisk = [&](const std::wstring& path) {
        DWORD attr = GetFileAttributesW(path.c_str());
        return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
    };
    std::vector<bool> landed(items.size(), false);
    bool encryptedOk = false;
    for (size_t i = 0; i < items.size(); ++i) {
        const Item& it = items[i];
        std::wstring path = destDir + L"\\" + diskName[it.index];
        if (!firstPassOk && _wcsicmp(diskName[it.index].c_str(), lastStarted.c_str()) == 0) {
            DeleteFileW(path.c_str()); // 可能寫到一半或內容已壞，重解
            continue;
        }
        landed[i] = onDisk(path);
        if (landed[i] && it.encrypted) encryptedOk = true;
    }
    if (!firstPassOk) {
        if (firstPassPwError && !encryptedOk) { r.status = ArchiveStatus::WrongPassword; return r; }
        for (size_t i = 0; i < items.size() && !cancelled; ++i) {
            if (landed[i]) continue;
            const Item& it = items[i];
            if (it.size > kRetryMaxBytes) continue; // 太大的不整個抓進記憶體，當作解不出來
            std::vector<bit7z::byte_t> buf;
            try {
                reader->extractTo(buf, it.index);
            } catch (const bit7z::BitException&) {
                continue; // 這個項目真的解不出來
            }
            std::wstring path = destDir + L"\\" + diskName[it.index];
            HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) continue;
            DWORD written = 0;
            bool ok = buf.empty() || (WriteFile(h, buf.data(), static_cast<DWORD>(buf.size()), &written, nullptr) && written == buf.size());
            CloseHandle(h);
            if (!ok) { DeleteFileW(path.c_str()); continue; }
            landed[i] = true;
        }
        if (cancelled) { r.status = ArchiveStatus::Cancelled; return r; }
    }

    // 6. 清單：沒落地的從清單拿掉並回 Incomplete，呼叫端提示後照開剩下的。
    r.expected = static_cast<int>(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        const Item& it = items[i];
        if (!landed[i]) continue;
        std::wstring path = destDir + L"\\" + diskName[it.index];
        if (it.noExt) {
            std::wstring ext = sniffImageExtension(path);
            if (ext.empty()) { DeleteFileW(path.c_str()); --r.expected; continue; } // 不是圖片，本來就不算
            if (MoveFileW(path.c_str(), (path + ext).c_str())) path += ext;
        }
        r.files.push_back(path);
        r.names.push_back(it.name);
    }
    r.written = static_cast<int>(r.files.size());
    if (r.files.empty()) {
        if (r.expected == 0) { r.status = ArchiveStatus::NoImages; return r; }
        r.status = firstPassOk ? ArchiveStatus::Incomplete : ArchiveStatus::Failed; // 一張都沒解出來，就是整個壞了
        return r;
    }
    r.status = r.written < r.expected ? ArchiveStatus::Incomplete : ArchiveStatus::Ok;
    return r;
}

} // namespace mp
