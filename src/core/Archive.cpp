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
#include "core/Lang.h"
#include "core/TempDir.h"
#include "core/TextUtil.h"

namespace mp {

namespace {

// 7z.dll。載入後不卸載；用到的人各拿一份 shared_ptr，不靠「反正不會釋放」這個前提。
// Bit7zLibrary 裡只有 DLL 的 handle 與 CreateObject 的函式指標，多條執行緒可以共用同一個（各開各的 reader）。
std::shared_ptr<bit7z::Bit7zLibrary> g_lib;
std::mutex g_libMutex;

std::shared_ptr<bit7z::Bit7zLibrary> library() {
    std::lock_guard<std::mutex> lock(g_libMutex);
    return g_lib;
}

// 沒有副檔名的項目只在這麼大以內才解出來看檔頭，免得把一個幾 GB 的不明檔也解了。
constexpr uint64_t kNoExtMaxBytes = 64ull << 20;
// 第一趟解壓中斷後，剩下的項目逐一解到記憶體補寫；超過這個大小的不補（當作解不出來）。
constexpr uint64_t kRetryMaxBytes = 256ull << 20;
// 解壓前目標磁碟除了圖片的總大小，還要多留這麼多空間。
constexpr uint64_t kFreeSpaceMargin = 64ull << 20;

bool isPasswordError(const bit7z::BitException& e) {
    const auto& c = e.code();
    return c == bit7z::BitFailureSource::WrongPassword || c == bit7z::BitError::InvalidZipPassword ||
           c == bit7z::BitFailureSource::DataError || c == bit7z::BitFailureSource::CRCError;
}

// bit7z 的例外 → 原因代碼。認得的種類給代碼（畫面上有翻譯）；Windows 的錯誤給錯誤碼；其他的留它的英文訊息。
ArchiveError errorOf(const bit7z::BitException& e) {
    ArchiveError err;
    const auto& c = e.code();
    if (c == bit7z::BitFailureSource::CRCError) err.reason = ArchiveFailure::CrcError;
    else if (c == bit7z::BitFailureSource::DataError) err.reason = ArchiveFailure::DataError;
    else if (c == bit7z::BitFailureSource::HeadersError) err.reason = ArchiveFailure::HeadersError;
    else if (c == bit7z::BitFailureSource::UnexpectedEnd) err.reason = ArchiveFailure::UnexpectedEnd;
    else if (c == bit7z::BitFailureSource::OperationNotSupported) err.reason = ArchiveFailure::UnsupportedMethod;
    else if (c == bit7z::BitFailureSource::InvalidArchive || c == bit7z::BitFailureSource::FormatDetectionError)
        err.reason = ArchiveFailure::InvalidArchive;
    else {
        const HRESULT hr = e.hresultCode();
        if (HRESULT_FACILITY(hr) == FACILITY_WIN32 && HRESULT_CODE(hr) != 0) {
            err.reason = ArchiveFailure::System;
            err.systemError = static_cast<unsigned long>(HRESULT_CODE(hr));
        } else {
            err.reason = ArchiveFailure::Other;
            err.detail = utf8ToWide(e.what());
        }
    }
    return err;
}

ArchiveError errorOf(ArchiveFailure reason) {
    ArchiveError err;
    err.reason = reason;
    return err;
}

bool onDisk(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// extractImages 的本體，一個階段一個函式。每個階段回 true 才往下做；回 false 時 r_ 已經是要回傳的結果。
class ImageExtraction {
public:
    ImageExtraction(bit7z::Bit7zLibrary& lib, const std::wstring& archivePath, const std::wstring& destDir, const ExtractOptions& opt)
        : lib_(lib), arcPath_(bit7z::to_tstring(archivePath)), destDir_(destDir), opt_(opt) {}

    ArchiveResult run() {
        if (open() && pickItems() && prepareDest() && firstPass() && repair()) collect();
        return std::move(r_);
    }

private:
    struct Item {
        uint32_t index;
        std::wstring name;     // 壓縮檔內的路徑，/ 分隔
        std::wstring diskName; // 磁碟上的檔名（序號）
        bool noExt;
        uint64_t size;
        bool encrypted;
        bool landed = false;   // 已經完整寫到磁碟
    };

    bool stop(ArchiveStatus status) {
        r_.status = status;
        return false;
    }
    std::wstring diskPath(const Item& it) const { return destDir_ + L"\\" + it.diskName; }

    // 1. 開檔（7z 加密檔頭在這裡就會失敗）。
    bool open() {
        try {
            reader_ = std::make_unique<bit7z::BitArchiveReader>(lib_, arcPath_, bit7z::BitFormat::Auto, bit7z::to_tstring(opt_.password));
            return true;
        } catch (const bit7z::BitException& e) {
            if (bit7z::BitArchiveReader::isHeaderEncrypted(lib_, arcPath_))
                return stop(opt_.password.empty() ? ArchiveStatus::NeedPassword : ArchiveStatus::WrongPassword);
            r_.error = errorOf(e);
            return stop(ArchiveStatus::NotArchive);
        }
    }

    // 2. 挑項目：支援的副檔名，或完全沒有副檔名的小檔（解出來再看檔頭）。依壓縮檔內完整路徑自然排序，
    // 序號照這個順序給：磁碟上的順序就是閱讀順序。
    bool pickItems() {
        try {
            r_.solid = reader_->isSolid();
            for (const auto& item : reader_->items()) {
                if (item.isDir() || item.isSymLink()) continue;
                std::wstring rel = item.nativePath();
                for (auto& ch : rel)
                    if (ch == L'\\') ch = L'/';
                if (isArchiveEntrySkipped(rel)) continue;
                const std::wstring ext = extensionOf(rel);
                const bool noExt = ext.empty();
                if (noExt ? item.size() > kNoExtMaxBytes : !isSupportedExtension(ext)) continue;
                items_.push_back({item.index(), rel, L"", noExt, item.size(), item.isEncrypted()});
                r_.totalBytes += item.size();
                if (item.isEncrypted()) anyEncrypted_ = true;
            }
        } catch (const bit7z::BitException& e) {
            r_.error = errorOf(e);
            return stop(ArchiveStatus::Failed);
        }
        if (items_.empty()) return stop(ArchiveStatus::NoImages);
        if (anyEncrypted_ && opt_.password.empty()) return stop(ArchiveStatus::NeedPassword);
        if (r_.totalBytes > opt_.maxTotalBytes) return stop(ArchiveStatus::TooLarge);
        std::stable_sort(items_.begin(), items_.end(),
                         [](const Item& a, const Item& b) { return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0; });
        for (size_t i = 0; i < items_.size(); ++i) {
            wchar_t buf[16];
            swprintf(buf, 16, L"%05u", static_cast<unsigned>(i + 1));
            items_[i].diskName = std::wstring(buf) + (items_[i].noExt ? L"" : extensionOf(items_[i].name));
            byIndex_[items_[i].index] = i;
        }
        return true;
    }

    // 3. 目標資料夾與剩餘空間。
    bool prepareDest() {
        if (!createDirectories(destDir_)) {
            r_.error = errorOf(ArchiveFailure::CannotCreateFolder);
            return stop(ArchiveStatus::Failed);
        }
        ULARGE_INTEGER freeBytes{};
        if (GetDiskFreeSpaceExW(destDir_.c_str(), &freeBytes, nullptr, nullptr) && freeBytes.QuadPart < r_.totalBytes + kFreeSpaceMargin)
            return stop(ArchiveStatus::NoSpace);
        return true;
    }

    // 4. 解壓：一趟解完（solid 也只解一次），每個項目的磁碟檔名由 RenameCallback 給，不要的回空字串略過。
    // bit7z 4.1 遇到第一個解不出來的項目（CRC 錯、資料損毀、目標寫不進去）就整趟停下來丟例外，後面的項目都還沒解。
    // 所以失敗後不直接放棄：記下原因，留下已經解好的，交給 repair 補。
    bool firstPass() {
        // 這些回呼掛在 reader_ 上，repair 逐一補解時還會被叫到：只能碰成員，不能捕捉這個函式的區域變數。
        total_ = r_.totalBytes;
        reader_->setOverwriteMode(bit7z::OverwriteMode::Overwrite);
        if (opt_.progress) {
            reader_->setTotalCallback([this](uint64_t t) { total_ = t; });
            reader_->setProgressCallback([this](uint64_t done) {
                if (!opt_.progress(done, total_)) cancelled_ = true;
                return !cancelled_;
            });
        }
        reader_->setFileCallback([this](const bit7z::tstring& f) { lastStarted_ = utf8ToWide(f); });
        try {
            reader_->extractTo(bit7z::to_tstring(destDir_), [&](const bit7z::BitArchiveItem& item) -> bit7z::tstring {
                auto it = byIndex_.find(item.index());
                return it == byIndex_.end() ? bit7z::tstring{} : bit7z::to_tstring(items_[it->second].diskName);
            });
        } catch (const bit7z::BitException& e) {
            firstPassOk_ = false;
            firstPassPwError_ = anyEncrypted_ && isPasswordError(e);
            r_.error = errorOf(e);
        }
        if (cancelled_) return stop(ArchiveStatus::Cancelled);
        // 落地檢查。第一趟成功：每個項目都該在磁碟上。失敗：出事的那個（最近一個開始寫的）先刪掉，
        // 它可能寫到一半或內容已壞，留給 repair 重解。
        const std::wstring failedName = lastStarted_;
        for (Item& it : items_) {
            if (!firstPassOk_ && _wcsicmp(it.diskName.c_str(), failedName.c_str()) == 0) {
                DeleteFileW(diskPath(it).c_str());
                continue;
            }
            it.landed = onDisk(diskPath(it));
            if (it.landed && it.encrypted) encryptedOk_ = true;
        }
        return true;
    }

    // 5. 補救：第一趟中斷時，沒落地的逐一解到記憶體再寫檔，解不出來的（真的壞了）略過。
    // 密碼錯的判定：第一趟是密碼類的錯誤、而且沒有任何加密項目解成功 → WrongPassword（錯密碼一定在第一個加密項目就倒）；
    // 已經有加密項目解成功過，之後再出的資料錯誤就是損毀，不是密碼。
    bool repair() {
        if (firstPassOk_) return true;
        if (firstPassPwError_ && !encryptedOk_) return stop(ArchiveStatus::WrongPassword);
        for (Item& it : items_) {
            if (cancelled_) break;
            if (it.landed || it.size > kRetryMaxBytes) continue; // 太大的不整個抓進記憶體，當作解不出來
            std::vector<bit7z::byte_t> buf;
            try {
                reader_->extractTo(buf, it.index);
            } catch (const bit7z::BitException&) {
                continue; // 這個項目真的解不出來
            }
            const std::wstring path = diskPath(it);
            HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) continue;
            DWORD written = 0;
            const bool ok = buf.empty() || (WriteFile(h, buf.data(), static_cast<DWORD>(buf.size()), &written, nullptr) && written == buf.size());
            CloseHandle(h);
            if (!ok) {
                DeleteFileW(path.c_str());
                continue;
            }
            it.landed = true;
        }
        if (cancelled_) return stop(ArchiveStatus::Cancelled);
        return true;
    }

    // 6. 清單：沒落地的從清單拿掉並回 Incomplete，呼叫端提示後照開剩下的。沒有副檔名的看檔頭，是圖片才留下並補副檔名。
    void collect() {
        r_.expected = static_cast<int>(items_.size());
        for (const Item& it : items_) {
            if (!it.landed) continue;
            std::wstring path = diskPath(it);
            if (it.noExt) {
                const std::wstring ext = sniffImageExtension(path);
                if (ext.empty()) { // 不是圖片，本來就不算
                    DeleteFileW(path.c_str());
                    --r_.expected;
                    continue;
                }
                if (MoveFileW(path.c_str(), (path + ext).c_str())) path += ext;
            }
            r_.files.push_back(path);
            r_.names.push_back(it.name);
        }
        r_.written = static_cast<int>(r_.files.size());
        if (r_.files.empty()) {
            // 一張都沒解出來：本來就沒有圖，或整個壞了。
            r_.status = r_.expected == 0 ? ArchiveStatus::NoImages : firstPassOk_ ? ArchiveStatus::Incomplete : ArchiveStatus::Failed;
            return;
        }
        r_.status = r_.written < r_.expected ? ArchiveStatus::Incomplete : ArchiveStatus::Ok;
        if (r_.status == ArchiveStatus::Ok) r_.error = {};
    }

    bit7z::Bit7zLibrary& lib_;
    const bit7z::tstring arcPath_;
    const std::wstring destDir_;
    const ExtractOptions& opt_;
    ArchiveResult r_;
    std::unique_ptr<bit7z::BitArchiveReader> reader_;
    std::vector<Item> items_;
    std::unordered_map<uint32_t, size_t> byIndex_; // 壓縮檔內的 index → items_ 的位置
    bool anyEncrypted_ = false;
    uint64_t total_ = 0;       // 進度的分母（7-Zip 回報的總量）
    std::wstring lastStarted_; // 最近一個開始寫的磁碟檔名（序號名）
    bool cancelled_ = false;
    bool firstPassOk_ = true;
    bool firstPassPwError_ = false;
    bool encryptedOk_ = false; // 有加密項目完整解出來了（密碼是對的）
};

} // namespace

LibraryLoad loadArchiveLibrary(const std::wstring& dllPath) {
    std::lock_guard<std::mutex> lock(g_libMutex);
    LibraryLoad result;
    if (g_lib) {
        result.status = LibraryLoad::Ok;
        return result;
    }
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        result.status = LibraryLoad::FileMissing;
        result.systemError = GetLastError();
        return result;
    }
    // 檔案在但不是能載入的 DLL（壞掉、位元數不對）時，Windows 預設會自己跳一個「映像錯誤」的系統對話框。
    // 關掉它（SEM_FAILCRITICALERRORS）：載入失敗由我們自己說明原因。載完就還原。VM 桌面確認過不再跳系統對話框。
    const UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS);
    SetErrorMode(oldMode | SEM_FAILCRITICALERRORS);
    try {
        g_lib = std::make_shared<bit7z::Bit7zLibrary>(bit7z::to_tstring(dllPath));
        result.status = LibraryLoad::Ok;
    } catch (const bit7z::BitException& e) {
        result.status = LibraryLoad::LoadFailed;
        const HRESULT hr = e.hresultCode();
        result.systemError = HRESULT_FACILITY(hr) == FACILITY_WIN32 ? static_cast<unsigned long>(HRESULT_CODE(hr)) : static_cast<unsigned long>(hr);
    }
    SetErrorMode(oldMode);
    return result;
}

