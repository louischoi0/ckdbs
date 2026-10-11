#include "kds/wal/checkpointer.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "kds/base/contention.hpp"
#include "kds/wal/analysis.hpp"

namespace kds::wal {

Checkpointer::Checkpointer(WalManager& wal, CheckpointTarget& target, ActiveTransactions& txns,
                           CheckpointAnchor& anchor, CheckpointerConfig config)
    : wal_(wal), target_(target), txns_(txns), anchor_(anchor), config_(config) {}

Status Checkpointer::LogBegin(std::span<const CheckpointActiveTxn> active_txns,
                              std::span<const CheckpointDirtyPage> dirty_pages) {
    payload_scratch_.assign(CheckpointBeginSize(active_txns.size(), dirty_pages.size()),
                            std::byte{0});
    auto encoded = EncodeCheckpointBegin(payload_scratch_, active_txns, dirty_pages);
    if (!encoded.ok()) {
        return encoded.status();
    }

    // The record says whose checkpoint it is (payload.hpp). The cast is
    // safe because `WalManager::Open`/`Attach` refuse a core id the byte
    // could not hold, so a manager that exists has one that fits.
    auto lsn = wal_.Append({RecordType::kCheckpointBegin, kNoTxnId, kInvalidPageId,
                            static_cast<std::uint8_t>(wal_.core_id())},
                           std::span(payload_scratch_).first(encoded.value()));
    if (!lsn.ok()) {
        return lsn.status();
    }
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
    // chunk naming its place in it (payload.hpp, AY-R7).
    auto cut = CutRun(cabin, wal.usable_payload_bytes());
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
    return Status::OK();
}

Status Checkpointer::LogAssertionSnapshots() {
    if (assertions_ == nullptr) {
        return Status::OK();  // no assertions on this core: no records, no cost
    }
    return assertions_->VisitSnapshots(
        wal_.usable_payload_bytes(),
        [this](const std::vector<AssertionCabinSnapshot>& cabins) -> Status {
            for (const AssertionCabinSnapshot& cabin : cabins) {
                if (Status s = LogAssertionSnapshot(wal_, cabin); !s.ok()) {
                    return s;
                }
            }
            return Status::OK();
        });
}

Status Checkpointer::Start() {
    if (in_progress_) {
        return Status::AlreadyExists("checkpointer: a checkpoint is already in progress");
    }
    const CheckpointPhaseScope scope(&wal_.clock(), run_report_);
    const PhaseTimer timed(CheckpointPhase::kStart);

    // Both tables are snapshotted before the record is written, so what
    // BEGIN carries is exactly what recovery's analysis phase will see.
    const std::vector<CheckpointActiveTxn> active_txns = txns_.Snapshot();
    const std::vector<CheckpointDirtyPage> dirty = target_.DirtyTable();

    if (Status s = LogBegin(active_txns, dirty); !s.ok()) {
        // BI-S1 counts it: the BEGIN ceiling refuses here, at the append.
        // Only a gated run's, like every other census field.
        if (run_report_ != nullptr) {
            CheckpointCensus::Of(wal_.core_id()).start_refused.fetch_add(1, std::memory_order_relaxed);
        }
        return s;
    }

    // AS6a's base, immediately after BEGIN: ahead of every ASSERT_* record that
    // will be folded onto it, and inside the checkpoint the anchor names, which
    // is what lets replay find it without scanning to the cabin's birth.
    if (Status s = LogAssertionSnapshots(); !s.ok()) {
        return s;
    }

    // Redo start: the oldest LSN any page in the snapshot still needs
    // replayed. Zero recLSNs are pages that are dirty but that no record
    // describes (created, never logged) - "nothing to replay", not "replay
    // from the head of the log", so they are skipped rather than min()ed
    // in. With no logged page in the snapshot, nothing before this
    // checkpoint matters and the redo start is the checkpoint itself.
    //
    // Pages dirtied *after* this point need no special handling: their
    // recLSNs are necessarily above begin_lsn_, which is itself an upper
    // bound on what this computes.
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

StatusOr<bool> Checkpointer::Step(Lsn* defer) {
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
        flushed = defer != nullptr ? target_.FlushPagesOrDefer(pages, defer)
                                   : target_.FlushPages(pages);
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

StatusOr<Lsn> Checkpointer::AppendEnd() {
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
    return wal_.Append({RecordType::kCheckpointEnd, kNoTxnId, kInvalidPageId,
                        static_cast<std::uint8_t>(wal_.core_id())},
                       std::span(payload_scratch_).first(encoded.value()));
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

Status Checkpointer::PublishAnchor() {
    const CheckpointPhaseScope scope(&wal_.clock(), run_report_);
    const CheckpointAnchorRecord anchor{wal_.core_id(),
                                        begin_lsn_,
                                        pending_redo_start_,
                                        wal_.durable_lsn(),
                                        pending_redo_start_ / wal_.segment_size()};
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
    return Status::OK();
}

Status Checkpointer::RunToCompletion() {
    // **A checkpoint a failed step or completion left running is resumed, not
    // refused** (BC-S4's review). `Start` refuses a nested one, so calling it
    // unconditionally wedged the checkpointer for good after one transient
    // writeback error: no later run completed, the anchor stopped moving, and
    // nothing was recycled again. The resumed run keeps its snapshot, whose
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
    if (!in_progress_) {
        if (Status s = Start(); !s.ok()) co_return FinishTask(core_id, s);
        co_await sched::Yield{};
        if (generation != task_generation_) co_return Status::OK();
    }
    // **The log's waits are parks, not syncs on this thread** (BI-R2): a
    // step that needs the log further hands back the LSN instead of waiting
    // (BI-Q4 (a)); the task asks the writer for the sync and parks, as a
    // commit does, counted into the writer's kick so the wake is the
    // writer's rather than the next drain tick's (BI-Q6, revised). The
    // reactor polls everything else meanwhile. The step then runs again;
    // after `kGateParks` parks it waits inline, the counted remainder.
    int parks = 0;
    while (true) {
        Lsn deferred = kNoLsn;
        auto done = Step(parks < kGateParks ? &deferred : nullptr);
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
            if (Status s = co_await ParkOnLog(deferred, core_id); !s.ok()) {
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

    // `CHECKPOINT_END`, then the same park for its LSN, then the publish.
    auto end_lsn = AppendEnd();
    if (!end_lsn.ok()) co_return FinishTask(core_id, end_lsn.status());
    if (!wal_.IsDurable(end_lsn.value())) ++task_report_.end_parks;
    if (Status s = co_await ParkOnLog(end_lsn.value(), core_id); !s.ok()) {
        co_return FinishTask(core_id, s);
    }
    if (generation != task_generation_) co_return Status::OK();
    co_return FinishTask(core_id, PublishAnchor());
}

namespace {

// As `WriteBack` answers it: nothing is written after a log fail-stop.
Status LogStopped() {
    return Status::IoError("checkpointer: the log fail-stopped; no page is written back and no "
                           "anchor published until a restart");
}

}  // namespace

sched::Coro Checkpointer::ParkOnLog(Lsn lsn, std::uint32_t core_id) {
    if (wal_.IsDurable(lsn)) co_return Status::OK();
    if (wal_.stopped()) co_return LogStopped();
    if (Status s = wal_.RequestSyncNow(); !s.ok()) co_return s;
    const std::function<bool()> durable = [this, lsn] {
        return wal_.IsDurable(lsn) || wal_.stopped();
    };
    if (!durable()) {
        const WalManager::DurableWait waiting(wal_, core_id);
        co_await sched::WaitUntil{&durable};
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
    // BA-R0's checkpoint count and longest run, on the periodic path - fed
    // from this record, so a run has one timing source (BI-Q1 (a)).
    Contention::Add(Tally::kCheckpointRuns);
    Contention::Max(Longest::kCheckpointNs, r.end_ns - r.start_ns);
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
                       " wall_us=" + std::to_string((r.end_ns - r.start_ns) / 1000) +
                       " steps=" + std::to_string(r.steps) + " pages=" + std::to_string(r.pages) +
                       " dirty_at_start=" + std::to_string(r.dirty_at_start) +
                       " step_max_us=" + std::to_string(r.step_max_ns / 1000) +
                       " step_page_write_max_us=" + std::to_string(r.step_page_write_max_ns / 1000) +
                       " gate_parks=" + std::to_string(r.gate_parks) +
                       " end_parks=" + std::to_string(r.end_parks);
    for (std::size_t i = 0; i < kCheckpointPhaseCount; ++i) {
        line += std::string(" ") + kCheckpointPhaseNames[i] +
                "_n=" + std::to_string(r.count[i]) + " " + kCheckpointPhaseNames[i] +
                "_sum_us=" + std::to_string(r.sum_ns[i] / 1000) + " " +
                kCheckpointPhaseNames[i] + "_max_us=" + std::to_string(r.max_ns[i] / 1000);
    }
    log_->Info("checkpoint", line);
}

}  // namespace kds::wal
