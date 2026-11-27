#include "kds/wal/checkpointer.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

#include "kds/base/contention.hpp"
#include "kds/stats/checkpoint_log.hpp"
#include "kds/wal/analysis.hpp"

namespace kds::wal {

Checkpointer::Checkpointer(WalManager& wal, CheckpointTarget& target, ActiveTransactions& txns,
                           CheckpointAnchor& anchor, CheckpointerConfig config)
    : wal_(wal), target_(target), txns_(txns), anchor_(anchor), config_(config) {}

Status Checkpointer::LogBegin(std::span<const CheckpointActiveTxn> active_txns,
                              std::span<const CheckpointDirtyPage> dirty_pages, bool* busy) {
    // **The dirty table is cut to what one record carries** (wal.md §11-1),
    // keeping the highest page id - the allocation floor analysis takes from
    // a recLSN-0 page, which no record names; the redo start and the
    // writeback stay the whole table's (`Start`). The bound is the largest
    // split's, not the ring (`WalManager::shared_record_payload_bytes`).
    std::vector<CheckpointDirtyPage> cut;
    const std::size_t budget = wal_.shared_record_payload_bytes();
    const std::size_t fixed = CheckpointBeginSize(active_txns.size(), 0);
    if (CheckpointBeginSize(active_txns.size(), dirty_pages.size()) > budget &&
        fixed + kDirtyEntrySize <= budget) {
        const std::size_t fits = (budget - fixed) / kDirtyEntrySize;
        cut.assign(dirty_pages.begin(), dirty_pages.begin() + fits);
        const auto highest = std::ranges::max_element(dirty_pages, {}, &CheckpointDirtyPage::page_id);
        if (highest - dirty_pages.begin() >= static_cast<std::ptrdiff_t>(fits)) {
            cut.back() = *highest;
        }
        dirty_pages = cut;
    }

    payload_scratch_.assign(CheckpointBeginSize(active_txns.size(), dirty_pages.size()),
                            std::byte{0});
    auto encoded = EncodeCheckpointBegin(payload_scratch_, active_txns, dirty_pages);
    if (!encoded.ok()) {
        return encoded.status();
    }

    const RecordSpec spec{RecordType::kCheckpointBegin, kNoTxnId, kInvalidPageId};
    const std::span<const std::byte> payload = std::span(payload_scratch_).first(encoded.value());
    auto lsn = busy != nullptr ? wal_.TryAppend(spec, payload, busy) : wal_.Append(spec, payload);
    if (!lsn.ok()) {
        return lsn.status();
    }
    if (busy != nullptr && *busy) return Status::OK();  // nothing appended
    begin_lsn_ = lsn.value();
    return Status::OK();
}

namespace {

// One chunk of a run: a range of `cabin.groups` and its payload bytes.
struct SnapshotChunk {
    std::size_t first = 0;  // into `cabin.groups`
    std::size_t end = 0;
    std::size_t bytes = kAssertSnapshotFixedSize;
};

// **The run is cut before anything is written**, because every chunk carries
// the count. That also puts both refusals ahead of the first record, so
// neither leaves a partial run in the log; an append that fails mid-run still
// can, and the reader discards it.
//
// Both refusals are `NotImplemented` (AZ-Q2): the bound is the snapshot
// format's - one record per group, a `u16` count - and a later release can
// widen it. `OutOfSpace` would say storage is full, which it is not.
StatusOr<std::vector<SnapshotChunk>> CutRun(const AssertionCabinSnapshot& cabin,
                                            std::size_t budget) {
    std::vector<SnapshotChunk> chunks(1);  // a cabin with no groups still gets one - see the header
    for (std::size_t at = 0; at < cabin.groups.size(); ++at) {
        const std::size_t cost = AssertSnapshotGroupBytes(cabin.groups[at].key.size());
        if (kAssertSnapshotFixedSize + cost > budget) {
            // One group too large for an entire record. Refused rather than
            // dropped: a base missing a group under-counts, and an admission
            // check built on it would admit a write that violates the
            // assertion.
            return Status::NotImplemented(
                "assertion snapshot: assertion " + std::to_string(cabin.assertion_id) +
                " has a group key of " + std::to_string(cabin.groups[at].key.size()) +
                " bytes, which no ASSERT_SNAPSHOT record can carry");
        }
        if (chunks.back().end > chunks.back().first && chunks.back().bytes + cost > budget) {
            chunks.push_back(SnapshotChunk{at, at, kAssertSnapshotFixedSize});
        }
        chunks.back().end = at + 1;
        chunks.back().bytes += cost;
    }
    if (chunks.size() > kMaxAssertSnapshotChunks) {
        // Refused for the same reason: the count could not say the run.
        return Status::NotImplemented("assertion snapshot: assertion " +
                                      std::to_string(cabin.assertion_id) + " needs " +
                                      std::to_string(chunks.size()) +
                                      " ASSERT_SNAPSHOT records, more than one run can count");
    }
    return chunks;
}

}  // namespace

Status AssertionSnapshotFits(const AssertionCabinSnapshot& cabin, std::size_t record_budget) {
    return CutRun(cabin, record_budget).status();
}

Status LogAssertionSnapshot(WalManager& wal, const AssertionCabinSnapshot& cabin) {
    // The chunk bound: what one record's payload may hold. A cabin's group count
    // is bounded by the data, so a cabin can need several records - a run, each
    // chunk naming its place in it (payload.hpp, AY-R7). The largest split's
    // bound, not the ring's: a ring-sized chunk on the shared stream
    // fail-stops the log (`shared_record_payload_bytes`, G17).
    auto cut = CutRun(cabin, wal.shared_record_payload_bytes());
    if (!cut.ok()) return cut.status();
    const std::vector<SnapshotChunk>& chunks = cut.value();

    std::vector<std::byte> scratch;
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        std::vector<SnapshotGroupEntry> entries;  // views into the owned keys
        entries.reserve(chunks[i].end - chunks[i].first);
        for (std::size_t at = chunks[i].first; at < chunks[i].end; ++at) {
            const AssertionSnapshotGroup& group = cabin.groups[at];
            SnapshotGroupEntry entry;
            entry.group_id = group.group_id;
            entry.count = group.count;
            entry.sum = group.sum;
            entry.key = std::as_bytes(std::span<const char>(group.key.data(), group.key.size()));
            entries.push_back(entry);
        }

        scratch.assign(chunks[i].bytes, std::byte{0});
        AssertSnapshotPayload fields{};
        fields.assertion_id = cabin.assertion_id;
        fields.chunk_index = static_cast<std::uint16_t>(i);
        fields.chunk_count = static_cast<std::uint16_t>(chunks.size());
        auto encoded = EncodeAssertSnapshot(scratch, fields, entries);
        if (!encoded.ok()) {
            return encoded.status();
        }
        auto lsn = wal.Append({RecordType::kAssertSnapshot, kNoTxnId, kInvalidPageId, 0},
                              std::span(scratch).first(encoded.value()));
        if (!lsn.ok()) {
            return lsn.status();
        }
    }
    // After the run, in the same hold: the reservations the headers count for
    // a transaction not yet decided (CL-R1). A mount that finds the run its
    // base reads these next, and compensates each whose transaction is not a
    // winner.
    for (const AssertionPendingReservation& r : cabin.pending) {
        std::vector<std::byte> payload(kAssertRollbackFixedSize + r.key.size());
        AssertRollbackPayload fields{};
        fields.assertion_id = cabin.assertion_id;
        fields.delta = r.delta;
        fields.index = r.index;
        auto used = EncodeAssertRollback(
            payload, fields, std::as_bytes(std::span<const char>(r.key.data(), r.key.size())));
        if (!used.ok()) return used.status();
        RecordSpec spec{RecordType::kAssertPending, r.txn_id, r.page_id};
        spec.flags = r.departure ? kAssertRollbackFlagDeparture : 0;
        if (auto lsn = wal.Append(spec, std::span(payload).first(used.value())); !lsn.ok()) {
            return lsn.status();
        }
    }
    return Status::OK();
}

