// 瀏覽資料夾的縮圖：SQLite 快取、快速解碼、縮圖的位元組、shell 的點陣圖、背景佇列與預覽。
#include <catch2/catch_test_macros.hpp>

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "core/ImageIO.h"
#include "core/PathUtil.h"
#include "core/ThumbCache.h"
#include "core/Thumbnails.h"
#include "test_util.h"

using namespace mp;
using namespace mp::test;
using Microsoft::WRL::ComPtr;

namespace {

std::vector<uint8_t> bytesOf(size_t n, uint8_t v) { return std::vector<uint8_t>(n, v); }

// 左半紅、右半藍的圖。
Image halves(int w, int h) {
    Image img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = img.px(x, y);
            const bool left = x < w / 2;
            p[0] = left ? 0 : 255; // B
            p[1] = 0;
            p[2] = left ? 255 : 0; // R
            p[3] = 255;
        }
    return img;
}

// 用 WIC 存一張 JPEG，EXIF 的方向欄位寫 orientation（saveImage 會把方向歸 1，所以這裡自己寫）。
void writeJpegWithOrientation(const std::wstring& path, const Image& img, unsigned short orientation) {
    ensureThreadCom();
    ComPtr<IWICImagingFactory> f;
    REQUIRE(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))));
    ComPtr<IWICStream> stream;
    REQUIRE(SUCCEEDED(f->CreateStream(&stream)));
    REQUIRE(SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)));
    ComPtr<IWICBitmapEncoder> enc;
    REQUIRE(SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)));
    REQUIRE(SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)));
    ComPtr<IWICBitmapFrameEncode> frame;
    REQUIRE(SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)));
    REQUIRE(SUCCEEDED(frame->Initialize(nullptr)));
    REQUIRE(SUCCEEDED(frame->SetSize(static_cast<UINT>(img.width), static_cast<UINT>(img.height))));
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    REQUIRE(SUCCEEDED(frame->SetPixelFormat(&pf)));
    ComPtr<IWICMetadataQueryWriter> qw;
    REQUIRE(SUCCEEDED(frame->GetMetadataQueryWriter(&qw)));
    PROPVARIANT v;
    PropVariantInit(&v);
    v.vt = VT_UI2;
    v.uiVal = orientation;
    REQUIRE(SUCCEEDED(qw->SetMetadataByName(L"/app1/ifd/{ushort=274}", &v)));
    ComPtr<IWICBitmap> bmp;
    REQUIRE(SUCCEEDED(f->CreateBitmapFromMemory(static_cast<UINT>(img.width), static_cast<UINT>(img.height), GUID_WICPixelFormat32bppBGRA,
                                                static_cast<UINT>(img.stride()), static_cast<UINT>(img.pixels.size()),
                                                const_cast<BYTE*>(img.pixels.data()), &bmp)));
    REQUIRE(SUCCEEDED(frame->WriteSource(bmp.Get(), nullptr)));
    REQUIRE(SUCCEEDED(frame->Commit()));
    REQUIRE(SUCCEEDED(enc->Commit()));
}

bool reddish(const uint8_t* p) { return p[2] > 150 && p[0] < 100; }
bool bluish(const uint8_t* p) { return p[0] > 150 && p[2] < 100; }

} // namespace

TEST_CASE("fitWithin：照比例縮進框裡、不放大、每邊至少 1") {
    int w = 0, h = 0;
    fitWithin(400, 300, 100, 100, w, h);
    CHECK((w == 100 && h == 75));
    fitWithin(300, 400, 100, 100, w, h);
    CHECK((w == 75 && h == 100));
    fitWithin(50, 40, 100, 100, w, h);
    CHECK((w == 50 && h == 40)); // 不放大
    fitWithin(10000, 1, 100, 100, w, h);
    CHECK((w == 100 && h == 1));
    fitWithin(400, 300, 0, 100, w, h);
    CHECK((w == 400 && h == 300)); // 框不合理就不縮
}

