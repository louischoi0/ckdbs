#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/aggregate.hpp"
#include "kds/exec/sort.hpp"
#include "kds/exec/assertion_check.hpp"
#include "kds/exec/budget.hpp"
#include "kds/exec/fk_check.hpp"
#include "kds/exec/plan_printer.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/server/session_step_client.hpp"
#include "kds/exec/cabin_ddl.hpp"
#include "kds/exec/cabin_optimizer_exec.hpp"
#include "kds/exec/index_maintain.hpp"
#include "kds/parser/ast.hpp"
#include "kds/stats/access_batch.hpp"
#include "kds/stats/access_stats.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/stats/trail_recorder.hpp"
#include "kds/stats/trail_store.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/coro.hpp"
#include "kds/server/core_affinity.hpp"
#include "kds/server/range_alloc.hpp"
#include "kds/stats/trace.hpp"
#include "kds/server/commit_phase_stats.hpp"
#include "kds/server/lease_refill_stats.hpp"
#include "kds/server/result_sink.hpp"
#include "kds/server/session.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/wal/manager.hpp"

// Command dispatch: turns one client-supplied line of text into an action
// against the running database and a text response. Deliberately pure
// engine logic - no sockets, no syscalls, no clock reads (rules.md #4:
// engine logic goes through injectable interfaces; there is nothing to
// inject here because Dispatch() needs none of those, which is exactly
// why this is split out from the platform-layer listener that calls it
// (TcpServer, tcp_server.hpp) - Dispatch() can be unit-tested directly,
// with no socket or thread involved.
//
// The SQL statements (CREATE TABLE, INSERT, SELECT, UPDATE) are parsed by
// src/parser and executed here. Column types are resolved through
// Catalog::ResolveTypeByName() against sys.types, which stands in for the
// type registry that does not exist yet; src/exec/row_codec.hpp names
// exactly what that covers - no NULLs, no float or decimal columns,
// fixed-width ints and varchar only.
//
// CREATE TABLE also keeps a bare-name form with no parens, which asks for
// a zero-column table and therefore always errors now that every relation
// needs a Keystone pk column. The two forms are disambiguated by whether a
// '(' follows the table name.
//
// Protocol: one command per line, case-insensitive keyword, arguments
// space-separated. A response is always exactly one line back (never
// containing embedded newlines) - the platform-layer listener appends the
// line terminator itself.
//
// ---- WAL: INSERT is logged, nothing else is -----------------------------
//
// Given a WalManager, INSERT appends the records that describe it and does
// not answer the client until they are durable to the configured class
// (wal.md sections 1 and 5.2). Every other mutating path - CREATE TABLE,
// UPDATE, and the catalog rows underneath both - still writes pages
// outside the log, so a crash still loses them. INSERT went first because
// it is the path with a benchmark pointed at it; the others follow the
// same shape.
//
// One INSERT is one implicit transaction, and it emits:
//
//     TXN_BEGIN
//     [FULL_PAGE_IMAGE  of the old tail]  only when the chain grew
//     [PAGE_INIT        of the new tail]  only when the chain grew
//     HEAP_INSERT       the tuple, its slot, its writer
//     TXN_COMMIT        + the durability class's wait
//
// The FULL_PAGE_IMAGE is there because chain growth mutates two pages: the
// new page's `next_page_id` link lives in the *old* tail's header, and a
// new page redo cannot reach is a new page redo cannot use. No record type
// describes a link edit on its own (record.hpp's enum is frozen and
// append-only, so inventing one is a format-version event), and an FPI is
// the existing record that makes a page whole. It costs one page of log
// per page of heap - roughly +50% log volume on small rows - and it is
// paid once per 8 KB of tuples, never per tuple. A HEAP_CHAIN_LINK record
// type would remove it; that is a format decision, not this file's.
//
// ---- Clustered type: one dispatcher, two storages ----------------------
//
// A relation is either a chain of heap pages (`ClusteredType::kHeap`,
// heap_chain.hpp) or a clustered B+ tree (`kBtree`, btree.hpp), and every
// statement handler below branches on `TableAccess::clustered_type` in
// exactly one place - `InsertIntoRelation`, `VisitRelation`, `LocateByPk`.
// Everything else in this file is storage-agnostic, which is possible
// because a btree **leaf is a heap page**: the row codec, `PageView`
// reads/overwrites, `HEAP_INSERT` and the `SHOW PAGE` dump all work on
// either without knowing which they hold.
//
// The observable differences are narrow and worth stating:
//
//   - `SELECT`/`UPDATE ... WHERE id = <n>` on a btree relation descends
//     the tree, which is **authoritative** - a miss means the row does not
//     exist, and no scan follows. The same statement on a heap relation
//     scans the chain, because a heap relation has no pk index at all.
//   - `INSERT` may split a leaf and grow the tree a level, in which case
//     the relation's `desc_page_id` is repointed at the new root before
//     the client is answered.
//   - A full scan of either is a left-to-right walk of the same
//     `next_page_id` links, so `SELECT *` returns rows in the same order.
//
// ---- Ordering: the records are appended after the page is mutated -------
//
// ChainInsert() writes the tuple into the page frame, and only then are
// the records appended and page_lsn stamped. That is safe here, and the
// reason is narrow enough to be worth stating: the server is a single
// cooperative thread (sched.md), the checkpoint and drain tasks are other
// tasks on it, and nothing suspends between the mutation and the stamp -
// so no flush can observe the page in between. What protects the interval
// is the store's WAL gate (device_page_store.hpp): once page_lsn is
// stamped, no write-back can outrun the log. A path that ever suspends
// mid-statement must generate the record while holding the page latch
// instead, which is what wal.md section 8-1 actually asks for.

namespace kds::sched {
// Only the pointer is held here; `set_scheduler_view` below says what for.
class Scheduler;
}  // namespace kds::sched

namespace kds::server {

// What the mount's recovery did (`server/mount_recovery.hpp`), reported by
// SHOW META. Forward-declared rather than included: only the pointer is held
// here, and the definition drags in the WAL and catalog headers that every
// consumer of this file would then pay for.
struct MountRecovery;

// The `physical_optimizer` config key's two legal states
// (docs/spec/physical-optimizer.md R3). There is deliberately no `kOn`:
// the config layer refuses `on` at startup naming §6's gates, so a mode a
// mover would need cannot exist before the mover does.
enum class PhysicalOptimizerMode : std::uint8_t {
    kOff = 0,
    kShadow = 1,
};

class IndexBuildClient;
class AssertionBuildClient;
class StatementShipClient;
class FkProbeClient;
class FkIntentTable;
class FkPendingDeleteTable;
class ShippedStatementExecutor;
// Forward-declared rather than included: this header is included nearly
// everywhere, and what it needs of the 2PC service is one pointer and one
// pointer-to-const parameter.
class Txn2pcClient;
struct TxnPhaseOutcome;

// A peer-owned relation's `CREATE INDEX` between its two phases on core 0
// (docs/inflight/in-progress/workplan-peer-writer.md §7c, PW1c-6b-3): the definition core 0
// prepared and sent - the oid issued, the root the owner's to fill - and
// what phase 2 needs to answer the client. Carried by value across the
// park: the statement's frame is the one thing that survives it.
struct PendingIndexBuild {
    std::uint64_t request_id = 0;
    std::uint32_t owner_core = 0;
    catalog::Catalog::IndexDef def;
    std::string table_name;       // the reply line
    std::string key_column_name;  // the Cabin warning
};

// A peer-owned relation's `CREATE ASSERTION` between its two phases on core
// 0 (`docs/inflight/in-progress/workplan-peer-writer.md` §7d, PW1c-6c): the id core 0
// issued, the declaration it sent, and what phase 2 needs to write the row
// and answer the client. Carried by value across the park, for
// `PendingIndexBuild`'s reason.
struct PendingAssertionBuild {
    std::uint64_t request_id = 0;
    std::uint32_t owner_core = 0;
    std::uint64_t assertion_id = 0;
    catalog::Oid target_oid = 0;
    std::string name;
    std::string table_name;
    std::string source_text;
};

// A statement this core sent to the core that owns its relation (SS2,
// statement_ship_service.hpp), between the send and the answer. What the
// waiter needs to be found again, and what its refusals need to name.
// **Whether the owner's half of the answer edge exists yet** (XG1).
//
// **True since 2026-09-01**, when the owner's half landed: it installs a
// batch sink on the shipped session, sends the result description on the
// answer edge, and streams `STEP_BATCH` to the tag the request carries.
//
// Kept as a named constant rather than deleted, because it is the one
// switch that turns the whole path off: a build that needed the pre-XG1
// behaviour - the refusal - flips this and gets it, with the wire, the
// receiver and the owner all still compiled and still tested. What it must
// never be is *half* true; the two halves are one feature, and shipping
// `form = 1` to an owner that renders text would answer a typed client
// with a rendered line, which is the failure the refusal existed to
// prevent.
inline constexpr bool kShippedTypedAnswerBuilt = true;

// A statement parked on a foreign parent's owner answering its forward
// check (AH-T2, `docs/spec/foreign-keys.md` §2a).
//
// **The one pending record that resumes by re-entering the statement**,
// where every other one finishes work. There is nothing to finish here: a
// probe answers a question the statement had before it started, so the
// statement runs afterwards rather than being completed by the reply. The
// verdicts land in `resumed_fk_verdicts_` and the line is dispatched
// again, at which point the extraction pass resolves everything from held
// state and sends nothing - the second pass is a plain local statement.
//
// The first pass wrote nothing: the probe is sent from the extraction pass,
// which runs before any row work, and the write scope is abandoned exactly
// as a shipped statement's is (`AbandonWriteForShipping`), so an explicit
// transaction is neither poisoned nor committed by having parked.
struct PendingFkProbe {
    // One per foreign owner - AH-R2's round, and why these are vectors.
    // `request_ids[g]` addresses the reply for `groups[g]`, whose
    // `parents` give the verdicts their identity: the reply is positional,
    // so the request's own order is what maps an answer back to a pk.
    std::vector<std::uint64_t> request_ids;
    std::vector<exec::FkParentVerdicts::ForeignGroup> groups;
    // AJ-T3: the reverse round's groups, filled instead of `groups` when
    // `reverse` is set. **A statement's round is one direction or the
    // other and never both**, which is a property of the statements
    // themselves rather than a restriction: a DELETE runs no forward check
    // (`DeleteInner` mints its check view for `fkeys_in` alone) and an
    // INSERT or UPDATE runs no reverse one. One flag is therefore enough
    // to say which vector the collect block should read, and a statement
    // that ever needed both would need a second park, which
    // `DispatchAsync` refuses by name.
    std::vector<exec::FkReverseProbeGroup> reverse_groups;
    bool reverse = false;
    // The statement to run once the verdicts are in. Empty is impossible
    // here: a path with no text (the KWP load chunk) keeps the refusal
    // instead, exactly as it keeps the shipping one.
    std::string line;
    // **When the last round left** (AH-T6's leg). Stamped after every
    // owner's request is away rather than before the first, because the
    // leg being measured is the wait for the *slowest* owner and a stamp
    // taken before the sends would charge the sending loop to it.
    sched::MonoTimeNs sent_at_ns = 0;
};

struct PendingShippedStatement {
    std::uint64_t request_id = 0;
    std::uint32_t owner_core = 0;
    std::string relation;
    // **This statement was a read** (RR1). Set only at the read dispatch
    // fork, so it is a fact about which site sent the statement and not a
    // guess about what the statement did.
    //
    // Two refusals answer differently for one: a read has no effect, so a
    // failure inside an explicit transaction must not poison it - a local
    // `SELECT` that fails does not, and the two have to agree - and a lost
    // answer is not an *unknown outcome*, because there is no outcome to be
    // unknown about. Both were right while only writes shipped.
    bool read = false;

    // **This statement was a DDL routed to core 0** (CR5/CB4). Set at the
    // one site that ships one, so it is a fact about which fork sent the
    // statement rather than a guess from its text. What it buys is in
    // `FinishShippedStatement`: this core drops its catalog cache when the
    // answer is a success, because the invalidation broadcast is a task and
    // nothing orders it against this reply.
    bool ddl = false;

    // **XG1: this read's answer is coming on an edge, and this is its
    // tag.** Set at the read fork alongside `read`, and only where the
    // session has a result sink - a text client's shipped read is answered
    // on the reply POD exactly as it always was, and leaves this zeroed.
    //
    // Carried on the pending record rather than re-derived, because the
    // statement parks between the ship and the answer and the tag has to
    // survive that. `FinishShippedStatement` forwards the edge's rows on an
    // OK terminator and closes the tag on **every** exit, success or not:
    // a registered receiver nobody drains holds its batches for the
    // session's life, which is the leak the fan-in's own `CloseAll` guard
    // exists to prevent.
    bool typed_answer = false;
    PipelineTag answer_tag{};
};

// A `COMMIT` of a transaction whose writes touched more than one owner
// (R6-3, D4), between the prepare that just left this core and the answer
// the client gets. **Carried by value across every park**, like every other
// pending record here: the statement's coroutine frame is the one thing
// that survives them, and the session's participant list is cleared by
// `Finish()` in the middle of this sequence.
//
// The two request ids are separate on purpose. One waiter per id is the
// transport's rule (`Txn2pcClient::OpenPhase`), and reusing prepare's id
// for decide would let a prepare answer that arrived after its phase timed
// out be delivered into the decide phase - the identity check cannot catch
// that one, since both legs of one transaction carry the same session and
// transaction id.
struct PendingCrossOwnerCommit {
    // Zero where **no prepare was sent** - a transaction whose only
    // cross-owner contact was a probe. The parked half reads it as "there
    // is no vote to collect", not as "the vote was lost".
    std::uint64_t prepare_request_id = 0;
    std::uint64_t decide_request_id = 0;
    std::uint64_t session_id = 0;
    std::uint64_t transaction_id = 0;
    std::vector<std::uint32_t> participants;
    // **Who hears the decision**, which is not the same list as who is
    // asked to prepare (work order AI, F4). A core holding only a
    // foreign-key reference intent has no context to prepare and no vote to
    // cast, and it must still be told the outcome, because the decide is
    // the only thing that ends an intent. `participants` is the prepare's
    // list; this is the decide's, and it is the union.
    std::vector<std::uint32_t> decide_targets;
    // Which of them hold an intent and no rows, so the decide can say so
    // per target and a holder's missing context reads as expected rather
    // than as a lost transaction half.
    std::vector<std::uint32_t> intent_only;

    // **XF4's two coordinator-side stamps**, carried here because the
    // commit's two halves live in two functions: `PrepareAcrossOwners`
    // sends the prepare and returns, and the parked half in `DispatchAsync`
    // is where every leg ends. Both are this core's own monotonic clock and
    // are never compared with another core's.
    //
    // `began_at_ns` is taken where the pending record is built - after
    // every pre-send refusal, so a transaction that never sent a prepare
    // records no leg at all - and `prepare_sent_ns` immediately after the
    // prepare is on its way. The gap between them is the send itself, and
    // it is deliberately outside the prepare leg so that leg measures the
    // *participants* rather than the ring.
    sched::MonoTimeNs began_at_ns = 0;
    sched::MonoTimeNs prepare_sent_ns = 0;
};

// How a finished fan-in becomes a reply when the chain's own projection or
// fold is what produces it (R4-A/AG3, `workplan-insert-spreading.md` §12).
//
// **Carried by value across the park, because the chain is not.** The
// compiled `StepChain` dies with `HandleSelect`'s frame while the read
// completes on the reactor, and `Aggregator::Reset` borrows its spec and
// its labels - so the fold's whole spec and the projection's headings are
// *copied* here rather than pointed at. That is also why the aggregator
// itself is a local of `FinishRemoteReads` and not the dispatcher's
// hoisted `aggregator_`: a fan-in parks, two aggregated fan-ins on one
// core would interleave inside that member's one-statement contract, and
// the failure would be a wrong number rather than a refusal.
//
// **Not the same fact as `RemoteRead::column_names`**, which the two-step
// pipeline fills. That one describes *the rows on the wire* - a projected
// final edge, rendered straight out. This one describes what the session
// computes **from whole rows**: the stage ships the relation's row,
// filtered by the WHERE it was given, and the projection or the fold is
// applied here. Empty is the P4c star shape, which renders from the
// relation's schema exactly as it always did.
struct PendingRemoteRender {
    // The select list, resolved (`StepChain::projection`). Empty for a star
    // read and for a fold, whose output is its items and not chain columns.
    std::vector<exec::ColumnRef> projection;