Status Checkpointer::LogAssertionSnapshots() {
    if (assertions_ == nullptr) {
        return Status::OK();  // no assertions on this core: no records, no cost
    }
    return assertions_->VisitSnapshots(
        wal_.shared_record_payload_bytes(),
        [this](const std::vector<AssertionCabinSnapshot>& cabins) -> Status {
            for (const AssertionCabinSnapshot& cabin : cabins) {
                if (Status s = LogAssertionSnapshot(wal_, cabin); !s.ok()) {
                    return s;
                }
            }
            return Status::OK();
        });
}

namespace {

// `CheckpointTarget::CollectDirty`'s default: the whole table at once.
class WholeCollection final : public DirtyCollection {
public:
    WholeCollection(CheckpointTarget& target, DirtyCollectionFor use) : target_(target), use_(use) {}
    bool Advance() override {
        if (use_ == DirtyCollectionFor::kStart) {
            table_ = target_.DirtyTable();
        } else {
            for (const PageId id : target_.DirtyPages()) table_.push_back({id, kNoLsn});
        }
        return true;
    }
    std::vector<CheckpointDirtyPage>& Table() override { return table_; }

private:
    CheckpointTarget& target_;
    DirtyCollectionFor use_;
    std::vector<CheckpointDirtyPage> table_;
};

}  // namespace

std::unique_ptr<DirtyCollection> CheckpointTarget::CollectDirty(DirtyCollectionFor use) {
    return std::make_unique<WholeCollection>(*this, use);
}

Status Checkpointer::Start() { return Start(nullptr); }

Status Checkpointer::Start(bool* busy) {
    if (busy != nullptr) *busy = false;
    if (in_progress_) {
        return Status::AlreadyExists("checkpointer: a checkpoint is already in progress");
    }
    const std::unique_ptr<DirtyCollection> collection = target_.CollectDirty(DirtyCollectionFor::kStart);
    while (!collection->Advance()) {
    }
    return BeginWith(collection->Table(), busy);
}