bool archiveLibraryLoaded() { return library() != nullptr; }

const wchar_t* archiveFailureMessage(ArchiveFailure reason) {
    switch (reason) {
    case ArchiveFailure::None: return L"";
    case ArchiveFailure::InvalidArchive: return trKey(L"not a supported archive");
    case ArchiveFailure::DataError: return trKey(L"The data in the archive is damaged");
    case ArchiveFailure::CrcError: return trKey(L"The checksum does not match (the archive is damaged)");
    case ArchiveFailure::HeadersError: return trKey(L"The archive headers are damaged");
    case ArchiveFailure::UnexpectedEnd: return trKey(L"The archive is incomplete");
    case ArchiveFailure::UnsupportedMethod: return trKey(L"The archive uses a compression method that is not supported");
    case ArchiveFailure::CannotCreateFolder: return trKey(L"Cannot create the folder to unpack into");
    case ArchiveFailure::NoLibrary: return trKey(L"7z.dll is not loaded");
    case ArchiveFailure::System: return trKey(L"System error");
    case ArchiveFailure::Other: return trKey(L"Unknown error");
    }
    return L"";
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

std::wstring sniffImageExtension(const std::wstring& path) {
    const ImageFormatInfo* f = imageFormatInfo(detectImageFormat(path));
    return f ? f->extensions.front() : L"";
}

ArchiveError extractAll(const std::wstring& archivePath, const std::wstring& destDir) {
    auto lib = library();
    if (!lib) return errorOf(ArchiveFailure::NoLibrary);
    if (!createDirectories(destDir)) return errorOf(ArchiveFailure::CannotCreateFolder);
    try {
        bit7z::BitArchiveReader reader(*lib, bit7z::to_tstring(archivePath), bit7z::BitFormat::Auto);
        reader.extractTo(bit7z::to_tstring(destDir));
        return {};
    } catch (const bit7z::BitException& e) {
        return errorOf(e);
    }
}

ArchiveStatus testPassword(const std::wstring& archivePath, const std::wstring& password) {
    auto lib = library();
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
    auto lib = library();
    if (!lib) {
        ArchiveResult r;
        r.status = ArchiveStatus::NoLibrary;
        return r;
    }
    return ImageExtraction(*lib, archivePath, destDir, opt).run();
}

int findPassword(const std::wstring& archivePath, const std::vector<std::wstring>& passwords, const PasswordHooks& hooks,
                 bool& cancelled) {
    cancelled = false;
    const int total = static_cast<int>(passwords.size());
    auto wantsCancel = [&] { return hooks.cancelled && hooks.cancelled(); };
    for (int i = 0; i < total; ++i) {
        if (wantsCancel()) break;
        if (hooks.onTry) hooks.onTry(i + 1, total);
        if (testPassword(archivePath, passwords[static_cast<size_t>(i)]) == ArchiveStatus::Ok) return i;
    }
    // 沒有一個對。試的途中（包括試最後一個的時候）按了取消，要算取消，不是「都不對」。
    cancelled = wantsCancel();
    return -1;
}

PasswordExtract extractWithPasswords(const std::wstring& archivePath, const std::wstring& destDir, const ExtractOptions& opt,
                                     const std::vector<std::wstring>& passwords, const PasswordHooks& hooks) {
    PasswordExtract out;
    out.result = extractImages(archivePath, destDir, opt);
    if (out.result.status != ArchiveStatus::NeedPassword || passwords.empty()) {
        if (out.result.usable()) out.password = opt.password;
        return out;
    }
    removeDirectoryRecursive(destDir);
    bool cancelled = false;
    const int hit = findPassword(archivePath, passwords, hooks, cancelled);
    if (cancelled) {
        out.result.status = ArchiveStatus::Cancelled;
        return out;
    }
    out.listTried = true;
    if (hit < 0) return out; // NeedPassword：列表裡沒有對的
    ExtractOptions withPassword = opt;
    withPassword.password = passwords[static_cast<size_t>(hit)];
    out.result = extractImages(archivePath, destDir, withPassword);
    out.password = withPassword.password;
    return out;
}

} // namespace mp
