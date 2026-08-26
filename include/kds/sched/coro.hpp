#pragma once

#include <coroutine>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>

#include "kds/base/status.hpp"
#include "kds/sched/task.hpp"

// C++20 stackless coroutines as the engine's task representation
// (`docs/spec/sched.md` §3, settled 2026-08-05).
//
// ---- The decision, and what it costs ------------------------------------
//
// sched.md §3 left task representation open between callback/future chains,
// stackless coroutines and stackful fibers, and `docs/rules/rules.md` §7 listed
// coroutines under the undecided language-feature whitelist. **Coroutines.**
//
// The thing that forced it: every cross-core operation is a request whose
// answer arrives later, and the engine had no way to express "wait" that was
// not either blocking the reactor or rewriting a call chain into a state
// machine by hand. `docs/inflight/in-progress/workplan-crosscore.md` P4's step pipeline is exactly
// that shape - send `STEP_OPEN`, then await batches - and it could not be
// written at all. Callbacks would work and would invert every one of the 22
// allocation sites and the whole executor; fibers would give the most natural
// code and cost a stack per in-flight statement, which an engine sizing
// itself in pages cannot price.
//
// What coroutines cost, stated plainly so nobody is surprised later:
//
//   - **A frame is a heap allocation.** One per coroutine, at first call.
//     That is real, and it is why this is for *suspendable* work - a
//     statement, a pipeline step, a lease request - and never for the
//     per-tuple path. sched.md §2's "no allocation in steady state" applies
//     to the reactor loop, which this does not touch.
//   - **A coroutine is not a task until it is submitted**, and the frame
//     lives until the handle is destroyed. `CoroTask` owns it, so a dropped
//     task destroys a suspended coroutine's frame rather than leaking it.
//   - **They compose badly with `Status`-returning code** unless the
//     coroutine returns one itself, which is why `Coro` carries a Status.
//
// ---- Why this needed no change to Scheduler -----------------------------
//
// None. `Task::Poll()` returning `kSuspended`/`kDone`, with the scheduler
// re-enqueueing on the former, is already precisely a coroutine's resume
// protocol - task.hpp predicted this in as many words ("a future coroutine-
// or fiber-backed Task subclass can be added later without any change to
// Scheduler"). `CoroTask::Poll()` resumes the handle and reports whether it
// finished.
//
// ---- Readiness, and the one thing to be careful about -------------------
//
// A suspended task is re-enqueued and polled again next iteration. For a
// coroutine waiting on something that has not happened, that is a **poll
// loop**: correct, bounded by the loop budget, and wasteful in proportion to
// how long the wait is. `WaitFor` below therefore checks a flag before
// resuming, so the coroutine body is entered only when its condition holds -
// the spin costs a predicate call per iteration, not a resume.
//
// An event-driven version (the resumer re-submits the task) is strictly
// better and is deliberately not built yet: it needs the waiter to be
// reachable from the resumer, which is a lifetime question worth answering
// against a real pipeline rather than in the abstract.

namespace kds::sched {

// A coroutine that runs on a reactor and finishes with a Status.
//
// Lazily started: `initial_suspend` suspends, so calling a `Coro` function
// creates the frame and runs nothing. The first `Poll()` runs it to its
// first suspension point. That is what makes submitting a coroutine and
// running it two separate acts, exactly as it is for any other task.
class Coro {
public:
    struct promise_type {
        Status result = Status::OK();

        // What this coroutine is waiting for, set by `WaitFor` as it
        // suspends and cleared by whoever resumes it.
        //
        // It lives on the promise rather than in the awaitable because the
        // *resumer* is the one that has to consult it: an awaitable's
        // `await_ready` runs once, at the `co_await`, so a condition that
        // was false then is never re-examined - the coroutine would suspend
        // once and resume on the next poll regardless. `CoroTask::Poll()`
        // reads this instead, which is what makes a wait actually wait.
        const bool* wait_flag = nullptr;

        // The predicate form of the same thing (see WaitUntil below): a
        // condition that is not a bool somebody sets, but a question worth
        // re-asking - "is my commit durable yet". Pointed at rather than
        // owned for the reason `wait_flag` is: the promise is polled from
        // outside the coroutine, and the object it points at lives in the
        // coroutine frame, which outlives every poll that can see it.
        const std::function<bool()>* wait_pred = nullptr;

        // The child this coroutine is co_awaiting, when it is (P4d-1). Set
        // by AwaitCoro as the parent suspends, cleared by await_resume when
        // the child's result is taken. It lives on the promise for
        // wait_flag's reason: the *poller* consults it - CoroTask::Poll
        // walks this chain to find the deepest pending coroutine, which is
        // the one a resume must enter. Non-owning: the awaiter object in
        // the parent's frame owns the child frame, and outlives every poll
        // that can see this handle by construction.
        std::coroutine_handle<promise_type> active_child{};