TEST_CASE("ThumbCache：存、讀、key 不分大小寫；原檔改過就當成沒有並刪掉那一筆") {
    const std::wstring dir = freshDir(L"thumbcache_basic");
    ThumbCache c;
    REQUIRE(c.open(joinPath(dir, kThumbCacheFileName), 1024 * 1024));
    CHECK(c.isOpen());
    const ThumbStamp st{100, 12345};
    CHECK(c.put(L"D:\\Pics\\A.jpg", 160, st, bytesOf(10, 7)));
    std::vector<uint8_t> out;
    CHECK(c.get(L"d:\\pics\\a.JPG", 160, st, out));
    CHECK(out == bytesOf(10, 7));
    CHECK_FALSE(c.get(L"D:\\Pics\\A.jpg", 96, st, out)); // 別的大小是另一筆
    CHECK(c.count() == 1);
    CHECK_FALSE(c.get(L"D:\\Pics\\A.jpg", 160, ThumbStamp{100, 99999}, out)); // 修改時間不同
    CHECK(c.count() == 0);
    CHECK(c.put(L"D:\\Pics\\A.jpg", 160, st, bytesOf(10, 1)));
    CHECK(c.put(L"D:\\Pics\\A.jpg", 160, st, bytesOf(20, 2))); // 同一個 key 取代
    CHECK(c.count() == 1);
    CHECK(c.totalBytes() == 20);
    CHECK(thumbCacheFileBytes(joinPath(dir, kThumbCacheFileName)) > 0);
}

TEST_CASE("ThumbCache：超過上限丟最久沒用到的（讀過的算用過）；改小上限立刻丟；0 全丟；clear") {
    const std::wstring dir = freshDir(L"thumbcache_trim");
    ThumbCache c;
    REQUIRE(c.open(joinPath(dir, kThumbCacheFileName), 3500));
    const ThumbStamp st{1, 1};
    std::vector<uint8_t> out;
    CHECK(c.put(L"a", 1, st, bytesOf(1000, 1)));
    CHECK(c.put(L"b", 1, st, bytesOf(1000, 2)));
    CHECK(c.put(L"c", 1, st, bytesOf(1000, 3)));
    CHECK(c.get(L"a", 1, st, out)); // a 變成最近用過的
    CHECK(c.put(L"d", 1, st, bytesOf(1000, 4))); // 4000 > 3500：丟到九成（3150）以下 → 丟 b
    CHECK(c.totalBytes() <= 3500);
    CHECK_FALSE(c.get(L"b", 1, st, out));
    CHECK(c.get(L"a", 1, st, out));
    CHECK(c.get(L"d", 1, st, out));
    c.setLimit(1500);
    CHECK(c.totalBytes() <= 1500);
    CHECK(c.get(L"d", 1, st, out)); // 最近用過的留著
    c.setLimit(0);
    CHECK(c.count() == 0);
    c.setLimit(100000);
    CHECK(c.put(L"e", 1, st, bytesOf(10, 5)));
    c.clear();
    CHECK(c.count() == 0);
    CHECK(c.totalBytes() == 0);
}

TEST_CASE("ThumbCache：兩個視窗（兩個連線）同時用同一個檔，一邊存的另一邊讀得到") {
    const std::wstring path = joinPath(freshDir(L"thumbcache_two"), kThumbCacheFileName);
    ThumbCache a, b;
    REQUIRE(a.open(path, 1024 * 1024));
    REQUIRE(b.open(path, 1024 * 1024));
    const ThumbStamp st{5, 6};
    CHECK(a.put(L"x.png", 96, st, bytesOf(30, 9)));
    std::vector<uint8_t> out;
    CHECK(b.get(L"x.png", 96, st, out));
    CHECK(out.size() == 30);
}

TEST_CASE("ThumbCache：檔案壞了（不是資料庫）就刪掉重建；上限 0、網路路徑不開") {
    const std::wstring dir = freshDir(L"thumbcache_bad");
    const std::wstring path = joinPath(dir, kThumbCacheFileName);
    writeBytes(path, std::string(8192, 'x'));
    ThumbCache c;
    REQUIRE(c.open(path, 1024 * 1024));
    CHECK(c.put(L"k", 1, ThumbStamp{1, 1}, bytesOf(4, 1)));
    CHECK(c.count() == 1);

    ThumbCache zero;
    CHECK_FALSE(zero.open(joinPath(dir, L"zero.db"), 0));
    CHECK_FALSE(zero.isOpen());
    std::vector<uint8_t> out;
    CHECK_FALSE(zero.get(L"k", 1, ThumbStamp{1, 1}, out));
    CHECK_FALSE(zero.put(L"k", 1, ThumbStamp{1, 1}, bytesOf(4, 1)));

    ThumbCache net;
    CHECK_FALSE(net.open(L"\\\\mpainter-no-such-server\\share\\thumbs.mpainter.db", 1024 * 1024));
}

