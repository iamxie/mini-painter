#include "core/Update.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include "core/AppInfo.h"
#include "core/Archive.h"
#include "core/PathUtil.h"
#include "core/TextUtil.h"

namespace mp {

const wchar_t* const kDefaultUpdateUrl = L"https://api.github.com/repos/iamxie/mini-painter/releases/latest";

namespace {
UpdateError failure(UpdateErrorCode code) {
    UpdateError e;
    e.code = code;
    return e;
}
} // namespace

std::wstring packageZipName(const std::wstring& version) { return L"mini-painter-" + version + L"-win64.zip"; }

bool parseLatestRelease(const std::string& text, ReleaseInfo& out) {
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    auto str = [&](const nlohmann::json& o, const char* key) -> std::wstring {
        auto it = o.find(key);
        return it != o.end() && it->is_string() ? utf8ToWide(it->get<std::string>()) : L"";
    };
    ReleaseInfo r;
    Version v;
    std::wstring tag = str(j, "tag_name");
    if (!parseVersion(tag, v)) return false;
    r.version = std::to_wstring(v.major) + L"." + std::to_wstring(v.minor) + L"." + std::to_wstring(v.patch);
    r.notes = str(j, "body");
    r.pageUrl = str(j, "html_url");
    std::wstring zipName = packageZipName(r.version), shaName = zipName + L".sha256";
    auto assets = j.find("assets");
    if (assets != j.end() && assets->is_array()) {
        for (const auto& a : *assets) {
            if (!a.is_object()) continue;
            std::wstring name = str(a, "name"), url = str(a, "browser_download_url");
            if (_wcsicmp(name.c_str(), zipName.c_str()) == 0) r.zipUrl = url;
            else if (_wcsicmp(name.c_str(), shaName.c_str()) == 0) r.shaUrl = url;
        }
    }
    out = r;
    return true;
}

UpdateError fetchLatestRelease(const std::wstring& url, ReleaseInfo& out, HttpCancel* cancel) {
    std::string body;
    UpdateError e = httpGetText(url, body, cancel);
    if (!e.ok()) return e;
    if (!parseLatestRelease(body, out)) return failure(UpdateErrorCode::BadResponse);
    return {};
}

UpdateError downloadUpdate(const ReleaseInfo& release, const std::wstring& workDir, std::wstring& stagingDir,
                           const DownloadHooks& hooks) {
    auto stage = [&](DownloadStage s) {
        if (hooks.onStage) hooks.onStage(s);
    };
    auto cancelled = [&] { return hooks.cancel && hooks.cancel->cancelled(); };
    const std::wstring zip = joinPath(workDir, packageZipName(release.version));
    const std::wstring pkg = joinPath(workDir, L"pkg");

    stage(DownloadStage::Downloading);
    UpdateError e = httpDownload(release.zipUrl, zip, [&](uint64_t done, uint64_t total) {
        if (hooks.progress) hooks.progress(done, total);
        return !cancelled();
    }, hooks.cancel);
    if (!e.ok()) return e;
    if (cancelled()) return failure(UpdateErrorCode::Cancelled);

    stage(DownloadStage::Verifying);
    std::string shaText;
    e = httpGetText(release.shaUrl, shaText, hooks.cancel);
    if (!e.ok()) return e;
    const std::string want = parseSha256Text(shaText), got = sha256OfFile(zip);
    if (want.empty() || got != want) return failure(UpdateErrorCode::ChecksumMismatch);
    if (cancelled()) return failure(UpdateErrorCode::Cancelled);

    stage(DownloadStage::Unpacking);
    ArchiveError unpack = extractAll(zip, pkg);
    if (!unpack.empty()) {
        e = failure(UpdateErrorCode::UnpackFailed);
        e.archive = std::move(unpack);
        return e;
    }
    // 解壓本身停不下來：解完再看一次，解的期間按了取消就不交出去（呼叫端會照樣安裝）。
    if (cancelled()) return failure(UpdateErrorCode::Cancelled);
    if (GetFileAttributesW(joinPath(pkg, kPackageExeName).c_str()) == INVALID_FILE_ATTRIBUTES)
        return failure(UpdateErrorCode::NoExeInPackage);
    stagingDir = pkg;
    return {};
}

} // namespace mp