Status Checkpointer::BeginWith(std::vector<CheckpointDirtyPage>& dirty, bool* busy) {
    if (busy != nullptr) *busy = false;
    const CheckpointPhaseScope scope(&wal_.clock(), run_report_);
    const PhaseTimer timed(CheckpointPhase::kStart);

    // Both tables are snapshotted before the record is written, so what
    // BEGIN carries is what recovery's analysis phase will see. **No
    // suspension lies between the active transactions and the append**
    // (CZ-R3, DZ-R1): a tried append that answers busy appends nothing, and
    // the task collects both again. The dirty table may have been collected
    // across yields (DZ-S1): it is then each frame's as of its slot's visit,
    // which DO's §5a, re-read at DZ-S0, proves enough for the redo start.
    // The transactions may not be, so they are taken last: one this core
    // began during a yield of the walk is listed if it is still undecided
    // now.
    const std::vector<CheckpointActiveTxn> active_txns = txns_.Snapshot();

    if (Status s = LogBegin(active_txns, dirty, busy); !s.ok()) {
        // BI-S1 counts it: the BEGIN ceiling refuses here, at the append.
        // Only a gated run's, like every other census field.
        if (run_report_ != nullptr) {
            CheckpointCensus::Of(wal_.core_id()).start_refused.fetch_add(1, std::memory_order_relaxed);
        }
        return s;
    }
    // Busy is not a refusal: not counted in `start_refused`.
    if (busy != nullptr && *busy) return Status::OK();

    // AS6a's base, immediately after BEGIN: ahead of every ASSERT_* record that
    // will be folded onto it, and inside the checkpoint the anchor names, which
    // is what lets replay find it without scanning to the cabin's birth.
    // **Blocking, inside the directory latch's one hold** (CZ-Q16 (a),
    // `[quiet-wrong]`): a run is a base only whole and its pending list is
    // read only directly behind it, so a task that gave up part-way would
    // leave undecided reservations no mount compensates. The visit's whole
    // time is counted.
    {
        const sched::MonoTimeNs visit = wal_.clock().Now();
        const Status s = LogAssertionSnapshots();
        if (run_report_ != nullptr && assertions_ != nullptr) {
            run_report_->assertion_visit_ns += wal_.clock().Now() - visit;
        }
        if (!s.ok()) return s;
    }

    // Redo start: the oldest LSN any page in the snapshot still needs
    // replayed. Zero recLSNs are pages that are dirty but that no record
    // describes (created, never logged) - "nothing to replay", not "replay
    // from the head of the log", so they are skipped rather than min()ed
    // in. With no logged page in the snapshot, nothing before this
    // checkpoint matters and the redo start is the checkpoint itself.
    //
    // Pages dirtied after the record need no special handling: their
    // recLSNs are necessarily above begin_lsn_, which is itself an upper
    // bound on what this computes. **Not so between the snapshot and the
    // append at `cores > 1`** (CZ-S0's census): another core can dirty and
    // log a page there, below begin_lsn_ and absent from the snapshot. Two
    // things cover it - the fold, whose minimum keeps that core's own last
    // redo start, below this record, until its next run snapshots the page;
    // and the publish, which writes every page dirty before the anchor's
    // page. Both rest on no core yielding between an append and its stamp.
    // §11-3's rule lives in RedoStartFrom (wal/analysis.hpp), shared with
    // recovery's analysis phase, which recomputes the same number backward
    // from the table it rebuilds. Two copies of "skip a recLSN of 0" is two
    // chances to lose the skip, and losing it drags the redo start to zero
    // and replays the whole stream.
    pending_redo_start_ = RedoStartFrom(begin_lsn_, dirty);
    pending_pages_.clear();
    pending_pages_.reserve(dirty.size());
    for (const CheckpointDirtyPage& page : dirty) {
        pending_pages_.push_back(page.page_id);
    }

    next_page_ = 0;
    in_progress_ = true;
    ++stats_.started;
    if (run_report_ != nullptr) run_report_->dirty_at_start = pending_pages_.size();
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("checkpoint", "started: begin_lsn=" + std::to_string(begin_lsn_) +
                                      " dirty_pages=" + std::to_string(pending_pages_.size()) +
                                      " active_txns=" + std::to_string(active_txns.size()) +
                                      " redo_start=" + std::to_string(pending_redo_start_));
    }
    return Status::OK();
}

StatusOr<bool> Checkpointer::Step(Lsn* defer, bool write_only) {
    if (!in_progress_) {
        return Status::OutOfRange("checkpointer: no checkpoint in progress");
    }
    ++stats_.steps;
    if (run_report_ != nullptr) ++run_report_->steps;
    if (next_page_ >= pending_pages_.size()) {
        return true;
    }
    const CheckpointPhaseScope scope(&wal_.clock(), run_report_);

    const std::size_t batch =
        std::min(config_.pages_per_step, pending_pages_.size() - next_page_);
    const std::span<const PageId> pages(pending_pages_.data() + next_page_, batch);
    Status flushed;
    {
        // BI-Q9 (ii)'s quantities: the step's whole hold, and its page writes
        // summed, each maxed over the run.
        const sched::MonoTimeNs step_start = wal_.clock().Now();
        const std::size_t write = static_cast<std::size_t>(CheckpointPhase::kPageWrite);
        const std::uint64_t writes_before = run_report_ != nullptr ? run_report_->sum_ns[write] : 0;
        if (!write_only) {
            flushed = target_.FlushPages(pages);
            if (flushed.ok()) Contention::Add(Tally::kCheckpointPagesWritten, pages.size());
        } else if (auto wrote = target_.WritePages(pages, defer); !wrote.ok()) {
            flushed = wrote.status();
        } else if (wrote.value() > 0) {
            Contention::Add(Tally::kCheckpointPagesWritten, wrote.value());
            // Asked for and not waited on (BI-Q13): the data-sync thread
            // keeps the unsynced backlog near one step, so a synchronous sync
            // landing mid-run - a carve's page 0, the reclaimer's maps - does
            // not inherit a run's worth of writeback; only the publish parks.
            (void)target_.RequestSync();
        }
        if (run_report_ != nullptr) {
            const std::uint64_t held = wal_.clock().Now() - step_start;
            run_report_->step_max_ns = std::max(run_report_->step_max_ns, held);
            run_report_->step_page_write_max_ns = std::max(
                run_report_->step_page_write_max_ns, run_report_->sum_ns[write] - writes_before);
        }
    }
    if (Status s = std::move(flushed); !s.ok()) {
        // The cursor does not move: these pages are still the
        // checkpoint's responsibility, and the next Step() retries them.
        // Logged because the retry makes this survivable and therefore
        // silent - a checkpoint that fails every step forever looks
        // exactly like one that is merely slow.
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("checkpoint", "flush of " + std::to_string(batch) +
                                          " page(s) failed at offset " +
                                          std::to_string(next_page_) + ", will retry: " +
                                          s.message());
        }
        return s;
    }
    // Deferred: the pages before the deferred run are clean now and the
    // next call skips them; the cursor stays on this batch.
    if (defer != nullptr && *defer != kNoLsn) return false;
    next_page_ += batch;
    stats_.pages_flushed += batch;
    if (run_report_ != nullptr) run_report_->pages += batch;
    if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
        log_->Trace("checkpoint", "flushed " + std::to_string(batch) + " page(s), " +
                                      std::to_string(pending_pages_.size() - next_page_) +
                                      " remaining");
    }
    return next_page_ >= pending_pages_.size();
}

