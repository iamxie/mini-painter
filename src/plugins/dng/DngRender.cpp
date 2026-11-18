// DNG decoding on top of the Adobe DNG SDK. See DngRender.h.
// This product includes DNG technology under license by Adobe.
#include "plugins/dng/DngRender.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <new>
#include <thread>
#include <vector>

#include "dng_area_task.h"
#include "dng_auto_ptr.h"
#include "dng_color_space.h"
#include "dng_errors.h"
#include "dng_exceptions.h"
#include "dng_host.h"
#include "dng_image.h"
#include "dng_info.h"
#include "dng_negative.h"
#include "dng_orientation.h"
#include "dng_pixel_buffer.h"
#include "dng_rect.h"
#include "dng_render.h"
#include "dng_stream.h"
#include "dng_tag_values.h"

#include "plugins/sdk/mpainter_plugin.h"

namespace mpdng {
namespace {

// Smallest grid line origin + k * step that is >= y.
int32 snapUp(int32 y, int32 origin, int32 step) {
    const int64 d = static_cast<int64>(y) - origin;
    const int64 k = d >= 0 ? (d + step - 1) / step : -((-d) / step);
    return static_cast<int32>(origin + k * step);
}

// dng_host whose area tasks (demosaic, colour conversion, opcodes...) run on several threads. The SDK's
// own dng_host runs every task on the calling thread.
class Host : public dng_host {
public:
    explicit Host(unsigned threads) : threads_(threads < 1 ? 1 : threads) {}

    uint32 PerformAreaTaskThreads() override { return threads_; }

