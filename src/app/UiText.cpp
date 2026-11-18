#include "app/UiText.h"

#include <windows.h>

#include <cwchar>

#include "core/Lang.h"
#include "core/PathUtil.h"
#include "core/TextUtil.h"

namespace mp {

std::wstring fmt1(const wchar_t* key, const std::wstring& a) {
    const wchar_t* f = tr(key);
    std::wstring out(wcslen(f) + a.size() + 8, L'\0');
    int n = swprintf(out.data(), out.size(), f, a.c_str());
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return out;
}

std::wstring fmt2(const wchar_t* key, const std::wstring& a, const std::wstring& b) {
    const wchar_t* f = tr(key);
    std::wstring out(wcslen(f) + a.size() + b.size() + 8, L'\0');
    int n = swprintf(out.data(), out.size(), f, a.c_str(), b.c_str());
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return out;
}

std::wstring archiveErrorText(const ArchiveError& e) {
    if (e.reason == ArchiveFailure::System && e.systemError) return systemErrorText(e.systemError);
    if (e.reason == ArchiveFailure::Other && !e.detail.empty()) return e.detail;
    return tr(archiveFailureMessage(e.reason));
}

namespace {
// 已經翻譯好的格式字串填一個 %s。（fmt1 要的是 key 的字串常值；這裡的 key 來自 core 的 trKey。）
std::wstring fillOne(const wchar_t* format, const std::wstring& a) {
    std::wstring out(wcslen(format) + a.size() + 8, L'\0');
    int n = swprintf(out.data(), out.size(), format, a.c_str());
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return out;
}
} // namespace

std::wstring updateErrorText(const UpdateError& e) {
    if (e.ok() || e.cancelled()) return L"";
    const wchar_t* format = tr(updateErrorMessage(e.code));
    std::wstring text;
    switch (e.code) {
    case UpdateErrorCode::HttpStatus: {
        wchar_t buf[128];
        swprintf(buf, 128, format, e.httpStatus);
        text = buf;
        break;
    }
    case UpdateErrorCode::UnpackFailed: text = fillOne(format, archiveErrorText(e.archive)); break;
    case UpdateErrorCode::BadUrl:
    case UpdateErrorCode::FolderInTheWay:
    case UpdateErrorCode::RenameFailed:
    case UpdateErrorCode::CopyFailed:
    case UpdateErrorCode::FolderNotWritable: text = fillOne(format, e.detail); break;
    default: text = format; break;
    }
    if (e.systemError != 0) text += L" (" + systemErrorText(e.systemError) + L")";
    return text;
}

std::wstring updateFailureText(const UpdateError& e) {
    switch (e.code) {
    case UpdateErrorCode::BadUrl:
    case UpdateErrorCode::NetworkUnavailable:
    case UpdateErrorCode::ConnectFailed:
    case UpdateErrorCode::HttpStatus:
    case UpdateErrorCode::DownloadInterrupted:
    case UpdateErrorCode::ResponseTooLarge:
    case UpdateErrorCode::CreateFileFailed:
    case UpdateErrorCode::WriteFileFailed: return fmt1(L"The download failed: %s", updateErrorText(e));
    default: return updateErrorText(e);
    }
}

namespace {
// 系統對這個碼的說明，去掉結尾的換行與句點；查不到回空字串。
std::wstring systemMessage(unsigned long code) {
    wchar_t buf[512];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buf, 512, nullptr);
    std::wstring s(buf, n);
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ' || s.back() == L'.' || s.back() == L'。'))
        s.pop_back();
    return s;
}
} // namespace

std::wstring systemErrorText(unsigned long code) {
    std::wstring s = systemMessage(code);
    return s.empty() ? L"error " + std::to_wstring(code) : s;
}

