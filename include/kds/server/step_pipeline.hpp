#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <span>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/sched/ring_message.hpp"
#include "kds/wire/row_codec.hpp"

// The cross-core step pipeline's data plane (docs/spec/crosscore.md §3-§4,
// workplan P4a): the tag every pipeline message carries, the payload codecs
// for BATCH / EOF / CREDIT / CANCEL / ERROR, per-edge credit accounting,
// and the batch builder over the KWP row encoder.
//
// Deliberately **pure**: no scheduler, no transport, no executor. This
// layer is to the pipeline what `wire/row_codec.hpp` was to it - the seam
// built before either consumer, which is the only way "one encoder, two
// consumers" survives whichever side is built first. The STEP_OPEN step
// descriptor is P4a's second half and is not here yet; nothing below
// depends on its shape.
//
// Wire forms are POD under ring_message.hpp's exception to the on-disk
// layout rules: they never leave the process. Every payload begins with
// the tag, so teardown-by-tag (§3's rule: a message whose tag matches no
// live pipeline state is discarded silently, and that is correctness, not
// an error) can be applied before the kind-specific fields are read.

namespace kds::server {

// ---- The tag ------------------------------------------------------------

// `(session_core, request_id, step_id)`. `request_id` is allocated per
// statement by the session core, sequential per core - never
// pointer-derived (sched.md §7's determinism rule).
struct PipelineTag {
    // The u64 leads so the struct packs with no padding - invariant 6's
    // spirit for a wire form, even an in-process one.
    std::uint64_t request_id = 0;
    std::uint32_t session_core = 0;
    std::uint32_t step_id = 0;

    friend bool operator==(const PipelineTag&, const PipelineTag&) = default;
};
static_assert(sizeof(PipelineTag) == 16);

// **RD7 grew this struct by a `sibling` field and the field was removed
// again at the row's review**, which is worth a sentence because §5 of
// `workplan-range-directory.md` still lists it as the fan-in's first cost.
//
// §5's argument was sound on its premise: siblings of one fan-in must be
// told apart, *a reply must be matched and not trusted* (SS1), and
// overloading `step_id` would have changed what a Waystone trail is keyed
// on. What changed is the premise. §5 assumed one `request_id` for the
// whole statement, so siblings could differ only inside the tag; the
// dispatcher mints `next_remote_request_++` **per stage**, so they differ
// already and every exact-tag site tells them apart with no new field.
//
// A field that is never written is worse than absent: it reads as the
// discriminator while something else discriminates, so the next planner
// sets it and believes it matters. Whichever future shape mints k stages
// under one request_id is the one that needs this back - and it will need
// to add it deliberately rather than find it already there and unwired.

// ---- The send seam ------------------------------------------------------

// `send(dst_core, kind, payload)` must deliver or report; it never blocks
// (the real one submits a send-retry task). Here rather than on either
// endpoint because **both** the server and the session client take one,
// and two identical declarations of one type is how they come to differ.
using StepSendFn =
    std::function<Status(std::uint32_t, sched::RingMessageKind, std::vector<std::byte>)>;

// "This sender writes into no ring, so no slot bounds it." True of every
// in-process fixture and of no production wiring.
//
// Deliberately not `0`: a sentinel that looks like an omission is the
// "told wrong" hazard the slot exists to remove, and `0` is also a
// plausible real size. Deliberately not the batch target either - that
// default silently turned a fixture's tiny target into a ceiling nothing
// could fit under, and fifteen tests said so.
inline constexpr std::size_t kNoRingSlot = std::numeric_limits<std::size_t>::max();

// **The send seam and the slot it sends through, as one value.**
//
// They are one fact and were two parameters, which is how they came to
// disagree: `kStepBatchTargetBytes` was 32x the ring slot for the
// pipeline's whole life, and a cross-core read of 42 rows answered zero
// rows with no error (`docs/inflight/known-gaps.md`, beside the
// shipped-reply cap). A sender and a ceiling taken from different places
// can drift again; taken from one object they cannot.
struct StepSendSeam {
    StepSendFn send;
    std::size_t max_message_bytes = kNoRingSlot;
};

// ---- Payloads -----------------------------------------------------------

// STEP_BATCH: this header, immediately followed by `row_count` rows in the
// KWP D5 encoding (wire/row_codec.hpp) - the same bytes RowBatchWriter
// produces, so there is exactly one row format in the engine. `seq` is
// per-edge and starts at 0; a receiver that sees a gap has lost a batch,
// which the ring's FIFO makes impossible per edge - asserted, not handled.
struct StepBatchHeader {
    PipelineTag tag{};
    std::uint32_t seq = 0;
    std::uint32_t row_count = 0;
};
static_assert(sizeof(StepBatchHeader) == 24);

// STEP_EOF and STEP_CANCEL carry the tag alone.
struct StepEofPayload {
    PipelineTag tag{};
};

// STEP_CREDIT: grants `credits` more batches to the upstream step.
struct StepCreditPayload {
    PipelineTag tag{};
    std::uint32_t credits = 0;
};

// STEP_ERROR: the failing core's status, retryable flag explicit because
// the receiver maps it onto the wire's one-bit surface (protocol D9).
struct StepErrorPayload {
    PipelineTag tag{};
    std::uint32_t status_code = 0;
    std::uint8_t retryable = 0;
};

// One encode/decode pair per payload, memcpy-based like every ring
// payload. Decode answers InvalidArgument on a size mismatch rather than
// reading garbage - the malformed-message rule every receiver applies.
template <typename Payload>
inline void EncodePipelinePayload(const Payload& payload, std::vector<std::byte>& out) {
    out.resize(sizeof(Payload));
    std::memcpy(out.data(), &payload, sizeof(Payload));
}

template <typename Payload>
inline StatusOr<Payload> DecodePipelinePayload(std::span<const std::byte> bytes) {
    if (bytes.size() < sizeof(Payload)) {
        return Status::InvalidArgument("pipeline payload is " + std::to_string(bytes.size()) +
                                        " bytes; " + std::to_string(sizeof(Payload)) +
                                        " needed");
    }
    Payload payload{};
    std::memcpy(&payload, bytes.data(), sizeof(Payload));
    return payload;
}

// ---- Credit accounting (§4) ---------------------------------------------

// One edge's flow control, held by the **upstream** side: it may send a
// batch only while it holds a credit, and the downstream grants more as it
// drains. Credits bound per-request buffering; the ring's own backpressure
// protects the transport - two mechanisms, deliberately not one.
//
// `kInitialCreditsPerEdge` is `[PROPOSED]` 4 and preallocated at
// STEP_OPEN; nothing may depend on the number.
inline constexpr std::uint32_t kInitialCreditsPerEdge = 4;

class EdgeCredit {
public:
    explicit EdgeCredit(std::uint32_t initial = kInitialCreditsPerEdge) noexcept
        : available_(initial), ceiling_(initial) {}

