#include "kds/server/shipped_statement_executor.hpp"

#include <utility>

#include "kds/sched/coro.hpp"

namespace kds::server {

void ShippedStatementExecutor::Execute(StatementShipServer::ShippedStatement statement,
                                       StatementShipServer::ReplyFn reply) {
    // **Before anything is allocated** (D5): the two answers this core can
    // give without running the statement are given here, on a path that
    // takes no session, no transaction and no page.
    Expire();
    const DedupKey key{statement.requester, statement.session_id};
    if (auto it = answered_.find(key); it != answered_.end()) {
        if (it->second.sequence == statement.sequence) {
            ++deduped_;
            if (log_ != nullptr && log_->enabled(LogLevel::kWarn)) {
                log_->Warn("ship", "core " + std::to_string(statement.requester) +
                                       " asked again for session " +
                                       std::to_string(statement.session_id) + " sequence " +
                                       std::to_string(statement.sequence) +
                                       "; answered from the record, not run again");
            }
            reply(it->second.status, it->second.text);
            return;
        }
        if (statement.sequence < it->second.sequence) {
            // Superseded: this core answered a later statement for this
            // session, so whatever it answered for this one is gone. It may
            // have committed. Saying so is D4's whole point - a guess here
            // is a double insert there.
            ++unanswerable_;
            reply(Status::UnknownOutcome(
                      "statement shipping: core " + std::to_string(core_id_) +
                      " has answered sequence " + std::to_string(it->second.sequence) +
                      " for this session and no longer holds the outcome of sequence " +
                      std::to_string(statement.sequence) +
                      "; whether it ran cannot be established from here"),
                  {});
            return;
        }
    }

    auto running = std::make_unique<Running>(std::move(statement.text),
                                             dispatcher_.default_isolation(), statement.role);
    running->requester = statement.requester;
    running->session_id = statement.session_id;
    running->sequence = statement.sequence;
    running->reply = std::move(reply);

    const std::uint64_t id = next_running_id_++;
    Running* state = running.get();
    running_.emplace(id, std::move(running));

    // `kForeground`, because this is a client's statement and the only
    // thing that distinguishes it from a local one is which core its client
    // is on. It is also what puts the shipped population into the §8a
    // scheduler accounting, which is where SS-B4 goes looking for it.
    scheduler_.Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground,
        dispatcher_.DispatchAsync(state->text, &state->session, &state->out),
        [this, id](const Status&) { Finish(id); }));
}

void ShippedStatementExecutor::Finish(std::uint64_t id) {
    auto it = running_.find(id);
    if (it == running_.end()) return;  // unreachable: one completion per statement
    std::unique_ptr<Running> state = std::move(it->second);
    running_.erase(it);
    ++executed_;

    // The rendered line back into a code and a text. An error line carries
    // its message in the status - `ErrorReply` on the arrival core puts it
    // back - so the text goes empty; a success's line **is** the answer.
    Status status = StatusFromErrorReply(state->out.response);
    std::string_view text;
    if (status.ok()) {
        text = state->out.response;
    }

    if (state->out.should_stop) {
        // A statement that ends a session cannot be shipped: the flag is
        // advisory to the *caller*, and this core is not the caller, so
        // honouring it would stop nothing while answering as though it had.
        // Unreachable from the dispatch fork, which ships only statements
        // that name a relation - and refused here rather than trusted to
        // stay unreachable, since the cost of being wrong is a client told
        // its session ended when it did not.
        status = Status::Unsupported(
            "statement shipping: a statement that ends the session may not be shipped; "
            "run it on the core the connection is on");
        text = {};
    }

    Remember(DedupKey{state->requester, state->session_id}, state->sequence, status, text);
    state->reply(status, text);
}

void ShippedStatementExecutor::Remember(const DedupKey& key, std::uint64_t sequence,
                                        const Status& status, std::string_view text) {
    const sched::MonoTimeNs now = clock_.Now();
    Answered& record = answered_[key];
    record.sequence = sequence;
    record.status = status;
    record.text.assign(text);
    record.at_ns = now;
    answered_order_.emplace_back(now, key);

    // The memory bound, under the time bound. Evicting here rather than at
    // the next request keeps the map's size a function of what it holds and
    // not of when it is next asked.
    while (answered_.size() > kShippedDedupMaxRecords && !answered_order_.empty()) {
        const auto [at_ns, oldest] = answered_order_.front();
        answered_order_.pop_front();
        auto it = answered_.find(oldest);
        if (it == answered_.end() || it->second.at_ns != at_ns) continue;  // superseded
        ++early_evictions_;
        if (log_ != nullptr && log_->enabled(LogLevel::kWarn)) {
            log_->Warn("ship", "the shipped-statement dedup record is full (" +
                                   std::to_string(kShippedDedupMaxRecords) +
                                   "); core " + std::to_string(oldest.first) + "'s session " +
                                   std::to_string(oldest.second) +
                                   " was dropped before its retention expired, so a duplicate "
                                   "of it would run again");
        }
        answered_.erase(it);
    }
}

void ShippedStatementExecutor::Expire() {
    const sched::MonoTimeNs now = clock_.Now();
    while (!answered_order_.empty()) {
        const auto [at_ns, key] = answered_order_.front();
        if (now - at_ns < kShippedDedupRetentionNs) return;  // and every later one is younger
        answered_order_.pop_front();
        auto it = answered_.find(key);
        // Stale entry: the session answered again, and the newer entry
        // later in the queue is what holds this key's record now.
        if (it == answered_.end() || it->second.at_ns != at_ns) continue;
        answered_.erase(it);
    }
}

}  // namespace kds::server