    // The reply's headings: the projected columns' names, or the fold's
    // labels (`count(*)`, `sum(v)`). Empty means "the relation's columns",
    // which is the star shape.
    std::vector<std::string> column_names;

    // One per projected column, in the same order - the reason a DATE
    // renders as a date rather than an epoch day.
    std::vector<std::uint32_t> projection_types;

    // And the same columns' `type_mod`, for `StepChain`'s reason: a typed
    // reply states a decimal's scale once, in the description, and a
    // fan-in's description is built here rather than from a chain that did
    // not survive the park.
    std::vector<std::uint32_t> projection_type_mods;

    // The fold, or nothing (AG1's spec, copied whole).
    std::optional<exec::AggregateSpec> aggregate;

    // Whether anything here changes how the reply is built. Both empty is
    // the star read, which is every fan-in before AG3.
    bool chain_rendered() const noexcept {
        return aggregate.has_value() || !projection.empty();
    }
};

struct DispatchOutcome {
    std::string response;
    bool should_stop = false;

    // **How many rows the statement affected**, for a caller that needs the
    // number rather than the sentence. `S_COMPLETE{tag, rows_affected}`
    // (docs/spec/protocol.md §7) is the one such caller today.
    //
    // Carried rather than read back out of `response`, which was the first
    // shape and is the drift this codebase refuses everywhere else: the
    // three DML replies do not share a spelling - `UPDATED 7` puts the
    // count second, `INSERTED oid=.. id=..` implies one, and a bulk insert
    // writes `rows=` - so a parser here would be a *second* reading of a
    // string the renderer owns, and would answer 0 for the commonest write
    // in the engine. Set where the count is known; 0 everywhere else, which
    // is the truthful answer for a read, a DDL and a session statement.
    std::uint64_t rows_affected = 0;

    // **Why the statement failed, as a `Status`.** OK on every success and
    // on the handful of refusals built from a bare message.
    //
    // Carried for `rows_affected`'s reason, one step further: a typed
    // client switches on the error's *category* (protocol.md §11, and the
    // `retryable` bit it makes a compatibility surface), and the only route
    // to one used to be `StatusFromErrorReply` parsing the rendered line -
    // whose bare arm folds `NotFound`, `Unsupported`, `OutOfRange`,
    // `Corruption`, `OutOfSpace`, `IoError`, `ResourceExhausted` and
    // `CardinalityViolation` into `InvalidArgument`. Every one of those
    // reached a KWP client as INVALID_ARGUMENT, which defeats the whole
    // point of a category taxonomy at the one seam that feeds it.
    //
    // `StatusFromErrorReply` stays, and is still right where it is used:
    // the cross-core path genuinely has only a rendered line to recover a
    // code from, and the four spellings it recovers exactly are the ones
    // that matter there.
    Status status = Status::OK();

    // A remote read this statement opened (workplan P4c): the reply is not
    // in `response` yet - the caller awaits the read and finishes through
    // `FinishRemoteRead()`. `DispatchAsync()` parks on it; the synchronous
    // `Dispatch()` can finish one only when it already completed (the
    // in-process loopback case), because with no reactor there is nothing
    // to pump the reply through.
    // **A group since RD7** (§5's third cost). A read of a split relation
    // opens one stage per range, so the statement parks on k tags rather
    // than one and completes only when every one of them has. In **range
    // order** (CC9's ascending `lo`), which is the order their rows are
    // concatenated in - the same order the local walk emits in, so a
    // split relation read remotely and read locally answer alike.
    //
    // Empty is "no remote read", which is what `std::optional`'s absence
    // used to say; one element is every read before RD7 and every read of
    // an unsplit relation after it.
    std::vector<PipelineTag> pending_remote = {};

    // What to do with the rows those stages return (AG3). Default-empty is
    // the star read: whole rows, rendered from the relation's schema.
    PendingRemoteRender remote_render = {};

    // A peer-owned relation's CREATE INDEX this statement sent to the owner
    // to build (PW1c-6b-3): the reply is not in `response` yet.
    // `DispatchAsync()` parks on the owner's reply under its deadline and
    // finishes through `FinishIndexBuild()`; the synchronous `Dispatch()`
    // has no reactor to receive one on and abandons it, telling the owner.
    std::optional<PendingIndexBuild> pending_index_build = std::nullopt;

    // A peer-owned relation's CREATE ASSERTION this statement sent to the
    // owner to build (PW1c-6c): the reply is not in `response` yet, and the
    // two paths differ exactly as the index build's do - `DispatchAsync()`
    // parks, the synchronous `Dispatch()` abandons and tells the owner.
    std::optional<PendingAssertionBuild> pending_assertion_build = std::nullopt;

    // A statement shipped to its owner core (SS2): the reply is not in
    // `response` yet. `DispatchAsync()` parks on the owner's answer under
    // its deadline and finishes through `FinishShippedStatement()`.
    //
    // **The synchronous `Dispatch()` never sees one**, and that is a
    // correctness statement rather than an accident: shipping is admitted
    // only where the statement can park, because a send from a path that
    // cannot wait would leave a statement the owner may have committed with
    // nowhere to deliver its answer - and the refusal `Dispatch()` would
    // have to invent could not be retryable (D4).
    std::optional<PendingShippedStatement> pending_shipped = std::nullopt;

    // A foreign parent's forward check, sent and not yet answered (AH-T2).
    // `DispatchAsync()` parks on every owner's reply under one deadline and
    // then **re-dispatches the line**; the synchronous `Dispatch()` has no
    // reactor to receive a reply on and refuses retryably.
    std::optional<PendingFkProbe> pending_fk_probe = std::nullopt;

    // A cross-owner `COMMIT` whose prepare phase is in flight (R6-3). The
    // reply is not in `response` yet: `DispatchAsync()` runs the rest of
    // the protocol - the prepare park, the decision, the decide park - and
    // writes the answer at the end.
    //
    // **The synchronous `Dispatch()` never sees one**, for the reason
    // `pending_shipped` never reaches it: the protocol has to park, and a
    // path that cannot wait has no honest answer to give a client whose
    // participants are already asked. `HandleCommit` refuses before the
    // first prepare leaves rather than after, so that refusal is an
    // ordinary retryable one and the transaction is still whole.
    std::optional<PendingCrossOwnerCommit> pending_cross_owner_commit = std::nullopt;

    // A write this core refused because the row it wanted is held by a
    // transaction this core **prepared and is in doubt about** (R6-5, D5).
    //
    // Set only where the refusal is one a re-run could get past: the
    // statement wrote nothing before it hit the conflict, so running it
    // again once the doubt clears is exactly what the client would do.
    // `DispatchAsync` parks on the doubt and re-runs; a statement that had
    // already written rows is not re-runnable - re-applying `SET v = v + 1`
    // to the rows it did write would be a second increment - and is
    // answered with the conflict it produced, unblocked.
    //
    // **The synchronous `Dispatch()` never waits on one**, for the reason
    // it never ships: a path with no reactor cannot park, and the honest
    // answer there is the retryable conflict itself. So the block is a
    // property of served connections, and a fixture sees the pre-R6-5
    // behaviour.
    struct InDoubtBlock {
        std::uint64_t trx_id = 0;  // the in-doubt writer holding the row
        std::uint64_t pk = 0;      // the row it holds
    };
    std::optional<InDoubtBlock> in_doubt_block = std::nullopt;

    // The commit this statement staged, when the client may not be told
    // about it until the log is durable (`durability = group`, docs/spec/wal.md
    // D2). `kNoLsn` means there is nothing to wait for - every relaxed
    // statement, every read, and every strict commit, which synced on its
    // own stack before returning.
    //
    // **The wait is deliberately not taken where the commit happens.** A
    // statement that syncs inline is a statement that holds the core while
    // the device works, which serializes every other connection behind it -
    // measured as a batch size of exactly 1 and TPS that does not move with
    // the connection count (bench/results-latency-matrix.md). Returning the
    // LSN instead lets the *caller* decide how to wait: `Dispatch()` waits
    // inline, because its callers have no scheduler to park on;
    // `DispatchAsync()` parks, which is what lets the next connection's
    // statement run and stage its own commit into the same sync.
    wal::Lsn pending_lsn = wal::kNoLsn;

    // **The COMMIT record's own LSN, whatever the class and whatever the
    // ack point** (XF4). Distinct from `pending_lsn`, which is *"the wait
    // this statement still owes"* and is deliberately empty where the
    // caller was answered at the append: a cross-owner participant under
    // `CommitAck::kAtAppend` leaves `pending_lsn` at `kNoLsn` precisely
    // because nobody is waiting, and yet **the record it appended is the
    // one thing XE1's timing question is about**.
    //
    // `HandleCommit` already had this value in hand and already exported
    // it through an out-parameter for the coordinator (`CommitLocal`'s
    // `commit_lsn`); carrying it on the outcome is that same fact reaching
    // the one caller that has no out-parameter to read it from -
    // `ShippedStatementExecutor::FinishDecision`, which needs it to time
    // the leg between its ack and its own durability. `kNoLsn` on every
    // statement that is not a commit.
    wal::Lsn commit_lsn = wal::kNoLsn;
};

// The one spelling of an error reply on the newline protocol (docs/spec/txn.md
// §5, docs/spec/protocol.md §11): `ERR <TOKEN> retryable=<b> <message>` for the
// codes a client library switches on - TXN_CONFLICT, FK_VIOLATION,
// ASSERTION_VIOLATION - and `ERR <message>` for everything else. Every
// dispatcher path reports through it, which is what keeps the shape from
// drifting between them; declared here so the spellings, a compatibility
// surface, can be pinned by a test that owns no socket and no dispatcher.
std::string ErrorReply(const Status& status);

// **The inverse**, and it exists for exactly one caller: a statement
// executed on its owner core answers in a rendered line, and what has to
// cross back to the arrival core is the *code* - because the arrival core
// re-renders through `ErrorReply` and the `retryable` bit a client's retry
// loop reads must be the bit the owner meant (SS3,
// server/shipped_statement_executor.hpp).
//
// A dispatcher's outcome carries no `Status`: every handler renders one at
// its own return, and threading a code back out would touch every write
// path in this file. Recovering it from the rendered line instead is exact
// where it matters - the four spellings a client switches on are recovered
// as themselves - and lossy only where nothing reads it: every other code
// renders as a bare `ERR <message>`, so all of them come back as one, and
// the re-render is byte-identical. `ErrorReply(StatusFromErrorReply(line))
// == line` for every line `ErrorReply` produces, which is the property
// worth having and the one its test asserts.
//
// A line that is not an error reply is a bare `kOk` - **the line itself is
// not carried**, because on the success arm the caller already holds it and
// copying it into a status message would only duplicate the answer. This
// refuses to invent a failure for a success; it does not claim to
// reconstruct the success.
//
// Which makes the classification purely prefix-shaped: a *success* line
// beginning `ERR ` would be read as a refusal. What keeps that unreachable
// is not this function but the replies themselves - a DML answer opens with
// a fixed keyword (`INSERTED`/`UPDATED`/`DELETED`/`OK`), and a SELECT's
// header line is comma-joined identifiers, so no shippable success has a
// space at byte 3. Stated because it is a property of the *callers*, and a
// reply shape added later that can lead with free text breaks it silently.
Status StatusFromErrorReply(std::string_view reply);

// Where a tuple lives, as a point lookup reports it. Local to the
// dispatcher because it is the shape of an answer to "skip the scan and
// look here", not a storage-layer concept.
struct TupleLocation {
    PageId page_id = kInvalidPageId;
    std::uint16_t slot = 0;
    // No bytes field, for btree.hpp Location's reason: a span here outlived
    // the pin that made it valid, and every reader already re-fetches by
    // page_id. Deleted with its one producer 2026-08-13.
};

class CommandDispatcher {
public:
    // `log` and `clock` are optional and independently so: a null logger
    // disables every diagnostic below, and a null clock only drops the
    // duration from the ones that report one. Both default to off so the
    // socket-free unit tests stay socket- *and* clock-free.
    //
    // The clock is the reason this class is no longer strictly free of
    // injectable interfaces (see the note above): reporting how long a
    // query took needs a monotonic reading, and taking one directly would
    // be the std::chrono call rules.md section 4 forbids.
    // `wal` is optional too, and null means INSERT mutates pages without
    // logging them - the unlogged path, which the socket-free
    // unit tests and the catalog-level tests still run on.
    CommandDispatcher(SuperBlock& superblock, catalog::Catalog& catalog,
                       storage::PageStore& page_store, Logger* log = nullptr,
                       const sched::Clock* clock = nullptr, wal::WalManager* wal = nullptr,
                       wal::DurabilityClass durability = wal::DurabilityClass::kGroup,
                       exec::Budget budget = exec::Budget(),
                       stats::TrailRecorder* recorder = nullptr,
                       bool replay_enabled = false,
                       bool access_statistics = true,
                       stats::CabinStore* cabins = nullptr,
                       txn::TransactionManager* txn = nullptr,
                       txn::IsolationLevel isolation =
                           txn::IsolationLevel::kReadCommitted,
                       std::uint32_t core_id = 0, bool indexes = true,
                       std::uint64_t max_insert_rows =
                           parser::kDefaultMaxInsertRows) noexcept
        : superblock_(superblock),
          catalog_(catalog),
          page_store_(page_store),
          log_(log),
          clock_(clock),
          wal_(wal),
          durability_(durability),
          budget_(budget),
          recorder_(recorder),
          replay_enabled_(replay_enabled),
          access_stats_enabled_(access_statistics),
          cabins_(cabins),
          txn_(txn),
          core_id_(core_id),
          indexes_enabled_(indexes),
          max_insert_rows_(max_insert_rows),
          // Last two, matching their declaration order below. Order-free:
          // every initializer here reads a constructor parameter, and none
          // reads another member.
          default_isolation_(isolation),
          autocommit_session_(isolation) {
        // The one place a catalog and a manager are known to belong
        // together, which is why DT9's wiring is here and not in each
        // server's startup: a new construction site - a test fixture
        // especially - cannot forget it. Two dispatchers over one catalog
        // leave the later manager installed, the same relation the single
        // `catalog_` reference already has.
        if (txn != nullptr) catalog.SetTransactionManager(txn);
    }

