#include "kds/storage/page_mgr/page_mgr.hpp"

#include <algorithm>
#include <string>

#include "kds/storage/page_header.hpp"

namespace kds::storage {

void Frame::MarkDirty(wal::Lsn record_lsn) noexcept {
    dirty_ = true;
    // recLSN is the *first* record that has to be replayed to rebuild this
    // frame, so only the first mutation since it was last clean sets it -
    // later records move page_lsn forward and must leave the recovery
    // start where it is.
    //
    // Keyed off recLSN being unset rather than off the dirty transition,
    // because a page created through AllocNew() is already dirty before
    // any record describes it (page bookkeeping, not a logged change). Its
    // first logged mutation is the one recovery would start from, and
    // keying off `dirty_` would have left recLSN at 0 - which is not
    // "start of the log", it is "nothing to replay", and min()ing it into
    // a checkpoint's redo start would drag recovery back to the head of
    // the log.
    if (rec_lsn_ == kNoPageLsn) {
        rec_lsn_ = record_lsn;
    }
    // Records arrive in LSN order, so this is a max only to keep a
    // caller replaying an older record from walking page_lsn backwards -
    // redo compares against the *last* record applied (wal.md section 9).
    if (record_lsn > page_lsn_) {
        page_lsn_ = record_lsn;
        SetPageLsn(bytes(), record_lsn);
        // Named debt (PW1c-3): the PL-C stream stamp is not written here
        // because a Frame knows no core. Nothing production-wired calls
        // MarkDirty today; the day the pool joins the logged write path,
        // it must stamp beside the LSN as DevicePageStore::StampPageLsn
        // does, or a pool-written page reads "never stamped" forever.
    }
    // The page-modification line. Reports rec_lsn too, because the pair
    // (page_lsn, rec_lsn) is what a checkpoint and a recovery argument
    // are made of, and reading them apart afterwards is the hard part.
    if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
        log_->Trace("page", "dirty page=" + std::to_string(page_id_) + " lsn=" +
                                std::to_string(page_lsn_) + " rec_lsn=" +
                                std::to_string(rec_lsn_));
    }
}

BufferPool::BufferPool(PageStore& backing, std::uint32_t nr_frames) : backing_(backing) {
    frames_.resize(nr_frames);
    free_slots_.reserve(nr_frames);
    for (std::uint32_t i = 0; i < nr_frames; ++i) {
        free_slots_.push_back(i);
    }
}

void BufferPool::SetLogger(Logger* log) noexcept {
    log_ = log;
    // Resident frames get it too: a pool that was opened before the log
    // was would otherwise stay silent for every page it is already
    // holding, which is exactly the set of pages a running server cares
    // about most.
    for (const auto& [page_id, slot] : page_to_slot_) {
        frames_[slot].log_ = log;
    }
}

StatusOr<std::uint32_t> BufferPool::TakeFreeFrameSlot() noexcept {
    if (free_slots_.empty()) {
        // Not an Error: the caller gets a Status and decides. But it is
        // the pool's own capacity running out, which no caller-visible
        // message names as a *pool-wide* condition, so it is worth a line.
        if (log_ != nullptr && log_->enabled(LogLevel::kWarn)) {
            log_->Warn("buffer", "frame table full: " + std::to_string(frames_.size()) +
                                     " frames, all resident, no eviction implemented");
        }
        return Status::OutOfSpace("buffer pool has no free frames (no eviction implemented)");
    }
    std::uint32_t slot = free_slots_.back();
    free_slots_.pop_back();
    return slot;
}

Frame& BufferPool::RegisterFrame(std::uint32_t slot, PageId page_id,
                                  std::span<std::byte, kPageSize> bytes, bool initial_dirty) {
    Frame& f = frames_[slot];
    f.page_id_ = page_id;
    f.bytes_ = bytes;
    f.log_ = log_;
    f.dirty_ = initial_dirty;
    // The mirror is seeded from the page's own header rather than zeroed,
    // so it says what the page actually claims: a page loaded from the
    // store arrives with the LSN it was last written at, and a created one
    // reads 0 out of its zeroed header - "never logged", the state the
    // flush gate lets through unwaited.
    f.page_lsn_ = GetPageLsn(bytes);
    f.rec_lsn_ = 0;
    f.pin_count_ = 0;
    page_to_slot_[page_id] = slot;
    return f;
}

StatusOr<Frame*> BufferPool::Lookup(PageId page_id) noexcept {
    auto it = page_to_slot_.find(page_id);
    if (it == page_to_slot_.end()) {
        return Status::NotFound("page not resident in buffer pool");
    }
    Frame& f = frames_[it->second];
    Pin(f);
    return &f;
}