    bool can_send() const noexcept { return available_ > 0; }

    // Called by the upstream when it sends a batch. Refuses at zero rather
    // than going negative: sending without a credit is the defect this
    // class exists to make impossible, so it is an error, not a clamp.
    Status ConsumeOnSend() noexcept {
        if (available_ == 0) {
            return Status::ResourceExhausted("no batch credit on this edge; wait for a grant");
        }
        --available_;
        return Status::OK();
    }

    // Called by the upstream when a STEP_CREDIT arrives. Grants above the
    // preallocated ceiling are refused: the downstream preallocated
    // `ceiling` batches of memory at STEP_OPEN, and a grant beyond it
    // promises buffer space that does not exist.
    Status Grant(std::uint32_t credits) noexcept {
        if (available_ + credits > ceiling_) {
            return Status::InvalidArgument("credit grant exceeds the edge's preallocated " +
                                            std::to_string(ceiling_));
        }
        available_ += credits;
        return Status::OK();
    }

    std::uint32_t available() const noexcept { return available_; }

private:
    std::uint32_t available_;
    std::uint32_t ceiling_;
};

// ---- The batch builder (§4) ---------------------------------------------

// Accumulates KWP-encoded rows toward the batch-size target and hands back
// full chunks. The target is `[PROPOSED]` 32 KiB and must stay at or below
// the ring's max message payload minus the header.
//
// **`crosscore.md` §4's "a single row larger than the target still ships
// alone - so a wide row is slow and never stuck" is retracted here as
// written**, and the retraction is a fact about the transport rather than
// a policy change. A row wider than `StepBatchCeiling` cannot be carried
// by *any* batching, because a batch is one ring message; before
// 2026-08-27 such a row was dropped silently, and now it is refused by
// name. Reachable at defaults - roughly fifteen full-width varchar
// columns at `inline_cell_width = 64`, and one value alone at its 4,096
// ceiling. Closing it needs either a batch fragmented across ring
// messages or `crosscore.md` §9's ring sizing, and both are that spec's;
// `docs/inflight/known-gaps.md` carries the gap.
//
// **That "must" was a sentence and nothing else for the whole life of the
// pipeline**, and 32 KiB is 32x the production ring slot
// (`sched::kCoreRingPayloadBytes`, 1,024), so every batch past 1,024 bytes
// was refused by `TrySend` and dropped by a send nobody was watching: a
// cross-core read of 42 rows answered zero rows, silently
// (docs/inflight/bugs/step-batch-wider-than-ring-slot-vanishes.md). The
// target is now a *target*, and `StepBatchCeiling` below is the bound.
inline constexpr std::size_t kStepBatchTargetBytes = 32 * 1024;

// The largest STEP_BATCH payload a transport whose slot carries
// `max_message_bytes` can accept: the slot, minus this layer's own header.
//
// Derived rather than configured, and asked of the transport rather than
// declared beside it, because the two numbers drifting apart is the defect
// above. Answers 0 for a slot too small to hold even the header, which no
// caller can satisfy and every caller must therefore refuse rather than
// truncate.
constexpr std::size_t StepBatchCeiling(std::size_t max_message_bytes) noexcept {
    return max_message_bytes > sizeof(StepBatchHeader)
               ? max_message_bytes - sizeof(StepBatchHeader)
               : 0;
}

class StepBatchBuilder {
public:
    explicit StepBatchBuilder(PipelineTag tag,
                              std::size_t target_bytes = kStepBatchTargetBytes) noexcept
        : tag_(tag), target_(target_bytes) {}