    // Parses and executes one line. Never fails outward: a malformed or
    // unrecognized line produces an "ERR ..." response rather than any
    // kind of error return - a bad line from one client must never be
    // able to bring the dispatcher (or the server driving it) down.
    //
    // Recognized commands (case-insensitive):
    //   PING                  -> "PONG"
    //   STOP                  -> "OK bye" and should_stop = true
    //   SYNC                  -> "OK synced" or "ERR ...". Writes the page
    //                            store back to stable storage. Until the
    //                            WAL lands, this and STOP are the only
    //                            things that make a mutation survive the
    //                            process dying.
    //   SHOW META             -> superblock stats, one line
    //   SHOW TABLES           -> space-separated table names
    //   SHOW PATTERNS         -> "patterns=<n>", then one "\n"-escaped
    //                            section per sys.patterns row, identified
    //                            by its hex pattern_id and carrying
    //                            `origin=` and `pinned=`, both of which
    //                            read `auto` / `no` on every row since
    //                            declared patterns were withdrawn.
    //                            An inspection surface: it lists rows from
    //                            older fingerprint revisions too, marked
    //                            `stale=v<n>`, because those are the dead
    //                            weight a version bump leaves behind and
    //                            seeing them is the point.
    //   SHOW ACCESS           -> "access_shapes=<n>", then one "\n"-escaped
    //                            section per recorded access shape:
    //                            "kind=<Lookup|Probe|Range|FilterScan|Scan>
    //                             rel=<s> columns=[<s>,...] uses=<n>
    //                             last_seen=<n>". The physical optimizer's
    //                            input (docs/spec/heap-and-tuple.md §7), keyed
    //                            by *columns* and never by values - so
    //                            `WHERE flag = 1` and `WHERE flag = 2` are
    //                            one shape, which is what keeps the list
    //                            bounded by the schema rather than by the
    //                            data.
    //   SHOW PAGE <page_id> [VALUES]
    //                         -> page dump: header + slot directory for a
    //                            heap page or a B+ tree leaf, or level +
    //                            separator array for a B+ tree internal
    //                            node.
    //                            Still exactly one wire line (never a raw
    //                            newline byte), but sections are joined
    //                            with the literal two-character escape
    //                            "\n" for a readable multi-line render on
    //                            the client side (tools/ckdbs_cli.py
    //                            unescapes it before printing). Development/
    //                            inspection only - not part of any
    //                            transactional read path. The optional
    //                            VALUES keyword additionally hex-encodes
    //                            each live slot's tuple payload (hex, not
    //                            raw text, since a payload can contain any
    //                            byte including '\n' - see HexEncode()'s
    //                            comment in the .cpp).
    //   DESCRIBE <name>       -> a summary line
    //                            "oid=<n> root_page_id=<n>
    //                             clustered_type=<HEAP|BTREE> next_id=<n>
    //                             columns=<n>" (plus height=<n> leaves=<n>
    //                             for a BTREE relation), then one "\n"-escaped
    //                            (see SHOW PAGE above) section per column:
    //                            "pos=<n> name=<s> type=<s> len=<n>
    //                             notnull=<yes|no> pk=<yes|no>
    //                             autoincrement=<yes|no>". Replaces the
    //                            former FIND TABLE, which reported the
    //                            same header and no schema. DESC is
    //                            accepted as a synonym.
    //   CREATE TABLE <name>   -> the bare, pre-parser form: a zero-column
    //                            table. Now always "ERR ...", because
    //                            every relation's first column is its
    //                            mandatory Keystone primary key
    //                            (heap-and-tuple.md section 4) and a
    //                            zero-column relation cannot have one.
    //                            Kept only so the failure names the
    //                            reason; use the column-list form.
    //   CREATE TABLE <name> (<col> <type> [, ...])
    //       [HEAP | BTREE] [EXPLICIT]
    //                         -> same CREATED/EXISTS response as above,
    //                            but with real columns: parsed via
    //                            src/parser, types resolved through
    //                            Catalog::ResolveTypeByName(). The storage
    //                            word: HEAP (default) is a chain of heap
    //                            pages, BTREE is a clustered B+ tree on the
    //                            Keystone pk. EXPLICIT is vacuous (§4.1) -
    //                            every relation takes a caller-named pk or
    //                            issues one when INSERT omits it, so the
    //                            word states the default; ASSIGNED is
    //                            refused. Either order, each at most once.
    //                            See src/exec/row_codec.hpp for the
    //                            supported column type set.
    //   INSERT INTO <name> VALUES (<val> [, ...])
    //                         -> "INSERTED oid=<table_oid> id=<n> slot=<n>"
    //                            or "ERR ...". Values are positional, one
    //                            per schema column in `pos` order, *after*
    //                            the primary key - see ast.hpp: no
    //                            explicit column list in this grammar. The
    //                            pk is not supplied: it is the Keystone id,
    //                            issued by Catalog::AllocateRowId() and
    //                            reported as `id=`. Supplying a full-width
    //                            value list is an error naming the pk
    //                            column (CLAUDE.md invariant 10).
    //   SELECT * FROM <name> [WHERE <cond> [AND <cond>]*]
    //                         -> a full ordered scan of the relation,
    //                            WHERE-filtered; a bare `WHERE <pk> = <n>`
    //                            instead takes the point path (a tree
    //                            descent, on a btree relation).
    //                            One wire line: "col1,col2,..." then one
    //                            "\n"-escaped (see SHOW PAGE above) section
    //                            per matching row, comma-joined values.
    //                            No rows matching -> just the header line.
    //   UPDATE <name> SET <col> = <val> [, ...] [WHERE <cond> [AND <cond>]*]
    //                         -> "UPDATED <n>" (n = row count touched) or
    //                            "ERR ...". In-place HOT-style overwrite
    //                            (PageView::OverwriteTuple) - fails with
    //                            an ERR (no fallback) if a changed value
    //                            no longer fits the tuple's original slot
    //                            capacity, e.g. growing a varchar.
    //
    // `session` carries the connection's transaction state (session.hpp).
    // **Null means autocommit through a private session**, which is what
    // every caller that predates transactions gets - and what keeps their
    // behaviour identical, because an autocommit statement outside an
    // explicit transaction is exactly what the engine did before.
    DispatchOutcome Dispatch(std::string_view line, Session* session = nullptr);

    // The statement path without the durability wait: it runs the statement
    // and reports any commit it staged through `DispatchOutcome::pending_lsn`.
    // Both entry points above go through it; they differ only in how they
    // wait.
    DispatchOutcome DispatchAndStage(std::string_view line, Session* session);

    // The suspendable form. Writes its reply through `out` - which the
    // **caller** owns and must keep alive across suspension - and finishes
    // with a Status describing the dispatch itself, not the statement (a
    // failed statement is an "ERR ..." in `out`, exactly as it is for the
    // synchronous form).
    //
    // ---- Why both forms exist -------------------------------------------
    //
    // This is the seam `docs/inflight/in-progress/workplan-crosscore.md` P4 needs: a statement
    // that reaches another core has to send and then *wait*, and a function
    // returning a finished reply cannot. Making it a coroutine is what lets
    // the executor grow a suspension point later without the server around
    // it changing again.
    //
    // The synchronous `Dispatch()` above stays, and is not deprecated. Every
    // caller that has no reactor - the socket-free tests, a peer's
    // `CoreRuntime`, the rollback on connection close - would otherwise have
    // to acquire one to run a statement, which is a lot of machinery to
    // demand of a caller that never suspends. It is implemented in terms of
    // nothing; the coroutine wraps *it*, so there is one dispatch path and
    // no chance of the two drifting.
    //
    // **`line` is not copied.** A coroutine's parameters live in its frame,
    // but a `string_view` parameter copies the view and not the bytes - and
    // the parser's tokens are themselves views into this buffer
    // (parser-v2.md's zero-copy tokens). The caller must keep the statement
    // text alive until the coroutine finishes, which is why `TcpServer`
    // copies each line out of its inbox before dispatching one.
    // **When a D2 commit inside this dispatch owes its acknowledgement**
    // (`docs/spec/cross-owner-txn.md` §2, ratified 2026-08-31 by
    // `instructions/v2.7.1/ratification-xd1.md`, enacted by
    // `instructions/v2.7.1/workorder-xd.md`).
    //
    // `kWhenDurable` is every client-facing path and the default: a client
    // told `COMMIT` under `group` has been told the record is on the
    // platter, and the wait is what makes that true.
    //
    // `kAtAppend` has exactly one caller - a cross-owner **participant**
    // applying a decide it was told. Its acknowledgement goes to the
    // coordinator, which has already made *the decision* durable in its own
    // stream and already answers the client from that record alone, with or
    // without this ack. So the participant's own record is a redo shortcut,
    // and waiting for it before acking serialized a third device sync
    // behind two that the protocol genuinely needs.
    //
    // **D1 and D3 are unreachable by this**, by construction rather than by
    // a second branch: `kStrict` synced inside `WalManager::Commit` before
    // it returned and `kRelaxed` waits for nothing, so neither ever stages
    // a `pending_commit_lsn_` for this to suppress. The one site that reads
    // it says so.
    enum class CommitAck {
        kWhenDurable,
        kAtAppend,
    };

    sched::Coro DispatchAsync(std::string_view line, Session* session, DispatchOutcome* out,
                              CommitAck commit_ack = CommitAck::kWhenDurable);

    // The level a fresh session starts at (`isolation`). TcpServer stamps
    // it on each connection's session at accept.
    txn::IsolationLevel default_isolation() const noexcept { return default_isolation_; }

private:
    // ---- Transaction control (docs/spec/txn.md sections 1, 6) ----------------
    DispatchOutcome HandleBegin(std::string_view args, Session& session);
    DispatchOutcome HandleCommit(Session& session);
    DispatchOutcome HandleRollback(Session& session);
    DispatchOutcome HandleSetIsolation(std::string_view args, Session& session);
    // `SET DURABILITY {STRICT|GROUP|RELAXED}` (protocol-wp.md P03,
    // docs/spec/protocol.md §9). The session rung of the same chain
    // `HandleSetIsolation` sets, and deliberately the same shape: a
    // session statement the dispatcher routes, not a `parser::Statement`
    // arm - see the note at its definition.
    DispatchOutcome HandleSetDurability(std::string_view args, Session& session);

    // The read view a statement reads through. In autocommit it is minted
    // fresh here and belongs to no transaction; inside an explicit one it
    // is the transaction's, re-minted per statement under READ COMMITTED
    // and held since BEGIN under REPEATABLE READ.
    //
    // Returns a snapshot that sees everything when no TransactionManager
    // was given - the pre-MVCC engine, exactly.
    StatusOr<txn::LeasedSnapshot> SnapshotFor(Session& session);

    // ---- The write scope (section 6's failure atomicity) ----------------
    //
    // A write statement runs inside a transaction whether or not the client
    // asked for one. `owned` says which: in autocommit this scope began the
    // transaction and must end it, and inside an explicit transaction it
    // borrows the session's and ends nothing.
    struct WriteScope {
        txn::Transaction* txn = nullptr;
        bool owned = false;
        // The session this write belongs to. Carried here rather than
        // threaded separately through every *Inner() because the scope
        // already *is* this write's transaction context, and the home-core
        // binding (crosscore.md CC3) is part of that context.
        Session* session = nullptr;
        bool ok() const noexcept { return txn != nullptr; }
    };

    // Fails only if a transaction cannot be started. A dispatcher with no
    // manager returns an empty scope, and the write path then stamps
    // kBootstrapXid exactly as it always did.
    StatusOr<WriteScope> BeginWrite(Session& session);

    // Ends what BeginWrite began. `result` is the statement's outcome: OK
    // commits an owned scope, anything else aborts it. Inside an explicit
    // transaction a failure **poisons the session** rather than unwinding -
    // rows already written stay, and the client must ROLLBACK (section 6).
    //
    // `statement_ends` is false on the one caller that ends a *scope*
    // without ending the statement - `AbandonWriteForShipping`, where the
    // statement is parking on a probe or has gone to another owner and will
    // run whole somewhere else. Only per-statement state hangs off it
    // (AJ-T1's pending-delete clear); the scope's own unwind is identical
    // on both arms.
    Status EndWrite(Session& session, WriteScope& scope, const Status& result,
                    bool statement_ends = true);

    // The trx_id a write stamps: the scope's transaction, or
    // kBootstrapXid when there is no manager.
    static std::uint64_t WriterId(const WriteScope& scope);

    // First-updater-wins, plus the one thing R6-5 adds to it: **who** the
    // conflicting writer is. `TransactionManager::CheckWriteConflict` is
    // unchanged and still decides the verdict; this notes, when the verdict
    // is a conflict against a transaction this core prepared and is in
    // doubt about, that the refusal is one a bounded wait could get past
    // (D5's ratified "block, with a bounded ceiling ending in a named
    // refusal"). One function for the two call sites, so the two write
    // paths cannot come to disagree about which conflicts are waitable.
    Status CheckWriteConflictBlocking(const WriteScope& scope, std::uint64_t cur,
                                      std::uint64_t pk);

    DispatchOutcome HandleShowMeta();
    DispatchOutcome HandleListTables(Session& session);

    // `SHOW NAMESPACES` (`docs/spec/namespace.md` NS9). `sys.objects`
    // filtered by type, the way `SHOW TABLES` filters by `kTypeTable`.
    DispatchOutcome HandleShowNamespaces(Session& session);

    // `{CREATE | DROP} NAMESPACE <name>` (AF-T3). One handler for both, as
    // the parser has one production - see `NamespaceStmt` (ast.hpp).
    DispatchOutcome HandleNamespace(std::string_view line, Session& session);
    DispatchOutcome HandleDescribe(std::string_view args, Session& session);
    DispatchOutcome HandleShowPage(std::string_view args);
    DispatchOutcome HandleShowPatterns();
    DispatchOutcome HandleShowAccess();

    // `SHOW BUDGET` - every relation's Keystone id consumption
    // (`docs/rules/keystoneid-invariant.md` K-M4). Listed for *every* relation
    // including the catalog's own, because some of those - sys.patterns,
    // sys.cabins, sys.assertions - genuinely issue ids, and a listing that
    // hid them would hide the only relations whose consumption an operator
    // does not control.
    DispatchOutcome HandleShowBudget();

    // Both take the session so a `CREATE TABLE` inside an explicit
    // transaction can stamp its catalog rows with that transaction's id
    // and register them for rollback (workplan-ddl-transactional.md
    // DT3b). In autocommit they behave exactly as they always did.
    DispatchOutcome HandleCreateTable(std::string_view args, Session& session);
    DispatchOutcome HandleCreateTableSql(std::string_view line, Session& session);

    // The other half of the duplicate-name refusal, for **both** CREATE
    // TABLE forms: the reply to send when `name` is claimed by a drop that
    // has not committed, or nullopt when it is genuinely free.
    //
    // The unfiltered duplicate check answers "is a live relation using this
    // name" and is deliberately unfiltered so a second create is refused
    // (ddl-transactional.md §6). It cannot see the case this covers -
    // `DROP TABLE` retypes the `sys.objects` row in place, so the name
    // reads as free to everyone while the drop is still undoable, and a
    // create that took it would leave two live rows claiming one name once
    // the drop rolled back.
    std::optional<DispatchOutcome> RefuseIfNameHeldByPendingDrop(std::string_view name,
                                                                 Session& session);

    // The DDL half of a transaction: the id a catalog row should carry,
    // and where to put the rows it wrote so `ROLLBACK` can retire them.
    // Answers `kBootstrapXid` and a null sink outside an explicit
    // transaction, which is every pre-DT3b caller.
    struct DdlScope {
        std::uint64_t trx_id = catalog::kBootstrapXid;
        std::vector<catalog::CatalogRowRef> written;
        txn::Transaction* txn = nullptr;
        std::vector<catalog::CatalogRowRef>* sink() {
            return txn != nullptr ? &written : nullptr;
        }
    };
    // RV3-3: the scope-based sibling every DDL handler now uses. The
    // transaction comes from the WriteScope - explicit or the implicit
    // one BeginWrite opened (D2: autocommit DDL is a real transaction) -
    // and installing the catalog's undo hook happens here, so a handler
    // cannot write catalog rows a crash loser could not roll back. The
    // hook is uninstalled by FinishDdlStatement, every exit.
    DdlScope DdlScopeFor(WriteScope& scope);
    // The one shape a DDL route may have: BeginWrite, the body, then
    // FinishDdlStatement on every exit - structural, so no route can
    // install the undo hook and leave it armed (review S4).
    template <typename Fn>
    DispatchOutcome InDdlStatement(Session& session, Fn&& body);
    // The tail every DDL route runs: uninstalls the undo hook, resolves
    // the write scope (commit/abort for an owned one), and for an owned
    // scope runs the DDL-resolution seam - cache invalidation and the §5d
    // purge - that explicit COMMIT/ROLLBACK reaches through EndDdlScope.
    void FinishDdlStatement(Session& session, WriteScope& scope, DispatchOutcome& out);
    // D1/D2's promise for the transactionless DDL statements (pattern,
    // assertion, cabin, ALTER): their records sync before the
    // acknowledgement - they have no commit record for the durability
    // class to ride on. Every route that writes without a WriteScope owes
    // this call on its success path; the .cpp says why D2 syncs rather
    // than batching.
    Status AwaitDdlDurability();
    // EndDdlScope's core, keyed by id: the session-based wrapper serves
    // explicit COMMIT/ROLLBACK, this serves an implicit DDL transaction
    // whose resolution EndWrite performed.
    void EndDdlScopeById(std::uint64_t txn_id);