        Coro get_return_object() {
            return Coro(std::coroutine_handle<promise_type>::from_promise(*this));
        }

        // Lazy start (see above).
        std::suspend_always initial_suspend() noexcept { return {}; }

        // **Suspends at the end rather than self-destructing**, so the
        // handle stays valid for `done()` and for reading `result` after
        // the body has run. CoroTask destroys the frame; a coroutine that
        // destroyed itself here would leave that owner with a dangling
        // handle.
        std::suspend_always final_suspend() noexcept { return {}; }

        void return_value(Status s) { result = std::move(s); }

        // rules.md forbids exceptions in engine logic, so nothing should
        // ever reach this. It cannot be omitted - the language requires the
        // member - and it deliberately does nothing rather than calling
        // std::terminate: a build that does enable exceptions should fail
        // at the throw site, not here.
        void unhandled_exception() {}
    };

    Coro() noexcept = default;
    explicit Coro(std::coroutine_handle<promise_type> handle) noexcept : handle_(handle) {}

    Coro(const Coro&) = delete;
    Coro& operator=(const Coro&) = delete;
    Coro(Coro&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    Coro& operator=(Coro&& other) noexcept {
        if (this != &other) {
            Destroy();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }
    ~Coro() { Destroy(); }

    bool valid() const noexcept { return handle_ != nullptr; }
    bool done() const noexcept { return handle_ == nullptr || handle_.done(); }

    // The status the body returned. Meaningful only once `done()`.
    const Status& result() const noexcept { return handle_.promise().result; }

    // Runs to the next suspension point.
    void Resume() {
        if (handle_ != nullptr && !handle_.done()) handle_.resume();
    }

    // The deepest pending coroutine in a chain - the one a resume must
    // enter. A parent suspended on `co_await child` makes no progress until
    // the child finishes, and resuming it early re-enters the await (the
    // bug the first conversion shipped for one build); a finished child
    // stops the walk at its parent, whose resume runs await_resume and
    // takes the result. One walk, shared by CoroTask::Poll and
    // TryResumeDeepest, so the two cannot drift.
    static std::coroutine_handle<promise_type> DeepestPending(
        std::coroutine_handle<promise_type> from) noexcept {
        auto active = from;
        while (active.promise().active_child != nullptr &&
               !active.promise().active_child.done()) {
            active = active.promise().active_child;
        }
        return active;
    }

    // The wait check, shared by CoroTask::Poll and TryResumeDeepest for
    // DeepestPending's reason - a copy of six lines is a place for the
    // two to drift apart. True when the coroutine may be entered; a
    // satisfied wait is consumed on the way (each cleared as it passes,
    // so a later re-park never reads a stale one), an unsatisfied one
    // is left in place and answers false.
    static bool ConsumeWaitIfSatisfied(std::coroutine_handle<promise_type> h) {
        auto& p = h.promise();
        if (p.wait_flag != nullptr && !*p.wait_flag) return false;
        p.wait_flag = nullptr;
        if (p.wait_pred != nullptr && !(*p.wait_pred)()) return false;
        p.wait_pred = nullptr;
        return true;
    }

    // Resumes the deepest pending coroutine - unless it is parked on a
    // wait that does not currently hold, in which case nothing is resumed
    // and `false` comes back. The synchronous boundary drivers (P4d-2's
    // RunToCompletionAtWalkBoundary) run on this instead of a bare resume:
    // a synchronous caller cannot deliver what a wait waits for, and
    // resuming past an unsatisfied `WaitFor` acts as though the reply had
    // arrived (the 5ec61da review finding).
    bool TryResumeDeepest() {
        if (handle_ == nullptr || handle_.done()) return true;
        auto active = DeepestPending(handle_);
        if (!ConsumeWaitIfSatisfied(active)) return false;
        active.resume();
        return true;
    }

    // Hands the frame to a new owner - CoroTask, in every intended use.
    std::coroutine_handle<promise_type> Release() noexcept {
        return std::exchange(handle_, {});
    }

    // The handle, still owned here - AwaitCoro links it into the parent's
    // promise while keeping ownership in the awaiter.
    std::coroutine_handle<promise_type> Handle() const noexcept { return handle_; }

private:
    void Destroy() noexcept {
        if (handle_ != nullptr) {
            handle_.destroy();
            handle_ = {};
        }
    }

    std::coroutine_handle<promise_type> handle_{};
};

// ---- Awaiting a child Coro (P4d-1) ---------------------------------------
//
// `co_await SomeCoroFn(...)` inside a Coro: the enabling primitive for the
// executor conversion, whose call graph is mutually recursive - a parent
// step cannot call a child step's coroutine without a way to wait for it.
//
// Poll-driven like everything here: suspending records the child on the
// parent's promise, and CoroTask::Poll resumes the *deepest* pending
// coroutine in that chain - so a grandchild parked on WaitFor costs the
// same one predicate check its flat equivalent would, and nothing resumes
// a parent whose child still runs. There is deliberately no symmetric
// transfer: the scheduler's unit is one Poll, and keeping every resume
// inside Poll is what keeps the suspend audit's placement (one call site)
// true.
//
// The awaiter owns the child frame (the Coro moves in) and lives in the
// parent's frame for exactly the span of the co_await expression - which is
// the lifetime argument in one sentence: the child cannot outlive the
// await that reads its result, and destroying the parent mid-await
// destroys the child with it, leak-free, exactly as CoroTask's owning
// destructor promises for the whole chain.
class AwaitCoro {
public:
    explicit AwaitCoro(Coro child) noexcept : child_(std::move(child)) {}

    // Never ready eagerly: the child is lazily started (initial_suspend
    // suspends), so it has not run a line yet, and running it here would
    // put a resume outside Poll.
    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<Coro::promise_type> parent) noexcept {
        parent_ = parent;
        parent.promise().active_child = child_.Handle();
    }

    Status await_resume() noexcept {
        if (parent_ != nullptr) parent_.promise().active_child = {};
        return child_.done() ? child_.result()
                             : Status::InvalidArgument(
                                   "co_await resumed before its child coroutine finished");
    }

private:
    Coro child_;
    std::coroutine_handle<Coro::promise_type> parent_{};
};

inline AwaitCoro operator co_await(Coro&& child) noexcept {
    return AwaitCoro(std::move(child));
}

// ---- Suspension safety --------------------------------------------------
//
// A hook a higher layer installs to answer one question: **is it safe for a
// coroutine to be suspended right now?** It returns a reason when it is
// not, and an empty view when it is.
//
// It exists because the executor is about to grow suspension points, and
// the thing it must never do is suspend while holding a **page span**. The
// existing rule (`docs/spec/parser-v2.md` I15's R1, enforced by
// `exec/step_vm.cpp`'s PageSpanGuard) already forbids a page *fetch* under
// a live span, because nothing pins the frame the span points into.
// Suspending under one is strictly worse: the span stays live not for the
// length of a nested call but for arbitrary wall time, across every other
// statement that runs on this core in between.
//
// A hook rather than a direct call because of layering - `sched/` sits
// below `exec/` and must not know what a page is. `exec` installs it.
//
// **Debug builds only** at the call site below, so release pays nothing.
using SuspendAuditFn = std::string_view (*)();

// Installs the audit. Null (the default) disables it.
void SetSuspendAudit(SuspendAuditFn fn) noexcept;
SuspendAuditFn suspend_audit() noexcept;

// Whether any audit has fired since the last reset, and what it said. A
// test reads these; nothing in the engine branches on them.
bool SuspendAuditTripped() noexcept;
std::string_view SuspendAuditReason() noexcept;
void ResetSuspendAudit() noexcept;

// Runs the audit, if one is installed, and records a failure. Called from
// CoroTask::Poll() in debug builds at the moment a coroutine suspends.
void NoteSuspension() noexcept;

// Runs a `Coro` on a scheduler. Owns the frame: dropping the task destroys
// a coroutine suspended anywhere, which is what makes a cancelled or
// abandoned statement leak nothing.
class CoroTask final : public Task {
public:
    using DoneFn = std::function<void(const Status&)>;

