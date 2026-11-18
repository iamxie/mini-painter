// JPEG XL reader plugin for mpainter, and the example to copy when writing a plugin.
//
// What a plugin needs (see ../sdk/mpainter_plugin.h for the full rules):
//   1. A manifest resource (jxl.rc + manifest.txt) so mpainter can list the plugin without running it.
//   2. One export, mpPluginGetFuncs, returning a table of three functions: probe, readSize, decode.
//   3. Those functions must be safe to call from several threads at once. Here every call creates its own
//      libjxl decoder, so nothing is shared.
//
// libjxl (BSD-3-Clause, https://github.com/libjxl/libjxl) is linked statically, together with its
// dependencies highway, brotli and lcms2. The DLL is built with the static C runtime (/MT), like mpainter,
// so it needs nothing but system DLLs.
//
// Behaviour:
//   - Only the first frame of an animation is decoded.
//   - Orientation stored in the file is applied (libjxl does it unless told to keep the orientation).
//   - Lossy (XYB) images are converted to sRGB. Lossless images keep the colour values stored in the file,
//     the same as mpainter does for its built-in formats (no colour management).
//   - JPEG XL has no resolution field of its own, so no DPI is reported.
#include <jxl/decode.h>
#include <jxl/encode.h> // JxlColorEncodingSetToSRGB
#include <jxl/resizable_parallel_runner.h>

#include <cstdio>
#include <cstring>

#include "plugins/sdk/mpainter_plugin.h"

