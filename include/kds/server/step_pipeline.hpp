#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "kds/base/status.hpp"
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
    // The u64 leads so the struct packs to 16 bytes with no padding -
    // invariant 6's spirit for a wire form, even an in-process one.
    std::uint64_t request_id = 0;
    std::uint32_t session_core = 0;
    std::uint32_t step_id = 0;

    friend bool operator==(const PipelineTag&, const PipelineTag&) = default;
};
static_assert(sizeof(PipelineTag) == 16);

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
// the ring's max message payload minus the header; a single row larger
// than the target still ships alone - the spec's rule, so a wide row is
// slow and never stuck.
inline constexpr std::size_t kStepBatchTargetBytes = 32 * 1024;

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
