#include "core/Prefetch.h"

#include <windows.h>

#include <algorithm>
#include <new>

#include "core/ImageOps.h"

namespace mp {

namespace {

bool samePath(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

bool contains(const std::vector<std::wstring>& v, const std::wstring& p) {
    for (const auto& x : v)
        if (samePath(x, p)) return true;
    return false;
}

size_t pixelBytes(int w, int h) { return w > 0 && h > 0 ? static_cast<size_t>(w) * static_cast<size_t>(h) * 4 : 0; }

} // namespace

Prefetcher::Prefetcher(Decoder decode)
    : decode_(decode ? std::move(decode) : Decoder(loadImage)), books_([this] {
          // 下一本準備好了（背景執行緒）：它的第一屏現在也是要解的頁。
          {
              std::lock_guard<std::mutex> lk(m_);
              spaceChangedLocked();
              idle_ = false;
          }
          work_.notify_all();
      }) {
    worker_ = std::thread(&Prefetcher::run, this);
}

Prefetcher::~Prefetcher() { shutdown(); }

void Prefetcher::shutdown() {
    books_.shutdown();
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
    }
    work_.notify_all();
    done_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lk(m_);
    entries_.clear();
}

bool Prefetcher::PathLess::operator()(const std::wstring& a, const std::wstring& b) const {
    return _wcsicmp(a.c_str(), b.c_str()) < 0;
}

Prefetcher::Stamp Prefetcher::stampOf(const std::wstring& path) {
    Stamp s;
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return s;
    s.ok = true;
    s.size = (static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    s.writeTime = (static_cast<uint64_t>(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    return s;
}

size_t Prefetcher::bytesOf(const Entry& e) {
    return (e.result.image ? e.result.image->pixels.size() : 0) + (e.scaled ? e.scaled->pixels.size() : 0);
}

std::vector<Prefetcher::Screen> Prefetcher::screensLocked() const {
    std::vector<Screen> s;
    Screen edge = books_.edge();
    if (!edge.empty()) s.push_back(std::move(edge));
    for (const auto& o : others_) s.push_back(o);
    return s;
}

bool Prefetcher::isCurrentLocked(const std::wstring& path) const { return contains(current_, path); }

bool Prefetcher::isWantedLocked(const std::wstring& path) const {
    if (isCurrentLocked(path)) return true;
    for (const auto& s : screensLocked())
        if (contains(s, path)) return true;
    return false;
}

size_t Prefetcher::othersBytesLocked() const {
    size_t sum = 0;
    for (const auto& [path, e] : entries_)
        if (!isCurrentLocked(path)) sum += bytesOf(e);
    return sum;
}

void Prefetcher::dropScreenLocked(const Screen& screen) {
    for (const auto& p : screen)
        if (!isCurrentLocked(p)) entries_.erase(p);
}

void Prefetcher::pruneLocked() {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (!isWantedLocked(it->first)) it = entries_.erase(it);
        else {
            if (!target_) it->second.scaled.reset();
            ++it;
        }
    }
    // 上限：依優先順序一屏一屏累加，放不下的那一屏起全部丟掉。
    size_t sum = 0;
    bool over = false;
    for (const auto& screen : screensLocked()) {
        size_t bytes = 0;
        for (const auto& p : screen) {
            if (isCurrentLocked(p)) continue;
            auto it = entries_.find(p);
            if (it != entries_.end()) bytes += bytesOf(it->second);
        }
        if (!over && sum + bytes <= limit_) {
            sum += bytes;
            continue;
        }
        over = true;
        dropScreenLocked(screen);
    }
}

bool Prefetcher::pickLocked(Task& t) {
    if (full_) return false;
    for (const auto& screen : screensLocked()) {
        // 先把這一屏的每一頁解完，才知道兩頁並排後的倍率。
        for (const auto& p : screen) {
            if (isCurrentLocked(p) || entries_.count(p)) continue;
            t = Task{};
            t.kind = Kind::Decode;
            t.path = p;
            t.screen = screen;
            return true;
        }
        if (!target_) continue;
        // 這一屏解好的頁：縮圖尺寸還沒算過（setWanted 之後、或剛解完）就先算，算過了就看哪一張還沒縮。
        Task targets;
        targets.kind = Kind::Targets;
        targets.target = target_;
        targets.targetGen = targetGen_;
        bool stale = false;
        for (const auto& p : screen) {
            auto it = entries_.find(p);
            if (it == entries_.end() || !it->second.result.image) continue;
            targets.paths.push_back(p);
            targets.images.push_back(it->second.result.image);
            if (it->second.wantGen != targetGen_) stale = true;
        }
        if (stale) {
            t = std::move(targets);
            return true;
        }
        for (const auto& p : targets.paths) {
            const Entry& e = entries_.at(p);
            auto [tw, th] = e.want;
            if (tw <= 0 || th <= 0) continue;
            if (e.scaled && e.scaled->width == tw && e.scaled->height == th) continue;
            t = Task{};
            t.kind = Kind::Scale;
            t.path = p;
            t.image = e.result.image;
            t.tw = tw;
            t.th = th;
            return true;
        }
    }
    return false;
}

void Prefetcher::run() {
    // 背景工作讓給使用者正在做的事。
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        Task t;
        {
            std::unique_lock<std::mutex> lk(m_);
            for (;;) {
                if (stop_) return;
                if (pickLocked(t)) break;
                idle_ = true;
                done_.notify_all();
                work_.wait(lk);
            }
            idle_ = false;
            busy_ = t;
        }
        // 例外逃出這條執行緒會讓整個程式結束：配置失敗就放掉這一件（解碼與縮圖各自已經處理）。
        try {
            switch (t.kind) {
            case Kind::Decode: runDecode(t); break;
            case Kind::Targets: runTargets(t); break;
            case Kind::Scale: runScale(t); break;
            case Kind::None: break;
            }
        } catch (const std::bad_alloc&) {
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            busy_ = Task{};
        }
        done_.notify_all();
    }
}

void Prefetcher::runDecode(const Task& t) {
    // 先讀檔頭估這一屏還沒解的頁共多大：整屏放不下就不解，不白做。讀不到檔頭的當 0，解完再看。
    std::vector<std::wstring> pending;
    {
        std::lock_guard<std::mutex> lk(m_);
        for (const auto& p : t.screen)
            if (!isCurrentLocked(p) && !entries_.count(p)) pending.push_back(p);
    }
    size_t estimate = 0;
    for (const auto& p : pending) {
        int w = 0, h = 0;
        if (readImageSize(p, w, h)) estimate += pixelBytes(w, h);
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!fitsLocked(estimate)) {
            dropScreenLocked(t.screen); // 這一屏已經解好的頁也不留：留半屏沒有用，還會讓後面的屏更放不下
            full_ = true;
            return;
        }
    }
    const Stamp st = stampOf(t.path);
    LoadResult r = decode_(t.path);
    std::lock_guard<std::mutex> lk(m_);
    if (!isWantedLocked(t.path) || entries_.count(t.path)) return;
    Entry e;
    e.result = std::move(r);
    e.stamp = st;
    entries_[t.path] = std::move(e);
    if (!isCurrentLocked(t.path) && !fitsLocked(0)) { // 估不準（檔頭讀不到）才會走到這裡
        dropScreenLocked(t.screen);
        full_ = true;
    }
}

void Prefetcher::runTargets(const Task& t) {
    std::vector<Size2> sizes;
    sizes.reserve(t.images.size());
    for (const auto& im : t.images) sizes.push_back({im->width, im->height});
    const std::vector<Size2> wants = t.target(sizes); // 不持鎖：這是呼叫端給的函式
    std::lock_guard<std::mutex> lk(m_);
    if (t.targetGen != targetGen_) return; // 尺寸的算法換了，這一份不要
    for (size_t i = 0; i < t.paths.size(); ++i) {
        auto it = entries_.find(t.paths[i]);
        if (it == entries_.end() || it->second.result.image != t.images[i]) continue;
        it->second.want = i < wants.size() ? wants[i] : Size2{0, 0};
        it->second.wantGen = t.targetGen;
    }
}

void Prefetcher::runScale(const Task& t) {
    const size_t bytes = pixelBytes(t.tw, t.th);
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = entries_.find(t.path);
        if (it == entries_.end() || it->second.result.image != t.image) return;
        // 縮圖多大事先就知道：放不下就不縮（換掉舊尺寸的那一張，所以先扣掉它）。
        const size_t old = it->second.scaled ? it->second.scaled->pixels.size() : 0;
        if (!isCurrentLocked(t.path) && othersBytesLocked() - old + bytes > limit_) {
            it->second.scaled.reset();
            full_ = true;
            return;
        }
    }
    ImagePtr out;
    try {
        out = std::make_shared<const Image>(resample(*t.image, t.tw, t.th));
    } catch (const std::bad_alloc&) {
        // 記憶體不夠縮：這一頁不縮（畫面顯示時會自己縮，再不行就用快速縮放）。想要的尺寸沒變就不再試。
        std::lock_guard<std::mutex> lk(m_);
        auto it = entries_.find(t.path);
        if (it != entries_.end() && it->second.result.image == t.image) {
            it->second.scaled.reset();
            it->second.want = {0, 0};
        }
        return;
    }
    std::lock_guard<std::mutex> lk(m_);
    auto it = entries_.find(t.path);
    if (it == entries_.end() || it->second.result.image != t.image || !isWantedLocked(t.path)) return;
    it->second.scaled = std::move(out);
    if (!isCurrentLocked(t.path) && !fitsLocked(0)) { // 縮的期間別的頁進來了
        it->second.scaled.reset();
        full_ = true;
    }
}

