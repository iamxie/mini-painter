#include "core/BookPrep.h"

#include <windows.h>

#include <chrono>

#include "core/FolderNav.h"
#include "core/ImageIO.h"
#include "core/PathUtil.h"
#include "core/Shelf.h"
#include "core/Spreads.h"
#include "core/TempDir.h"

namespace mp {

namespace {
bool samePath(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
// take 等背景的時候多久醒來一次回報進度、看要不要取消。
constexpr auto kTakePoll = std::chrono::milliseconds(50);
} // namespace

bool BookRequest::operator==(const BookRequest& o) const {
    return samePath(fromBook, o.fromBook) && dir == o.dir && twoPage == o.twoPage && firstAlone == o.firstAlone &&
           shiftOne == o.shiftOne && pinned == o.pinned && passwords == o.passwords && archiveRoot == o.archiveRoot;
}

bool BookPlan::usable() const { return !book.empty() && !files.empty() && (!archive || extracted.usable()); }

bool isWidePage(const std::wstring& path) {
    int w = 0, h = 0;
    return readImageSize(path, w, h) && w > h;
}

BookPlan planBook(const std::wstring& book, const BookRequest& req, const std::wstring& dest,
                  const std::function<bool(uint64_t, uint64_t)>& progress) {
    BookPlan p;
    p.book = book;
    auto cancelled = [&] { return progress && !progress(0, 0); };
    if (isDirectoryPath(book)) {
        p.files = listImageFiles(book);
        movePinnedFirst(p.files, req.pinned);
    } else {
        p.archive = true;
        ExtractOptions o;
        o.progress = progress;
        PasswordHooks hooks;
        hooks.cancelled = cancelled;
        PasswordExtract x = extractWithPasswords(book, dest, o, req.passwords, hooks);
        p.password = x.password;
        p.passwordsTried = x.listTried && x.result.status == ArchiveStatus::NeedPassword;
        if (!x.result.usable()) {
            removeDirectoryRecursive(dest);
            p.extracted = std::move(x.result);
            return p;
        }
        movePinnedFirst(x.result.files, x.result.names, req.pinned);
        p.dest = dest;
        p.files = x.result.files;
        p.extracted = std::move(x.result);
    }
    if (p.files.empty()) return p;
    if (req.twoPage) {
        std::vector<bool> wide;
        wide.reserve(p.files.size());
        for (const auto& f : p.files) {
            if (cancelled()) { // 一本幾百頁時逐頁讀檔頭要一點時間：取消了就不讀完
                if (!p.dest.empty()) removeDirectoryRecursive(p.dest);
                p.dest.clear();
                p.files.clear();
                p.wide.clear();
                p.extracted.status = ArchiveStatus::Cancelled;
                return p;
            }
            const bool isWide = isWidePage(f);
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

BookPreparer::BookPreparer(std::function<void()> onReady) : onReady_(std::move(onReady)) {
    worker_ = std::thread(&BookPreparer::run, this);
}

BookPreparer::~BookPreparer() { shutdown(); }

void BookPreparer::orphanLocked(const std::wstring& dir) {
    if (!dir.empty()) orphanDirs_.push_back(dir);
}

void BookPreparer::request(std::optional<BookRequest> req) {
    {
        std::lock_guard<std::mutex> lk(m_);
        const bool same = req_.has_value() == req.has_value() && (!req || *req_ == *req);
        if (same) return;
        // 做到一半的作廢（背景做完發現 generation 對不上，自己把資料夾排進刪除）；做好沒人接手的現在就排進去。
        if (state_ == State::Running) cancel_ = true;
        if (state_ == State::Ready) orphanLocked(plan_.dest);
        ++generation_;
        plan_ = BookPlan{};
        neighbor_ = Neighbor::Unknown;
        neighborPath_.clear();
        req_ = std::move(req);
        state_ = req_ ? State::Queued : State::Idle;
    }
    work_.notify_all();
    done_.notify_all();
}

void BookPreparer::drop() {
    request(std::nullopt);
    std::unique_lock<std::mutex> lk(m_);
    done_.wait(lk, [&] { return stop_ || (!workerBusy_ && orphanDirs_.empty()); });
}

std::optional<BookPlan> BookPreparer::take(const std::wstring& book, const std::function<bool(uint64_t, uint64_t)>& progress) {
    std::unique_lock<std::mutex> lk(m_);
    for (;;) {
        if (stop_ || state_ == State::Idle || state_ == State::Taken) return std::nullopt;
        if (state_ == State::Ready) {
            if (!samePath(plan_.book, book)) return std::nullopt;
            state_ = State::Taken;
            return plan_;
        }
        // Queued / Running：已經知道鄰居不是這一本（或根本沒有鄰居）就不用等。
        if (neighbor_ == Neighbor::None || (neighbor_ == Neighbor::Found && !samePath(neighborPath_, book))) return std::nullopt;
        done_.wait_for(lk, kTakePoll);
        if (!progress || state_ == State::Ready) continue;
        const uint64_t d = progressDone_, t = progressTotal_;
        const uint64_t generation = generation_;
        lk.unlock();
        const bool go = progress(d, t);
        lk.lock();
        if (go || generation != generation_) continue;
        // 使用者取消：停掉正在做的，結果不要了；同一個要求之後不再重做。
        cancel_ = true;
        if (state_ == State::Ready) orphanLocked(plan_.dest);
        plan_ = BookPlan{};
        state_ = State::Taken;
        work_.notify_all();
        done_.wait(lk, [&] { return stop_ || !workerBusy_; });
        BookPlan c;
        c.book = book;
        c.archive = true;
        c.extracted.status = ArchiveStatus::Cancelled;
        return c;
    }
}

std::vector<std::wstring> BookPreparer::edge() const {
    std::lock_guard<std::mutex> lk(m_);
    if ((state_ == State::Ready || state_ == State::Taken) && plan_.usable()) return plan_.edge;
    return {};
}

void BookPreparer::shutdown() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
        cancel_ = true;
    }
    work_.notify_all();
    done_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::vector<std::wstring> dirs;
    {
        std::lock_guard<std::mutex> lk(m_);
        dirs.swap(orphanDirs_);
        if (state_ == State::Ready && !plan_.dest.empty()) dirs.push_back(plan_.dest);
        plan_ = BookPlan{};
        state_ = State::Idle;
    }
    for (const auto& d : dirs) removeDirectoryRecursive(d);
}

void BookPreparer::waitIdle() {
    std::unique_lock<std::mutex> lk(m_);
    done_.wait(lk, [&] { return stop_ || (!workerBusy_ && state_ != State::Queued && orphanDirs_.empty()); });
}

void BookPreparer::run() {
    // 背景工作讓給使用者正在做的事。
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::unique_lock<std::mutex> lk(m_);
    for (;;) {
        work_.wait(lk, [&] { return stop_ || !orphanDirs_.empty() || state_ == State::Queued; });
        if (stop_) return;
        workerBusy_ = true;
        if (!orphanDirs_.empty()) {
            std::vector<std::wstring> dirs;
            dirs.swap(orphanDirs_);
            lk.unlock();
            for (const auto& d : dirs) removeDirectoryRecursive(d);
            lk.lock();
        } else {
            state_ = State::Running;
            cancel_ = false;
            progressDone_ = 0;
            progressTotal_ = 0;
            const BookRequest req = *req_;
            const uint64_t generation = generation_;
            lk.unlock();
            prepare(req, generation);
            lk.lock();
        }
        workerBusy_ = false;
        done_.notify_all();
    }
}

void BookPreparer::prepare(const BookRequest& req, uint64_t generation) {
    const std::wstring target = neighborBook(req.fromBook, req.dir);
    std::wstring dest;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (generation == generation_) {
            neighbor_ = target.empty() ? Neighbor::None : Neighbor::Found;
            neighborPath_ = target;
        }
        if (!target.empty() && !isDirectoryPath(target) && !req.archiveRoot.empty() && archiveLibraryLoaded())
            dest = req.archiveRoot + L"\\pre" + std::to_wstring(++destSeq_);
    }
    done_.notify_all(); // 等著接手的人：鄰居是哪一本知道了

    BookPlan plan;
    plan.book = target;
    if (!target.empty() && !cancel_) {
        const bool isArchive = !isDirectoryPath(target);
        if (isArchive && dest.empty()) {
            plan.archive = true;
            plan.notPrepared = true;
        } else {
            plan = planBook(target, req, dest, [&](uint64_t done, uint64_t total) {
                if (total) {
                    progressDone_ = done;
                    progressTotal_ = total;
                }
                return !cancel_.load();
            });
        }
    }
    bool ready = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!stop_ && generation == generation_ && state_ == State::Running && !cancel_) {
            plan_ = std::move(plan);
            state_ = State::Ready;
            ready = true;
        } else {
            // 要求換了、被取消、或要關了：這一份不要了。
            orphanLocked(plan.dest);
            if (generation == generation_ && state_ == State::Running) state_ = State::Taken;
        }
    }
    done_.notify_all();
    if (ready && onReady_) onReady_();
}

} // namespace mp