    // `on_done` is invoked once, with the coroutine's returned Status, when
    // it finishes. Optional.
    CoroTask(SchedulingGroup group, Coro coro, DoneFn on_done = {})
        : Task(group), handle_(coro.Release()), on_done_(std::move(on_done)) {}

    CoroTask(const CoroTask&) = delete;
    CoroTask& operator=(const CoroTask&) = delete;

    ~CoroTask() override {
        if (handle_ != nullptr) handle_.destroy();
    }

    PollResult Poll() override {
        if (handle_ == nullptr) return PollResult::kDone;

        // What the idle policy reads back (Task::advanced_in_last_poll).
        // A Poll that resumes nothing executed none of this coroutine's
        // code: it asked a predicate, got false, and returned. The reactor
        // may sleep on a queue of those; it may not sleep on a task that
        // ran. Counted from `resumes_` rather than a separate flag so the
        // two can never disagree.
        const std::uint64_t resumes_at_entry = resumes_;
        last_poll_advanced_ = true;

        // One Poll runs the chain until it genuinely parks or finishes -
        // the multi-resume loop is what a flat coroutine gets for free by
        // running through ready awaits in one resume, owed equally to a
        // chain whose child just finished: stopping after each level would
        // charge one scheduler round-trip per stack frame (P4d-1).
        while (!handle_.done()) {
            // The deepest pending coroutine is the one a resume must enter
            // (Coro::DeepestPending's rationale).
            auto active = Coro::DeepestPending(handle_);

            // The wait check, before the resume: a coroutine parked on a
            // condition that still does not hold costs one predicate call
            // this iteration and is not entered at all.
            if (!Coro::ConsumeWaitIfSatisfied(active)) {
                // The park. Advanced only if an earlier turn of this same
                // loop already resumed something - a chain that ran two
                // frames and then parked did work, and the reactor must
                // not sleep on it.
                last_poll_advanced_ = (resumes_ != resumes_at_entry);
                return PollResult::kSuspended;
            }

            ++resumes_;
            active.resume();

            // Parked mid-body (WaitFor/Yield set a wait, or a plain yield):
            // this Poll is over. A finished child instead loops: the walk
            // now stops at its parent, which resumes in this same Poll.
            if (!active.done() && active.promise().active_child == nullptr) break;
        }
        if (!handle_.done()) {
#ifndef NDEBUG
            // The coroutine gave the core back. Anything it is still
            // holding that only makes sense within a call is now held
            // across every other task on this core - see SuspendAuditFn.
            NoteSuspension();
#endif
            return PollResult::kSuspended;
        }

        if (on_done_) on_done_(handle_.promise().result);
        return PollResult::kDone;
    }

