#include "core/Prefetch.h"

#include <windows.h>

#include <algorithm>
#include <chrono>

#include "core/FolderNav.h"
#include "core/ImageOps.h"
#include "core/Shelf.h"
#include "core/Spreads.h"
#include "core/TempDir.h"

namespace mp {

namespace {

const wchar_t kNoBook[] = L"\x01"; // bookTarget_：查過了，沒有鄰居

bool samePath(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

bool contains(const std::vector<std::wstring>& v, const std::wstring& p) {
    for (const auto& x : v)
        if (samePath(x, p)) return true;
    return false;
}

} // namespace

bool BookRequest::operator==(const BookRequest& o) const {
    return samePath(fromBook, o.fromBook) && dir == o.dir && twoPage == o.twoPage && firstAlone == o.firstAlone &&
           shiftOne == o.shiftOne && pinned == o.pinned && passwords == o.passwords && archiveRoot == o.archiveRoot;
}

bool BookPlan::usable() const {
    return !book.empty() && !files.empty() &&
           (!archive || extracted.status == ArchiveStatus::Ok || extracted.status == ArchiveStatus::Incomplete);
}

BookPlan planBook(const std::wstring& book, const BookRequest& req, const std::wstring& dest,
                  const std::function<bool(uint64_t, uint64_t)>& progress) {
    BookPlan p;
    p.book = book;
    if (isDirectoryPath(book)) {
        p.files = listImageFiles(book);
        movePinnedFirst(p.files, req.pinned);
    } else {
        // 和 MainWindow::openArchive 同一套：先不帶密碼解，要密碼就試密碼列表；都不對就不解（跳過去時才問使用者）。
        p.archive = true;
        ExtractOptions o;
        o.progress = progress;
        ArchiveResult r = extractImages(book, dest, o);
        if (r.status == ArchiveStatus::NeedPassword) {
            removeDirectoryRecursive(dest);
            for (const auto& pw : req.passwords) {
                if (progress && !progress(0, 0)) { r.status = ArchiveStatus::Cancelled; break; }
                if (testPassword(book, pw) != ArchiveStatus::Ok) continue;
                o.password = pw;
                p.password = pw;
                r = extractImages(book, dest, o);
                break;
            }
        }
        if (r.status != ArchiveStatus::Ok && r.status != ArchiveStatus::Incomplete) {
            removeDirectoryRecursive(dest);
            p.extracted = std::move(r);
            return p;
        }
        movePinnedFirst(r.files, r.names, req.pinned);
        p.dest = dest;
        p.files = r.files;
        p.extracted = std::move(r);
    }
    if (p.files.empty()) return p;
    if (req.twoPage) {
        // 和 MainWindow::updateSpreads 同一套配對。
        std::vector<bool> wide;
        wide.reserve(p.files.size());
        for (const auto& f : p.files) {
            int w = 0, h = 0;
            bool isWide = readImageSize(f, w, h) && w > h;
            p.wide.emplace(f, isWide);
            wide.push_back(isWide);
        }
        auto spreads = buildSpreads(wide, req.firstAlone, req.shiftOne);
        const Spread& s = req.dir > 0 ? spreads.front() : spreads.back();
        p.edge.push_back(p.files[static_cast<size_t>(s.first)]);
        if (s.second >= 0) p.edge.push_back(p.files[static_cast<size_t>(s.second)]);
    } else {
        p.edge.push_back(req.dir > 0 ? p.files.front() : p.files.back());
    }
    return p;
}

// ---------------------------------------------------------------------------

Prefetcher::Prefetcher(Decoder decode) : decode_(decode ? std::move(decode) : Decoder(loadImage)) {
    worker_ = std::thread(&Prefetcher::run, this);
}

Prefetcher::~Prefetcher() { shutdown(); }

void Prefetcher::shutdown() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
        bookCancel_ = true;
    }
    work_.notify_all();
    done_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::vector<std::wstring> dirs;
    {
        std::lock_guard<std::mutex> lk(m_);
        entries_.clear();
        dirs.swap(orphanDirs_);
        if (bookPlan_ && !bookTaken_ && !bookPlan_->dest.empty()) dirs.push_back(bookPlan_->dest);
        bookPlan_.reset();
    }
    for (const auto& d : dirs) removeDirectoryRecursive(d);
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

std::vector<std::vector<std::wstring>> Prefetcher::screensLocked() const {
    std::vector<std::vector<std::wstring>> s;
    if (bookPlan_ && bookPlan_->usable() && !bookPlan_->edge.empty()) s.push_back(bookPlan_->edge);
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

void Prefetcher::pruneLocked() {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (!isWantedLocked(it->first)) it = entries_.erase(it);
        else {
            if (!target_) it->second.scaled.reset();
            ++it;
        }
    }
    // 上限：依優先順序累加，放不下的那一屏起全部丟掉。
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
        for (const auto& p : screen)
            if (!isCurrentLocked(p)) entries_.erase(p);
    }
}