    // The view a statement resolves relation names under
    // (workplan-ddl-transactional.md DT3c), or `nullopt` for "see
    // everything" - which is the *fast* path and the common one.
    //
    // **A view is minted only while some transaction holds uncommitted
    // DDL.** With none in flight every catalog row is either a bootstrap
    // row or a committed one, so an unfiltered read is correct for every
    // reader - and a filtered read would cost a catalog page scan per
    // statement, because a filtered lookup deliberately bypasses the
    // shared cache (DT3). That is ddl-transactional.md §6's cache
    // decision, taken: pay for isolation only where isolation is at
    // stake.
    std::optional<txn::ReadView> ViewFor(Session& session);

    // **The statement boundary, taken exactly once per statement.**
    // Under READ COMMITTED a transaction re-mints its view at each
    // statement (`txn.md` §1), and before DT3c only the routes that
    // reached `SnapshotFor`/`BeginWrite` ever took it — so `DESCRIBE`,
    // `SHOW TABLES`, `SHOW INDEXES`, `ALTER`, `DROP TABLE` and the FK
    // parent lookup resolved under whatever view the transaction last
    // happened to hold, and could miss a relation committed since. That
    // is a READ COMMITTED violation and it breaks DT3c's own property
    // that every route agrees.
    //
    // Latched rather than called per site, because the alternative -
    // each caller taking its own boundary - moves the view *within* one
    // statement as soon as a handler resolves twice (the FK loop did),
    // and then two resolutions in one statement disagree. One latch,
    // reset per statement, is the single answer to "when does the view
    // move".
    Status EnsureStatementBoundary(Session& session);
    bool statement_boundary_taken_ = false;
    // Registers everything `written` holds on the transaction's trail.
    // Called **even when the DDL failed**: rows written before the failure
    // are on the page either way, and a rollback that skipped them would
    // leave the half-built relation this feature exists to prevent.
    // The rollback trail for a DDL route that changes rows rather than
    // inserting them (`DROP TABLE`, `DROP NAMESPACE`). No-op outside an
    // explicit transaction.
    void NoteCatalogRowChanges(DdlScope& scope,
                               const std::vector<catalog::CatalogRowChange>& changed);

    void NoteDdlRows(DdlScope& scope);

    // Transactions holding catalog rows nobody has committed yet. Empty
    // is the normal state and the one `ViewFor` optimises for. Entries
    // are removed when the transaction resolves, by `EndDdlScope`.
    std::vector<std::uint64_t> ddl_txns_;
    void EndDdlScope(const Session& session);
    // Delete-marked catalog rows retired since mount by the horizon-gated
    // purge EndDdlScope runs (ddl-transactional.md §5d). SHOW META
    // prints it beside `catalog_marks_finalized`, whose count is the
    // previous mount's leftovers - this one is this mount's own.
    std::uint64_t catalog_marks_purged_ = 0;
    // Records that this transaction now holds uncommitted catalog rows,
    // which is what turns on `ViewFor`'s filtering.
    void MarkHoldsDdl(const txn::Transaction& txn);

    // `CREATE CABIN` / `DROP CABIN` (docs/spec/cabin.md §10). One handler
    // for both: they share a parse and a reply shape, and differ only in
    // which catalog call they reach. Takes the whole statement line rather
    // than a suffix, because the parser is what resolves the two
    // identifiers.
    DispatchOutcome HandleCabin(std::string_view line);

    // `ALTER TABLE ... RENAME TO | RENAME COLUMN` (docs/spec/alter.md,
    // workplan ALT03). One handler for both forms, for HandleCabin's
    // reason; the AL4 assertion RESTRICT and the AL7 system-relation
    // refusal live here, before the catalog write.
    DispatchOutcome HandleAlter(std::string_view line, Session& session);

    // `DROP TABLE <name>` (docs/spec/drop-table.md, workplan DT03). The
    // DT3 RESTRICT gate lives here - a referencing foreign key and an
    // assertion each refuse naming the blocker - before the catalog's
    // tombstone-and-retire; the in-memory Cabin sets are forgotten after.
    DispatchOutcome HandleDropTable(std::string_view line, Session& session);

    // `SHOW CABINS` - every declared Cabin, with what it has observed.
    //
    // The line joins two sources on purpose. The catalog says which
    // relation and column, who declared it, and whether it is serving - the
    // *declaration*, which is DDL and survives a restart. The core-local
    // store says how many values are observed, how many entries they hold,
    // and how the probes have gone - runtime state, which by §9 does not
    // survive a restart at all. Reporting them together is what makes "this
    // Cabin exists but has never been probed" visible.
    DispatchOutcome HandleShowCabins();

    // `CREATE INDEX` / `DROP INDEX` (docs/spec/index.md §10). One handler
    // for both, for HandleCabin's reason: they share a parse and a reply
    // shape and differ only in which catalog call they reach.
    // Emits one INDEX_INSERT per index mutation, or a full page image per
    // page a split restructured. Called **before** the HEAP_INSERT or
    // HEAP_OVERWRITE the entries point at (docs/spec/index.md §12.1): a
    // dangling entry is dropped by verification, a row with no entry is
    // lost.
    Status LogIndexWrites(const std::vector<exec::IndexWrite>& writes, std::uint64_t txn_id);

    DispatchOutcome HandleIndex(std::string_view line, Session& session);
    // The foreign arm's two phases (PW1c-6b-3). Phase 1: the refusal inside
    // an explicit transaction, the definition under the session's view with
    // the oid issued, the request sent, the outcome returned pending. Phase
    // 2, once `IndexBuildClient::Settled`: the owner's root read, the
    // `sys.indexes` row written with no anchor seed under a DDL scope, the
    // commit, `done` - or the timeout / the owner's refusal as the error
    // and `done(aborted)`. Phase 2 stages its commit through
    // `pending_commit_lsn_` exactly as DispatchAndStage does, so the
    // caller's durability wait is unchanged.
    DispatchOutcome BeginForeignIndexBuild(const parser::IndexStmt& stmt,
                                           std::uint32_t owner_core, Session& session);
    DispatchOutcome FinishIndexBuild(const PendingIndexBuild& build, Session& session);
    DispatchOutcome HandleShowIndexes(Session& session);

    // `CREATE ASSERTION` / `DROP ASSERTION` (docs/spec/assertion.md §3,
    // workplan AST03). One handler for both, for HandleCabin's reason.
    //
    // Validates the declaration against the catalog (§3.1), builds the
    // Bound Cabin (AST06), publishes the row and adopts the live directory
    // into this core's registry, which is what makes the reply's
    // `enforcing=1` true rather than a claim about a row.
    //
    // **On a relation another core owns the cabin is built there**
    // (PW1c-6c, the two phases below): every write to the relation appends
    // to the cabin, and only the owner may write the owner's pages.
    // No session: assertions are non-transactional DDL (`ddl-transactional.md`
    // §5), so neither arm has anything to ask it (AK-S1 dropped the last
    // read, the foreign arm's explicit-transaction refusal).
    DispatchOutcome HandleAssertion(std::string_view line);

    // The foreign arm's two phases (PW1c-6c, assertion_build_service.hpp).
    // Phase 1: the checks and the id under `exec::PrepareAssertionDef`, the
    // declaration sent, the outcome returned pending. Phase 2, once
    // `AssertionBuildClient::Settled`: the owner's root read, the
    // `sys.assertions` row published, `done` - or the timeout / the owner's
    // refusal as the error and `done(aborted)`.
    //
    // The live directory is **not** adopted here: it belongs to the core
    // that will append to it, which adopted it at the end of its own build.
    DispatchOutcome BeginForeignAssertionBuild(const parser::AssertionStmt& stmt,
                                               std::uint32_t owner_core);
    DispatchOutcome FinishAssertionBuild(const PendingAssertionBuild& build);

    // `SHOW ASSERTIONS` - every declared assertion, with the relation it is
    // on and its declaration verbatim.
    //
    // The `SHOW` surface rather than `SELECT * FROM sys.assertions`: a
    // catalog *view* is read through `catalog::Catalog` alone, and a
    // row-codec relation's rows need a `PageStore` to resolve their
    // var-heap spills. So the one row-codec catalog relation is surfaced by
    // `SHOW`, which has one. `sys.pattern_defs` had no view for the same
    // reason until it was withdrawn on 2026-08-31.
    DispatchOutcome HandleShowAssertions();
    DispatchOutcome HandleShowRelayout(std::string_view rest);
    DispatchOutcome HandleSetCabinOptimizer(std::string_view rest);

    // `SHOW CABIN_OPTIMIZER` - PO9's view (workplan PHY06): the switch and
    // budget line, the executor's applied-action counters, and one line
    // per managed candidate with its state, last B/C scores, S3 quality
    // rates and last logged action. Everything it prints already exists on
    // an inspection surface (`ManagedEntries`, `DecisionLog`, `counters`,
    // `QualityOf`) - this handler renders and never computes.
    DispatchOutcome HandleShowCabinOptimizer();

    // H6 step 3 (`observability.md` §10): `TRACE ON|OFF` is the manual
    // sampler, `SHOW TRACES` the ring, `SHOW TRACE <id>` one span tree with
    // self-time separated from child-time - which is the number that finds
    // the culprit, where total only says which subtree to open next.
    DispatchOutcome HandleTrace(std::string_view rest);
    DispatchOutcome HandleShowTraces();
    DispatchOutcome HandleShowTrace(std::string_view rest);

    // ---- Foreign-key checks (docs/spec/foreign-keys.md §§2-4) -----------
    //
    // The write paths' three entry points. They live here rather than in
    // `exec/` because they are what turns a verdict into a *reply* - which
    // needs relation names, the access-statistics switch, and the retryable
    // spelling - while the verdicts themselves are `exec::fk_check`'s, so
    // there is exactly one implementation of each check.

    // A read view of **now**, for a constraint check. See §4: not the
    // statement's snapshot, because a check reads latest state.
    StatusOr<txn::ReadView> CheckView(const WriteScope& scope);

    // The forward check for one foreign key and one written value (§2),
    // **answered from what the extraction pass already resolved** (§2a,
    // AH-T1). OK when the value is not an id at all - the row codec has the
    // better error for that.
    //
    // `check_view` is still taken, and is used by exactly one arm: a
    // **self-referencing** foreign key, which `ResolveForeignKeyParents`
    // deliberately does not hoist. See its comment for why that arm is not
    // a hole in AH-R1.
    Status CheckForeignKeyOnWrite(const catalog::TableAccess& child,
                                  const catalog::ForeignKeyRef& fk, const parser::AstValue& value,
                                  const txn::ReadView& check_view,
                                  const exec::FkParentVerdicts& held);

    // The extraction pass (§2a, AH-R1): resolves every parent pk one row's
    // body names, into `into`, deduplicated by (parent relation, pk) so a
    // statement naming one parent from a thousand rows descends once.
    //
    // Called **before any row work** - at the dispatch fork for a statement
    // whose rows are all known there, and once per row otherwise. Nothing it
    // does may depend on a row having been written, which is what makes it
    // legal to run early and what the self-referencing carve-out protects.
    Status ResolveForeignKeyParents(const catalog::TableAccess& child,
                                     const std::vector<parser::AstValue>& body,
                                     const txn::ReadView& check_view,
                                     exec::FkParentVerdicts& into);

    // What the extraction pass deferred because its owner is not this
    // core: **sent** as one probe per owner, and the statement parked
    // (AH-T2). `line` is the statement to resume with; empty means a
    // caller with no text, which refuses instead - the KWP load chunk,
    // which keeps its cross-core refusal exactly as it keeps the
    // shipping one.
    //
    // Fills `out.pending_fk_probe` on success. Enrols each owner as a 2PC
    // participant when this runs inside an explicit transaction, after the
    // send, for the reason the shipping enrolment states: a participant
    // recorded for a request that never left would be prepared for a
    // transaction it holds nothing of.
    // Ends this statement's reference intents where there is nothing to
    // park on - the synchronous dispatch and the fork's own send failure.
    // Always an abort: both callers are refusals, and a statement that did
    // not run has nothing to commit. Autocommit only; see the definition.
    void ReleaseIntentsWithoutWaiting(Session& session);

    // AJ-T1: every row this session registered as about to be deleted,
    // released. Called where the session's transaction ends and **not** at
    // the decide sites beside `ReleaseIntentsWithoutWaiting` above, for the
    // reason its definition states.
    void ClearPendingDeletes(Session& session);

    // AJ-T3's sender. The reverse's counterpart to `SendForeignKeyProbes`
    // below, and shorter by everything the forward does about intents:
    // a reverse round **enrols nobody** (AJ-R5), so there is no
    // `EnrolIntentHolder`, no decide target and no release leg. What holds
    // the window open is the pending-delete registration this core made
    // before calling here, which its own transaction's end clears.
    Status SendReverseForeignKeyProbes(const std::vector<exec::FkReverseProbeGroup>& groups,
                                       Session& session, std::string_view line,
                                       DispatchOutcome& out);

    Status SendForeignKeyProbes(const exec::FkParentVerdicts& held, Session& session,
                                 std::string_view line, DispatchOutcome& out);

    // The refusal that stands where a probe cannot be sent - no client
    // (no reactor), or no text to resume with. Fail-closed: the
    // alternative is `CheckParentPresent` descending a page this core may
    // not fault.
    Status RefuseUnsentForeignKeyProbes(const exec::FkParentVerdicts& held);

    // The body `ResolveForeignKeyParents` and the FK checks index into: the
    // columns after the pk, which is the shape every downstream consumer
    // takes. Mirrors `InsertOneRow`'s arity split without repeating its
    // refusals - a row of neither legal length yields an empty body here and
    // is refused there, in the order it always was.
    static std::vector<parser::AstValue> InsertBodyOf(
        const catalog::TableAccess& ta, const std::vector<parser::AstValue>& values);

    // The reverse check for every foreign key pointing at `parent` (§3),
    // run per row about to be delete-marked.
    // AJ-T3: resolve which of `parent`'s children live on other cores, and
    // if any do, register the row and build one reverse group per foreign
    // owner. Returns the groups still to send - empty when every foreign
    // child is already answered from a resumed round, which is what makes
    // the second pass a plain local statement.
    StatusOr<std::vector<exec::FkReverseProbeGroup>> HoistReverseForeignKeyChecks(
        const catalog::TableAccess& parent, const std::vector<parser::Condition>& where,
        Session& session, exec::FkParentVerdicts& held);

    // `reverse_held` carries AJ-T3's answers for children this core does
    // **not** own: one verdict per (child relation, parent pk), resolved at
    // the fork and read here. A foreign child with no entry is a caller
    // bug and is refused rather than checked locally - the same rule
    // `FkParentVerdicts` states for the forward, and for the same reason:
    // checking locally is precisely the wrong answer, because this core
    // cannot see the rows.
    Status CheckNoChildrenBeforeDelete(const catalog::TableAccess& parent, std::uint64_t parent_pk,
                                       const txn::ReadView& check_view,
                                       const exec::FkParentVerdicts& reverse_held);

    // One access shape, recorded by hand because a check is not a step
    // (FK-M4). Never fails a write.
    void RecordFkAccess(exec::AccessKind kind, catalog::Oid rel_oid, std::uint64_t column_mask);

    // The namespace a `CREATE TABLE ns.t` names, or `kNamespacePublic` for
    // an unqualified name (AF-T3). The one qualifier in the grammar that
    // decides rather than asserts: through AF-T2 it decides the relation's
    // owner core. An unknown namespace is refused with its byte and is
    // **not** created - see `ast.hpp`'s namespace-qualifier rule.
    StatusOr<catalog::Oid> ResolveCreateNamespace(std::string_view qualifier,
                                                  std::uint32_t byte_offset,
                                                  const txn::ReadView* view);

    // A relation's name for a human-readable reply, or `oid=<n>` when it
    // cannot be resolved. Inspection surfaces only: catalog rows store oids
    // so they stay fixed width, and printing one is where the name is
    // needed. Never called from an execute path - resolving a name during
    // execution is what parser-v2.md I11 forbids.
    std::string RelationNameOf(catalog::Oid oid);