StatusOr<Lsn> Checkpointer::AppendEnd(bool* busy) {
    if (busy != nullptr) *busy = false;
    if (!in_progress_) {
        return Status::OutOfRange("checkpointer: no checkpoint in progress");
    }
    if (next_page_ < pending_pages_.size()) {
        // wal.md section 8-3: CHECKPOINT_END is written only once every
        // page in the table has been flushed. Writing it now would leave
        // an anchor recovery trusts pointing past pages that are not on
        // disk.
        return Status::OutOfRange(
            "checkpointer: " + std::to_string(pending_pages_.size() - next_page_) +
            " pages of the checkpoint's dirty table are still unflushed");
    }
    const CheckpointPhaseScope scope(&wal_.clock(), run_report_);
    payload_scratch_.assign(kCheckpointEndPayloadSize, std::byte{0});
    const CheckpointEndPayload fields{pending_redo_start_};
    auto encoded = EncodeCheckpointEnd(payload_scratch_, fields);
    if (!encoded.ok()) {
        return encoded.status();
    }
    const RecordSpec spec{RecordType::kCheckpointEnd, kNoTxnId, kInvalidPageId};
    const std::span<const std::byte> payload = std::span(payload_scratch_).first(encoded.value());
    return busy != nullptr ? wal_.TryAppend(spec, payload, busy) : wal_.Append(spec, payload);
}

Status Checkpointer::Complete() {
    auto end_lsn = AppendEnd();
    if (!end_lsn.ok()) return end_lsn.status();

    // The end record has to be on disk before anything points at it: an
    // anchor naming a redo start that a crash then erases is worse than no
    // checkpoint at all, because recovery would skip records it still
    // needs.
    {
        const CheckpointPhaseScope scope(&wal_.clock(), run_report_);
        const PhaseTimer timed(CheckpointPhase::kEndWait);
        if (Status s = wal_.EnsureDurable(end_lsn.value()); !s.ok()) {
            return s;
        }
    }
    return PublishAnchor();
}

CheckpointAnchorRecord Checkpointer::AnchorRecord() const {
    return CheckpointAnchorRecord{wal_.core_id(), begin_lsn_, pending_redo_start_,
                                  wal_.durable_lsn(), pending_redo_start_ / wal_.segment_size()};
}

Status Checkpointer::PublishAnchor() {
    const CheckpointPhaseScope scope(&wal_.clock(), run_report_);
    const CheckpointAnchorRecord anchor = AnchorRecord();
    if (Status s = anchor_.Publish(anchor); !s.ok()) {
        // The end record is durable but nothing points at it: recovery
        // will replay from the previous anchor, which is correct and
        // slower. Worth an Error - the checkpoint bought nothing.
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("checkpoint", "anchor publish failed after a durable CHECKPOINT_END: " +
                                          s.message());
        }
        return s;
    }
    Published(anchor);
    return Status::OK();
}

void Checkpointer::Published(const CheckpointAnchorRecord& anchor) {
    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("checkpoint", "anchor published: core=" + std::to_string(anchor.core_id) +
                                     " checkpoint_lsn=" + std::to_string(anchor.checkpoint_lsn) +
                                     " redo_start=" + std::to_string(anchor.redo_start_lsn) +
                                     " durable_lsn=" + std::to_string(anchor.durable_lsn) +
                                     " segment=" + std::to_string(anchor.segment_no));
    }
    redo_start_lsn_ = pending_redo_start_;
    last_checkpoint_lsn_ = begin_lsn_;
    in_progress_ = false;
    pending_pages_.clear();
    next_page_ = 0;
    ++stats_.completed;
}

Status Checkpointer::RunToCompletion() {
    // **A checkpoint a failed step or completion left running is resumed, not
    // refused** (BC-S4): `Start` refuses a nested one, so one transient
    // writeback error would otherwise wedge the checkpointer - the anchor
    // stops moving and nothing is recycled again. The resumed run keeps its snapshot, whose
    // redo start was valid when taken and stays so; a `Complete` that failed
    // after appending its `CHECKPOINT_END` appends a second one, which no
    // reader consumes.
    if (!in_progress_) {
        if (Status s = Start(); !s.ok()) {
            return s;
        }
    }
    while (true) {
        auto done = Step();
        if (!done.ok()) {
            return done.status();
        }
        if (done.value()) {
            break;
        }
    }
    return Complete();
}

Status Checkpointer::RunGated(CheckpointGate* gate, std::uint32_t core_id) {
    // A run task of this checkpointer is queued or parked: a synchronous run
    // now would finish its snapshot under it. Shutdown abandons the task
    // first (BI-R4); any other caller skips, as at a held gate.
    if (task_in_flight_) {
        LogSkip(core_id);
        return Status::OK();
    }
    const CheckpointGate::Hold run(gate);
    if (!run.entered()) {
        LogSkip(core_id);
        return Status::OK();
    }
    CheckpointRunReport report;
    BeginRun(core_id, report);
    const Status ran = RunToCompletion();
    EndRun(core_id, ran);
    return ran;
}