    void PerformAreaTask(dng_area_task& task, const dng_rect& area, dng_area_task_progress* progress) override {
        const uint32 wanted = std::min<uint32>(threads_, task.MaxThreads());
        if (wanted <= 1 || area.IsEmpty()) {
            dng_host::PerformAreaTask(task, area, progress);
            return;
        }
        const dng_point tileSize = task.FindTileSize(area);
        // Horizontal bands whose edges sit on the grid that dng_area_task::ProcessOnThread walks anyway (the
        // first repeating tile, or else the tile size counted from the top of the area). Each band is then
        // cut into exactly the tiles a single thread would use.
        const dng_rect repeat = task.RepeatingTile1();
        const int32 step = repeat.IsEmpty() ? tileSize.v : static_cast<int32>(repeat.H());
        const int32 origin = repeat.IsEmpty() ? area.t : repeat.t;
        std::vector<dng_rect> bands;
        int32 top = area.t;
        for (uint32 i = 1; i < wanted && top < area.b; ++i) {
            const int32 target = area.t + static_cast<int32>(static_cast<int64>(area.H()) * i / wanted);
            const int32 edge = snapUp(target, origin, step);
            if (edge <= top || edge >= area.b) continue;
            bands.push_back(dng_rect(top, area.l, edge, area.r));
            top = edge;
        }
        bands.push_back(dng_rect(top, area.l, area.b, area.r));
        if (bands.size() == 1) {
            dng_host::PerformAreaTask(task, area, progress);
            return;
        }

        const uint32 used = static_cast<uint32>(bands.size());
        task.Start(used, area, tileSize, &Allocator(), Sniffer());
        std::vector<std::exception_ptr> errors(used);
        auto run = [&](uint32 i) {
            try {
                task.ProcessOnThread(i, bands[i], tileSize, Sniffer(), progress);
            } catch (...) {
                errors[i] = std::current_exception();
            }
        };
        // Workers get the caller's priority: mpainter decodes ahead in a low-priority background thread.
        const int priority = GetThreadPriority(GetCurrentThread());
        std::vector<std::thread> pool;
        uint32 started = 1;
        try {
            for (; started < used; ++started) {
                pool.emplace_back([&, started] {
                    SetThreadPriority(GetCurrentThread(), priority);
                    run(started);
                });
            }
        } catch (...) {
            // Could not start a thread: the calling thread does the bands that have none.
        }
        run(0);
        for (uint32 i = started; i < used; ++i) run(i);
        for (auto& t : pool) t.join();
        task.Finish(used);
        for (const auto& e : errors)
            if (e) std::rethrow_exception(e);
    }

private:
    uint32 threads_;
};

int32_t fail(std::string* message, int32_t code, const std::string& text) {
    if (message) *message = text;
    return code;
}

const char* sdkErrorName(dng_error_code code) {
    switch (code) {
    case dng_error_unknown: return "unknown";
    case dng_error_not_yet_implemented: return "not_yet_implemented";
    case dng_error_silent: return "silent";
    case dng_error_user_canceled: return "user_canceled";
    case dng_error_host_insufficient: return "host_insufficient";
    case dng_error_memory: return "memory";
    case dng_error_bad_format: return "bad_format";
    case dng_error_matrix_math: return "matrix_math";
    case dng_error_open_file: return "open_file";
    case dng_error_read_file: return "read_file";
    case dng_error_write_file: return "write_file";
    case dng_error_end_of_file: return "end_of_file";
    case dng_error_file_is_damaged: return "file_is_damaged";
    case dng_error_image_too_big_dng: return "image_too_big_dng";
    case dng_error_image_too_big_tiff: return "image_too_big_tiff";
    case dng_error_unsupported_dng: return "unsupported_dng";
    case dng_error_overflow: return "overflow";
    case dng_error_jxl_encoder: return "jxl_encoder";
    case dng_error_jxl_decoder: return "jxl_decoder";
    default: return "other";
    }
}

// The SDK's error code goes into the message as it is, so that bug reports carry it.
int32_t fromSdkError(dng_error_code code, std::string* message) {
    char tag[64];
    snprintf(tag, sizeof(tag), " (DNG SDK error %d %s)", static_cast<int>(code), sdkErrorName(code));
    switch (code) {
    case dng_error_memory: return fail(message, MP_ERR_OUT_OF_MEMORY, std::string("not enough memory") + tag);
    case dng_error_bad_format:
    case dng_error_read_file:
    case dng_error_end_of_file:
    case dng_error_file_is_damaged:
    case dng_error_jxl_decoder:
        return fail(message, MP_ERR_CORRUPT, std::string("the file is damaged or is not a valid DNG") + tag);
    case dng_error_not_yet_implemented:
    case dng_error_unsupported_dng:
        return fail(message, MP_ERR_UNSUPPORTED, std::string("this DNG uses something the DNG SDK does not support") + tag);
    default: return fail(message, MP_ERR_INTERNAL, std::string("the DNG SDK could not read the file") + tag);
    }
}

// Runs body and turns every exception into an MP_* code: nothing may leave the plugin.
template <typename F>
int32_t guarded(F&& body, std::string* message) {
    try {
        return body();
    } catch (const dng_exception& e) {
        return fromSdkError(e.ErrorCode(), message);
    } catch (const std::bad_alloc&) {
        return fail(message, MP_ERR_OUT_OF_MEMORY, "not enough memory");
    } catch (const std::exception& e) {
        return fail(message, MP_ERR_INTERNAL, std::string("unexpected error: ") + e.what());
    } catch (...) {
        return fail(message, MP_ERR_INTERNAL, "unexpected error");
    }
}

// Tags into info and a negative; the image data is not read. False when the file is not a valid DNG.
bool parse(Host& host, dng_stream& stream, dng_info& info, AutoPtr<dng_negative>& negative) {
    info.Parse(host, stream);
    info.PostParse(host);
    if (!info.IsValidDNG()) return false;
    negative.Reset(host.Make_dng_negative());
    negative->Parse(host, stream, info);
    negative->PostParse(host, stream, info);
    return true;
}

int32_t copyOut(const dng_image& image, AllocFn alloc, void* allocContext, std::string* message) {
    const dng_rect bounds = image.Bounds();
    const uint32 planes = image.Planes();
    if (bounds.IsEmpty() || (planes != 1 && planes != 3))
        return fail(message, MP_ERR_INTERNAL, "the rendered image has " + std::to_string(planes) + " planes");
    const int32 w = static_cast<int32>(bounds.W()), h = static_cast<int32>(bounds.H());
    uint8_t* dst = alloc(allocContext, w, h);
    if (!dst) return fail(message, MP_ERR_OUT_OF_MEMORY, "not enough memory for the output image");
    constexpr int32 kRows = 64; // copy in strips: no second full-size buffer
    std::vector<uint8> strip(static_cast<size_t>(w) * planes * kRows);
    for (int32 top = 0; top < h; top += kRows) {
        const int32 rows = std::min(kRows, h - top);
        const dng_rect r(bounds.t + top, bounds.l, bounds.t + top + rows, bounds.r);
        dng_pixel_buffer buffer(r, 0, planes, ttByte, pcInterleaved, strip.data());
        image.Get(buffer);
        for (int32 y = 0; y < rows; ++y) {
            const uint8* s = strip.data() + static_cast<size_t>(y) * w * planes;
            uint8_t* d = dst + static_cast<size_t>(top + y) * w * 4;
            for (int32 x = 0; x < w; ++x, d += 4, s += planes) {
                if (planes == 3) {
                    d[0] = s[2];
                    d[1] = s[1];
                    d[2] = s[0];
                } else {
                    d[0] = d[1] = d[2] = s[0];
                }
                d[3] = 255;
            }
        }
    }
    return MP_OK;
}

} // namespace

bool looksLikeDng(const uint8_t* data, size_t size) {
    if (!data || size < 8) return false;
    bool little;
    if (data[0] == 'I' && data[1] == 'I') little = true;
    else if (data[0] == 'M' && data[1] == 'M') little = false;
    else return false;
    auto u16 = [&](size_t o) -> uint32_t {
        return little ? (data[o] | (data[o + 1] << 8)) : ((data[o] << 8) | data[o + 1]);
    };
    auto u32 = [&](size_t o) -> uint32_t {
        return little ? (data[o] | (data[o + 1] << 8) | (data[o + 2] << 16) | (static_cast<uint32_t>(data[o + 3]) << 24))
                      : ((static_cast<uint32_t>(data[o]) << 24) | (data[o + 1] << 16) | (data[o + 2] << 8) | data[o + 3]);
    };
    if (u16(2) != 42) return false; // not classic TIFF (DNG is never BigTIFF)
    const uint64_t ifd = u32(4);
    if (ifd < 8 || ifd + 2 > size) return false;
    const uint32_t count = u16(static_cast<size_t>(ifd));
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t entry = ifd + 2 + static_cast<uint64_t>(i) * 12;
        if (entry + 12 > size) return false;
        if (u16(static_cast<size_t>(entry)) == 50706) return true; // DNGVersion
    }
    return false;
}