    // Every declared foreign key (docs/spec/foreign-keys.md §1). One line
    // per sys.fkeys row: which relation references which, through which
    // column. Prints `action=RESTRICT` unconditionally, because v1 has one
    // action (F2) - a stored action field would have exactly one value.
    DispatchOutcome HandleShowFkeys();
    DispatchOutcome HandleInsert(std::string_view line, Session& session);

    // The statement itself, inside a write scope the wrapper opened and
    // will close. Split so that every early return below is an ordinary
    // return rather than one that has to remember to end a transaction.
    DispatchOutcome InsertInner(std::string_view line, WriteScope& scope);

    // Where one row landed, for the reply.
    struct InsertRowResult {
        std::uint64_t id = 0;
        PageId page_id = kInvalidPageId;
        std::uint16_t slot = 0;
    };

    // The per-row write pipeline, verbatim and in order (bulkinsert.md
    // §4, BI2): arity, FK forward check, assertion admission, id, encode +
    // spill, placement, Cabin witness, index maintenance, reservation,
    // rollback trail, WAL, root repoint. **A refactor of InsertInner's
    // body, not a second write path** - there is exactly one place a row
    // becomes durable state, and it is this one for one row and for a
    // thousand. Returns the full error reply on failure (spellings intact -
    // ErrorReply's leading tokens are a compatibility surface, which is why
    // the bulk loop appends its row ordinal rather than prefixing it).
    //
    // `ta` is a live borrow the callee may *refresh*: a btree level growth
    // repoints the relation's root, which invalidates the catalog cache -
    // harmless on the last row, fatal to the next one, so the pointer is
    // re-borrowed before returning.
    //
    // `fk_held` is what the caller's extraction pass resolved over **every**
    // row of the statement (§2a): this function answers from it and starts
    // no descent of its own, which is what makes a bulk insert against one
    // parent cost one.
    std::optional<std::string> InsertOneRow(catalog::Oid oid, const catalog::TableAccess*& ta,
                                            const std::vector<parser::AstValue>& values,
                                            WriteScope& scope,
                                            const exec::FkParentVerdicts& fk_held,
                                            InsertRowResult& out);

    // T3, the sorted heap fill (docs/inflight/in-progress/workplan-t3.md). The gate is T3-2's,
    // conservative and only able to widen: heap-clustered, nothing that
    // maintains per-row (no index, no Cabin, no assertion), no spillable
    // schema. FK stays allowed - its checks run per row before anything
    // burns. Outside the gate the row loop runs, with byte-identical
    // replies and relation state - the equivalence test is the contract.
    bool SortedFillEligible(const catalog::TableAccess& ta, catalog::Oid oid) const;
    // `line` is carried only so a foreign-key probe has a statement to
    // resume with (AH-T2). Empty for a caller with no text - the KWP load
    // chunk - which then keeps the refusal, exactly as it keeps the
    // shipping one.
    DispatchOutcome SortedFillInner(const parser::InsertStmt& stmt, std::string_view line,
                                    catalog::Oid oid, const catalog::TableAccess& ta,
                                    WriteScope& scope);

    // The already-parsed half of InsertInner: everything after the parse -
    // cap, manager guard, resolution, affinity, the T3 gate, the row loop.
    // Split out for the KWP load session (docs/inflight/in-progress/workplan-kwp-load.md KW5),
    // whose rows arrive binary and never had text - BI2's "same write
    // path" made literal, since this IS the path a T1 statement takes.
    // `line` is the statement's text, for the one thing only text can do:
    // be shipped to another core (SS2). Empty from the KWP load path, whose
    // rows never were text - so that path keeps the cross-core refusal it
    // has always had, structurally rather than by a flag.
    DispatchOutcome InsertParsed(const parser::InsertStmt& stmt, WriteScope& scope,
                                 std::string_view line);

public:
    // KW5's public seam: run one parsed INSERT under `session` exactly as
    // HandleInsert runs a textual one - same write scope, same verdict
    // rule, same atomicity. The load session synthesizes an InsertStmt per
    // chunk and calls this.
    DispatchOutcome ExecuteInsert(const parser::InsertStmt& stmt, Session& session);

    // For the sibling platform layers (the KWP load endpoint's schema
    // reads). The catalog's own discipline applies unchanged.
    catalog::Catalog& catalog() noexcept { return catalog_; }

private:
    // `analyze` switches the reply from rows to the compiled plan plus
    // the per-step counters the run produced. Everything before that -
    // parse, compile, execute - is the same code on the same statement
    // text, which is the point: an ANALYZE that took a different path
    // would describe a run nobody performed.
    //
    // `line` is always the *stripped* statement, never the ANALYZE-
    // prefixed text. Dispatch() strips the keyword before anything sees
    // the line, so a fingerprint taken anywhere below here is the same
    // one the unprefixed statement would produce - which is what keeps
    // `sys.patterns` and a Waystone trail from splitting in two over a
    // diagnostic prefix.
    DispatchOutcome HandleSelect(std::string_view line, Session& session,
                                 bool analyze = false);

    // The ANALYZE reply: run the chain for its counters, print the plan
    // beside them. Split out so HandleSelect's row-formatting path and
    // this one visibly share everything above the sink.
    //
    // `sql` is the stripped statement, taken so the reply can report the
    // statement's `pattern_id` - the same number `SHOW PATTERNS` lists a
    // row under, which is how an operator checks which observed pattern a
    // statement actually matched.
    // `trail` and `replay` are the same two halves an ordinary execution
    // gets. ANALYZE takes them because its contract is that the run it
    // describes is the run that actually happened: a diagnostic that
    // skipped replay would report descents no real execution performs.
    //
    // It takes no statement text: the `pattern_id` it prints comes from
    // `instance`, which the caller got from the parse. It used to re-lex
    // `sql` to recompute a number it had already been handed.
    DispatchOutcome RunAnalyze(const exec::StepChain& chain, exec::TrailCollector* trail,
                               const exec::TrailReplay* replay,
                               const std::optional<stats::InstanceKey>& instance,
                               const txn::Snapshot& snapshot);

public:
    // AG11's caps, from `aggregate_max_groups` / `aggregate_max_distinct`.
    //
    // A setter rather than a fifteenth constructor parameter: the ceiling
    // is read once at boot and never varies per statement, so it does not
    // need to be threaded through every test's construction - and the
    // defaults are the spec's `[PROPOSED]` numbers, so a dispatcher that is
    // never told behaves exactly as the documented configuration does.
    void set_aggregate_limits(exec::AggregateLimits limits) noexcept {
        aggregate_limits_ = limits;
    }

    // `sort_max_rows`, from the config. A setter for the same reason.
    void set_sort_max_rows(std::size_t rows) noexcept { sort_max_rows_ = rows; }
    // Set on the statement budget template directly: the knob rides
    // `Budget` into every runner and sub-chain (exec/budget.hpp), so the
    // dispatcher needs no member of its own for it.
    void set_join_build_max_rows(std::size_t rows) noexcept {
        budget_.set_join_build_max_rows(rows);
    }

    // Arms the remote-read path (workplan P4c): a single-step star SELECT
    // of a relation another core owns ships to that core instead of taking
    // the affinity refusal. `client` must outlive the dispatcher. With
    // this never called, every statement behaves exactly as before.
    void SetRemoteReads(SessionStepClient* client) noexcept { remote_reads_ = client; }

    // Whether this core's catalog belongs to another core (see
    // `catalog_read_only_`). Called by CoreRuntime::Open for every
    // non-system core, before the first statement can arrive; a
    // dispatcher never told behaves exactly as it did before PW4.
    void SetCatalogReadOnly(bool read_only) noexcept { catalog_read_only_ = read_only; }

    // Where CheckWriteAffinity records that a relation this core owns has
    // no write rights here (PW1c-7, core_affinity.hpp). Installed by
    // CoreRuntime::Open on every non-system core, beside
    // SetCatalogReadOnly; a dispatcher never told skips the probe. `demand`
    // must outlive this.
    void SetRelationGrantDemand(RelationGrantDemand* demand) noexcept { grant_demand_ = demand; }

    // RD5's `range_size_ids`, which is the same key as "are ranges armed"
    // because a range **is** a lease grant (`server/range_alloc.hpp` says
    // why one key sizes both, and why a second name for it is forbidden).
    // The dispatcher reads it for one question only - whether a foreign
    // INSERT should leave a demand behind it (R4/IS1) - and an instance
    // that never sets it keeps `kRangeSizeOff`, which is every dispatcher
    // built outside `CoreRuntime::Open`.
    void set_range_size_ids(std::uint64_t ids) noexcept { range_size_ids_ = ids; }

    // H6: the core-local trace ring this dispatcher records into when a
    // session has asked for it (`TRACE ON`). A dispatcher never told
    // collects nothing, which is every configuration that does not want the
    // instrument. `sink` must outlive this.
    void SetTraceSink(stats::TraceSink* sink) noexcept { traces_ = sink; }

    // Where CheckWriteAffinity reads that an index of a relation this core
    // owns is being built here (PW1c-6b-2, core_affinity.hpp). Installed
    // beside the demand sink, on the same cores; a dispatcher never told
    // admits every write the shape gate does. `builds` must outlive this.
    void SetPendingIndexBuilds(const PendingIndexBuilds* builds) noexcept {
        pending_index_builds_ = builds;
    }

    // The foreign-key probe client (AH-T2, fk_probe_service.hpp). Without
    // one - a dispatcher with no reactor - a foreign parent is refused
    // rather than asked, which is what every unit fixture sees. `client`
    // must outlive this.
    void SetFkProbes(FkProbeClient* client) noexcept { fk_probes_ = client; }

    // This core's reference-intent table (AH-T3): what a foreign
    // transaction left behind on a parent row this core owns, and what a
    // local `DELETE` of that row must consult before it may proceed.
    // Without one, a delete answers from local evidence alone - which is
    // correct on every core that never grants an intent, and is what a
    // unit fixture is. `intents` must outlive this.
    void SetFkIntents(FkIntentTable* intents) noexcept { fk_intents_ = intents; }

    // AJ-T1's half of the same mechanism, running the other way: the rows
    // this core is about to delete, registered by a DELETE before it fans
    // out to a foreign child's owner and consulted by `FkProbeServer`
    // before it vouches for a parent. Without one a DELETE registers
    // nothing, which is correct on a core no foreign child ever probes -
    // and is what a unit fixture is. `pending` must outlive this.
    void SetFkPendingDeletes(FkPendingDeleteTable* pending) noexcept {
        fk_pending_deletes_ = pending;
    }

    // Arms the foreign arm of CREATE INDEX (PW1c-6b-3,
    // index_build_service.hpp): a relation another core owns has its index
    // built there, with this dispatcher parked between the request and the
    // row. Core 0 only. `client` must outlive the dispatcher. Installed by
    // the Expeditor on every multi-core instance since PW1c-6b-4, which
    // lifted the owner's shape gate in the same step - so what a
    // dispatcher never told refuses is a fixture with no reactor to park
    // on, not production.
    void SetIndexBuilds(IndexBuildClient* client) noexcept { index_builds_ = client; }

    // Arms the foreign arm of CREATE ASSERTION (PW1c-6c,
    // assertion_build_service.hpp), on the same terms and for the same
    // reason as `SetIndexBuilds`. Core 0 only; `client` must outlive the
    // dispatcher. A dispatcher never told refuses the statement by name
    // rather than building a Bound Cabin in the wrong core's pages.
    void SetAssertionBuilds(AssertionBuildClient* client) noexcept {
        assertion_builds_ = client;
    }

    // Arms **statement shipping** (SS2, statement_ship_service.hpp): an
    // autocommit statement whose relation another core owns is carried
    // there and answered back, where without this it is refused
    // (`docs/spec/crosscore.md` §6). Installed on every core of a multi-core
    // instance; `client` must outlive the dispatcher.
    //
    // A dispatcher never told refuses exactly as it did before - which is
    // every single-core instance and every fixture, and is what keeps
    // `cores = 1` byte-identical.
    void SetStatementShip(StatementShipClient* client) noexcept { statement_ship_ = client; }

    // Drops every fact this core caches about the catalog. Wired by
    // `CoreRuntime` to the same `InvalidateCatalog()` the `kCatalogInvalidate`
    // ring handler runs, so a DDL this core shipped and a DDL core 0 was
    // told about converge on one implementation. Unset on a dispatcher
    // nobody wired (the tests' path), where no DDL can be shipped either.
    void SetCatalogInvalidate(std::function<void()> fn) { catalog_invalidate_ = std::move(fn); }

    // **Where this core's access shapes go** (CR7). Unset on core 0, which
    // writes `sys.access_stats` directly because it is the only core that
    // may. Set on a peer, whose accesses are folded here and flushed to
    // core 0 on the reactor tick - and setting it is also what *enables*
    // recording on a peer, which was constructed with it off because there
    // was nowhere to put a shape. `batch` must outlive the dispatcher.
    void SetAccessBatch(stats::AccessBatch* batch) noexcept {
        access_batch_ = batch;
        access_batch_counters_ = batch == nullptr ? nullptr : &batch->counters();
    }

    // **Where this core's `SHOW META` reads its CR7 block from when it is
    // the core that *applies* batches** - core 0, which folds a peer's
    // counts into `sys.access_stats` and owns the counters the handler
    // fills. A peer sets the pointer through `SetAccessBatch` instead,
    // because its batch carries its own; the block is one block either way,
    // and which half of it is non-zero says which end of the wire this core
    // is.
    void SetAccessStatsApplied(const stats::AccessBatchCounters* counters) noexcept {
        access_batch_counters_ = counters;
    }

    // Arms the **coordinator's half of the cross-owner commit** (R6-3,
    // txn_2pc_service.hpp): a `COMMIT` of a transaction that enrolled
    // participants runs D4's two phases instead of committing straight
    // away. Installed on every core of a multi-core instance beside
    // `SetStatementShip`; `client` must outlive the dispatcher.
    //
    // A dispatcher never told has no participants to prepare either - a
    // session enrols one only where a statement shipped inside a
    // transaction - so a single-core instance and every fixture keep the
    // path they had, byte for byte. That is D1's fast path stated as a
    // wiring property rather than as a branch.
    void SetTxn2pc(Txn2pcClient* client) noexcept { txn_2pc_ = client; }

    // ---- R6-5: D5's bounded wait, the one function it is reached through -
    //
    // How long a writer of a row held by an in-doubt transaction waits
    // before it is refused by name. `kTxnInDoubtCeilingNs` is the default
    // and carries the derivation; `in_doubt_ceiling_ms` is the config key
    // that sweeps it, per the ratification's "a named constant reached
    // through one function, and config-swept". **The writer's block is
    // what this sweeps, and only that**: the participant's *ask cadence*
    // (`ShippedStatementExecutor::ExpireEnrolled`) reads
    // `kTxnInDoubtCeilingNs` at the constant and is not swept with it, on
    // purpose - the two are the same number by derivation but not the same
    // quantity, and a sweep to 0 that means "refuse a writer at once" would
    // mean "ask the coordinator every reactor tick" on the other. Sweeping
    // the ask cadence is its own knob and nothing needs one yet.
    //
    // 0 is not an off-switch and is not special: it means a writer waits no
    // time at all and is refused immediately, which is the *other* branch of
    // D5's `[OPEN]` - "refuse retryably up front" - reachable by
    // configuration for anyone who wants to measure the two against each
    // other. It is not the server's default, and the operator ratified the
    // block; it *is* what an unconfigured dispatcher holds, for the reason
    // at the member's declaration.
    sched::MonoTimeNs InDoubtCeilingNs() const noexcept { return in_doubt_ceiling_ns_; }
    void set_in_doubt_ceiling_ns(sched::MonoTimeNs ns) noexcept { in_doubt_ceiling_ns_ = ns; }