bool Prefetcher::pickLocked(Task& t) {
    if (bookReq_ && !bookPlan_ && !bookRunning_ && !bookTaken_) {
        t = Task{};
        t.kind = Kind::Book;
        t.book = *bookReq_;
        return true;
    }
    if (full_) return false;
    for (const auto& screen : screensLocked()) {
        // 先把這一屏的每一頁解完，才知道兩頁並排後的倍率。
        for (const auto& p : screen) {
            if (isCurrentLocked(p) || entries_.count(p)) continue;
            t = Task{};
            t.kind = Kind::Decode;
            t.path = p;
            return true;
        }
        if (!target_) continue;
        std::vector<Size2> sizes;
        std::vector<std::wstring> paths;
        for (const auto& p : screen) {
            auto it = entries_.find(p);
            if (it == entries_.end() || !it->second.result.image) continue;
            sizes.push_back({it->second.result.image->width, it->second.result.image->height});
            paths.push_back(p);
        }
        if (sizes.empty()) continue;
        auto targets = target_(sizes);
        for (size_t i = 0; i < paths.size() && i < targets.size(); ++i) {
            auto [tw, th] = targets[i];
            if (tw <= 0 || th <= 0) continue;
            const Entry& e = entries_.at(paths[i]);
            if (e.scaled && e.scaled->width == tw && e.scaled->height == th) continue;
            t = Task{};
            t.kind = Kind::Scale;
            t.path = paths[i];
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
        std::vector<std::wstring> orphans;
        {
            std::unique_lock<std::mutex> lk(m_);
            for (;;) {
                if (stop_) return;
                if (!orphanDirs_.empty()) {
                    orphans.swap(orphanDirs_);
                    break;
                }
                if (pickLocked(t)) break;
                idle_ = true;
                done_.notify_all();
                work_.wait(lk);
            }
            idle_ = false;
            busy_ = t;
            if (t.kind == Kind::Book) {
                bookRunning_ = true;
                bookCancel_ = false;
                bookDone_ = 0;
                bookTotal_ = 0;
                bookTarget_.clear();
            }
        }
        for (const auto& d : orphans) removeDirectoryRecursive(d);

        if (t.kind == Kind::Decode) {
            // 先讀檔頭估大小：放不下就這一輪停手，不白解。
            int w = 0, h = 0;
            size_t estimate = readImageSize(t.path, w, h) ? static_cast<size_t>(w) * h * 4 : 0;
            bool fits;
            {
                std::lock_guard<std::mutex> lk(m_);
                fits = othersBytesLocked() + estimate <= limit_;
                if (!fits) full_ = true;
            }
            if (fits) {
                Stamp st = stampOf(t.path);
                LoadResult r = decode_(t.path);
                std::lock_guard<std::mutex> lk(m_);
                if (isWantedLocked(t.path) && !entries_.count(t.path)) {
                    entries_[t.path] = Entry{std::move(r), st, nullptr};
                    if (!isCurrentLocked(t.path) && othersBytesLocked() > limit_) {
                        entries_.erase(t.path);
                        full_ = true;
                    }
                }
            }
        } else if (t.kind == Kind::Scale) {
            auto out = std::make_shared<const Image>(resample(*t.image, t.tw, t.th));
            std::lock_guard<std::mutex> lk(m_);
            auto it = entries_.find(t.path);
            if (it != entries_.end() && it->second.result.image == t.image && isWantedLocked(t.path)) {
                it->second.scaled = std::move(out);
                if (!isCurrentLocked(t.path) && othersBytesLocked() > limit_) {
                    it->second.scaled.reset();
                    full_ = true;
                }
            }
        } else if (t.kind == Kind::Book) {
            runBook(t.book);
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            busy_ = Task{};
        }
        done_.notify_all();
    }
}

void Prefetcher::runBook(const BookRequest& req) {
    std::wstring target = neighborBook(req.fromBook, req.dir);
    {
        std::lock_guard<std::mutex> lk(m_);
        if (bookReq_ && *bookReq_ == req) bookTarget_ = target.empty() ? std::wstring(kNoBook) : target;
    }
    done_.notify_all();
    BookPlan plan;
    plan.book = target;
    if (!target.empty()) {
        bool isArchive = !isDirectoryPath(target);
        std::wstring dest;
        if (isArchive && !req.archiveRoot.empty() && archiveLibraryLoaded()) {
            std::lock_guard<std::mutex> lk(m_);
            dest = req.archiveRoot + L"\\pre" + std::to_wstring(++destSeq_);
        }
        if (!isArchive || !dest.empty()) {
            plan = planBook(target, req, dest, [this](uint64_t done, uint64_t total) {
                bookDone_ = done;
                bookTotal_ = total;
                return !bookCancel_.load();
            });
        } else {
            plan.archive = true;
            plan.extracted.status = ArchiveStatus::NoLibrary; // 不能預開：跳過去時照原本的流程
        }
    }
    std::lock_guard<std::mutex> lk(m_);
    bookRunning_ = false;
    if (stop_ || bookCancel_ || !bookReq_ || *bookReq_ != req) {
        if (!plan.dest.empty()) orphanDirs_.push_back(plan.dest);
    } else {
        bookPlan_ = std::move(plan);
    }
}

void Prefetcher::setWanted(std::vector<std::wstring> current, std::vector<std::vector<std::wstring>> others,
                           std::optional<BookRequest> book, TargetSizes target, size_t limitBytes) {
    {
        std::lock_guard<std::mutex> lk(m_);
        current_ = std::move(current);
        others_ = std::move(others);
        target_ = std::move(target);
        limit_ = limitBytes;
        full_ = false;
        bool same = bookReq_.has_value() == book.has_value() && (!book || *bookReq_ == *book);
        if (!same) resetBookLocked(std::move(book));
        pruneLocked();
        idle_ = false;
    }
    work_.notify_all();
}

void Prefetcher::resetBookLocked(std::optional<BookRequest> book) {
    if (bookRunning_) bookCancel_ = true;
    if (bookPlan_ && !bookTaken_ && !bookPlan_->dest.empty()) orphanDirs_.push_back(bookPlan_->dest);
    bookPlan_.reset();
    bookTaken_ = false;
    bookTarget_.clear();
    bookReq_ = std::move(book);
}

void Prefetcher::dropBook() {
    std::unique_lock<std::mutex> lk(m_);
    resetBookLocked(std::nullopt);
    pruneLocked(); // 下一本的那一屏不要了
    work_.notify_all();
    done_.wait(lk, [&] { return !bookRunning_; });
}

std::optional<LoadResult> Prefetcher::take(const std::wstring& path) {
    std::unique_lock<std::mutex> lk(m_);
    for (;;) {
        auto it = entries_.find(path);
        if (it != entries_.end()) {
            if (stampOf(path) == it->second.stamp) return it->second.result;
            entries_.erase(it); // 解碼後檔案被改過（例如剛存檔）
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
    entries_[path] = Entry{r, st, nullptr};
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

std::optional<BookPlan> Prefetcher::takeBook(const std::wstring& book,
                                             const std::function<bool(uint64_t, uint64_t)>& progress) {
    std::unique_lock<std::mutex> lk(m_);
    for (;;) {
        if (!bookReq_ || bookTaken_ || stop_) return std::nullopt;
        if (bookPlan_) {
            if (!samePath(bookPlan_->book, book)) return std::nullopt;
            bookTaken_ = true;
            return *bookPlan_;
        }
        if (!bookTarget_.empty() && !samePath(bookTarget_, book)) return std::nullopt; // 含「沒有鄰居」
        done_.wait_for(lk, std::chrono::milliseconds(50));
        if (!progress || bookPlan_) continue;
        uint64_t d = bookDone_, t = bookTotal_;
        lk.unlock();
        bool go = progress(d, t);
        lk.lock();
        if (go) continue;
        // 使用者取消：停掉解壓，丟掉結果。
        bookCancel_ = true;
        bookTaken_ = true;
        work_.notify_all();
        done_.wait(lk, [&] { return !bookRunning_; });
        if (bookPlan_) {
            if (!bookPlan_->dest.empty()) orphanDirs_.push_back(bookPlan_->dest);
            bookPlan_.reset();
            work_.notify_all();
        }
        BookPlan c;
        c.book = book;
        c.archive = true;
        c.extracted.status = ArchiveStatus::Cancelled;
        return c;
    }
}

size_t Prefetcher::heldBytes() const {
    std::lock_guard<std::mutex> lk(m_);
    return othersBytesLocked();
}

void Prefetcher::waitIdle() {
    std::unique_lock<std::mutex> lk(m_);
    done_.wait(lk, [&] { return stop_ || (idle_ && busy_.kind == Kind::None && orphanDirs_.empty()); });
}

} // namespace mp