TEST_CASE("isNetworkPath：UNC 與 \\\\?\\UNC 是網路；本機磁碟與 \\\\?\\C: 不是") {
    CHECK(isNetworkPath(L"\\\\server\\share\\x"));
    CHECK(isNetworkPath(L"//server/share/x"));
    CHECK(isNetworkPath(L"\\\\?\\UNC\\server\\share\\x"));
    CHECK_FALSE(isNetworkPath(L"\\\\?\\C:\\x"));
    CHECK_FALSE(isNetworkPath(L"C:\\Windows"));
    CHECK_FALSE(isNetworkPath(L"relative\\path"));
}

TEST_CASE("fileStamp：檔案的大小與修改時間；資料夾與不存在的回 false") {
    const std::wstring dir = freshDir(L"thumb_stamp");
    const std::wstring f = joinPath(dir, L"a.bin");
    writeBytes(f, "12345");
    ThumbStamp st;
    REQUIRE(fileStamp(f, st));
    CHECK(st.size == 5);
    CHECK(st.mtime != 0);
    CHECK_FALSE(fileStamp(dir, st));
    CHECK_FALSE(fileStamp(joinPath(dir, L"none"), st));
}

TEST_CASE("decodeScaled：PNG 縮進框裡，給原圖的寬高與位元深度；不放大；帶 metadata 時有 info") {
    const std::wstring dir = freshDir(L"decode_scaled");
    const std::wstring png = joinPath(dir, L"a.png");
    writePng(png, 400, 300);
    ScaledDecode d = decodeScaled(png, 100, 100, false);
    REQUIRE(d.image);
    CHECK(d.image->width == 100);
    CHECK(d.image->height == 75);
    CHECK(d.width == 400);
    CHECK(d.height == 300);
    CHECK(d.info.format == ImageFormat::Png);
    CHECK(d.info.bitsPerPixel > 0);
    d = decodeScaled(png, 1000, 1000, true);
    REQUIRE(d.image);
    CHECK(d.image->width == 400); // 不放大
    // 不是圖
    const std::wstring txt = joinPath(dir, L"b.png");
    writeBytes(txt, "not an image");
    d = decodeScaled(txt, 100, 100, false);
    CHECK_FALSE(d.image);
    CHECK_FALSE(d.error.ok());
}

TEST_CASE("decodeScaled：大 JPEG 走解碼器的縮小解，大小與內容對；EXIF 方向轉正") {
    const std::wstring dir = freshDir(L"decode_scaled_jpeg");
    const std::wstring jpg = joinPath(dir, L"big.jpg");
    const Image src = halves(1600, 800);
    REQUIRE(saveImage(src, jpg));
    ScaledDecode d = decodeScaled(jpg, 200, 200, false);
    REQUIRE(d.image);
    CHECK(d.image->width == 200);
    CHECK(d.image->height == 100);
    CHECK(d.width == 1600);
    CHECK(d.height == 800);
    CHECK(reddish(d.image->px(20, 50)));
    CHECK(bluish(d.image->px(180, 50)));

    // 方向 6（順時針轉 90 度看）：顯示成 800 × 1600，左半（紅）到上面
    const std::wstring rot = joinPath(dir, L"rot6.jpg");
    writeJpegWithOrientation(rot, src, 6);
    d = decodeScaled(rot, 200, 200, false);
    REQUIRE(d.image);
    CHECK(d.width == 800);
    CHECK(d.height == 1600);
    CHECK(d.image->width == 100);
    CHECK(d.image->height == 200);
    CHECK(d.info.orientation == 6);
    CHECK(reddish(d.image->px(50, 20)));
    CHECK(bluish(d.image->px(50, 180)));
}

