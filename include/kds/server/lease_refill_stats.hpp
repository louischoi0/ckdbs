#pragma once

#include <cstdint>

// What one core's lease refills cost, per lease kind (extent, transaction
// id, row id) - the counters PW6's four-writer cell asked for
// (`bench/v2.0.0/results-multicore-writers-v2.0.0-48-g314a06d.md` §6a,
// §11): every refill there completed hundreds of milliseconds to seconds
// after a round trip that idle takes 2-7 ms, and nothing in the logs said
// which leg held the time. PW7 read the answer off these (docs/
// workplan-peer-writer.md §6): 546 of 547 ms sat in the first leg. Four
// stamps, three legs, each leg in nanoseconds *and* reactor iterations
// (`Scheduler::iterations()`) - a leg long in time and short in iterations
// is a blocked loop, long in both is a loop that ran and never reached
// the task or the inbox:
//
//   submit --(1)--> sent --(2)--> grant received --(3)--> resumed
//
// (1) is the scheduler queueing the request task; (2) the ring and core
// 0's handler; (3) this reactor reaching the parked coroutine. Stamped by
// CoreRuntime (submit, completion), the request coroutine (sent) and the
// receive handler (grant). Sched-free on purpose: the dispatcher prints
// these from `SHOW META`, and the dispatcher header must not drag the
// scheduler in (the PW1c-7 review's S3).

namespace kds::server {

struct LeaseRefillStats {
    std::uint64_t requests = 0;
    std::uint64_t grants = 0;

    // The in-flight request's stamps. `in_flight` is the sentinel, not a
    // zero stamp: a ManualClock starts at 0, and a store opened on one
    // would otherwise record nothing and print zeroes for it.
    bool in_flight = false;
    std::uint64_t requested_at_ns = 0;
    std::uint64_t sent_at_ns = 0;
    std::uint64_t granted_at_ns = 0;
    std::uint64_t requested_iter = 0;
    std::uint64_t sent_iter = 0;
    std::uint64_t granted_iter = 0;

    // Maxima over every completed request.
    std::uint64_t submit_lag_max_ns = 0;     // submit -> sent
    std::uint64_t wait_to_grant_max_ns = 0;  // sent -> grant received
    std::uint64_t resume_lag_max_ns = 0;     // grant received -> completed
    std::uint64_t wait_total_max_ns = 0;     // submit -> completed
    std::uint64_t submit_lag_max_iters = 0;
    std::uint64_t grant_lag_max_iters = 0;
    std::uint64_t resume_lag_max_iters = 0;

    void NoteSubmit(std::uint64_t now_ns, std::uint64_t now_iter) noexcept {
        in_flight = true;
        requested_at_ns = now_ns;
        requested_iter = now_iter;
        sent_at_ns = granted_at_ns = 0;
        sent_iter = granted_iter = 0;
    }
    void NoteSent(std::uint64_t now_ns, std::uint64_t now_iter) noexcept {
        ++requests;
        sent_at_ns = now_ns;
        sent_iter = now_iter;
    }
    // Stamped where a grant is *taken* - beside the release of the waiting
    // coroutine - never on a message's arrival: a malformed or stale
    // message that releases nothing must not end leg (2) early and charge
    // the rest of the wire wait to this reactor (the PW7 review's C1).
    void NoteGrant(std::uint64_t now_ns, std::uint64_t now_iter) noexcept {
        granted_at_ns = now_ns;
        granted_iter = now_iter;
    }

    // The completion callback's arithmetic, in one place: folds the
    // in-flight request's stamps into the maxima. A request that never
    // saw a grant (a failed send) records only what it can.
    void Complete(std::uint64_t now_ns, std::uint64_t now_iter) noexcept {
        if (!in_flight) return;
        in_flight = false;
        const std::uint64_t total = now_ns >= requested_at_ns ? now_ns - requested_at_ns : 0;
        if (total > wait_total_max_ns) wait_total_max_ns = total;
        const std::uint64_t sent = sent_at_ns >= requested_at_ns ? sent_at_ns : requested_at_ns;
        const std::uint64_t sent_it = sent_iter >= requested_iter ? sent_iter : requested_iter;
        if (sent - requested_at_ns > submit_lag_max_ns) submit_lag_max_ns = sent - requested_at_ns;
        if (sent_it - requested_iter > submit_lag_max_iters) {
            submit_lag_max_iters = sent_it - requested_iter;
        }
        if (granted_at_ns >= sent && now_ns >= granted_at_ns) {
            const std::uint64_t to_grant = granted_at_ns - sent;
            const std::uint64_t resume = now_ns - granted_at_ns;
            if (to_grant > wait_to_grant_max_ns) wait_to_grant_max_ns = to_grant;
            if (resume > resume_lag_max_ns) resume_lag_max_ns = resume;
            if (granted_iter >= sent_it && granted_iter - sent_it > grant_lag_max_iters) {
                grant_lag_max_iters = granted_iter - sent_it;
            }
            if (now_iter >= granted_iter && now_iter - granted_iter > resume_lag_max_iters) {
                resume_lag_max_iters = now_iter - granted_iter;
            }
        }
    }
};

}  // namespace kds::server
