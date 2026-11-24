#include "core/Thumbnails.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>

#include "core/ImageIO.h"
#include "core/ImageOps.h"
#include "core/ThumbCache.h"

namespace mp {

using Microsoft::WRL::ComPtr;

ImagePtr imageFromHBitmap(void* hbitmap) {
    auto* bmp = static_cast<HBITMAP>(hbitmap);
    BITMAP info{};
    if (!bmp || GetObjectW(bmp, sizeof(info), &info) == 0 || info.bmBitsPixel != 32 || info.bmWidth <= 0 || info.bmHeight == 0)
        return nullptr;
    const int w = info.bmWidth, h = info.bmHeight < 0 ? -info.bmHeight : info.bmHeight;
    try {
        auto img = std::make_shared<Image>(w, h);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h; // 由上往下
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC dc = GetDC(nullptr);
        const int rows = GetDIBits(dc, bmp, 0, static_cast<UINT>(h), img->pixels.data(), &bi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, dc);
        if (rows != h) return nullptr;
        bool anyAlpha = false;
        for (size_t i = 3; i < img->pixels.size(); i += 4)
            if (img->pixels[i]) { anyAlpha = true; break; }
        for (size_t i = 0; i < img->pixels.size(); i += 4) {
            uint8_t* p = &img->pixels[i];
            if (!anyAlpha) { p[3] = 255; continue; }
            for (int c = 0; c < 3; ++c) p[c] = std::min(p[c], p[3]);
        }
        return img;
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

ImagePtr windowsCachedThumbnail(const std::wstring& path, int side) {
    if (side <= 0) return nullptr;
    ensureThreadCom();
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return nullptr;
    HBITMAP bmp = nullptr;
    if (FAILED(factory->GetImage(SIZE{side, side}, SIIGBF_THUMBNAILONLY | SIIGBF_INCACHEONLY, &bmp)) || !bmp) return nullptr;
    ImagePtr img = imageFromHBitmap(bmp);
    DeleteObject(bmp);
    if (!img || (img->width <= side && img->height <= side)) return img;
    int tw = 0, th = 0;
    fitWithin(img->width, img->height, side, side, tw, th);
    try {
        return std::make_shared<const Image>(resampleFast(*img, tw, th));
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

ThumbResult loadThumbnail(const std::wstring& path, int side, ThumbCache* cache, bool useWindowsCache) {
    ThumbResult r;
    if (useWindowsCache) {
        if ((r.image = windowsCachedThumbnail(path, side))) {
            r.source = ThumbSource::Windows;
            return r;
        }
    }
    ThumbStamp stamp;
    const bool stamped = fileStamp(path, stamp);
    std::vector<uint8_t> bytes;
    if (cache && stamped && cache->get(path, side, stamp, bytes)) {
        if ((r.image = decodeThumbnail(bytes))) {
            r.source = ThumbSource::Cache;
            return r;
        }
    }
    ScaledDecode d = decodeScaled(path, side, side, false);
    if (!d.image) {
        r.error = d.error;
        return r;
    }
    r.image = d.image;
    r.source = ThumbSource::Decoded;
    if (cache && stamped) {
        bytes = encodeThumbnail(*d.image);
        if (!bytes.empty()) cache->put(path, side, stamp, bytes);
    }
    return r;
}

ThumbQueue::~ThumbQueue() { shutdown(); }

void ThumbQueue::start(int threads, Loader loader, Done done) {
    shutdown();
    loader_ = std::move(loader);
    done_ = std::move(done);
    stop_ = false;
    for (int i = 0; i < std::max(1, threads); ++i) threads_.emplace_back([this] { run(); });
}

void ThumbQueue::reset(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mu_);
    generation_ = generation;
    jobs_.clear();
}

void ThumbQueue::request(const ThumbJob& job) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (job.generation != generation_ || stop_) return;
        jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [&](const ThumbJob& j) { return j.index == job.index; }), jobs_.end());
        jobs_.push_front(job);
    }
    cv_.notify_one();
}

void ThumbQueue::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
        jobs_.clear();
    }
    cv_.notify_all();
    for (auto& t : threads_)
        if (t.joinable()) t.join();
    threads_.clear();
}

size_t ThumbQueue::pending() {
    std::lock_guard<std::mutex> lock(mu_);
    return jobs_.size();
}

void ThumbQueue::run() {
    // 背景執行緒：不要搶畫面的 CPU。
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        ThumbJob job;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [&] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        ThumbResult r;
        try {
            r = loader_(job.path, job.side);
        } catch (const std::bad_alloc&) {
            r = ThumbResult{};
            r.error.code = IoErrorCode::OutOfMemory;
        }
        done_(job, std::move(r));
    }
}

PreviewWorker::~PreviewWorker() { shutdown(); }

void PreviewWorker::start(Decoder decoder, Done done) {
    shutdown();
    decoder_ = std::move(decoder);
    done_ = std::move(done);
    stop_ = false;
    has_ = false;
    thread_ = std::thread([this] { run(); });
}

void PreviewWorker::request(uint64_t ticket, const std::wstring& path, int maxW, int maxH) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_) return;
        job_ = Job{ticket, path, maxW, maxH};
        has_ = true;
    }
    cv_.notify_one();
}

void PreviewWorker::cancel() {
    std::lock_guard<std::mutex> lock(mu_);
    has_ = false;
}

void PreviewWorker::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
        has_ = false;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void PreviewWorker::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [&] { return stop_ || has_; });
            if (stop_) return;
            job = std::move(job_);
            has_ = false;
        }
        ScaledDecode d;
        try {
            d = decoder_(job.path, job.maxW, job.maxH);
        } catch (const std::bad_alloc&) {
            d = ScaledDecode{};
            d.error.code = IoErrorCode::OutOfMemory;
        }
        done_(job.ticket, job.path, std::move(d));
    }
}

} // namespace mp