    // Appends one encoded row (the writer's per-row bytes). Returns true
    // when the batch crossed the target and should be taken with Take().
    bool Append(std::span<const std::byte> row_bytes) {
        rows_.insert(rows_.end(), row_bytes.begin(), row_bytes.end());
        ++row_count_;
        return rows_.size() >= target_;
    }

    bool empty() const noexcept { return row_count_ == 0; }
    std::uint32_t row_count() const noexcept { return row_count_; }

    // The finished STEP_BATCH payload - header then rows - and the builder
    // resets for the next chunk. `seq` advances per take, per §3.
    std::vector<std::byte> Take() {
        StepBatchHeader header{};
        header.tag = tag_;
        header.seq = seq_++;
        header.row_count = row_count_;

        std::vector<std::byte> payload(sizeof(header) + rows_.size());
        std::memcpy(payload.data(), &header, sizeof(header));
        std::memcpy(payload.data() + sizeof(header), rows_.data(), rows_.size());

        rows_.clear();
        row_count_ = 0;
        return payload;
    }

private:
    PipelineTag tag_;
    std::size_t target_;
    std::uint32_t seq_ = 0;
    std::uint32_t row_count_ = 0;
    std::vector<std::byte> rows_;
};

// ---- XG1: SHIPPED_ROW_DESC, the answer edge's description ---------------
//
// A shipped read answered in typed rows needs its **description** before
// its rows, and only the owner has one - it compiled the statement, and a
// projected read's field list is derivable from no relation's schema on
// the arrival core.
//
// **Chunked, because the engine has no column-count cap.** A description
// can exceed one ring message, so it crosses as an ordered sequence
// reassembled before the receiver is armed. That is what answers the bound
// XG-R3 was asked for: no ceiling is named and no constant is added.
//
// **Its own kind and its own sequence**, never a `kStepBatch` with a flag:
// `StepBatchHeader::seq` is per-edge and asserted contiguous, so a
// differently-shaped payload folded into it would break the assertion or
// force chunks to be counted as batches.
//
// The bytes it carries are `wire::EncodeRowDescription`'s - the same
// encoding an `S_ROW_DESC` frame holds - so a description has one encoder
// in the engine exactly as a row does.
struct ShippedRowDescHeader {
    PipelineTag tag{};
    // This chunk's index, from 0, and how many there are. `chunks` rides
    // every chunk rather than only the first: a receiver that lost the
    // first would otherwise not know it was assembling a description at
    // all, and the ring's per-edge FIFO makes the two cheap to keep
    // consistent.
    std::uint32_t seq = 0;
    std::uint32_t chunks = 0;
};
static_assert(sizeof(ShippedRowDescHeader) == 24);

// The largest description chunk a transport whose slot carries
// `max_message_bytes` can hold. `StepBatchCeiling`'s shape and its reason:
// derived from the transport rather than declared beside it, because two
// numbers that must agree and are written in two places are two numbers
// that will disagree.
constexpr std::size_t ShippedRowDescCeiling(std::size_t max_message_bytes) noexcept {
    return max_message_bytes > sizeof(ShippedRowDescHeader)
               ? max_message_bytes - sizeof(ShippedRowDescHeader)
               : 0;
}

// Splits a SHIPPED_ROW_DESC payload into its header and its slice of the
// encoded description.
inline StatusOr<ShippedRowDescHeader> DecodeShippedRowDescHeader(
    std::span<const std::byte> payload, std::span<const std::byte>& bytes_out) {
    auto header = DecodePipelinePayload<ShippedRowDescHeader>(payload);
    if (!header.ok()) return header.status();
    bytes_out = payload.subspan(sizeof(ShippedRowDescHeader));
    return header;
}

// Splits a STEP_BATCH payload back into its header and the row bytes the
// KWP decoder takes. The row *contents* are DecodeRowBatch's business
// (wire/row_codec.hpp) - one decoder, like one encoder.
inline StatusOr<StepBatchHeader> DecodeStepBatchHeader(std::span<const std::byte> payload,
                                                       std::span<const std::byte>& rows_out) {
    auto header = DecodePipelinePayload<StepBatchHeader>(payload);
    if (!header.ok()) return header.status();
    rows_out = payload.subspan(sizeof(StepBatchHeader));
    return header;
}

}  // namespace kds::server
