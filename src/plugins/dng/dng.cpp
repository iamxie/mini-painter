// DNG (Adobe Digital Negative) reader plugin for mpainter.
// This product includes DNG technology under license by Adobe.
//
// Decoding is done by the Adobe DNG SDK 1.7.1 (src/third_party/dng_sdk, DNG SDK License Agreement in its
// LICENSE.txt; the changes made for mpainter are listed in its MODIFICATIONS.txt). libjpeg 9c comes with
// the SDK; zlib and libjxl (with highway, brotli and lcms2) come from vcpkg. Everything is linked statically
// with the static C runtime (/MT), so the DLL needs nothing but system DLLs.
//
// Behaviour:
//   - The main image is rendered the way the SDK's dng_validate does: default crop, the camera profile and
//     tone curve stored in the file, lens corrections stored as opcodes, orientation, sRGB, 8 bits.
//     When the file has an enhanced image (for example Lightroom's Denoise), that one is shown.
//   - The embedded previews are not used: the picture always comes from the raw data.
//   - No DPI is reported: the resolution tags of a DNG describe the preview, not the raw image.
//   - The SDK is built without the XMP SDK, so XMP metadata in the file is ignored.
#include <cstdio>
#include <string>

#include "plugins/dng/DngRender.h"
#include "plugins/sdk/mpainter_plugin.h"

namespace {

void setMessage(MpDecodeInfo* info, const std::string& text) {
    if (info) snprintf(info->message, sizeof(info->message), "%s", text.c_str());
}

int32_t probe(const uint8_t* data, size_t size) { return mpdng::looksLikeDng(data, size) ? 0 : -1; }

int32_t readSize(int32_t, const uint8_t* data, size_t size, int32_t* width, int32_t* height) {
    std::string message;
    return mpdng::dngReadSize(data, size, width, height, &message);
}

int32_t decode(int32_t, const uint8_t* data, size_t size, const MpHost* host, MpDecodeInfo* info) {
    std::string message;
    const int32_t rc = mpdng::dngDecode(data, size, mpdng::defaultThreads(), host->allocImage, host->context, &message);
    if (rc != MP_OK) setMessage(info, message);
    return rc;
}

const MpPluginFuncs kFuncs = {MP_PLUGIN_API_VERSION, probe, readSize, decode};

} // namespace

MP_PLUGIN_EXPORT const MpPluginFuncs* mpPluginGetFuncs(uint32_t hostApiVersion) {
    return hostApiVersion == MP_PLUGIN_API_VERSION ? &kFuncs : nullptr;
}