TEST_CASE("encodeThumbnail / decodeThumbnail：不透明的存 JPEG、有透明的存 PNG，解回來大小一樣、顏色接近") {
    const Image opaque = halves(64, 32);
    std::vector<uint8_t> jpg = encodeThumbnail(opaque);
    REQUIRE(jpg.size() > 2);
    CHECK((jpg[0] == 0xFF && jpg[1] == 0xD8));
    ImagePtr back = decodeThumbnail(jpg);
    REQUIRE(back);
    CHECK(back->width == 64);
    CHECK(back->height == 32);
    CHECK(reddish(back->px(5, 16)));
    CHECK(bluish(back->px(60, 16)));

    Image alpha(8, 8);
    for (size_t i = 0; i < alpha.pixels.size(); i += 4) { alpha.pixels[i] = 0; alpha.pixels[i + 1] = 64; alpha.pixels[i + 2] = 0; alpha.pixels[i + 3] = 128; }
    std::vector<uint8_t> png = encodeThumbnail(alpha);
    REQUIRE(png.size() > 4);
    CHECK((png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G'));
    back = decodeThumbnail(png);
    REQUIRE(back);
    CHECK(back->px(3, 3)[3] == 128);
    CHECK(std::abs(back->px(3, 3)[1] - 64) <= 2); // premultiplied 照樣回來

    CHECK(decodeThumbnail({}) == nullptr);
    CHECK(decodeThumbnail(bytesOf(10, 1)) == nullptr);
    CHECK(encodeThumbnail(Image()).empty());
}

TEST_CASE("imageFromHBitmap：alpha 全 0 當成不透明；顏色大於 alpha 的夾到 alpha；不是 32 位元回 nullptr") {
    auto make = [](const uint8_t (&px)[4][4]) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = 2;
        bi.bmiHeader.biHeight = -2;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HBITMAP h = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        std::memcpy(bits, px, 16);
        return h;
    };
    const uint8_t noAlpha[4][4] = {{10, 20, 30, 0}, {40, 50, 60, 0}, {70, 80, 90, 0}, {1, 2, 3, 0}};
    HBITMAP h = make(noAlpha);
    ImagePtr img = imageFromHBitmap(h);
    DeleteObject(h);
    REQUIRE(img);
    CHECK(img->width == 2);
    CHECK(img->px(0, 0)[3] == 255);
    CHECK(img->px(0, 0)[2] == 30);
    CHECK(img->px(1, 0)[0] == 40); // 由上往下：第一列第二個

    const uint8_t straight[4][4] = {{200, 100, 50, 100}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 255}};
    h = make(straight);
    img = imageFromHBitmap(h);
    DeleteObject(h);
    REQUIRE(img);
    CHECK(img->px(0, 0)[0] == 100); // 200 夾到 alpha 100
    CHECK(img->px(0, 0)[2] == 50);

    HBITMAP mono = CreateBitmap(2, 2, 1, 1, nullptr);
    CHECK(imageFromHBitmap(mono) == nullptr);
    DeleteObject(mono);
    CHECK(imageFromHBitmap(nullptr) == nullptr);
}

TEST_CASE("windowsCachedThumbnail：只查快取，有就不超過要的大小；沒有回 nullptr；不存在的檔不出事") {
    const std::wstring dir = freshDir(L"wincache");
    const std::wstring png = joinPath(dir, L"a.png");
    writePng(png, 300, 200);
    ImagePtr img = windowsCachedThumbnail(png, 96); // 剛建的檔通常不在快取裡；在的話大小要對
    if (img) {
        CHECK(img->width <= 96);
        CHECK(img->height <= 96);
    }
    CHECK(windowsCachedThumbnail(joinPath(dir, L"none.png"), 96) == nullptr);
    CHECK(windowsCachedThumbnail(png, 0) == nullptr);
}

TEST_CASE("windowsCachedThumbnail：Windows 自己算過一次之後，只查快取也拿得到（關掉縮圖快取的機器拿不到，只印 WARN）") {
    const std::wstring dir = freshDir(L"wincache_filled");
    const std::wstring png = joinPath(dir, L"filled.png");
    writePng(png, 320, 240);
    ensureThreadCom();
    ComPtr<IShellItemImageFactory> f;
    REQUIRE(SUCCEEDED(SHCreateItemFromParsingName(png.c_str(), nullptr, IID_PPV_ARGS(&f))));
    HBITMAP made = nullptr;
    const HRESULT hr = f->GetImage(SIZE{256, 256}, SIIGBF_THUMBNAILONLY, &made); // 叫 Windows 算（會放進它的快取）
    if (made) DeleteObject(made);
    ImagePtr img = windowsCachedThumbnail(png, 256);
    if (!img) {
        WARN("Windows thumbnail cache did not keep it (GetImage hr " << std::hex << hr << ")");
        return;
    }
    CHECK(img->width <= 256);
    CHECK(img->height <= 256);
    CHECK(img->width >= 200); // 320×240 縮進 256：256×192
    ThumbResult r = loadThumbnail(png, 256, nullptr, true);
    CHECK(r.source == ThumbSource::Windows);
}