    // The **owner's** half, for `SHOW META` only (D7): this core executes
    // other cores' statements, and nothing else in this class would ever
    // read that. A pointer rather than a counters struct because the
    // executor already owns the numbers and a second copy of them is a
    // second thing to keep true.
    //
    // **The borrow is withdrawn, not outlived.** `CoreRuntime` declares the
    // executor *below* the dispatcher - the server holds the executor's
    // `Seam()`, which fixes that order - so reverse destruction drops the
    // executor first and the dispatcher would keep a dangling pointer.
    // Both holders therefore call this with `nullptr` at teardown:
    // `~CoreRuntime`'s body, and `Expeditor::Serve`'s `ClearReactorBorrows`
    // guard. Same rule as `SetStatementShip` beside it.
    void SetShippedStatements(const ShippedStatementExecutor* executor) noexcept {
        shipped_statements_ = executor;
    }

    // The physical optimizer's shadow surface (docs/spec/physical-optimizer.md
    // R3/R10, workplan PX06). A setter for `set_aggregate_limits`'s reason,
    // with the same default posture: a dispatcher never told behaves as the
    // documented configuration - shadow on, the spec's `[PROPOSED]` 600 s
    // half-life. `on` never reaches here: the config layer refuses it at
    // startup naming §6's gates.
    void set_relayout(PhysicalOptimizerMode mode, sched::MonoTimeNs half_life_ns) noexcept {
        relayout_mode_ = mode;
        decay_half_life_ns_ = half_life_ns;
    }

    // The cabin optimizer's signal collector (workplan PHY01), a setter
    // for the two above's reason. Null - every existing construction site,
    // and any configuration without the collector - records nothing and
    // costs one predicate per successful SELECT.
    void set_optimizer_signals(stats::OptimizerSignals* signals) noexcept {
        optimizer_signals_ = signals;
    }

    // PO8's switch, boot half (workplan PHY05): the config key seeds it,
    // SET CABIN_OPTIMIZER flips it at runtime, SHOW META reports it. The
    // consumer is PHY04's cadence task, which reads it at every batch
    // boundary.
    void set_cabin_optimizer_enabled(bool enabled) noexcept {
        cabin_optimizer_enabled_ = enabled;
    }
    bool cabin_optimizer_enabled() const noexcept { return cabin_optimizer_enabled_; }

    // XF4's coordinator legs, for `SHOW META` and for the tests that assert
    // a one-owner commit records nothing. Read-only: nothing outside the
    // parked commit block may write them.
    const CoordinatorCommitStats& xowner_commit_stats() const noexcept { return xowner_commit_; }

    // What the mount's recovery did, for `SHOW META` (RC09). A pointer into
    // the report the mount owns - `Expeditor::recovery_`, which outlives this
    // dispatcher - and null everywhere that mounts nothing, where SHOW META
    // then omits the block rather than printing zeroes that read as "recovery
    // ran and found nothing".
    void set_recovery(const MountRecovery* recovery) noexcept { recovery_ = recovery; }

    // What this core's lease refills cost, for `SHOW META` on a peer
    // (lease_refill_stats.hpp): pointers into CoreRuntime's three refill
    // states, which outlive this dispatcher; null on core 0, which leases
    // from nobody, and everywhere the block is then omitted rather than
    // printed as zeroes.
    void set_lease_refill_stats(const LeaseRefillStats* extent, const LeaseRefillStats* trx_id,
                                const LeaseRefillStats* row_id) noexcept {
        extent_refill_stats_ = extent;
        trx_id_refill_stats_ = trx_id;
        row_id_refill_stats_ = row_id;
    }

    // This core's reactor, for `SHOW META`'s group-accounting block
    // (`docs/spec/sched.md` §4's last bullet, owed since `bench/v2.1.0` §11-5).
    // The scheduler outlives this dispatcher on every core: core 0's is a
    // local in `Expeditor::Serve`, a peer's is `CoreRuntime::scheduler_`.
    // Null wherever no reactor runs the dispatcher - every socket-free test
    // - and the block is then omitted rather than printed as zeroes, the
    // rule the recovery block already follows.
    void set_scheduler_view(const sched::Scheduler* scheduler) noexcept {
        scheduler_view_ = scheduler;
    }

    // The assertion registry, exposed for the two things only a mount does:
    // refilling it after recovery (RC07's `ResumeAssertionsAfterRecovery`) and
    // handing it to the checkpointer as AS6a's snapshot source. Every other
    // caller reaches assertions through the write paths on this class, which is
    // why this is the only accessor and why it is not const.
    exec::AssertionEnforcer& assertions() noexcept { return enforcer_; }

    // RD5's decline counters, written on the drain tick (CoreRuntime) and
    // printed by `SHOW META`. Non-const because the one writer is not a
    // statement, so it cannot go through a statement path.
    RangeSplitDeclineCounters& range_split_declines() noexcept { return range_split_declines_; }

    // SB-R4: what CC10's pre-grant Cabin discard dropped, keyed by
    // relation. Filled by core 0's row-id lease handler, which is where
    // the split runs, and read here because `SHOW META` is where its
    // sibling counters are.
    CabinSplitDiscardCounters& cabin_split_discards() noexcept { return cabin_split_discards_; }

    // The view's two sources (workplan PHY06), a setter for
    // `set_optimizer_signals`'s reason. Both null - every construction
    // site without the controller - and `SHOW CABIN_OPTIMIZER` then
    // reports the surface as absent rather than printing zeros wearing a
    // fresh face (SHOW ASSERTIONS' rule).
    void set_cabin_optimizer_view(const stats::CabinOptimizer* controller,
                                  const exec::CabinOptimizerExecutor* executor) noexcept {
        cabin_controller_ = controller;
        cabin_executor_ = executor;
    }

private:
    // The aggregated SELECT path (docs/spec/aggregate.md AG1): the same
    // execution, with an `Aggregator` in the sink and the fold's output
    // emitted after it. `header` is the column-heading line the caller
    // already built.
    //
    // A sibling of RunAnalyze rather than a branch inside the row loop, for
    // the reason ANALYZE is one: the two differ in what consumes the rows
    // and in nothing else, and a per-row `if` would put that difference
    // where it is paid for on every row of every statement.
    // `os` is the caller's buffer, already holding the column-heading line -
    // taken by reference rather than as a copied header, because building a
    // second `std::ostringstream` costs a stringbuf and a locale and was
    // measured as most of the fold's per-statement overhead (AP03).
    // `sink` is where the rows go; `text_sink` is the same object when
    // nothing else was installed, and is what the reply is taken from.
    // Two references to one thing on the newline path, because a sink that
    // is somebody else's has no reply to give back.
    DispatchOutcome RunAggregated(ResultSink& sink, TextResultSink& text_sink,
                                  const exec::StepChain& chain, exec::TrailCollector* trail,
                                  const exec::TrailReplay* replay,
                                  const std::optional<stats::InstanceKey>& instance,
                                  const txn::Snapshot& snapshot);

    // **The success-path recording point.** Three collectors observe the
    // same moment - a completed execution - and they are called from one
    // place so a fourth cannot be added to two of the three sites. Every
    // caller reaches here only after the execution succeeded; there is
    // deliberately no failure-path form (see RecordTrail).
    void RecordExecution(const std::optional<stats::InstanceKey>& instance,
                         exec::TrailCollector* trail, const exec::StepChain& chain,
                         const exec::ExecStats& stats);

    // Hands a successful execution's trail to the recorder. Shared by the
    // row-returning path and ANALYZE so the two cannot come to disagree
    // about when a trail is written.
    void RecordTrail(const std::optional<stats::InstanceKey>& instance,
                     exec::TrailCollector* trail, const exec::StepChain& chain);

    // Counts one execution of every step's access shape. Shared by the
    // row-returning path and ANALYZE, for the reason RecordTrail is: two
    // call sites that could disagree about when a statistic is written
    // would make the statistic mean two things.
    void RecordAccessShapes(const exec::StepChain& chain);

    // The cabin optimizer's S1/S2 (physical-optimizer.md §II.2,
    // workplan PHY01): one decayed touch per successful fingerprinted
    // SELECT, carrying the statement's page count. Beside RecordTrail and
    // RecordAccessShapes because it is the same moment - a completed
    // execution - observed by a third collector.
    void RecordOptimizerSignals(const std::optional<stats::InstanceKey>& instance,
                                const exec::StepChain& chain, const exec::ExecStats& stats);

    // ---- The Cabin write hook (docs/spec/cabin.md §5) --------------------
    //
    // **This is what "observed ⇒ complete" costs**, and the whole reason a
    // Cabin can be authoritative where a Waystone trail cannot: absence has
    // a witness, and this is the witness. One directory probe per cabined
    // column per write - core-local, in-memory, O(1), and skipped entirely
    // by the `cabin_mask == 0` test for a relation with no Cabin.
    //
    // Every mandatory action is an **append**. Nothing is ever removed here:
    // an older snapshot may still be entitled to match a row through the
    // undo chain, so eager removal is *incorrect* and not merely
    // unnecessary. The surplus is subtracted at read time by verification.
    //
    // `values[i]` is the value of column `first_col_pos + i`, which lets
    // INSERT pass the VALUES list (whose first entry is column 1, since the
    // pk is engine-issued) and UPDATE pass the whole decoded row. `pk`,
    // `page_id` and `slot` are the tuple's identity and its location - both
    // already in hand at both call sites, which is why C6's hints cost
    // nothing to produce.
    //
    // `previous`, when non-empty, is the row **before** the write, indexed
    // the same way. It is what implements §5's third row - an UPDATE that
    // did not touch the key column does nothing - and it is not an
    // optimization: appending on every write is correct (the set stays a
    // superset) but unbounded, so a relation updated often enough would
    // grow one value's set until the cap un-observed it. INSERT passes
    // nothing, having no previous row.
    //
    // Never fails: a Cabin that cannot witness a write un-observes the value
    // instead, which returns it to the authoritative scan path (§1's
    // corollary) and is always legal.
    void NoteCabinWrite(const catalog::TableAccess& access,
                        std::span<const parser::AstValue> values, std::uint16_t first_col_pos,
                        std::uint64_t pk, PageId page_id, std::uint16_t slot,
                        std::span<const parser::AstValue> previous = {});
    DispatchOutcome HandleUpdate(std::string_view line, Session& session);

    // `DELETE FROM <t> [WHERE ...]` (docs/spec/txn.md sections 4.3, 6).
    //
    // A **delete-mark**, never a physical removal: the slot keeps its bytes
    // and gains kSlotFlagDeleted, and the deleter's id goes in the tuple's
    // writer field. That pair is the whole of DELETE in the no-xmax model,
    // and it is why an older snapshot still reads the row - it steps back
    // over the kDeleteMark undo record and finds the tuple's own payload
    // unchanged.
    //
    // **The Cabin write hook is deliberately not called here.** By
    // cabin.md section 5 removal is forbidden: an older snapshot may
    // still be entitled to match the row through the undo chain, so
    // dropping its entry would break the superset invariant. The surplus is
    // subtracted at read time, which now includes the visibility predicate.
    DispatchOutcome HandleDelete(std::string_view line, Session& session);
    DispatchOutcome DeleteInner(std::string_view line, WriteScope& scope,
                                const txn::Snapshot& snapshot);
    DispatchOutcome UpdateInner(std::string_view line, WriteScope& scope,
                                const txn::Snapshot& snapshot);
    DispatchOutcome HandleSync();

    // Runs the insert against whichever storage the relation uses, and
    // reports the result in the vocabulary both share
    // (storage/insert_placement.hpp).
    StatusOr<storage::InsertPlacement> InsertIntoRelation(const catalog::TableAccess& access,
                                                          std::uint64_t id,
                                                          std::span<const std::byte> payload,
                                                          std::uint64_t trx_id);

    // **R4/IS2: a row may only be placed in a range this core owns**, asked
    // at the id rather than left to the store's `MayWrite` backstop naming
    // a page number. Two heap insert paths reach a chain head and neither
    // goes through the other - the per-row `InsertIntoRelation` and
    // `SortedFillInner`'s batch - so both ask, and they ask through one
    // function because two spellings of this refusal is two chances for
    // one of them to be forgotten (it was: the fill wrote a batch into the
    // top range, which on a spread relation is the last core to have
    // leased a block).
    //
    // **Every caller keeps it behind `ranges.empty()`** and this function
    // does not re-test it, because CD1's zero-cost invariant is measured on
    // the unsplit insert line: it must reach the chain having paid one
    // predictable branch on a cached field, never an out-of-line call.
    Status CheckRangePlacement(const catalog::TableAccess& access, std::uint64_t id) const;

    // A full ordered scan of the relation, whichever storage it uses. Both
    // walk sibling/next links left to right, so the row order is identical.
    //
    // `page_access` must be kWrite whenever `fn` modifies a tuple - UPDATE
    // and DELETE scan through here - and kRead otherwise, which is what
    // keeps a SELECT from dirtying every page it reads (page_store.hpp).
    //
    // `fn` returns storage::VisitControl: kStop ends the scan successfully,
    // which is what `LIMIT` and an `Exists` step will need and what no
    // caller here does yet.
    // `SELECT ... FROM sys.<view>`. Answered without the compiler: a
    // catalog view is materialized from the catalog's typed readers, not
    // walked out of pages, so it is not a relation a step can read
    // (exec/catalog_view.hpp).
    DispatchOutcome HandleCatalogView(const parser::SelectStmt& stmt);

    // `span` is the pk window the statement can possibly touch (R4/IS4),
    // and it narrows *which ranges are walked* - never which rows match,
    // which stays `fn`'s. `PkSpan::Whole()` is the whole relation, which is
    // what a predicate naming no pk means; a `WHERE pk = k` write passes
    // `PkSpan::Equality`, and on a spread relation that is the difference
    // between walking one range and meeting the ownership refusal on
    // somebody else's. Not defaulted: both callers have an answer, and a
    // default here would let a third one walk every range by omission.
    Status VisitRelation(
        const catalog::TableAccess& access, storage::PageAccess page_access,
        const std::function<StatusOr<storage::VisitControl>(PageId, heap::PageView&,
                                                            std::uint16_t)>& fn,
        catalog::PkSpan span);

    // Appends the record set above for one placed tuple, stamps page_lsn
    // on every page it touched, and applies the durability class. A no-op
    // returning OK when no WalManager was supplied.
    //
    // A failure here is reported to the client and the tuple stays in the
    // page frame: the mutation happened, and the record describing it did
    // not. That is a lost write on a crash, not a wrong answer now, and
    // the alternative - unwinding a heap insert with no transaction
    // manager to unwind it - would be the worse lie. The WAL gate still
    // holds, because an unstamped page carries page_lsn 0 and a page whose
    // records failed to append is indistinguishable from one nothing
    // logged; closing that needs the abort path a transaction layer owns.
    // `leaf_type` is the page type a PAGE_INIT record names for a new tuple
    // page: kHeap for a chain, kBtreeLeaf for a tree.
    // `spills` are the var-heap values this tuple's cells point at. They
    // are logged *first*, before the HEAP_INSERT, which is the ordering
    // docs/rules/rule-fixed-length-tuple.md section 5 requires: a replay must
    // never reach a tuple whose pointer resolves to nothing. A crash
    // between the two leaves an unreferenced value for purge, which is the
    // harmless direction.
    // `own_txn` false means a TransactionManager owns the transaction and
    // has already logged TXN_BEGIN; this emits only the page records and
    // leaves TXN_COMMIT and its durability wait to EndWrite().
    // `owner_oid` (page.md §2a): the target relation's oid, carried by any
    // PAGE_INIT this insert emits so redo re-stamps what the live path
    // stamped.
    // Gives every spilled value a rollback: one undo record per spill, in
    // the writing transaction's chain, plus the trail entry a live Abort
    // reads. Both compensate by releasing the slot.
    //
    // **Called before whatever logs the VARHEAP_APPENDs**, because an
    // UNDO_WRITE must precede the record it can undo - RV3's rule for
    // catalog writes, for the same reason: redo alone must never resurrect
    // an append the undo phase has no record to release.
    //
    // A scope with no transaction records nothing, which is the pre-existing
    // unowned path (a dispatcher built without a manager, and the
    // `kNoTxnId` writes `LogChainInsert` makes for the assertion catalog).
    // Those spills still leak on rollback, and that is stated in
    // `workplan-varchar-char.md` rather than silently true.
    Status NoteSpills(const WriteScope& scope, std::uint32_t rel_oid, std::uint64_t pk,
                      const std::vector<exec::AppendedSpill>& spills);