void Checkpointer::SubmitRun(sched::Scheduler& scheduler, CheckpointGate* gate,
                             std::uint32_t core_id) {
    // A tick during a run submits nothing; `SubmitEvery` already coalesces
    // the periods a run outlasts.
    if (task_in_flight_) return;
    task_in_flight_ = true;
    scheduler.Submit(sched::MakeCoroTask(sched::SchedulingGroup::kSystem,
                                         RunTask(gate, core_id, task_generation_)));
}

sched::Coro Checkpointer::RunTask(CheckpointGate* gate, std::uint32_t core_id,
                                  std::uint64_t generation) {
    // Abandoned before its first poll (BI-R4): nothing of this checkpointer
    // is its own any more.
    if (generation != task_generation_) co_return Status::OK();
    task_core_ = core_id;
    task_hold_.emplace(gate, core_id, wal_.clock().Now());
    if (!task_hold_->entered()) {
        task_hold_.reset();
        task_in_flight_ = false;
        LogSkip(core_id);
        co_return Status::OK();
    }
    task_report_ = CheckpointRunReport{};
    BeginRun(core_id, task_report_);

    // **One synchronous call per poll** (BI-R1): `Start`, each `Step`, and
    // `Complete`, a yield after each. A failed one ends the task with the
    // run left in progress; the next tick's task resumes it.
    //
    // **The begin record is tried** (CZ-S3, C1b): on a roll the task parks
    // before it snapshots anything, so no snapshot is taken to be thrown
    // away; a tried `Start` that finds the latch held appends nothing and is
    // called again after the park, snapshots and all (CZ-R3). Each busy
    // answer, the roll flag's included, counts against `kLatchTries`.
    if (!in_progress_) {
        CheckpointRunReport::LatchSite& site = Site(CheckpointLatchSite::kBegin);
        for (int answers = 0;; ++answers) {
            bool busy = false;
            if (answers < kLatchTries) {
                busy = wal_.rolling();
                if (!busy) {
                    // **The table in slices, a yield between** (DZ-S1); the
                    // transactions and the tried append in the one call after
                    // it (`BeginWith`). A busy answer collects both again.
                    const std::unique_ptr<DirtyCollection> collection =
                        target_.CollectDirty(DirtyCollectionFor::kStart);
                    if (!(co_await CollectSliced(*collection, generation)).ok()) co_return Status::OK();
                    if (Status s = BeginWith(collection->Table(), &busy); !s.ok()) {
                        co_return FinishTask(core_id, s);
                    }
                }
            } else {
                ++site.bound;
                const sched::MonoTimeNs blocked = wal_.clock().Now();
                const Status s = Start();
                site.blocking_ns += wal_.clock().Now() - blocked;
                if (!s.ok()) co_return FinishTask(core_id, s);
            }
            if (!busy) break;
            ++site.busy;
            // The sixteenth busy answer is the last: the blocking entry next,
            // with no park before it (§8.8 item 6).
            if (site.busy >= static_cast<std::uint64_t>(kLatchTries)) continue;
            if (Status s = co_await ParkOnBusy(CheckpointLatchSite::kBegin, kNoLsn, generation); !s.ok()) {
                co_return FinishTask(core_id, s);
            }
            if (generation != task_generation_) co_return Status::OK();
        }
        co_await sched::Yield{};
        if (generation != task_generation_) co_return Status::OK();
    }
    // **One park for the whole snapshot first** (BI-S5): every recLSN in it
    // is ahead of `CHECKPOINT_BEGIN`, so once the log is durable through
    // that record a step defers only for a page changed since - where a park per step cost each of a run's steps a
    // reactor iteration, and under load an iteration is a statement long.
    if (!wal_.IsDurable(begin_lsn_)) ++task_report_.gate_parks;
    if (Status s = co_await ParkOnLog(begin_lsn_, core_id, generation); !s.ok()) {
        co_return FinishTask(core_id, s);
    }
    if (generation != task_generation_) co_return Status::OK();
    // **Every wait is a park** (BI-R2). A step that needs the log further
    // hands back the LSN instead of waiting (BI-Q4 (a)); the task asks the
    // writer for the sync and parks, counted into the writer's kick
    // (BI-Q6-R), and runs the step again, inline after `kGateParks` parks.
    // **A step asks for no data-file sync** (BI-Q13): the publish's sync of
    // its collection follows every step's writes and precedes the anchor's
    // page, which is the order the anchor needs, and a sync per step cost
    // each step a park. The reactor polls everything else meanwhile.
    int parks = 0;
    while (true) {
        Lsn deferred = kNoLsn;
        auto done = Step(parks < kGateParks ? &deferred : nullptr, /*write_only=*/true);
        if (!done.ok()) co_return FinishTask(core_id, done.status());
        if (deferred != kNoLsn) {
            // A page_lsn at or past the append point names no record, and no
            // sync satisfies a park on it: the next step's inline gate
            // refuses it, as before, rather than the task parking for good on
            // the instance's gate.
            if (deferred >= wal_.appended_lsn()) {
                parks = kGateParks;
                continue;
            }
            ++parks;
            ++task_report_.gate_parks;
            if (Status s = co_await ParkOnLog(deferred, core_id, generation); !s.ok()) {
                co_return FinishTask(core_id, s);
            }
            if (generation != task_generation_) co_return Status::OK();
            continue;
        }
        parks = 0;
        co_await sched::Yield{};
        if (generation != task_generation_) co_return Status::OK();
        if (done.value()) break;
    }

    // `CHECKPOINT_END`, durable before anything points at it. **Tried**
    // (CZ-S3, C1b): no snapshot is behind it, so a busy answer parks and
    // appends again, to `kLatchTries` answers and then the blocking append.
    Lsn end_lsn = kNoLsn;
    {
        CheckpointRunReport::LatchSite& site = Site(CheckpointLatchSite::kEnd);
        for (int answers = 0;; ++answers) {
            bool busy = false;
            StatusOr<Lsn> appended = kNoLsn;
            if (answers < kLatchTries) {
                appended = AppendEnd(&busy);
            } else {
                ++site.bound;
                const sched::MonoTimeNs blocked = wal_.clock().Now();
                appended = AppendEnd();
                site.blocking_ns += wal_.clock().Now() - blocked;
            }
            if (!appended.ok()) co_return FinishTask(core_id, appended.status());
            if (!busy) {
                end_lsn = appended.value();
                break;
            }
            ++site.busy;
            // The sixteenth busy answer is the last: the blocking entry next,
            // with no park before it (§8.8 item 6).
            if (site.busy >= static_cast<std::uint64_t>(kLatchTries)) continue;
            if (Status s = co_await ParkOnBusy(CheckpointLatchSite::kEnd, kNoLsn, generation); !s.ok()) {
                co_return FinishTask(core_id, s);
            }
            if (generation != task_generation_) co_return Status::OK();
        }
    }
    if (!wal_.IsDurable(end_lsn)) ++task_report_.end_parks;
    if (Status s = co_await ParkOnLog(end_lsn, core_id, generation); !s.ok()) {
        co_return FinishTask(core_id, s);
    }
    if (generation != task_generation_) co_return Status::OK();

    // **The publish, paced** (BI-Q5 (a)). Every page dirty now, written in a
    // step's batches with a yield between, then the maps after all of them
    // (BI-Q12), then one parked sync. Only then is the anchor encoded and its
    // page written and synced, so the anchor reaches the device after every
    // page it stands for - a snapshot page another core's writeback carried,
    // which no step of this run synced, among them.
    // The maps go in two halves with a park between (`page.md` section 5,
    // the map write order), owed only when a headerless map changed.
    // The collection in slices, a yield between (DZ-S1): DZ-S0 measured it
    // whole at about 10 ms on the reactor at 1 Mi dirty frames.
    const std::unique_ptr<DirtyCollection> collection =
        target_.CollectDirty(DirtyCollectionFor::kPublish);
    if (!(co_await CollectSliced(*collection, generation)).ok()) co_return Status::OK();
    const std::vector<CheckpointDirtyPage>& dirty = collection->Table();
    std::vector<PageId> batch;
    batch.reserve(config_.pages_per_step);
    for (std::size_t at = 0; at < dirty.size(); at += config_.pages_per_step) {
        batch.clear();
        const std::size_t end = std::min(dirty.size(), at + config_.pages_per_step);
        for (std::size_t i = at; i < end; ++i) batch.push_back(dirty[i].page_id);
        if (Status s = co_await WriteForPublish(batch, core_id, generation); !s.ok()) {
            co_return FinishTask(core_id, s);
        }
        if (generation != task_generation_) co_return Status::OK();
        co_await sched::Yield{};
        if (generation != task_generation_) co_return Status::OK();
    }
    std::uint64_t map_ticket = 0;
    {
        const CheckpointPhaseScope scope(&wal_.clock(), run_report_);
        const PhaseTimer timed(CheckpointPhase::kPublishWriteback);
        auto staged = target_.WriteHeaderlessMaps();
        if (!staged.ok()) co_return FinishTask(core_id, staged.status());
        map_ticket = staged.value();
    }
    if (Status s = co_await ParkOnDataSyncTicket(map_ticket); !s.ok()) {
        co_return FinishTask(core_id, s);
    }
    if (generation != task_generation_) co_return Status::OK();
    {
        const CheckpointPhaseScope scope(&wal_.clock(), run_report_);
        const PhaseTimer timed(CheckpointPhase::kPublishWriteback);
        if (Status s = target_.WriteFreeMaps(); !s.ok()) co_return FinishTask(core_id, s);
    }
    if (Status s = co_await ParkOnDataSync(); !s.ok()) co_return FinishTask(core_id, s);
    if (generation != task_generation_) co_return Status::OK();

    const CheckpointAnchorRecord anchor = AnchorRecord();
    auto anchor_page = anchor_.Encode(anchor);
    if (!anchor_page.ok()) co_return FinishTask(core_id, anchor_page.status());
    if (anchor_page.value() != kInvalidPageId) {
        const PageId page[] = {anchor_page.value()};
        if (Status s = co_await WriteForPublish(page, core_id, generation); !s.ok()) {
            co_return FinishTask(core_id, s);
        }
        if (generation != task_generation_) co_return Status::OK();
        if (Status s = co_await ParkOnDataSync(); !s.ok()) co_return FinishTask(core_id, s);
        if (generation != task_generation_) co_return Status::OK();
        // The recycle it owes, tried (C1c): skipped on a busy latch, and
        // counted either way it went past the try.
        switch (anchor_.Commit()) {
            case RecycleOutcome::kSkipped: ++task_report_.recycle_skips; break;
            case RecycleOutcome::kBlocked: ++task_report_.recycle_blocked; break;
            case RecycleOutcome::kNone:
            case RecycleOutcome::kDone: break;
        }
    }
    Published(anchor);
    co_return FinishTask(core_id, Status::OK());
}

