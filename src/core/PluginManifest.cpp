#include "core/PluginManifest.h"

#include <windows.h>

#include "core/TextUtil.h"
#include "plugins/sdk/mpainter_plugin.h"

namespace mp {

namespace {

bool validExtension(const std::wstring& ext) {
    if (ext.size() < 2 || ext[0] != L'.') return false;
    for (size_t i = 1; i < ext.size(); ++i) {
        const wchar_t c = ext[i];
        if (c <= L' ' || wcschr(L".*?;|/\\:\"<>", c)) return false;
    }
    return true;
}

// "名稱|.a;.b" → PluginFormatDecl。不合格回 false 與說明。
bool parseFormat(const std::wstring& value, PluginFormatDecl& out, std::wstring& problem) {
    const size_t bar = value.find(L'|');
    if (bar == std::wstring::npos) { problem = L"format line has no '|': " + value; return false; }
    out.name = trim(value.substr(0, bar));
    if (out.name.empty()) { problem = L"format line has no name: " + value; return false; }
    std::wstring rest = value.substr(bar + 1);
    size_t p = 0;
    while (p <= rest.size()) {
        const size_t semi = rest.find(L';', p);
        std::wstring ext = lowerCopy(trim(rest.substr(p, semi == std::wstring::npos ? std::wstring::npos : semi - p)));
        if (!ext.empty()) {
            if (!validExtension(ext)) { problem = L"bad extension '" + ext + L"' in: " + value; return false; }
            bool dup = false;
            for (const auto& e : out.extensions) dup = dup || e == ext;
            if (!dup) out.extensions.push_back(ext);
        }
        if (semi == std::wstring::npos) break;
        p = semi + 1;
    }
    if (out.extensions.empty()) { problem = L"format line has no extension: " + value; return false; }
    return true;
}

} // namespace

bool parsePluginManifest(const std::string& utf8, PluginManifest& out, std::wstring& problem) {
    out = PluginManifest{};
    problem.clear();
    std::string bytes = utf8;
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF)
        bytes.erase(0, 3);
    const std::wstring text = utf8ToWide(bytes);
    bool haveApi = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find(L'\n', pos);
        std::wstring line = trim(text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos));
        pos = nl == std::wstring::npos ? text.size() + 1 : nl + 1;
        if (line.empty() || line[0] == L'#') continue;
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        const std::wstring key = lowerCopy(trim(line.substr(0, eq))), value = trim(line.substr(eq + 1));
        if (key == L"api") {
            wchar_t* end = nullptr;
            const unsigned long v = wcstoul(value.c_str(), &end, 10);
            if (value.empty() || *end != L'\0' || v == 0 || v > 0xFFFF) { problem = L"bad api: " + value; return false; }
            out.api = static_cast<unsigned>(v);
            haveApi = true;
        } else if (key == L"name") {
            out.name = value;
        } else if (key == L"version") {
            out.version = value;
        } else if (key == L"format") {
            PluginFormatDecl f;
            if (!parseFormat(value, f, problem)) return false;
            out.formats.push_back(std::move(f));
        }
    }
    if (!haveApi) { problem = L"missing api"; return false; }
    if (out.name.empty()) { problem = L"missing name"; return false; }
    if (out.version.empty()) { problem = L"missing version"; return false; }
    if (out.formats.empty()) { problem = L"no format"; return false; }
    return true;
}

ManifestRead readPluginManifest(const std::wstring& dllPath) {
    ManifestRead r;
    HMODULE h = LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!h) {
        r.error = ManifestError::Unreadable;
        r.systemError = GetLastError();
        return r;
    }
    HRSRC res = FindResourceW(h, L"" MP_PLUGIN_MANIFEST_NAME, RT_RCDATA);
    HGLOBAL mem = res ? LoadResource(h, res) : nullptr;
    const void* data = mem ? LockResource(mem) : nullptr;
    if (!data) {
        r.error = ManifestError::NotFound;
    } else {
        const std::string text(static_cast<const char*>(data), SizeofResource(h, res));
        if (!parsePluginManifest(text, r.manifest, r.problem)) r.error = ManifestError::Invalid;
    }
    FreeLibrary(h);
    return r;
}

} // namespace mp