    // Resumes made. Diagnostics and tests - a coroutine that waits a long
    // time shows up here as a large number, which is the poll loop the
    // header describes.
    std::uint64_t resumes() const noexcept { return resumes_; }

    bool advanced_in_last_poll() const noexcept override { return last_poll_advanced_; }

private:
    std::coroutine_handle<Coro::promise_type> handle_;
    DoneFn on_done_;
    std::uint64_t resumes_ = 0;
    // Starts true: a task that has never been polled is not a parked one,
    // and the idle policy must not read "never ran" as "nothing to do".
    bool last_poll_advanced_ = true;
};

// Makes a submittable task out of a coroutine.
inline std::unique_ptr<CoroTask> MakeCoroTask(SchedulingGroup group, Coro coro,
                                              CoroTask::DoneFn on_done = {}) {
    return std::make_unique<CoroTask>(group, std::move(coro), std::move(on_done));
}

// ---- Awaitables ---------------------------------------------------------

// `co_await Yield{}` - give the core back and continue on a later
// iteration. This is `docs/spec/sched.md` §3's mandatory cooperative yield,
// which until now could only be spelled by returning `kSuspended` from a
// hand-written task and remembering where you were.
struct Yield {
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
};

// `co_await WaitFor{&flag}` - suspend until `flag` becomes true.
//
// The shape every cross-core request/response has: send, then wait for the
// handler that receives the reply to set the flag. **The flag must outlive
// the wait**, which in practice means it lives in the same per-request
// state the reply is routed to - not on the coroutine's own stack frame
// before a `co_await` that could outlive it.
//
// It records the flag on the promise as it suspends, and `CoroTask::Poll()`
// is what re-tests it (see `promise_type::wait_flag` for why it cannot be
// `await_ready`'s job). A long wait therefore costs one predicate call per
// reactor turn and never a resumed frame.
// Parks until `pred()` answers true, re-tested once per poll.
//
// `WaitFor`'s sibling, and the difference is what the condition *is*. A flag
// is set by whoever satisfies it, which suits a request whose reply arrives.
// A predicate is asked, which suits a watermark that moves for reasons this
// coroutine has no hook into - a commit waiting on `durable_lsn` is the
// first of those: the sync that satisfies it covers every waiter at once and
// knows about none of them.
//
// The predicate must not fetch a page or touch anything a suspension is not
// allowed to hold (see SuspendAuditFn); asking whether a number has moved is
// the shape it is for.
struct WaitUntil {
    const std::function<bool()>* pred = nullptr;

    bool await_ready() const noexcept { return pred == nullptr || (*pred)(); }

    template <typename P>
    void await_suspend(std::coroutine_handle<P> h) const noexcept {
        h.promise().wait_pred = pred;
    }

    void await_resume() const noexcept {}
};

struct WaitFor {
    const bool* flag = nullptr;

    // Already true: no suspension at all, so a reply that arrived before
    // the wait began costs nothing.
    bool await_ready() const noexcept { return flag != nullptr && *flag; }

    // Templated on the promise so it can reach `wait_flag`; a plain
    // `coroutine_handle<>` has no promise to reach.
    template <typename P>
    void await_suspend(std::coroutine_handle<P> h) const noexcept {
        h.promise().wait_flag = flag;
    }

    void await_resume() const noexcept {}
};

}  // namespace kds::sched
