#include "core/Sha256.h"

#include <windows.h>
#include <bcrypt.h>

#include <cctype>

namespace mp {

std::string parseSha256Text(const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && !isxdigit(static_cast<unsigned char>(text[i]))) ++i;
        size_t j = i;
        while (j < text.size() && isxdigit(static_cast<unsigned char>(text[j]))) ++j;
        if (j - i == 64) {
            std::string h = text.substr(i, 64);
            for (char& c : h) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            return h;
        }
        // 跳過這個 token（可能是檔名裡的一段 hex）
        while (j < text.size() && !isspace(static_cast<unsigned char>(text[j]))) ++j;
        i = j;
    }
    return "";
}

std::string sha256OfFile(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    std::string result;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::string buf(1 << 20, '\0');
        bool ok = true;
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) { ok = false; break; }
            if (got == 0) break;
            if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), got, 0) != 0) { ok = false; break; }
        }
        unsigned char digest[32];
        if (ok && BCryptFinishHash(hash, digest, 32, 0) == 0) {
            static const char* hex = "0123456789abcdef";
            for (unsigned char c : digest) {
                result += hex[c >> 4];
                result += hex[c & 15];
            }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return result;
}

} // namespace mp
