#pragma once

#include <cstdint>

// What one core's lease refills cost, per lease kind (extent, transaction
// id, row id) - the counters PW6's four-writer cell asked for
// (`bench/v2.0.0/results-multicore-writers-v2.0.0-48-g314a06d.md` §6a,
// §11): every refill there completed hundreds of milliseconds to seconds
// after a round trip that idle takes 2-7 ms, and nothing in the logs said
// which leg held the time. Three legs, three maxima:
//
//   submit --(1)--> grant received --(2)--> coroutine resumed
//     \______________________(3)______________________/
//
// (1) is the ring and core 0's handler; (2) is this core's own reactor
// getting back to the parked coroutine; (3) is what a statement waits.
// Stamped with the scheduler's clock by the receive handler (grant) and by
// CoreRuntime's submit and completion callback. Sched-free on purpose:
// the dispatcher prints these from `SHOW META`, and the dispatcher header
// must not drag the scheduler in (the PW1c-7 review's S3).

namespace kds::server {

struct LeaseRefillStats {
    std::uint64_t requests = 0;
    std::uint64_t grants = 0;

    // The in-flight request's stamps, monotonic nanoseconds; 0 = none.
    // `sent_at` is the coroutine's first poll - the request actually
    // leaving - which separates the scheduler's queueing of the task from
    // the ring's round trip. Beside each, the reactor's iteration count
    // (`Scheduler::iterations()`): a leg that is long in time and short in
    // iterations is a *blocked* loop; long in both is a loop that ran and
    // did not reach the task or the inbox.
    std::uint64_t requested_at_ns = 0;
    std::uint64_t sent_at_ns = 0;
    std::uint64_t granted_at_ns = 0;
    std::uint64_t requested_iter = 0;
    std::uint64_t sent_iter = 0;
    std::uint64_t granted_iter = 0;

    // Maxima over every completed request, and the last one whole.
    std::uint64_t submit_lag_max_ns = 0;     // submit -> sent
    std::uint64_t wait_to_grant_max_ns = 0;  // sent -> grant received
    std::uint64_t resume_lag_max_ns = 0;     // grant received -> completed
    std::uint64_t wait_total_max_ns = 0;     // submit -> completed
    std::uint64_t wait_total_last_ns = 0;
    std::uint64_t submit_lag_max_iters = 0;
    std::uint64_t grant_lag_max_iters = 0;
    std::uint64_t resume_lag_max_iters = 0;

    // The completion callback's arithmetic, in one place: folds the
    // in-flight request's stamps into the maxima. A request that never
    // saw a grant (a failed send) records only what it can.
    void Complete(std::uint64_t now_ns, std::uint64_t now_iter) noexcept {
        if (requested_at_ns == 0) return;
        const std::uint64_t total = now_ns - requested_at_ns;
        wait_total_last_ns = total;
        if (total > wait_total_max_ns) wait_total_max_ns = total;
        const std::uint64_t sent = sent_at_ns >= requested_at_ns ? sent_at_ns : requested_at_ns;
        const std::uint64_t sent_it = sent_iter >= requested_iter ? sent_iter : requested_iter;
        if (sent - requested_at_ns > submit_lag_max_ns) submit_lag_max_ns = sent - requested_at_ns;
        if (sent_it - requested_iter > submit_lag_max_iters) {
            submit_lag_max_iters = sent_it - requested_iter;
        }
        if (granted_at_ns >= sent) {
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
        requested_at_ns = sent_at_ns = granted_at_ns = 0;
        requested_iter = sent_iter = granted_iter = 0;
    }
};

}  // namespace kds::server