StatusOr<Frame*> BufferPool::LookupOrLoad(PageId page_id) {
    auto hit = Lookup(page_id);
    if (hit.ok()) return hit;

    auto slot = TakeFreeFrameSlot();
    if (!slot.ok()) return slot.status();

    auto bytes = backing_.Get(page_id);
    if (!bytes.ok()) {
        free_slots_.push_back(slot.value());
        return bytes.status();
    }

    Frame& f = RegisterFrame(slot.value(), page_id, bytes.value().bytes(), /*initial_dirty=*/false);
    Pin(f);
    if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
        log_->Trace("buffer", "load page=" + std::to_string(page_id) + " slot=" +
                                  std::to_string(slot.value()) + " page_lsn=" +
                                  std::to_string(f.page_lsn_));
    }
    return &f;
}

StatusOr<Frame*> BufferPool::AllocNew(PageId page_id) {
    // Up-front check: reject if already resident, which is a caller bug.
    auto existing = Lookup(page_id);
    if (existing.ok()) {
        Unpin(*existing.value());
        return Status::AlreadyExists("page id already resident in buffer pool");
    }

    auto slot = TakeFreeFrameSlot();
    if (!slot.ok()) return slot.status();

    auto created = backing_.CreateAt(page_id);
    if (!created.ok()) {
        free_slots_.push_back(slot.value());
        return created.status();
    }

    // Marked dirty up front: a caller-visible new page must be guaranteed
    // to reach persistence even if the caller never touches it again
    // before eviction - which holds even though there is no real disk to
    // flush to yet (see the file-level comment).
    Frame& f = RegisterFrame(slot.value(), page_id, created.value().bytes(), /*initial_dirty=*/true);
    Pin(f);
    if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
        log_->Trace("buffer", "alloc page=" + std::to_string(page_id) + " slot=" +
                                  std::to_string(slot.value()) + " (dirty, unlogged)");
    }
    return &f;
}

Status BufferPool::AwaitWalDurability(PageId page_id, wal::Lsn page_lsn) {
    if (page_lsn == kNoPageLsn) {
        return Status::OK();  // never logged; wal.md section 8-1 has nothing to say
    }
    if (wal_ == nullptr) {
        // Refusing is the only honest answer: the page's bytes describe
        // changes some log recorded, and this pool cannot ask whether that
        // log is durable. Writing anyway would be exactly the ordering
        // violation the gate exists to prevent.
        //
        // Logged at Error because it is a wiring fault, not a workload
        // condition: every flush of a logged page will fail this way until
        // someone injects the seam, and the caller's Status alone tends to
        // surface far from the missing SetWalDurability() call.
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("buffer", "flush refused: page " + std::to_string(page_id) +
                                      " carries page_lsn " + std::to_string(page_lsn) +
                                      " but no WalDurability seam is injected");
        }
        return Status::InvalidArgument("buffer pool: page " + std::to_string(page_id) +
                                       " carries page_lsn " + std::to_string(page_lsn) +
                                       " but no WalDurability seam is injected");
    }
    if (wal_->IsDurable(page_lsn)) {
        return Status::OK();
    }
    // The stall page.md section 11 wants attributable. Synchronous today,
    // a suspension once the reactor can suspend the flushing task.
    ++wal_waits_;
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("buffer", "wal wait: page " + std::to_string(page_id) + " needs lsn " +
                                  std::to_string(page_lsn) + " durable");
    }
    Status s = wal_->EnsureDurable(page_lsn);
    if (!s.ok() && log_ != nullptr && log_->enabled(LogLevel::kError)) {
        log_->Error("buffer", "wal wait failed for page " + std::to_string(page_id) + ": " +
                                  s.message());
    }
    return s;
}

void BufferPool::PrepareForWriteOut(Frame& frame) {
    // The checksum covers the page as it will land, so it goes last
    // (page.md section 8). A page nobody has formatted yet - created but
    // never given a header - has no checksum to compute and no page_lsn to
    // gate on; it is zeroes, which is what a never-written page reads back
    // as anyway. Stamping one would write a checksum over a page whose
    // type byte says "unformatted", making garbage look verified.
    if (ValidatePageHeader(frame.bytes()).ok()) {
        StampPageChecksum(frame.bytes());
    } else {
        ++unformatted_flushes_;
        // Expected for a page created and not yet formatted, suspicious
        // for anything else - which is why the page id is here rather than
        // only the counter.
        if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
            log_->Trace("buffer", "write-out of unformatted page " +
                                      std::to_string(frame.page_id_) + " (no checksum stamped)");
        }
    }
}