    Status LogInsert(const storage::InsertPlacement& placed, PageType leaf_type,
                     std::span<const std::byte> tuple, std::uint64_t trx_id,
                     std::uint64_t owner_oid,
                     const std::vector<exec::AppendedSpill>& spills = {},
                     const std::vector<exec::IndexWrite>& index_writes = {},
                     bool own_txn = true);

    // One page's full image, logged and the page stamped behind it.
    //
    // **Four call sites wrote these ten lines identically** - an index split, a
    // var-heap link edit, a heap structural change, and bulk insert's per-page
    // images - and a fifth lives in `assertion_build.cpp`. An image is the
    // instrument for "no record type describes this change", so the pattern
    // recurs by design; what does not need to recur is the stamp, which is the
    // step a copy can silently omit (`redo.cpp` gates every record on
    // `page_lsn`, so a missing stamp is a record that replays when it should
    // not).
    //
    // No-op with no WAL attached, like every other logging helper here.
    Status LogFullPageImage(PageId page_id, std::uint64_t txn_id);

    // Every record a set of var-heap appends owes, in replay order: the
    // PAGE_INIT for a page the append created, the full page image for the
    // tail whose link now reaches it, then the VARHEAP_APPEND for the value.
    //
    // Shared by INSERT and UPDATE deliberately. The first two records were
    // missing entirely and the third was missing on the UPDATE path
    // (`docs/inflight/known-gaps.md`'s var-heap entry), and two copies of this
    // sequence is two chances to lose one of them again.
    //
    // The caller owes the *ordering*: these records precede the HEAP_INSERT or
    // HEAP_OVERWRITE whose cell points at the value, so a replay never reaches
    // a pointer that resolves to nothing (spec §5).

    // What a `WHERE id = <const>` statement should do instead of scanning.
    // The three cases are distinct because the *authority* of the answer
    // differs:
    //
    //   kScan    no shortcut. Scan; the scan is the authoritative path and
    //            produces the same answer. Every heap relation lands here,
    //            having no pk index to descend.
    //   kAt      look at this (page, slot) - a btree descent, which is
    //            authoritative.
    //   kAbsent  **no such row**, on authority. Only a btree descent can
    //            say this, so a heap relation never produces it.
    struct PkLookup {
        enum class Kind { kScan, kAt, kAbsent };
        Kind kind = Kind::kScan;
        TupleLocation at;
    };
    PkLookup LocateByPk(const catalog::TableAccess& access, std::uint64_t pk);

    // The row-relocation callback a rollback needs when a leaf division has
    // moved rows this transaction wrote (txn/manager.hpp's RowLocator).
    // Built per abort, never stored on the manager - see the definition.
    txn::TransactionManager::RowLocator RowLocatorForRollback();

    // The bytes of the page a located tuple sits on, for a reader. Reuses
    // the span the locator carried out when it has one, and fetches
    // read-only when it does not.
    //
    // Read paths only. A writer must go through page_store_.Get() even
    // when TupleLocation::page is populated: the span is the same frame
    // either way, but only Get() marks it dirty, and a write to a frame
    // nothing will write back is a write that never happened.

    // The pk value a WHERE clause is a *bare* equality against, or nullopt
    // if it is anything else - no WHERE, more than one condition, a non-pk
    // column, a non-equality operator, or a non-integer or negative
    // literal.
    //
    // Shared by SELECT and UPDATE so the two cannot disagree about which
    // predicates take the point path. Duplicating this check is how one
    // path ends up descending for a query the other correctly scans.
    std::optional<std::uint64_t> PkEqualityTarget(
        const catalog::TableAccess& access,
        const std::vector<parser::Condition>& where) const;

    // Diagnostics. Levels are chosen so the default (info) is quiet under
    // load: DDL and SYNC are Info because they are rare and consequential,
    // a completed query is Debug, and the per-tuple heap events are Trace.
    // Enabling trace on a busy server costs a write() per tuple - it is a
    // development tool, not an operating mode.
    // Dispatch() wraps this to time it and log the outcome once, in one
    // place, rather than at every return of every handler.
    DispatchOutcome DispatchInner(std::string_view line, Session& session);

    // Formats a completed remote read into the exact reply the local path
    // would have produced (workplan P4c) - same header, same row shape -
    // and closes the read. Call only when the read is done.
    // Frames the reply for a whole fan-in: the header once, then every
    // stage's rows in `tags` order. Closes each read whatever the outcome,
    // because a read left open holds its batches for the session's life.
    // `render` says whether the chain's own projection or fold produces the
    // reply (AG3); default-empty is the star read this began as.
    // `sink` is the session's (`Session::result_sink`), passed rather than
    // read off a member: this runs **after a park**, and a per-dispatcher
    // pointer would by then be whichever connection's statement ran while
    // this one waited.
    DispatchOutcome FinishRemoteReads(ResultSink* sink, const std::vector<PipelineTag>& tags,
                                      const PendingRemoteRender& render);


    bool logging(LogLevel level) const noexcept {
        return log_ != nullptr && log_->enabled(level);
    }
    sched::MonoTimeNs NowNs() const noexcept { return clock_ == nullptr ? 0 : clock_->Now(); }

    SuperBlock& superblock_;
    catalog::Catalog& catalog_;
    storage::PageStore& page_store_;
    // Whether this dispatcher's catalog is another core's to write
    // (CoreRuntime asymmetry 1: catalog pages have one writer, core 0).
    // Set by CoreRuntime for every non-system core; false everywhere else,
    // including the P4e equivalence harness's stand-in dispatchers, which
    // call themselves core 1 over a writable store precisely because no
    // peer writer exists yet. Gates the PW4 DDL refusal (PeerDdlRefused),
    // CheckWriteAffinity's PW1c-5 shape gate, and the multi-row VALUES
    // refusal - so the name is narrower than the flag: it reads "this
    // core writes no page the system core allocated", the catalog being
    // the first such page.
    bool catalog_read_only_ = false;
    // PW1c-7's demand sink; null on core 0 and on hook-less fixtures.
    RelationGrantDemand* grant_demand_ = nullptr;
    // PW1c-6b-2's window; null on the same cores.
    const PendingIndexBuilds* pending_index_builds_ = nullptr;

    // AH-T2's client, or null where nothing pumps a reactor. Not owned -
    // `CoreRuntime` owns it, and the outlives-the-dispatcher rule the
    // other service pointers carry applies here too.
    FkProbeClient* fk_probes_ = nullptr;

    // AH-T3's half: the intents foreign transactions hold on rows this
    // core owns. Not owned - `CoreRuntime` declares it ahead of the server
    // that fills it, for the reason stated there.
    FkIntentTable* fk_intents_ = nullptr;

    // AJ-T1's mirror: what this core is about to delete. Not owned, for
    // `fk_intents_`'s reason and beside it in `CoreRuntime`.
    FkPendingDeleteTable* fk_pending_deletes_ = nullptr;

    // **The verdicts a parked statement came back with**, consulted by the
    // extraction pass before it resolves anything. Held on the dispatcher
    // for `pending_commit_lsn_`'s reason: one statement runs at a time on
    // a core, so there is no second value to confuse it with, and
    // threading it through `HandleInsert` -> `InsertInner` ->
    // `InsertParsed` -> `SortedFillInner` would put a parameter on four
    // signatures for one path. Cleared at the top of every dispatch, so a
    // statement that did not park sees an empty one.
    exec::FkParentVerdicts resumed_fk_verdicts_;

    // AJ-T3's mirror, and it reuses `FkParentVerdicts` deliberately. What
    // that class actually is - stripped of the forward's naming - is
    // **(relation oid, pk) -> verdict**, resolved once at a fork and read
    // per row, which is exactly what the reverse needs too. The forward
    // reads the oid as the *parent* relation; here it is the **child**
    // relation, and the pk is the parent row being deleted. Only `Find` and
    // `Put` are shared - the grouping half is direction-specific and lives
    // in `exec::FkReverseProbeGroup` - so nothing here has to pretend the
    // two directions ask the same question.
    exec::FkParentVerdicts resumed_fk_reverse_verdicts_;
    // PW1c-6b-3's client, core 0's; null everywhere the PW1c-6 refusal
    // stands (see SetIndexBuilds).
    IndexBuildClient* index_builds_ = nullptr;
    // PW1c-6c's client, core 0's; null on every other core and on a
    // fixture with no ring (see SetAssertionBuilds).
    AssertionBuildClient* assertion_builds_ = nullptr;
    // SS2's client, on every core of a multi-core instance; null wherever
    // the cross-core refusal still stands (see SetStatementShip).
    StatementShipClient* statement_ship_ = nullptr;

    // `SetAccessBatch`. Null on core 0 and on every dispatcher nobody wired.
    stats::AccessBatch* access_batch_ = nullptr;

    // What `SHOW META`'s CR7 block reads: a peer's own batch counters, or on
    // core 0 the handler's applied counts.
    const stats::AccessBatchCounters* access_batch_counters_ = nullptr;

    // `SetCatalogInvalidate`. Empty on a dispatcher nobody wired, which is
    // every test fixture and every single-core instance - both of which are
    // also instances where no DDL is ever shipped.
    std::function<void()> catalog_invalidate_;
    // R6-3's coordinator half; null on a single-core instance and every
    // fixture, which is also where no session ever has a participant.
    Txn2pcClient* txn_2pc_ = nullptr;
    // D7's owner-side reporting; null on a core that answers for nobody.
    const ShippedStatementExecutor* shipped_statements_ = nullptr;
    // Whether the statement running right now can park (set by
    // `DispatchAsync`, never by `Dispatch`). One statement runs at a time
    // per core (sched.md §3), which is what makes a member the right place
    // for it - the same argument `pending_commit_lsn_` makes one line up.
    bool may_park_ = false;
    // Where the statement now in flight owes a D2 commit's acknowledgement
    // (see `CommitAck`). A member for `may_park_`'s reason; unlike it, the
    // stamp is scoped, because leaking this one drops a durability wait
    // rather than granting a parking allowance.
    CommitAck commit_ack_ = CommitAck::kWhenDurable;

    // The stamp, and the whole of its lifetime. Restores rather than
    // assigning the default, so a nested dispatch would compose - there is
    // none today, and a guard that assumed so would be the kind of thing
    // that stops being true quietly.
    class CommitAckScope {
    public:
        CommitAckScope(CommandDispatcher& owner, CommitAck ack) noexcept
            : owner_(owner), saved_(owner.commit_ack_) {
            owner_.commit_ack_ = ack;
        }
        ~CommitAckScope() { owner_.commit_ack_ = saved_; }
        CommitAckScope(const CommitAckScope&) = delete;
        CommitAckScope& operator=(const CommitAckScope&) = delete;

    private:
        CommandDispatcher& owner_;
        CommitAck saved_;
    };
    // The next `Session::ship_id()` this core mints. From 1, because 0 is
    // "never shipped"; per core, and paired with the arrival core in the
    // owner's record, which is what makes it unique instance-wide.
    std::uint64_t next_ship_session_id_ = 1;
    Logger* log_;
    const sched::Clock* clock_;
    wal::WalManager* wal_;
    // The **server's** class - the bottom rung of §9's chain. Never read
    // by a commit path directly: `effective_durability_` below is what a
    // commit is owed, and it is this one only for a session that overrode
    // nothing.
    wal::DurabilityClass durability_;

    // The class the statement now in flight commits under
    // (`Session::EffectiveDurability`), stamped once at the top of
    // `DispatchInner` and read by every ack-timing site beneath it.
    //
    // A plain member for `statement_boundary_taken_`'s reason, stated in
    // the same place: one statement runs at a time on a core (sched.md
    // §3), so per-statement state is a member and not a parameter. The
    // alternative was threading a `Session&` into `LogInsert` and
    // `AwaitDdlDurability`, neither of which has one or wants one - and a
    // second copy of the precedence chain at each site, which is the drift
    // `Session::EffectiveDurability` exists to prevent.
    //
    // Initialised to the server's class so a caller that reaches a commit
    // path without a dispatch - there is none today, and the field must
    // still be defined if one appears - behaves exactly as before.
    wal::DurabilityClass effective_durability_ = durability_;

    // The per-statement work ceiling, from `max_rows_touched`. Held by
    // value and handed to each execution, which takes its own copy - so
    // one statement's spend never carries into the next.
    exec::Budget budget_;

    // AG11's caps, handed to every fold this dispatcher runs. Held by value
    // for the reason `budget_` is: a limit is a property of the server's
    // configuration, and reading it per statement from somewhere else would
    // let one statement's fold see a different ceiling from the next.
    // AG07 makes the two numbers config keys; until then they are spec §6's
    // `[PROPOSED]` defaults.
    exec::AggregateLimits aggregate_limits_;

    // The fold, **reused rather than constructed per statement** (workplan
    // AP03). Same reason `trail_scratch_` and `replay_scratch_` beside it
    // are hoisted, and the same shape of measurement: building one per
    // statement cost about 4 microseconds of server CPU on a pk lookup,
    // roughly 6.5% of what that statement spends there, nearly all of it
    // allocation for buffers the previous statement had already sized.
    //
    // `Reset` points it at each statement's spec and labels, which live on
    // that statement's chain - so between statements it holds pointers that
    // are not valid, and nothing may read it there. That is the same
    // contract `trail_scratch_` has with `Clear()`.
    exec::Aggregator aggregator_;

    // The output sort (OB4), hoisted for `aggregator_`'s reason and holding
    // the same contract: `Reset` points it at one statement's keys, and
    // between statements it holds a buffer nothing may read. Statements
    // that wrote no `ORDER BY`, and those whose order the compiler elided,
    // leave it inactive and untouched.
    exec::OutputSort sorter_;

    // `sort_max_rows` - how many rows one sort may hold before the
    // statement is refused. A cap, not a budget: it never truncates.
    std::size_t sort_max_rows_ = exec::kDefaultSortMaxRows;

    // Where a successful SELECT reports the tuples it found, or null when
    // nothing is recording - which is a valid production configuration
    // (`waystone_recording = off`) and the default here, so every
    // socket-free unit test stays recorder-free too.
    //
    stats::TrailRecorder* recorder_ = nullptr;

    // Whether a SELECT may be served from a previously recorded trail
    // (`waystone_replay`). Independent of `recorder_`: replaying trails
    // while recording no new ones is a legitimate configuration, and it is
    // one of the five the advisory-contract suite compares.
    //
    // **Turning this on cannot change a reply.** A trail supplies only a
    // location, which is then read and filtered by the same code a
    // descent's location would have been, and every entry is validated
    // first. Defaults off here so a dispatcher built without one - which
    // is every pre-existing test - behaves exactly as it always did.
    bool replay_enabled_ = false;

    // Reused across statements so recording costs no allocation on the read
    // path - a collector reserves a whole trail's worth of room, and doing
    // that per SELECT is an 8 KB malloc per query. Cleared at the start of
    // each execution, never read between them.
    exec::TrailCollector trail_scratch_{stats::kMaxTrailEntries};

    // The replay index, reused across statements for the same reason.
    exec::TrailReplay replay_scratch_;

    // Whether a successful SELECT records its access shapes
    // (`access_statistics`). Defaults **on**: unlike Waystone this collects
    // input for a decision nobody has made yet, and a physical optimizer
    // that arrives to an empty history is a physical optimizer that has to
    // wait for one.
    bool access_stats_enabled_;
    stats::AccessStatsCounters access_counters_;

    // The core-local Cabin store, or null when cabins are switched off
    // (`cabins = off`). Null is the default here so a dispatcher built
    // without one - which is every pre-existing test - behaves exactly as it
    // always did, and so that "identical replies with cabins on and off" is
    // a property of the structure rather than of the test data.
    stats::CabinStore* cabins_ = nullptr;