namespace {

// As `WriteBack` answers it: nothing is written after a log fail-stop.
Status LogStopped() {
    return Status::IoError("checkpointer: the log fail-stopped; no page is written back and no "
                           "anchor published until a restart");
}

}  // namespace

sched::Coro Checkpointer::CollectSliced(DirtyCollection& collection, std::uint64_t generation) {
    while (!collection.Advance()) {
        co_await sched::Yield{};
        // Abandoned meanwhile: nothing of this checkpointer is the task's.
        if (generation != task_generation_) co_return Status::Cancelled("checkpoint run abandoned");
    }
    co_return Status::OK();
}

sched::Coro Checkpointer::ParkOnDataSync() {
    co_return co_await ParkOnDataSyncTicket(target_.RequestSync());
}

sched::Coro Checkpointer::ParkOnDataSyncTicket(std::uint64_t ticket) {
    if (ticket == 0) co_return Status::OK();  // a target with nothing to wait on
    const std::function<bool()> synced = [this, ticket] { return target_.SyncDone(ticket); };
    const sched::MonoTimeNs parked = wal_.clock().Now();
    {
        sched::WaitScope wait(stats::WaitEvent::kSysioCheckpoint);
        co_await sched::WaitUntil{&synced};
    }
    task_report_.data_park_ns += wal_.clock().Now() - parked;
    co_return target_.SyncResult(ticket);
}