void Prefetcher::setWanted(std::vector<std::wstring> current, std::vector<std::vector<std::wstring>> others,
                           std::optional<BookRequest> book, TargetSizes target, size_t limitBytes) {
    books_.request(std::move(book));
    {
        std::lock_guard<std::mutex> lk(m_);
        current_ = std::move(current);
        others_ = std::move(others);
        target_ = std::move(target);
        ++targetGen_;
        limit_ = limitBytes;
        spaceChangedLocked();
        pruneLocked();
        idle_ = false;
    }
    work_.notify_all();
}

void Prefetcher::dropBook() {
    books_.drop();
    {
        std::lock_guard<std::mutex> lk(m_);
        pruneLocked(); // 下一本的那一屏不要了
        spaceChangedLocked();
        idle_ = false;
    }
    work_.notify_all();
}

std::optional<LoadResult> Prefetcher::get(const std::wstring& path) {
    std::unique_lock<std::mutex> lk(m_);
    for (;;) {
        auto it = entries_.find(path);
        if (it != entries_.end()) {
            // 當時解失敗的不給：失敗可能只是一時的（檔案被別的程式鎖住），讓呼叫端現在再解一次。
            // 解碼後檔案被改過（例如剛存檔）的也不給。兩種都把那一筆丟掉。
            const bool good = it->second.result.image && stampOf(path) == it->second.stamp;
            if (good) return it->second.result;
            entries_.erase(it);
            spaceChangedLocked();
            return std::nullopt;
        }
        if (busy_.kind == Kind::Decode && samePath(busy_.path, path)) {
            done_.wait(lk);
            continue;
        }
        return std::nullopt;
    }
}