std::wstring flattenErrorText(const wchar_t* kind, long hr) {
    std::wstring text = tr(L"Could not draw the annotations onto the image");
    if (kind && *kind) text += std::wstring(L" [") + kind + L"]";
    if (hr != 0) {
        const wchar_t* f = tr(L"%s (HRESULT 0x%08X)");
        std::wstring out(wcslen(f) + text.size() + 16, L'\0');
        int n = swprintf(out.data(), out.size(), f, text.c_str(), static_cast<unsigned>(hr));
        out.resize(n > 0 ? static_cast<size_t>(n) : 0);
        text = out;
        // 系統查得到說明才加（記憶體不足這類通用的碼、Direct2D 的碼在 Win10 都查得到），查不到就只留代碼。
        if (std::wstring why = systemMessage(static_cast<unsigned long>(hr)); !why.empty()) text += L" (" + why + L")";
    }
    return text;
}

std::wstring ioErrorText(const IoError& e) {
    if (e.ok()) return L"";
    const wchar_t* key = ioErrorMessage(e.code);
    const bool hasDetail = e.code == IoErrorCode::UnsupportedSaveFormat || e.code == IoErrorCode::PluginFailed;
    std::wstring text = hasDetail ? fmt1(key, e.detail) : trs(key);
    if (e.hresult != 0) {
        const wchar_t* f = tr(L"%s (HRESULT 0x%08X)");
        std::wstring out(wcslen(f) + text.size() + 16, L'\0');
        int n = swprintf(out.data(), out.size(), f, text.c_str(), static_cast<unsigned>(e.hresult));
        out.resize(n > 0 ? static_cast<size_t>(n) : 0);
        text = out;
    }
    if (e.systemError != 0) text += L" (" + systemErrorText(e.systemError) + L")";
    return text;
}

std::wstring metaLabelText(const MetaItem& item) { return item.labelIsKey ? trs(item.label.c_str()) : item.label; }

std::wstring metaValueText(const MetaValue& value) {
    wchar_t buf[64];
    switch (value.kind) {
    case MetaValue::Kind::Text: return value.text;
    case MetaValue::Kind::ByteCount:
        swprintf(buf, 64, tr(L"(%lu bytes)"), static_cast<unsigned long>(value.number));
        return buf;
    case MetaValue::Kind::Bool: return value.number ? tr(L"Yes") : tr(L"No");
    case MetaValue::Kind::UnknownType:
        swprintf(buf, 64, tr(L"(type %u)"), static_cast<unsigned>(value.number));
        return buf;
    }
    return L"";
}

std::wstring fileSizeText(unsigned long long bytes) { return formatFileSize(bytes, tr(L"%llu bytes")); }

std::wstring quotedNameList(const std::vector<std::wstring>& names) {
    if (names.empty()) return L"";
    std::wstring out = fmt1(L"“%s”", names[0]);
    for (size_t i = 1; i < names.size(); ++i)
        out = i + 1 == names.size() ? fmt2(L"%s and “%s”", out, names[i]) : fmt2(L"%s, “%s”", out, names[i]);
    return out;
}

std::wstring pluginCrashText(const std::wstring& name, const PluginCrash& c) {
    const std::wstring quoted = quotedNameList({name});
    const std::wstring text =
        c.path.empty()
            ? fmt1(L"Last time mpainter crashed because the plugin %s failed, so the plugin has been disabled. "
                   L"To use it again, enable it on the “Plugins” page in Settings.",
                   quoted)
            : fmt2(L"Last time mpainter crashed while reading %s because the plugin %s failed, so the plugin has been disabled. "
                   L"To use it again, enable it on the “Plugins” page in Settings.",
                   fileNameOf(c.path), quoted);
    std::wstring diag = c.pluginId;
    if (!c.func.empty()) diag += L", " + c.func;
    wchar_t code[32];
    swprintf(code, 32, L"0x%08lX", c.exceptionCode);
    diag += L", ";
    diag += c.exceptionCode ? code : L"no exception record";
    return fmt2(L"%s (%s)", text, diag);
}