sched::Coro Checkpointer::WriteForPublish(std::span<const PageId> pages, std::uint32_t core_id,
                                          std::uint64_t generation) {
    for (int parks = 0;; ++parks) {
        Lsn deferred = kNoLsn;
        StatusOr<std::size_t> wrote = std::size_t{0};
        {
            const CheckpointPhaseScope scope(&wal_.clock(), run_report_);
            const PhaseTimer timed(CheckpointPhase::kPublishWriteback);
            wrote = target_.WritePages(pages,
                                       parks < kGateParks && !pages.empty() ? &deferred : nullptr);
        }
        if (!wrote.ok()) co_return wrote.status();
        Contention::Add(Tally::kCheckpointPagesWritten, wrote.value());
        if (deferred == kNoLsn) co_return Status::OK();
        if (deferred >= wal_.appended_lsn()) {
            parks = kGateParks - 1;  // the next attempt's inline gate refuses it
            continue;
        }
        ++task_report_.gate_parks;
        if (Status s = co_await ParkOnLog(deferred, core_id, generation); !s.ok()) co_return s;
        if (generation != task_generation_) co_return Status::OK();
    }
}

sched::Coro Checkpointer::ParkOnBusy(CheckpointLatchSite site, Lsn durable,
                                     std::uint64_t generation) {
    const bool on_roll = wal_.rolling();
    const sched::MonoTimeNs parked = wal_.clock().Now();
    const sched::MonoTimeNs deadline = parked + (on_roll ? kRollParkCeilingNs : kBusyParkNs);
    const std::function<bool()> ended = [this, on_roll, deadline, durable] {
        if (wal_.stopped() || wal_.clock().Now() >= deadline) return true;
        if (durable != kNoLsn && wal_.IsDurable(durable)) return true;
        return on_roll && !wal_.rolling();
    };
    CheckpointRunReport::LatchSite& counted = Site(site);
    if (on_roll) ++counted.roll_parks;
    if (!ended()) {
        sched::WaitScope wait(stats::WaitEvent::kSysioWalRoll, static_cast<std::uint64_t>(site),
                              on_roll ? 1u : 0u);
        co_await sched::WaitUntil{&ended};
    }
    if (generation != task_generation_) co_return Status::OK();
    counted.park_ns += wal_.clock().Now() - parked;
    // The stop ends every park, and the task ends with it (CZ-R8).
    co_return wal_.stopped() ? LogStopped() : Status::OK();
}

sched::Coro Checkpointer::ParkOnLog(Lsn lsn, std::uint32_t core_id, std::uint64_t generation) {
    if (wal_.IsDurable(lsn)) co_return Status::OK();
    if (wal_.stopped()) co_return LogStopped();
    // **The flush is tried** (C1a, CZ-Q14 (a) and CZ-Q15 (a)). Busy, the
    // writer was asked for `flushed_lsn()` alone (`TryRequestSyncNow`), and
    // the park below is also the durability park: under a roll it ends
    // durable, the roll's seal having flushed the ring and its sync
    // published it, with no second try; on any other holder it ends after
    // 1 ms and the flush is tried again. Core 0's drain, which flushes the
    // whole ring within `relaxed_flush_interval`, is the backstop the model
    // does not rest on.
    {
        // The bound is the run's at this site (CZ-Q15 (a), "one bound of 16"),
        // not this call's: a run parks on the log once per begin, deferred
        // step, end and publish write, so a per-call count would let one run
        // take 16 busy answers at each.
        CheckpointRunReport::LatchSite& site = Site(CheckpointLatchSite::kFlush);
        for (;;) {
            bool busy = false;
            if (site.busy < static_cast<std::uint64_t>(kLatchTries)) {
                if (Status s = wal_.TryRequestSyncNow(&busy); !s.ok()) co_return s;
            } else {
                ++site.bound;
                const sched::MonoTimeNs blocked = wal_.clock().Now();
                const Status s = wal_.RequestSyncNow();
                site.blocking_ns += wal_.clock().Now() - blocked;
                if (!s.ok()) co_return s;
            }
            if (!busy) break;
            ++site.busy;
            // The sixteenth busy answer is the last: the blocking entry next,
            // with no park before it (§8.8 item 6).
            if (site.busy >= static_cast<std::uint64_t>(kLatchTries)) continue;
            {
                // Counted into the writer's kick for the length of the park:
                // the writer's sync of `flushed_lsn()` may be what ends it.
                const WalManager::DurableWait waiting(wal_, core_id);
                if (Status s = co_await ParkOnBusy(CheckpointLatchSite::kFlush, lsn, generation); !s.ok()) {
                    co_return s;
                }
            }
            if (generation != task_generation_) co_return Status::OK();
            if (wal_.IsDurable(lsn)) co_return Status::OK();
        }
    }
    const std::function<bool()> durable = [this, lsn] {
        return wal_.IsDurable(lsn) || wal_.stopped();
    };
    if (!durable()) {
        const WalManager::DurableWait waiting(wal_, core_id);
        const sched::MonoTimeNs parked = wal_.clock().Now();
        {
            sched::WaitScope wait(stats::WaitEvent::kSysioWalWrite, lsn);
            co_await sched::WaitUntil{&durable};
        }
        task_report_.log_park_ns += wal_.clock().Now() - parked;
    }
    co_return wal_.IsDurable(lsn) ? Status::OK() : LogStopped();
}

