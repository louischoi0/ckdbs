#pragma once

// **The statement epoch** (BF-R9, BF-Q9 (b);
// `instructions/v3.0.0/workorder-bf-drop-table-page-reclaim.md`).
//
// A drop committed during the run may have its pages freed only once no
// statement that could have bound the relation is still running. Which
// statements could: every one whose catalog memo predates the drop's commit,
// because a memo is revalidated against the instance's schema word only at a
// statement's head (`Catalog::Revalidate`). So each core publishes, in a slot
// of its own, the schema word its running statement revalidated against -
// from that `Revalidate` to the statement's return - and a drop committed at
// word `W` may be reclaimed once every slot is idle or at least `W`.
//
// **Publish before read.** A statement stores `kEntering` in its slot, issues
// a `seq_cst` fence, and only then reads the word; the reclaimer reads the
// slots after a `seq_cst` fence that follows the drop's commit. Either the
// statement's read sees the drop's word - its memo cannot hold the relation
// - or the reclaimer sees `kEntering`, which blocks every reclaim. Read first
// and published after, a statement could read the old word, be seen idle,
// and bind from its stale memo after the free.
//
// **One slot per core is enough** (BF-S1's Census B): every statement head
// is `DispatchAndStage`, which is synchronous, and the executor cannot park,
// so a core runs one statement's binding at a time; the one page id held
// across a park, a mid-walk write's cursor, is fenced by its relation `IX`.
// The KWP load endpoint, the one reader outside `DispatchAndStage`, takes the
// same slot around its handlers. The Cabin optimizer's tick, which BF-R9
// also names, takes none: it runs on core 0's reactor, synchronously, as
// the reclaim's own tick does, so a reclaim step never runs while it holds
// a page id (BF-Q16 (a)).
//
// **Threading.** A slot's one writer is its core; any thread reads it. The
// slots are the instance's, owned by `Expeditor`, sized to the core count.

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>

namespace kds::server {

class StatementEpochs {
public:
    // Idle: no statement on the core can hold a memo.
    static constexpr std::uint64_t kIdle = std::numeric_limits<std::uint64_t>::max();
    // Entering: a statement is between publishing and reading the word;
    // below every word, so it blocks every reclaim.
    static constexpr std::uint64_t kEntering = 0;

    explicit StatementEpochs(std::uint32_t cores)
        : cores_(cores), slots_(std::make_unique<Slot[]>(cores)) {}

    // The statement head's two halves, around `Catalog::Revalidate()`.
    void Enter(std::uint32_t core) noexcept {
        slots_[core].word.store(kEntering, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }
    void Publish(std::uint32_t core, std::uint64_t word) noexcept {
        slots_[core].word.store(word, std::memory_order_release);
    }
    void Leave(std::uint32_t core) noexcept {
        slots_[core].word.store(kIdle, std::memory_order_release);
    }

    // Whether every statement running now revalidated at or after `word`.
    bool AllAtLeast(std::uint64_t word) const noexcept {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        for (std::uint32_t core = 0; core < cores_; ++core) {
            if (slots_[core].word.load(std::memory_order_acquire) < word) return false;
        }
        return true;
    }

private:
    // A cache line each: one core's publish does not bounce another's slot.
    struct alignas(64) Slot {
        std::atomic<std::uint64_t> word{kIdle};
    };
    std::uint32_t cores_;
    std::unique_ptr<Slot[]> slots_;
};

}  // namespace kds::server