std::wstring pluginConflictText(const std::vector<std::wstring>& names, const PluginConflictGroup& g) {
    std::wstring exts, diagExts;
    for (const auto& e : g.extensions) {
        exts += (exts.empty() ? L"" : L"/") + e;
        diagExts += (diagExts.empty() ? L"" : L", ") + e;
    }
    const std::wstring text =
        names.size() == 2
            ? fmt2(L"The plugins %s both want to open %s files and cannot be enabled together, so both have been disabled. "
                   L"Go to the “Plugins” page in Settings and choose one to enable.",
                   quotedNameList(names), exts)
            : fmt2(L"The plugins %s all want to open %s files and cannot be enabled together, so all of them have been disabled. "
                   L"Go to the “Plugins” page in Settings and choose one to enable.",
                   quotedNameList(names), exts);
    std::wstring diag = L"plugin conflict: ";
    for (size_t i = 0; i < g.ids.size(); ++i) diag += (i ? L", " : L"") + g.ids[i];
    diag += L": " + diagExts;
    return fmt2(L"%s (%s)", text, diag);
}

std::wstring pluginProblemText(const PluginEntry& e, const PluginLoadResult* loaded) {
    const ManifestRead& m = e.manifest;
    switch (m.error) {
    case ManifestError::Unreadable:
        return fmt2(L"%s (%s)", trs(L"The plugin file could not be read."), L"manifest unreadable, error " + std::to_wstring(m.systemError));
    case ManifestError::NotFound: return fmt2(L"%s (%s)", trs(L"This is not an mpainter plugin."), L"no MPAINTER_PLUGIN resource");
    case ManifestError::Invalid: return fmt2(L"%s (%s)", trs(L"The plugin information is not valid."), L"bad manifest: " + m.problem);
    case ManifestError::None: break;
    }
    const wchar_t* otherVersion = trKey(L"This plugin was made for a different version of mpainter.");
    if (!pluginApiSupported(m.manifest.api))
        return fmt2(L"%s (%s)", trs(otherVersion),
                    L"plugin api " + std::to_wstring(m.manifest.api) + L", mpainter api " + std::to_wstring(kPluginApiOldest) +
                        L"-" + std::to_wstring(kPluginApiVersion));
    if (!loaded) return L"";
    switch (loaded->error) {
    case PluginLoadError::LoadFailed:
        return fmt2(L"%s (%s)", fmt1(L"The plugin could not be loaded: %s", systemErrorText(loaded->systemError)),
                    L"LoadLibrary error " + std::to_wstring(loaded->systemError));
    case PluginLoadError::NoEntry: return fmt2(L"%s (%s)", trs(L"This is not an mpainter plugin."), L"no mpPluginGetFuncs export");
    case PluginLoadError::Incompatible: return fmt2(L"%s (%s)", trs(otherVersion), L"mpPluginGetFuncs returned no usable table");
    default: return L"";
    }
}

std::wstring pluginStatusText(const PluginEntry& e, bool checked, const PluginSetting* record, const PluginLoadResult* loaded) {
    if (pluginStaticCheck(e) != PluginLoadError::None) return tr(L"Cannot be used");
    const bool running = loaded && loaded->error == PluginLoadError::None;
    if (checked) {
        if (running) return tr(L"Enabled");
        return loaded ? tr(L"Could not load") : tr(L"Enabled after restart");
    }
    if (running) return tr(L"Disabled after restart");
    if (record && record->state == PluginState::Crashed) return tr(L"Disabled: it crashed mpainter");
    if (record && record->state == PluginState::Conflict) return tr(L"Disabled: conflicts with another plugin");
    return tr(L"Disabled");
}

std::wstring editLockText(EditLock lock) {
    switch (lock) {
    case EditLock::Archive:
        return tr(L"This image is inside an archive and is read-only.\n\nExtract the archive first if you want to edit it.");
    case EditLock::Plugin:
        return tr(L"This image was opened by a plugin and is read-only.\n\nUse Export As to save a copy in another format.");
    case EditLock::None: break;
    }
    return L"";
}

} // namespace mp