Status BufferPool::FlushSlots(std::vector<std::uint32_t> slots) {
    if (slots.empty()) {
        return Status::OK();
    }
    // Page-id order, so a batch turns into sequential I/O once there is
    // real I/O to order (page.md section 18-7).
    std::sort(slots.begin(), slots.end(), [this](std::uint32_t a, std::uint32_t b) {
        return frames_[a].page_id_ < frames_[b].page_id_;
    });

    wal::Lsn highest_page_lsn = kNoPageLsn;
    for (std::uint32_t slot : slots) {
        highest_page_lsn = std::max(highest_page_lsn, frames_[slot].page_lsn_);
    }
    // One wait for the batch: the log is a watermark, so waiting for the
    // highest page_lsn in it clears every frame below that too.
    if (Status s = AwaitWalDurability(frames_[slots.front()].page_id_, highest_page_lsn);
        !s.ok()) {
        return s;
    }

    for (std::uint32_t slot : slots) {
        PrepareForWriteOut(frames_[slot]);
    }
    // The write-out barrier. Frame bytes are a view into the backing
    // store's own memory today (see the file comment), so there is nothing
    // to copy - Sync() is the whole of "the bytes are now on stable
    // storage". Once a disk-backed store exists this becomes write-then-
    // barrier, and only the inside of this call changes.
    if (Status s = backing_.Sync(); !s.ok()) {
        // Nothing in the batch went clean, so no data was lost - but a
        // barrier that will not complete is how a database stops being
        // durable, and the flushing caller is often a background task.
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("buffer", "flush barrier failed for " + std::to_string(slots.size()) +
                                      " page(s), all left dirty: " + s.message());
        }
        return s;  // every frame stays dirty; nothing was made durable
    }
    ++flush_barriers_;
    for (std::uint32_t slot : slots) {
        ++flushes_;
        MarkClean(frames_[slot]);
    }
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("buffer", "flushed " + std::to_string(slots.size()) +
                                  " page(s), highest page_lsn " +
                                  std::to_string(highest_page_lsn));
    }
    return Status::OK();
}

Status BufferPool::Flush(Frame& frame) {
    if (!frame.dirty_) {
        return Status::OK();
    }
    const auto it = page_to_slot_.find(frame.page_id_);
    if (it == page_to_slot_.end()) {
        return Status::NotFound("buffer pool: frame for page " +
                                std::to_string(frame.page_id_) + " is not resident");
    }
    return FlushSlots({it->second});
}

Status BufferPool::FlushPages(std::span<const PageId> page_ids) {
    std::vector<std::uint32_t> slots;
    slots.reserve(page_ids.size());
    for (PageId page_id : page_ids) {
        const auto it = page_to_slot_.find(page_id);
        // Not resident, or already written back by the background writer:
        // either way there is nothing this batch has to do for it.
        if (it != page_to_slot_.end() && frames_[it->second].dirty_) {
            slots.push_back(it->second);
        }
    }
    return FlushSlots(std::move(slots));
}

Status BufferPool::FlushPage(PageId page_id) {
    const PageId one[] = {page_id};
    return FlushPages(one);
}

Status BufferPool::FlushAll() {
    std::vector<std::uint32_t> slots;
    for (const auto& [page_id, slot] : page_to_slot_) {
        if (frames_[slot].dirty_) {
            slots.push_back(slot);
        }
    }
    return FlushSlots(std::move(slots));
}

std::vector<BufferPool::DirtyPage> BufferPool::DirtyTable() const {
    std::vector<DirtyPage> table;
    for (const auto& [page_id, slot] : page_to_slot_) {
        const Frame& f = frames_[slot];
        if (f.dirty_) {
            table.push_back(DirtyPage{page_id, f.rec_lsn_});
        }
    }
    // Sorted so a checkpoint record is a deterministic function of pool
    // state - the simulator compares them byte for byte.
    std::sort(table.begin(), table.end(),
              [](const DirtyPage& a, const DirtyPage& b) { return a.page_id < b.page_id; });
    return table;
}

BufferPool::Stats BufferPool::stats() const noexcept {
    Stats s{};
    s.total = static_cast<std::uint32_t>(frames_.size());
    s.free = static_cast<std::uint32_t>(free_slots_.size());
    s.valid = s.total - s.free;
    s.flushes = flushes_;
    s.flush_barriers = flush_barriers_;
    s.wal_waits = wal_waits_;
    s.unformatted_flushes = unformatted_flushes_;
    return s;
}

StatusOr<std::span<std::byte, kPageSize>> BufferPool::CreateAtUnpinned(PageId page_id) {
    auto frame = AllocNew(page_id);
    if (!frame.ok()) return frame.status();
    auto bytes = frame.value()->bytes();
    Unpin(*frame.value());
    return bytes;
}

StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> BufferPool::CreateNewUnpinned() {
    auto slot = TakeFreeFrameSlot();
    if (!slot.ok()) return slot.status();

    auto created = backing_.CreateNew();
    if (!created.ok()) {
        free_slots_.push_back(slot.value());
        return created.status();
    }
    auto& [new_id, bytes_ref] = created.value();
    const std::span<std::byte, kPageSize> bytes = bytes_ref.bytes();

    RegisterFrame(slot.value(), new_id, bytes, /*initial_dirty=*/true);
    if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
        log_->Trace("buffer", "alloc page=" + std::to_string(new_id) + " slot=" +
                                  std::to_string(slot.value()) + " (new id, dirty, unlogged)");
    }
    return std::make_pair(new_id, bytes);
}

StatusOr<std::span<std::byte, kPageSize>> BufferPool::GetUnpinned(PageId page_id) {
    auto frame = LookupOrLoad(page_id);
    if (!frame.ok()) return frame.status();
    auto bytes = frame.value()->bytes();
    Unpin(*frame.value());
    return bytes;
}

}  // namespace kds::storage