TEST_CASE("loadThumbnail：第一次自己算並存進快取，第二次從快取拿；原檔改了重算；不是圖回 Failed") {
    const std::wstring dir = freshDir(L"load_thumb");
    ThumbCache cache;
    REQUIRE(cache.open(joinPath(dir, kThumbCacheFileName), 1024 * 1024));
    const std::wstring png = joinPath(dir, L"a.png");
    writePng(png, 400, 200);
    ThumbResult r = loadThumbnail(png, 96, &cache, false);
    REQUIRE(r.image);
    CHECK(r.source == ThumbSource::Decoded);
    CHECK(r.image->width == 96);
    CHECK(r.image->height == 48);
    CHECK(cache.count() == 1);
    r = loadThumbnail(png, 96, &cache, false);
    REQUIRE(r.image);
    CHECK(r.source == ThumbSource::Cache);
    CHECK(r.image->width == 96);
    // 換一張大小不同的圖（檔案大小變了）
    writePng(png, 100, 200);
    r = loadThumbnail(png, 96, &cache, false);
    REQUIRE(r.image);
    CHECK(r.source == ThumbSource::Decoded);
    CHECK(r.image->width == 48);
    // 沒有快取也行
    r = loadThumbnail(png, 96, nullptr, false);
    CHECK(r.source == ThumbSource::Decoded);
    // 不是圖
    const std::wstring bad = joinPath(dir, L"bad.png");
    writeBytes(bad, "nope");
    r = loadThumbnail(bad, 96, &cache, false);
    CHECK(r.source == ThumbSource::Failed);
    CHECK_FALSE(r.image);
}

TEST_CASE("ThumbQueue：新的要求排最前面；reset 換一批，舊的不收也不做；shutdown 等執行緒結束") {
    std::mutex mu;
    std::condition_variable cv;
    bool release = false;
    std::vector<std::wstring> order;
    std::atomic<int> done{0};
    ThumbQueue q;
    q.start(
        1,
        [&](const std::wstring& path, int) {
            if (path == L"block") {
                std::unique_lock<std::mutex> lock(mu);
                cv.wait(lock, [&] { return release; });
            }
            ThumbResult r;
            r.source = ThumbSource::Decoded;
            return r;
        },
        [&](const ThumbJob& job, ThumbResult&&) {
            {
                std::lock_guard<std::mutex> lock(mu);
                order.push_back(job.path);
            }
            ++done;
        });
    q.reset(1);
    q.request({1, 0, L"block", 96});
    // 等它拿走 block 開始卡住
    for (int i = 0; i < 200 && q.pending() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    q.request({1, 1, L"b", 96});
    q.request({1, 2, L"c", 96});
    q.request({1, 1, L"b", 96}); // 同一格再要一次：移到最前面
    q.request({0, 3, L"old", 96}); // 不是這一批：不收
    CHECK(q.pending() == 2);
    {
        std::lock_guard<std::mutex> lock(mu);
        release = true;
    }
    cv.notify_all();
    for (int i = 0; i < 400 && done < 3; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    {
        std::lock_guard<std::mutex> lock(mu);
        CHECK(order == std::vector<std::wstring>{L"block", L"b", L"c"});
    }
    q.reset(2);
    q.request({1, 9, L"stale", 96});
    CHECK(q.pending() == 0);
    q.shutdown();
    q.request({2, 0, L"after", 96}); // 停了就不收
    CHECK(q.pending() == 0);
}

TEST_CASE("PreviewWorker：只做最新的要求，中間被取代的不做") {
    std::mutex mu;
    std::condition_variable cv;
    bool release = false;
    std::vector<uint64_t> tickets;
    std::atomic<int> started{0};
    PreviewWorker w;
    w.start(
        [&](const std::wstring& path, int, int) {
            ++started;
            if (path == L"first") {
                std::unique_lock<std::mutex> lock(mu);
                cv.wait(lock, [&] { return release; });
            }
            return ScaledDecode{};
        },
        [&](uint64_t ticket, const std::wstring&, ScaledDecode&&) {
            std::lock_guard<std::mutex> lock(mu);
            tickets.push_back(ticket);
        });
    w.request(1, L"first", 10, 10);
    for (int i = 0; i < 200 && started < 1; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    w.request(2, L"second", 10, 10);
    w.request(3, L"third", 10, 10); // 取代 2
    {
        std::lock_guard<std::mutex> lock(mu);
        release = true;
    }
    cv.notify_all();
    for (int i = 0; i < 400; ++i) {
        {
            std::lock_guard<std::mutex> lock(mu);
            if (tickets.size() >= 2) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    {
        std::lock_guard<std::mutex> lock(mu);
        CHECK(tickets == std::vector<uint64_t>{1, 3});
    }
    w.request(4, L"x", 1, 1);
    w.cancel(); // 還沒開始的丟掉（可能已經開始了：只檢查不會卡住）
    w.shutdown();
}