namespace {

// Closes the decoder and the thread pool on every return path.
struct Decoder {
    JxlDecoder* dec = JxlDecoderCreate(nullptr);
    void* runner = nullptr;
    ~Decoder() {
        if (dec) JxlDecoderDestroy(dec);
        if (runner) JxlResizableParallelRunnerDestroy(runner);
    }
};

void setMessage(MpDecodeInfo* info, const char* text) {
    if (info) snprintf(info->message, sizeof(info->message), "%s", text);
}

// Width and height after orientation: orientations 5 to 8 swap them.
void orientedSize(const JxlBasicInfo& bi, int32_t* width, int32_t* height) {
    const bool swap = bi.orientation >= JXL_ORIENT_TRANSPOSE;
    *width = static_cast<int32_t>(swap ? bi.ysize : bi.xsize);
    *height = static_cast<int32_t>(swap ? bi.xsize : bi.ysize);
}

int32_t probe(const uint8_t* head, size_t size) {
    // A bare codestream starts with FF 0A; the container with the 12-byte "JXL " box.
    const JxlSignature sig = JxlSignatureCheck(head, size);
    return (sig == JXL_SIG_CODESTREAM || sig == JXL_SIG_CONTAINER) ? 0 : -1;
}

int32_t readSize(int32_t, const uint8_t* data, size_t size, int32_t* width, int32_t* height) {
    Decoder d;
    if (!d.dec) return MP_ERR_OUT_OF_MEMORY;
    // Ask only for the basic info: libjxl stops after the header and never touches the pixel data.
    if (JxlDecoderSubscribeEvents(d.dec, JXL_DEC_BASIC_INFO) != JXL_DEC_SUCCESS) return MP_ERR_INTERNAL;
    if (JxlDecoderSetInput(d.dec, data, size) != JXL_DEC_SUCCESS) return MP_ERR_INTERNAL;
    JxlDecoderCloseInput(d.dec);
    if (JxlDecoderProcessInput(d.dec) != JXL_DEC_BASIC_INFO) return MP_ERR_CORRUPT;
    JxlBasicInfo bi;
    if (JxlDecoderGetBasicInfo(d.dec, &bi) != JXL_DEC_SUCCESS) return MP_ERR_CORRUPT;
    orientedSize(bi, width, height);
    return MP_OK;
}

int32_t decode(int32_t, const uint8_t* data, size_t size, const MpHost* host, MpDecodeInfo* info) {
    Decoder d;
    if (!d.dec) return MP_ERR_OUT_OF_MEMORY;
    if (JxlDecoderSubscribeEvents(d.dec, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS)
        return MP_ERR_INTERNAL;
    // Output: 8-bit RGBA, alpha not premultiplied (mpainter wants straight alpha and premultiplies itself).
    const JxlPixelFormat format = {4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    JxlDecoderSetUnpremultiplyAlpha(d.dec, JXL_TRUE);
    if (JxlDecoderSetInput(d.dec, data, size) != JXL_DEC_SUCCESS) return MP_ERR_INTERNAL;
    JxlDecoderCloseInput(d.dec);

    JxlBasicInfo bi{};
    int32_t width = 0, height = 0;
    uint8_t* pixels = nullptr;
    for (;;) {
        const JxlDecoderStatus status = JxlDecoderProcessInput(d.dec);
        switch (status) {
        case JXL_DEC_BASIC_INFO: {
            if (JxlDecoderGetBasicInfo(d.dec, &bi) != JXL_DEC_SUCCESS) { setMessage(info, "cannot read the basic info"); return MP_ERR_CORRUPT; }
            orientedSize(bi, &width, &height);
            // Use several threads for big images; the pool lives only for this call.
            d.runner = JxlResizableParallelRunnerCreate(nullptr);
            if (d.runner) {
                JxlResizableParallelRunnerSetThreads(d.runner, JxlResizableParallelRunnerSuggestThreads(bi.xsize, bi.ysize));
                JxlDecoderSetParallelRunner(d.dec, JxlResizableParallelRunner, d.runner);
            }
            break;
        }
        case JXL_DEC_COLOR_ENCODING:
            // Lossy images are stored in XYB and can be turned into any colour space: ask for sRGB.
            // Lossless images keep the stored values (libjxl refuses a preferred profile for them).
            if (!bi.uses_original_profile) {
                JxlColorEncoding srgb;
                JxlColorEncodingSetToSRGB(&srgb, bi.num_color_channels == 1 ? JXL_TRUE : JXL_FALSE);
                JxlDecoderSetPreferredColorProfile(d.dec, &srgb);
            }
            break;
        case JXL_DEC_NEED_IMAGE_OUT_BUFFER: {
            size_t needed = 0;
            if (JxlDecoderImageOutBufferSize(d.dec, &format, &needed) != JXL_DEC_SUCCESS) { setMessage(info, "cannot size the output buffer"); return MP_ERR_INTERNAL; }
            if (needed != static_cast<size_t>(width) * static_cast<size_t>(height) * 4) { setMessage(info, "unexpected output buffer size"); return MP_ERR_INTERNAL; }
            pixels = host->allocImage(host->context, width, height);
            if (!pixels) return MP_ERR_OUT_OF_MEMORY;
            if (JxlDecoderSetImageOutBuffer(d.dec, &format, pixels, needed) != JXL_DEC_SUCCESS) { setMessage(info, "cannot set the output buffer"); return MP_ERR_INTERNAL; }
            break;
        }
        case JXL_DEC_FULL_IMAGE: {
            // The first frame is complete. libjxl wrote R, G, B, A; mpainter wants B, G, R, A.
            const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
            for (size_t i = 0; i < count; ++i) {
                uint8_t* p = pixels + i * 4;
                const uint8_t r = p[0];
                p[0] = p[2];
                p[2] = r;
            }
            return MP_OK;
        }
        case JXL_DEC_NEED_MORE_INPUT:
            setMessage(info, "the file is truncated");
            return MP_ERR_CORRUPT;
        case JXL_DEC_SUCCESS: // all input consumed without a full image
            setMessage(info, "the file has no image");
            return MP_ERR_CORRUPT;
        case JXL_DEC_ERROR:
            setMessage(info, "libjxl could not decode the file");
            return MP_ERR_CORRUPT;
        default:
            break; // events we did not subscribe to are not delivered; anything else, keep going
        }
    }
}

const MpPluginFuncs kFuncs = {MP_PLUGIN_API_VERSION, probe, readSize, decode};

} // namespace

MP_PLUGIN_EXPORT const MpPluginFuncs* mpPluginGetFuncs(uint32_t hostApiVersion) {
    return hostApiVersion == MP_PLUGIN_API_VERSION ? &kFuncs : nullptr;
}