Status Checkpointer::FinishTask(std::uint32_t core_id, const Status& ran) {
    EndRun(core_id, ran);
    // A timer-driven run has no caller to return a status to, so a failure
    // unlogged is a silently widening loss window. A synchronous caller logs
    // its own, with what the failure costs it.
    if (!ran.ok() && log_ != nullptr && log_->enabled(LogLevel::kError)) {
        log_->Error("checkpoint",
                    "core " + std::to_string(core_id) + ": checkpoint failed: " + ran.message());
    }
    task_hold_.reset();
    task_in_flight_ = false;
    return ran;
}

void Checkpointer::Abandon() noexcept {
    if (run_report_ == &task_report_) {
        // The run's census edge is closed as a run's end would close it, and
        // its record is not published: it did not finish.
        CheckpointCensus::RunEdge(task_core_);
        run_report_ = nullptr;
    }
    ++task_generation_;  // a frame that is ever polled again returns at once
    task_hold_.reset();
    task_in_flight_ = false;
    in_progress_ = false;
    pending_pages_.clear();
    next_page_ = 0;
}

void Checkpointer::LogSkip(std::uint32_t core_id) const {
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("checkpoint", "core " + std::to_string(core_id) +
                                      ": skipped: another core's checkpoint is running or due");
    }
}

void Checkpointer::BeginRun(std::uint32_t core_id, CheckpointRunReport& report) {
    report.start_ns = wal_.clock().Now();
    report.resumed = in_progress_;
    run_report_ = &report;
    CheckpointCensus::RunEdge(core_id);
}

void Checkpointer::EndRun(std::uint32_t core_id, const Status& ran) {
    CheckpointRunReport& r = *run_report_;
    run_report_ = nullptr;
    r.end_ns = wal_.clock().Now();
    r.completed = ran.ok();
    CheckpointCensus::Publish(core_id, r);
    CheckpointCensus::RunEdge(core_id);
    // `sys.checkpoints`' row (BO-S7): the report handed over, nothing else
    // written. The wall clock dates it; the injected clock timed it.
    stats::CheckpointLog::Record(
        core_id, r,
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    // BA-R0's checkpoint count and longest run, on the periodic path - fed
    // from this record, so a run has one timing source (BI-Q1 (a)).
    Contention::Add(Tally::kCheckpointRuns);
    Contention::Max(Longest::kCheckpointNs, r.wall_ns());
    LogRun(core_id, r);
}

void Checkpointer::LogRun(std::uint32_t core_id, const CheckpointRunReport& r) const {
    // One line per run at Info, which the server logs by default: what a
    // probe sample is classed against (BI-S1, §5.1). `start_ns`/`end_ns` are
    // the injected monotonic clock's, CLOCK_MONOTONIC on a real instance.
    if (log_ == nullptr || !log_->enabled(LogLevel::kInfo)) return;
    std::string line = "run: core=" + std::to_string(core_id) +
                       " completed=" + (r.completed ? "1" : "0") +
                       " resumed=" + (r.resumed ? "1" : "0") +
                       " start_ns=" + std::to_string(r.start_ns) +
                       " end_ns=" + std::to_string(r.end_ns) +
                       " wall_us=" + std::to_string(r.wall_ns() / 1000) +
                       " steps=" + std::to_string(r.steps) + " pages=" + std::to_string(r.pages) +
                       " dirty_at_start=" + std::to_string(r.dirty_at_start) +
                       " step_max_us=" + std::to_string(r.step_max_ns / 1000) +
                       " step_page_write_max_us=" + std::to_string(r.step_page_write_max_ns / 1000) +
                       " gate_parks=" + std::to_string(r.gate_parks) +
                       " end_parks=" + std::to_string(r.end_parks) +
                       " log_park_us=" + std::to_string(r.log_park_ns / 1000) +
                       " data_park_us=" + std::to_string(r.data_park_ns / 1000);
    // CZ-S3's sites (§8.7 of the CZ order), then the assertion visit and the
    // recycle.
    for (std::size_t i = 0; i < r.latch_sites.size(); ++i) {
        const CheckpointRunReport::LatchSite& site = r.latch_sites[i];
        const std::string at = std::string(" latch_") + kCheckpointLatchSiteNames[i];
        line += at + "_busy=" + std::to_string(site.busy) + at +
                "_roll_parks=" + std::to_string(site.roll_parks) + at +
                "_bound=" + std::to_string(site.bound) + at +
                "_park_us=" + std::to_string(site.park_ns / 1000) + at +
                "_blocking_us=" + std::to_string(site.blocking_ns / 1000);
    }
    line += " assertion_visit_us=" + std::to_string(r.assertion_visit_ns / 1000) +
            " recycle_skips=" + std::to_string(r.recycle_skips) +
            " recycle_blocked=" + std::to_string(r.recycle_blocked);
    for (std::size_t i = 0; i < kCheckpointPhaseCount; ++i) {
        line += std::string(" ") + kCheckpointPhaseNames[i] +
                "_n=" + std::to_string(r.count[i]) + " " + kCheckpointPhaseNames[i] +
                "_sum_us=" + std::to_string(r.sum_ns[i] / 1000) + " " +
                kCheckpointPhaseNames[i] + "_max_us=" + std::to_string(r.max_ns[i] / 1000);
    }
    log_->Info("checkpoint", line);
}

}  // namespace kds::wal