void Prefetcher::put(const std::wstring& path, const LoadResult& r) {
    Stamp st = stampOf(path);
    std::lock_guard<std::mutex> lk(m_);
    auto it = entries_.find(path);
    if (it != entries_.end() && it->second.result.image == r.image) return; // 就是從這裡拿走的那一張，縮圖留著
    Entry e;
    e.result = r;
    e.stamp = st;
    entries_[path] = std::move(e);
}

ImagePtr Prefetcher::scaled(const ImagePtr& image, int tw, int th) {
    if (!image) return nullptr;
    std::unique_lock<std::mutex> lk(m_);
    for (;;) {
        for (const auto& [path, e] : entries_) {
            if (e.result.image != image) continue;
            if (e.scaled && e.scaled->width == tw && e.scaled->height == th) return e.scaled;
            break;
        }
        if (busy_.kind == Kind::Scale && busy_.image == image && busy_.tw == tw && busy_.th == th) {
            done_.wait(lk);
            continue;
        }
        return nullptr;
    }
}

size_t Prefetcher::heldBytes() const {
    std::lock_guard<std::mutex> lk(m_);
    return othersBytesLocked();
}

void Prefetcher::waitIdle() {
    // 換本預備做完會讓這邊多一屏要解，所以先等它，再等這邊。
    books_.waitIdle();
    std::unique_lock<std::mutex> lk(m_);
    done_.wait(lk, [&] { return stop_ || (idle_ && busy_.kind == Kind::None); });
}

} // namespace mp