    // The live assertions and their reservation bookkeeping (workplan
    // AST06/AST07): CREATE ASSERTION's build moves its LiveAssertion in
    // here, DROP evicts it, the three write paths check and reserve through
    // it, and the commit/abort hooks below settle what a transaction
    // reserved. Core-local like everything on this dispatcher
    // (assertion.md §6.1). The entry *pages* are durable; this
    // registry is the memory-resident half a restart loses until recovery
    // replays it (AST05's fold) - and SHOW ASSERTIONS derives `enforcing`
    // from its presence, so the loss reports itself instead of hiding.
    exec::AssertionEnforcer enforcer_;

    // The commit a write path staged and did not wait for, read out at the
    // end of DispatchAndStage(). One statement runs at a time on a core, so
    // this cannot hold two.
    wal::Lsn pending_commit_lsn_ = wal::kNoLsn;

    // R6-5's three, on `pending_commit_lsn_`'s terms and for its reason:
    // one statement runs at a time on a core, so a member is exact and
    // threading three values through every `*Inner()` and `EndWrite` would
    // be signatures on a dozen functions for a case that fires on a
    // failure. All three are zeroed at the top of `DispatchAndStage` and
    // read out at its end.
    //
    // `in_doubt_blocker_` is the in-doubt transaction that refused this
    // statement, `in_doubt_blocked_pk_` the row it holds, and
    // `statement_trail_mark_` the transaction's trail length when this
    // statement's write scope opened - which is what says whether the
    // statement wrote anything before it was refused, and therefore whether
    // re-running it is a repeat or a second application.
    std::uint64_t in_doubt_blocker_ = 0;
    std::uint64_t in_doubt_blocked_pk_ = 0;
    std::size_t statement_trail_mark_ = 0;

    // D5's ceiling for this core, `InDoubtCeilingNs()`'s storage.
    //
    // **Zero here, and the number lives in one place** - the constant
    // `kTxnInDoubtCeilingNs` in `server/txn_2pc_service.hpp`, which this
    // header deliberately does not include (see the forward declarations
    // above: it is included nearly everywhere). Every server sets this from
    // the `in_doubt_ceiling_ms` config key, whose default *is* that
    // constant, on both the core-0 and the peer dispatcher. A dispatcher
    // nobody configured therefore does not wait, which is the pre-R6-5
    // behaviour and the right one for a fixture with no cross-owner
    // transactions to be in doubt about.
    sched::MonoTimeNs in_doubt_ceiling_ns_ = 0;

    // The transaction manager, or null when this dispatcher predates
    // transactions - which every socket-free test does, and which is why
    // null must behave exactly as the engine did before MVCC: every write
    // stamped kBootstrapXid, every read seeing everything.
    txn::TransactionManager* txn_ = nullptr;

    // The session a caller who passed none gets. One per dispatcher rather
    // than one per statement so `SET ISOLATION LEVEL` still means something
    // to a single-connection tool, and so an autocommit write does not
    // allocate a session per statement.
    // The level a new session starts at, from the `isolation` config key.
    // Held so TcpServer can stamp it on each connection's session rather
    // than every connection defaulting to the compiled-in level.
    // ---- Core affinity (crosscore.md CC3/§6) ---------------------------
    //
    // Which core this dispatcher runs on, and the refused-write counters
    // §6 asks for. 0 is the system core and the only value a single-core
    // build ever has, so every pre-multicore construction site is unchanged.
    std::uint32_t core_id_ = 0;

    // The session side of remote reads (workplan P4c), null until the
    // Expeditor wires it - with it null every cross-core chain keeps the
    // affinity refusal it always had.
    SessionStepClient* remote_reads_ = nullptr;
    // Per-statement pipeline ids: sequential, never pointer-derived
    // (crosscore.md §3, sched.md §7's determinism rule).
    std::uint64_t next_remote_request_ = 1;

    // RR0 / D3: cross-owner transactions refused because a participant
    // answered from a snapshot other than the one this transaction had been
    // reading it at. Projected by `SHOW META` as `txn_watermark_refusals`,
    // and only when non-zero.
    std::uint64_t watermark_refusals_ = 0;

    // The read-path index switch (`indexes`, default on). Read-path only:
    // maintenance is not switchable, because an index that stops being
    // maintained is wrong rather than slow.
    bool indexes_enabled_ = true;

    // BI3's per-statement row cap, from the `max_insert_rows` config key.
    // A refusal, never a truncation.
    std::uint64_t max_insert_rows_ = parser::kDefaultMaxInsertRows;

    // **The coordinator's per-leg commit times** (XF4,
    // `commit_phase_stats.hpp`). A plain member rather than an injected
    // pointer, unlike the lease refills beside it in `SHOW META`: those are
    // stamped by `CoreRuntime` and the ring handlers and so must live where
    // both can reach them, while every one of these four legs begins and
    // ends inside this class's own parked commit block. Nothing else writes
    // it and nothing off this core reads it.
    CoordinatorCommitStats xowner_commit_;
    // AH-T6's two legs. Walked only on a statement that crosses, so a
    // colocated foreign key and a relation with none read no clock -
    // `CoordinatorCommitStats`'s cost guard, unchanged.
    ForeignKeyRoundStats fk_rounds_;

    // The physical optimizer's mode and R1 half-life (workplan PX06).
    // Shadow costs nothing at rest - the planner is pull-only, computed
    // when `SHOW RELAYOUT` asks - so shadow is the default here as it is
    // in the config.
    PhysicalOptimizerMode relayout_mode_ = PhysicalOptimizerMode::kShadow;
    sched::MonoTimeNs decay_half_life_ns_ = 600'000'000'000ULL;

    // PHY01's collector, and the per-statement counters that feed its S2.
    // The ExecStats is hoisted for the aggregator's reason: `For()` sizes a
    // vector, and a member reused across statements makes the ordinary
    // SELECT allocate nothing for its counting.
    stats::OptimizerSignals* optimizer_signals_ = nullptr;
    exec::ExecStats exec_stats_;
    bool cabin_optimizer_enabled_ = false;  // §II.6: off, experimental
    const MountRecovery* recovery_ = nullptr;  // RC09, set_recovery()
    // A peer's lease refill stats, set_lease_refill_stats(); null on core 0.
    const LeaseRefillStats* extent_refill_stats_ = nullptr;
    const LeaseRefillStats* trx_id_refill_stats_ = nullptr;
    const LeaseRefillStats* row_id_refill_stats_ = nullptr;

    // PHY06's view sources: the controller's managed table and decision
    // log, the executor's applied-action counters. Read-only - the view
    // renders, it never drives - and null wherever the controller was
    // never constructed (`cabins = off`, or a test that wired neither).
    const stats::CabinOptimizer* cabin_controller_ = nullptr;
    const exec::CabinOptimizerExecutor* cabin_executor_ = nullptr;
    CrossCoreWriteCounters cross_core_writes_;
    // C3 (workplan-range-directory.md §9e): declined range openings, per
    // relation and gate. Written by the *runtime's* drain tick rather than
    // by any statement - RD5's allocator is the one caller - and it lives
    // here because this is where `SHOW META` reads its counters from, and
    // because it is `cross_core_writes_`'s neighbour in form and purpose:
    // per core, aggregate, the evidence a placement decision is made from.
    // ---- H6: per-request tracing (`observability.md` §10 steps 1-3) -----
    //
    // `traces_` is the core-local ring; `tracing_` is the session's
    // `TRACE ON`; `trace_` is the context of the statement in flight, null
    // on every untraced one - which is what every `SpanScope` in this file
    // branches on and the whole of the disabled path.
    //
    // Owned here rather than passed, for `pending_commit_lsn_`'s reason
    // stated at `DispatchAndStage`: one statement runs at a time on a core,
    // so there is no second value to confuse it with, and the alternative
    // is a parameter on a dozen signatures. Off by default, so a dispatcher
    // that is never told behaves exactly as it did.
    stats::TraceSink* traces_ = nullptr;
    stats::TraceContext* trace_ = nullptr;
    bool tracing_ = false;

    RangeSplitDeclineCounters range_split_declines_;
    CabinSplitDiscardCounters cabin_split_discards_;
    // `set_range_size_ids`; `kRangeSizeOff` means no range ever opens and this
    // dispatcher's write path is the one it always was.
    std::uint64_t range_size_ids_ = kRangeSizeOff;
    // This core's reactor, set_scheduler_view(); null off a reactor.
    const sched::Scheduler* scheduler_view_ = nullptr;

    // Refuses a write to a relation this core may not write, and binds the
    // transaction's home core on the first one that is allowed. See
    // core_affinity.hpp - the restriction is decided (CC3), not a stand-in
    // for the pipeline.
    // ---- Statement shipping's fork (SS2) -------------------------------
    //
    // **Whether this statement may be shipped at all** - the single home
    // of the fork's conditions, because the four call sites used to carry
    // this argument verbatim and a decision with four homes is a decision
    // nobody can amend.
    //
    // Four questions, each a decision rather than a guard: is shipping
    // armed (a single-core instance and every fixture: no, on a null
    // pointer, which is what keeps that path byte-identical); can the
    // statement park (see `DispatchOutcome::pending_shipped`); is it
    // autocommit (D1 - nothing crosses transaction state, so an explicit
    // transaction keeps its refusal); and did it arrive shipped
    // (session.hpp's hop limit).
    //
    // Shipping is **unconditional** where those hold, per D6: whether to
    // ship or to refuse by load is placement policy (`docs/spec/crosscore.md`
    // §9's open decision) and does not ride along. What it converts is what
    // the pretasks measured as refused - 80-92% of an unrouted client's
    // writes (`bench/v2.1.0/results-shipping-pretasks-v2.1.0-10-g82a2749.md`
    // §9b).
    //
    // Each site adds the one condition only it can ask: a foreign owner, a
    // predicate that names no second relation, and for reads a chain every
    // step of which is on one foreign core.
    bool MayShip(const Session& session) const noexcept;

    // Sends `line` to `owner_core` and returns the outcome that parks on
    // it. Every refusal it can produce happens **before** the send, so a
    // client that sees one knows the statement did not run.
    DispatchOutcome ShipStatement(std::string_view line, catalog::Oid oid,
                                  std::uint32_t owner_core, std::string_view relation,
                                  Session& session, bool read = false);

    // The parked statement's other end: the owner's answer, its deadline,
    // or a waiter that vanished.
    // XG1: puts a typed shipped read's description and rows into the
    // session's sink. Called on the OK arm alone - a partial result set
    // must never reach a client as a whole one.
    Status ForwardAnswerEdge(const PendingShippedStatement& shipped, Session& session);

    DispatchOutcome FinishShippedStatement(const PendingShippedStatement& shipped,
                                          Session& session);

    // ---- R6-3: the coordinator's commit --------------------------------

    // This core's own half of the transaction, ended. Both are the bodies
    // `HandleCommit` and `HandleRollback` have always had, lifted out
    // unchanged so the cross-owner path ends the local transaction through
    // exactly the code a one-owner one does - a second commit path is how
    // two commits stop meaning the same thing.
    //
    // `commit_lsn` answers the record's LSN whatever the durability class,
    // which is what the cross-owner path needs and the local one does not:
    // the decision must be **durable before participants are told**, and
    // `pending_commit_lsn_` is only set under `group`.
    DispatchOutcome CommitLocal(Session& session, wal::Lsn* commit_lsn = nullptr);
    DispatchOutcome RollbackLocal(Session& session);

    // Opens the prepare phase over the session's participants and returns
    // the outcome that parks on it. Every refusal here happens **before**
    // the first prepare leaves, so a client that sees one knows nothing was
    // asked and the transaction is still whole.
    DispatchOutcome PrepareAcrossOwners(Session& session);

    // What the client is told when a participant refuses or is unheard
    // from: one message naming the first participant that did not prepare,
    // in that participant's own words where it gave any. Built before the
    // phase is closed, since closing frees what it reads.
    Status DescribePrepareFailure(const TxnPhaseOutcome* phase) const;

    // The one core that owns every relation this chain reads, when there is
    // one and it is not this core. Nothing otherwise: a chain touching this
    // core's relations cannot run anywhere else, and one spanning two
    // foreign owners is R6's multi-owner statement, which stays refused.
    std::optional<std::uint32_t> SoleForeignOwner(const exec::StepChain& chain);

    // Ends a write scope that wrote nothing because its statement went to
    // another core. Autocommit by D1, so this is `EndWrite`'s abort arm:
    // the transaction holds no page, and the status it carries is never
    // client-visible - the answer is the owner's.
    Status AbandonWriteForShipping(Session& session, WriteScope& scope);

    // **R6-8: the write shape `MayShip` refuses and D4 now admits.** A
    // statement inside an explicit transaction whose relation another core
    // owns: shipped to that owner, which runs it under a transaction it
    // holds open (R6-2), and the owner is recorded as a **participant** so
    // this session's `COMMIT` runs the two phases over it.
    //
    // Separate from `MayShip` rather than folded into it, and the reason is
    // scope rather than tidiness: this admits *writes* only, and its three
    // callers are the three write paths. A cross-core **read** inside a
    // transaction keeps the behaviour it had, because shipping one would
    // enrol a participant to give a snapshot D3's watermark is what makes
    // meaningful - and the watermark is not built. Reads are R6-9's
    // `crosscore.md` question, not this row's.
    bool MayEnrolShip(const Session& session) const noexcept;

    // `target_id`, when present, is the pk of the row this statement is
    // about to place, and it makes the check ask the **range's** owner
    // rather than the relation's (R4/IS2). Absent - every caller but the
    // INSERT path - the two are the same question, and on an unsplit
    // relation they are the same question either way, off `ranges.empty()`.
    //
    // Only the INSERT path passes one because only it knows the id before
    // the row is written: it comes from this core's own lease, and a range
    // **is** a lease grant, which is the whole of why an insert can be
    // routed to a core that does not own the relation.
    // Not defaulted, for `VisitRelation`'s reason: every caller knows
    // whether it has a row id, and a default would make "no id" the answer
    // a fourth write path gave by forgetting to think about it.
    Status CheckWriteAffinity(const catalog::TableAccess& access, std::string_view relation,
                              Session& session, std::optional<std::uint64_t> target_id);

    // **Which core a predicate-shaped write belongs on** (R4/IS4), for the
    // two verbs that name their rows by WHERE rather than by the row they
    // are about to place. Sets `*target_id` when the predicate is a bare pk
    // equality, which is what lets the affinity check and the walk narrow
    // to that one range.
    //
    // Three answers, and the third is a refusal rather than a core:
    //   - no directory: `owner_core`, off `ranges.empty()`, which is the
    //     field this was before ranges existed and costs one branch;
    //   - a pk equality, or a relation whose every range has one owner:
    //     that owner, and the statement ships there or runs here;
    //   - anything else over a **multi-owner** relation: `NotImplemented`,
    //     naming R6. That is the cost of arming spreading and it is stated
    //     rather than discovered - a non-pk-predicate UPDATE or DELETE on a
    //     spread relation stops working until multi-range writes exist.
    //     Refused before a single page is written, never half-applied.
    StatusOr<std::uint32_t> WriteTargetCore(const catalog::TableAccess& access,
                                            const std::vector<parser::Condition>& where,
                                            std::optional<std::uint64_t>* target_id) const;

    // Refuses a read whose chain touches a relation owned by another core.
    // Temporary in a way the write check is not: this is what the step
    // pipeline will replace, and it exists so the refusal names the reason
    // instead of surfacing as a page-store fault.
    Status CheckReadAffinity(const exec::StepChain& chain);

    txn::IsolationLevel default_isolation_ = txn::IsolationLevel::kReadCommitted;

    Session autocommit_session_;

    // Implicit-transaction ids for the statements this dispatcher logs.
    // Process-local and restarting from 1 every boot, which is wrong the
    // moment recovery reads two boots' worth of one stream back - ids from
    // different runs would collide. Allocating them durably is the
    // transaction manager's job (wal.md section 12 has no owner yet), so
    // this is deliberately the cheapest thing that produces a distinct id
    // per statement within a run, and it is a known gap, not an oversight.
    std::uint64_t next_txn_id_ = 1;
};

}  // namespace kds::server
