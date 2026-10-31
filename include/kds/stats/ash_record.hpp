#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// **The ASH sample record** (`instructions/v3.2.0/workorder-cj-wait-events-ash.md`
// §2.7, CJ's WE06; decisions WD7, WD8): 64 bytes, little-endian, written and
// read field by field at fixed offsets. No struct is laid over the bytes
// (`reinterpret_cast` is forbidden for a persisted-shaped format, invariant 6),
// and the ring is memory only, so no version word exists (WD8).
//
// `state`'s low two bits are the task state and bit 7 marks a background task;
// `flags` carries `kAshLate`, `kAshCrossCore` and `kAshArgHashValid`.

namespace kds::stats {

inline constexpr std::size_t kAshRecordBytes = 64;

// The offsets of §2.7's table, with the sum checked below.
namespace ash_off {
inline constexpr std::size_t kSampleTs = 0;
inline constexpr std::size_t kSessionId = 8;
inline constexpr std::size_t kCoreId = 12;
inline constexpr std::size_t kState = 14;
inline constexpr std::size_t kWaitClass = 15;
inline constexpr std::size_t kEventId = 16;
inline constexpr std::size_t kSchedGroup = 18;
inline constexpr std::size_t kFlags = 19;
inline constexpr std::size_t kFingerprint = 20;
inline constexpr std::size_t kStepId = 28;
inline constexpr std::size_t kOriginSession = 32;
inline constexpr std::size_t kTrxId = 36;
inline constexpr std::size_t kP1 = 44;
inline constexpr std::size_t kP2 = 52;
inline constexpr std::size_t kP3 = 56;
inline constexpr std::size_t kWaitElapsedUs = 60;
}  // namespace ash_off

static_assert(ash_off::kWaitElapsedUs + 4 == kAshRecordBytes, "the record is 64 bytes (WD7)");
static_assert(ash_off::kFingerprint == ash_off::kFlags + 1 && ash_off::kStepId == ash_off::kFingerprint + 8 &&
                  ash_off::kTrxId == ash_off::kOriginSession + 4 && ash_off::kP1 == ash_off::kTrxId + 8 &&
                  ash_off::kP2 == ash_off::kP1 + 8 && ash_off::kP3 == ash_off::kP2 + 4,
              "the offsets follow §2.7's table with no gap");

inline constexpr std::uint8_t kAshStateMask = 0x03;
inline constexpr std::uint8_t kAshBackground = 0x80;
inline constexpr std::uint8_t kAshLate = 0x01;
inline constexpr std::uint8_t kAshCrossCore = 0x02;
inline constexpr std::uint8_t kAshArgHashValid = 0x04;

struct AshRecord {
    std::uint64_t sample_ts_us = 0;
    std::uint32_t session_id = 0;
    std::uint16_t core_id = 0;
    std::uint8_t state = 0;  // `kAshStateMask` bits + `kAshBackground`
    std::uint8_t wait_class = 0;
    std::uint16_t event_id = 0;
    std::uint8_t sched_group = 0;
    std::uint8_t flags = 0;
    std::uint64_t fingerprint = 0;
    std::uint32_t step_id = 0;
    std::uint32_t origin_session = 0;
    std::uint64_t trx_id = 0;  // the low 48 bits are stored as 8 bytes, upper 16 zero
    std::uint64_t p1 = 0;
    std::uint32_t p2 = 0;
    std::uint32_t p3 = 0;
    std::uint32_t wait_elapsed_us = 0;

    bool operator==(const AshRecord&) const = default;
};

using AshBytes = std::array<std::uint8_t, kAshRecordBytes>;

namespace detail {

template <class T>
void PutLe(AshBytes& out, std::size_t off, T v) noexcept {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        out[off + i] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(v) >> (8 * i));
    }
}

template <class T>
T GetLe(const AshBytes& in, std::size_t off) noexcept {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) v |= static_cast<std::uint64_t>(in[off + i]) << (8 * i);
    return static_cast<T>(v);
}

}  // namespace detail

inline AshBytes EncodeAsh(const AshRecord& r) noexcept {
    AshBytes b{};
    detail::PutLe(b, ash_off::kSampleTs, r.sample_ts_us);
    detail::PutLe(b, ash_off::kSessionId, r.session_id);
    detail::PutLe(b, ash_off::kCoreId, r.core_id);
    detail::PutLe(b, ash_off::kState, r.state);
    detail::PutLe(b, ash_off::kWaitClass, r.wait_class);
    detail::PutLe(b, ash_off::kEventId, r.event_id);
    detail::PutLe(b, ash_off::kSchedGroup, r.sched_group);
    detail::PutLe(b, ash_off::kFlags, r.flags);
    detail::PutLe(b, ash_off::kFingerprint, r.fingerprint);
    detail::PutLe(b, ash_off::kStepId, r.step_id);
    detail::PutLe(b, ash_off::kOriginSession, r.origin_session);
    detail::PutLe(b, ash_off::kTrxId, r.trx_id & 0x0000FFFFFFFFFFFFull);
    detail::PutLe(b, ash_off::kP1, r.p1);
    detail::PutLe(b, ash_off::kP2, r.p2);
    detail::PutLe(b, ash_off::kP3, r.p3);
    detail::PutLe(b, ash_off::kWaitElapsedUs, r.wait_elapsed_us);
    return b;
}

inline AshRecord DecodeAsh(const AshBytes& b) noexcept {
    AshRecord r;
    r.sample_ts_us = detail::GetLe<std::uint64_t>(b, ash_off::kSampleTs);
    r.session_id = detail::GetLe<std::uint32_t>(b, ash_off::kSessionId);
    r.core_id = detail::GetLe<std::uint16_t>(b, ash_off::kCoreId);
    r.state = detail::GetLe<std::uint8_t>(b, ash_off::kState);
    r.wait_class = detail::GetLe<std::uint8_t>(b, ash_off::kWaitClass);
    r.event_id = detail::GetLe<std::uint16_t>(b, ash_off::kEventId);
    r.sched_group = detail::GetLe<std::uint8_t>(b, ash_off::kSchedGroup);
    r.flags = detail::GetLe<std::uint8_t>(b, ash_off::kFlags);
    r.fingerprint = detail::GetLe<std::uint64_t>(b, ash_off::kFingerprint);
    r.step_id = detail::GetLe<std::uint32_t>(b, ash_off::kStepId);
    r.origin_session = detail::GetLe<std::uint32_t>(b, ash_off::kOriginSession);
    r.trx_id = detail::GetLe<std::uint64_t>(b, ash_off::kTrxId) & 0x0000FFFFFFFFFFFFull;
    r.p1 = detail::GetLe<std::uint64_t>(b, ash_off::kP1);
    r.p2 = detail::GetLe<std::uint32_t>(b, ash_off::kP2);
    r.p3 = detail::GetLe<std::uint32_t>(b, ash_off::kP3);
    r.wait_elapsed_us = detail::GetLe<std::uint32_t>(b, ash_off::kWaitElapsedUs);
    return r;
}

}  // namespace kds::stats