int32_t dngReadSize(const uint8_t* data, size_t size, int32_t* width, int32_t* height, std::string* message) {
    return guarded(
        [&]() -> int32_t {
            if (size > 0xFFFFFFFFu) return fail(message, MP_ERR_UNSUPPORTED, "files larger than 4 GB are not supported");
            Host host(1);
            dng_stream stream(data, static_cast<uint32>(size));
            dng_info info;
            AutoPtr<dng_negative> negative;
            if (!parse(host, stream, info, negative)) return fail(message, MP_ERR_CORRUPT, "not a valid DNG file");
            negative->SynchronizeMetadata();
            uint32 w = negative->DefaultFinalWidth(), h = negative->DefaultFinalHeight();
            if (negative->Orientation().FlipD()) std::swap(w, h);
            if (w == 0 || h == 0 || w > 0x7FFFFFFF || h > 0x7FFFFFFF) return fail(message, MP_ERR_CORRUPT, "bad image size");
            *width = static_cast<int32_t>(w);
            *height = static_cast<int32_t>(h);
            return MP_OK;
        },
        message);
}

int32_t dngDecode(const uint8_t* data, size_t size, unsigned threads, AllocFn alloc, void* allocContext, std::string* message) {
    return guarded(
        [&]() -> int32_t {
            if (size > 0xFFFFFFFFu) return fail(message, MP_ERR_UNSUPPORTED, "files larger than 4 GB are not supported");
            Host host(threads);
            dng_stream stream(data, static_cast<uint32>(size));
            dng_info info;
            AutoPtr<dng_negative> negative;
            if (!parse(host, stream, info, negative)) return fail(message, MP_ERR_CORRUPT, "not a valid DNG file");
            // Same steps as the SDK's dng_validate -tif (dng_validate.cpp), minus checks and extra outputs.
            if (info.fEnhancedIndex != -1 && !host.IgnoreEnhanced()) negative->ReadEnhancedImage(host, stream, info);
            else negative->ReadStage1Image(host, stream, info);
            if (info.fMaskIndex != -1) negative->ReadTransparencyMask(host, stream, info);
            negative->SynchronizeMetadata();
            if (negative->Stage1Image()) negative->BuildStage2Image(host);
            if (negative->Stage2Image()) negative->BuildStage3Image(host);
            else negative->ResizeTransparencyToMatchStage3(host);
            if (negative->NeedFlattenTransparency(host)) negative->FlattenTransparency(host);

            AutoPtr<dng_image> image;
            {
                dng_render render(host, *negative);
                render.SetFinalSpace(dng_space_sRGB::Get());
                render.SetFinalPixelType(ttByte);
                image.Reset(render.Render());
            }
            image->Rotate(negative->Orientation());
            negative.Reset(); // free the raw stages before the output is allocated
            return copyOut(*image, alloc, allocContext, message);
        },
        message);
}

unsigned defaultThreads() {
    const unsigned n = std::thread::hardware_concurrency();
    return std::clamp(n, 1u, 8u);
}

} // namespace mpdng
