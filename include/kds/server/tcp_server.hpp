#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/server/auth.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/connection_handoff.hpp"
#include "kds/server/kwp_session.hpp"
#include "kds/server/transaction_placement.hpp"
#include "kds/server/wire_channel.hpp"

// The client-facing accept/read/dispatch/write loop - the actual
// "platform layer" doing real socket syscalls (rules.md #4: engine logic
// must go through injectable interfaces for I/O; this file *is* one of
// the boundary places, like main.cpp, allowed to call real syscalls
// directly). All the logic that can be unit-tested without a socket lives
// in CommandDispatcher instead; this file is deliberately as thin as
// possible around the syscalls themselves.
//
// Concurrency: this is a *reactor participant*, not a loop of its own. The
// listening socket and every accepted client are non-blocking fds
// registered with the Scheduler's io backend, and all the work happens in
// handlers the reactor calls (sched.md phase 1), so `system`-group work -
// the checkpointer's cadence above all (wal.md section 11) - gets a turn.
// One server per core: its clients are served cooperatively on that core's
// thread, and no client blocks another.

namespace kds::server {

// **Which protocol a listener speaks** (docs/spec/protocol.md §12,
// protocol-wp.md P13).
//
// One socket layer with two framings, not two servers: the accept, the TLS
// channel, the outbox, the write-interest bookkeeping and the async
// dispatch are the same code either way, and only the framing and the
// dispatch shape differ.
enum class Protocol : std::uint8_t {
    // KWP/1 (docs/spec/protocol.md). The default since KW-D6's cut-over.
    kKwp,
    // The newline text protocol (docs/spec/client-manual.md, appendix).
    // Kept as a documented loopback debug surface behind
    // `--debug-text-port`, off unless a port is configured.
    kText,
};

// The default of `tcp_keepalive_s` (CN-10 U7, the operator's answer of
// 2026-09-29: on by default, one setting). What it means is
// `ConfigureAcceptedSocket`'s.
inline constexpr std::uint32_t kDefaultTcpKeepaliveS = 60;

// The largest idle Linux accepts for `TCP_KEEPIDLE` (`MAX_TCP_KEEPIDLE`,
// include/net/tcp.h). A larger `tcp_keepalive_s` is refused at startup
// rather than clamped.
inline constexpr std::uint32_t kMaxTcpKeepaliveS = 32767;

// The default of `txn_lifetime_ceiling_ms` (AN-R14, docs/spec/txn.md §1):
// an explicit transaction older than this, counted from its `BEGIN`, is
// rolled back by the listener's sweep. The operator's number, provisional
// until AS-E measures the lifetime distribution; 60 s is six times the
// 10 s envelope the engine is built for.
inline constexpr sched::MonoTimeNs kDefaultTxnLifetimeCeilingNs = 60ull * 1000 * 1000 * 1000;

// **The accept queue's depth** (CN-10 §3.1). A handshake completes into
// this queue whether or not the server has called accept yet, and past its
// depth the kernel drops the SYN and the client retransmits a second later -
// so this is the burst a listener absorbs without a stall. 4096 is Linux's
// `SOMAXCONN` since 5.4; the kernel silently caps it at
// `net.core.somaxconn`, so the operator's sysctl is the effective bound.
inline constexpr int kListenBacklog = 4096;

// Every accepted client socket's options, on every listener: `TCP_NODELAY`,
// and TCP keepalive at `keepalive_s` seconds of idle (0 for none) with the
// probe schedule and user timeout derived from it (tcp_server.cpp says how,
// and what it costs). An error is the caller's to fail closed on.
Status ConfigureAcceptedSocket(int fd, std::uint32_t keepalive_s);

// Accepts one readable event on the listener takes before yielding to the
// reactor. Bounded, not an EAGAIN drain, so a flood cannot starve the
// timers (the checkpointer's cadence above all); 64 clears a 1,000-connection
// burst in 16 reactor turns rather than 1,000.
inline constexpr int kMaxAcceptsPerEvent = 64;

// The address every listener binds unless the default port is given
// another by `listen_address`.
inline constexpr const char* kLoopbackAddress = "127.0.0.1";

class TcpServer {
public:
    // Binds and listens on `address`:port - an IPv4 address in dotted
    // form, loopback unless the caller passes the instance's
    // `listen_address`; one that does not parse is InvalidArgument. Fails
    // with IoError if the
    // socket/bind/listen syscalls fail (e.g. the port is already in use).
    // `reuse_port` sets SO_REUSEPORT before bind, so N listeners - one per
    // core - share one port and the kernel distributes accepted
    // connections among them (crosscore.md M3, workplan-peer-writer.md
    // PW5). Every socket on the port must pass it, core 0's included: the
    // option must be set on the first binder or later binds fail. **A
    // platform that refuses the option answers Unsupported**, never a
    // listener that silently binds exclusively - the caller's cue for D19's
    // fallback, core 0 accepting alone and handing off (`set_handoff`).
    static StatusOr<TcpServer> Listen(std::uint16_t port, bool reuse_port = false,
                                      const std::string& address = kLoopbackAddress);

    // Whether `address` is an IPv4 address `Listen` binds, so a config file
    // naming one it would not is refused before anything opens.
    static bool IsListenAddress(const std::string& address);

    // **A server with no socket** (AT-S10c): it accepts nothing and runs the
    // connections handed to it (`Host`). A peer's arrangement under D19's
    // fallback, where core 0 alone can bind the port.
    static TcpServer Hosting() { return TcpServer(-1, /*hosting=*/true); }

    TcpServer(TcpServer&& other) noexcept;
    TcpServer& operator=(TcpServer&& other) noexcept;
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;
    ~TcpServer();

    // Registers the listening socket with `scheduler` and starts accepting.
    // From here on the reactor drives everything: accepted clients are
    // registered too, and readable events turn into frames (KWP) or command
    // lines (text) run through `dispatcher`.
    //
    // A dispatched command setting DispatchOutcome::should_stop runs the
    // stop handler (`set_stop_handler`), or Scheduler::Stop() where none is
    // set - the whole instance, not just that connection.
    //
    // Both references must outlive this TcpServer. Call Detach() before
    // destroying the scheduler if it might outlive this object.
    // `log` is optional; null disables the connection/request diagnostics.
    Status Attach(sched::Scheduler& scheduler, CommandDispatcher& dispatcher,
                   Logger* log = nullptr);

    // Unregisters the listener and every live client, and closes the
    // clients. Idempotent; called by the destructor.
    void Detach() noexcept;

    // ---- D19's fallback (AT-S10c, `connection_handoff.hpp`) --------------
    //
    // Core 0's listener, where the port cannot be shared: each accepted
    // connection goes to `handoff.NextCore()`, and one not placed on core 0
    // is offered to that core's inbox rather than run here. Before Attach();
    // `handoff` must outlive this server.
    void set_handoff(ConnectionHandoff* handoff) noexcept { handoff_ = handoff; }

    // The receiving side, after Attach(): parks a task on this reactor that
    // wakes whenever `core`'s inbox holds a socket and runs each one here,
    // as though this server had accepted it - or, for a live connection
    // another core moved (BL-S2), adopts it and runs the statement it
    // carried. The task outlives nothing it uses - a server moved or
    // detached is told so, and the task ends at its next wake. `handoff`
    // must outlive the scheduler's frames.
    Status Host(ConnectionHandoff& handoff, std::uint32_t core);

    // ---- BL-S2: a session placed per transaction -------------------------
    //
    // Each time a KWP session with no transaction open asks for a dispatch,
    // `placer` names the core that transaction runs on; one other than this
    // server's moves the whole connection there through `handoff`'s inbox
    // (`transaction_placement.hpp`). Before the reactor runs; both must
    // outlive this server, and the destination must `Host` the same handoff.
    void set_mover(ConnectionHandoff* handoff, TransactionPlacer* placer) noexcept {
        mover_ = handoff;
        placer_ = placer;
    }

    // Installs the wire-byte transform every *subsequently accepted*
    // connection runs through (wire_channel.hpp) - in practice the TLS
    // channel, installed by the Expeditor when `tls = on`. Set it before
    // Attach(): connections accepted earlier keep the identity path.
    // Unset (the default) means plaintext.
    using ChannelFactory = std::function<std::unique_ptr<WireChannel>()>;
    void set_channel_factory(ChannelFactory factory) noexcept {
        channel_factory_ = std::move(factory);
    }

    // Installs the authentication gate every subsequently accepted
    // connection must pass (auth.hpp) - in practice the SCRAM gate,
    // installed by the Expeditor when `auth = scram`. Set before
    // Attach(). Unset (the default) means every connection starts
    // authenticated.
    using AuthGateFactory = std::function<std::unique_ptr<AuthGate>()>;
    // What STOP does. Unset (core 0, single-core): Scheduler::Stop() on
    // the attached reactor, which ends Serve() and takes the whole
    // instance down. A *peer* listener must not do that - stopping its own
    // reactor alone leaves a half-dead instance whose bound socket keeps
    // receiving a share of new connections nobody will drain - so
    // CoreRuntime installs a handler that routes the stop to the system
    // core instead, keeping "STOP has always meant the whole server" true
    // on every core.
    void set_stop_handler(std::function<void()> handler) { stop_handler_ = std::move(handler); }

    void set_auth_gate_factory(AuthGateFactory factory) noexcept {
        auth_gate_factory_ = std::move(factory);
    }

    // The protocol every *subsequently accepted* connection speaks. Set
    // before Attach(), like the two factories above.
    void set_protocol(Protocol protocol) noexcept { protocol_ = protocol; }
    Protocol protocol() const noexcept { return protocol_; }
    std::uint32_t keepalive_s() const noexcept { return keepalive_s_; }

    // The server's own class, for `S_TXN_OK`'s RELAXED flag (§9). Only the
    // KWP path reads it; the text protocol reports durability nowhere.
    void set_durability(wal::DurabilityClass durability) noexcept { durability_ = durability; }

    // What this server tells a KWP client about itself, and the seam that
    // mints a session's identifiers.
    //
    // **The default source is a counter, and a counter is not a secret.**
    // §10 makes `cancel_key` the whole of a cancel's authorization - "a
    // wrong key is silently ignored" - so a guessable one lets any peer
    // cancel any session. The default is therefore *not* offered as the
    // `CANCEL` capability: a server with no identity source installed, or
    // no `CancelRegistry` (`ClientSetup::cancels`), negotiates the bit away,
    // and `C_CANCEL` is refused as any unexpected first frame. The
    // Expeditor installs a real source and the instance's registry where
    // OpenSSL is available (BA-S15).
    using IdentitySource = std::function<std::uint64_t()>;
    void set_identity_source(IdentitySource source) noexcept {
        identity_source_ = std::move(source);
    }
    void set_server_info(std::string info) noexcept { server_info_ = std::move(info); }

    // **What every client listener on the instance's port shares** (AT-S8).
    // Every core accepts on the port, so core 0's listener and each peer's
    // are configured from one value: a peer listener missing the TLS channel
    // or the SCRAM gate would serve plaintext, unauthenticated sessions on
    // the port the operator secured. Unset factories and an unset identity
    // source keep their setters' defaults. Before Attach().
    struct ClientSetup {
        Protocol protocol = Protocol::kKwp;
        wal::DurabilityClass durability = wal::DurabilityClass::kGroup;
        IdentitySource identity;
        ChannelFactory channel;
        AuthGateFactory auth;
        // `tcp_keepalive_s` (`ConfigureAcceptedSocket`).
        std::uint32_t keepalive_s = kDefaultTcpKeepaliveS;
        // The instance's cancel registry (§10, BA-S15). Used, and `CANCEL`
        // offered, only beside an identity source: a counter's key is no
        // authorization.
        CancelRegistry* cancels = nullptr;
        // `txn_lifetime_ceiling_ms` (AN-R14). Never 0; the Expeditor
        // refuses it (docs/spec/txn.md §1).
        sched::MonoTimeNs txn_lifetime_ceiling_ns = kDefaultTxnLifetimeCeilingNs;
    };
    void Configure(const ClientSetup& setup) {
        protocol_ = setup.protocol;
        durability_ = setup.durability;
        keepalive_s_ = setup.keepalive_s;
        txn_lifetime_ceiling_ns_ = setup.txn_lifetime_ceiling_ns;
        if (setup.identity) identity_source_ = setup.identity;
        if (setup.cancels != nullptr) cancels_ = setup.cancels;
        if (setup.channel) channel_factory_ = setup.channel;
        if (setup.auth) auth_gate_factory_ = setup.auth;
    }

    // Live client connections, for tests and for the shutdown path.
    std::size_t open_connections() const noexcept { return clients_.size(); }

    // `send()` calls that carried bytes, over the listener's life (BN-R7,
    // `workorder-bn-opt-read-update-path.md`: a
    // statement's reply leaves in one). Reactor-thread state: read it once
    // the reactor has stopped.
    std::uint64_t reply_writes() const noexcept { return reply_writes_; }

private:
    // One client's in-flight buffers. A read can stop mid-line or
    // mid-frame, so the remainder survives in `inbox` until the next
    // readable event; a non-blocking socket can take a partial reply, so
    // the tail survives in `outbox` until the fd is writable again - which
    // is also what lets one write() serve a batch of pipelined commands.
    struct Connection {
        // `inbox` holds *application* bytes (command lines); `outbox`
        // holds *wire* bytes, ready for send(). With no channel the two
        // vocabularies coincide; with one, the channel is the only code
        // that converts between them.
        std::string inbox;
        std::string outbox;
        bool want_writable = false;

        // The wire transform (TLS), or null for plaintext. Owned by the
        // connection: the TLS handshake and record state are exactly as
        // per-connection as the inbox tail is.
        std::unique_ptr<WireChannel> channel;

        // The authentication gate, or null once passed (or never
        // required). Non-null means every line routes to the gate and
        // none reaches the dispatcher - "authenticated" is the gate's
        // absence, so there is no flag to disagree with it.
        std::unique_ptr<AuthGate> auth_gate;

        // ---- One statement at a time (the async dispatch seam) ----------
        //
        // A statement may suspend, so its reply is not ready when
        // DrainCommands returns.
        //
        // **`in_flight` serializes the connection**, required twice over:
        // the session is stateful (an open transaction, the failed-txn
        // flag), and neither protocol has request ids, so replies leave in
        // the order the commands arrived. Pipelining still works - a batch
        // is drained one at a time and the replies leave in one write; only
        // concurrency within one connection is excluded.
        bool in_flight = false;

        // The statement being run, copied out of `inbox` before dispatch.
        // It has to live somewhere stable: the parser's tokens are views
        // into it (parser-v2.md's zero-copy tokens) and the inbox is
        // rewritten as more bytes arrive.
        std::string current_line;

        // The client hung up, or a write failed, while a statement was
        // running. The connection cannot be destroyed yet - the statement
        // holds a reference to its session - so it is marked, its statement
        // asked to stop (a `SELECT`'s walk sees it at its next slice
        // boundary, BA-S15), and it is torn down when the statement ends.
        bool closing = false;

        // In `CancelRegistry` under this session's id (§10, BA-S15), and so
        // owed an `Unregister` when the connection goes.
        bool cancel_registered = false;

        // **When this connection's open transaction began**, for the
        // lifetime sweep (AN-R14), and which transaction that was. Stamped
        // at the first statement completion that finds a transaction the
        // stamp does not name - the `BEGIN`'s own - so a clock is read once
        // per transaction and never per statement (AW's H2). Keyed on the
        // id rather than a flag so a `COMMIT` and the next `BEGIN` cannot
        // leave one transaction timed from another's start.
        sched::MonoTimeNs txn_began_at_ns = 0;
        std::uint64_t txn_stamped_id = 0;

        // The finished statement's reply, waiting to be appended to the
        // outbox. Owned by the connection because the coroutine writes
        // through a pointer to it and may outlive the call that started it.
        DispatchOutcome pending;


        // This connection's transaction state (session.hpp). Per
        // connection and not per server: two clients on one dispatcher must
        // not see each other's open transaction (docs/spec/txn.md section 10-8).
        Session session;

        // ---- KWP/1 (P13) -------------------------------------------------
        //
        // Both null on a text connection. `decoder` accumulates frames out
        // of `inbox` - the same buffer the line splitter reads, holding
        // frame bytes instead of command bytes - and `kwp` is the protocol
        // state machine, which **borrows** `session` above.
        wire::FrameDecoder decoder;
        std::optional<KwpSession> kwp;

        // Reply frames the session produced for the frame being handled.
        // A member rather than a local because a dispatched statement
        // finishes in `OnStatementComplete`, which has to append to the
        // same buffer the frame handler started.
        std::vector<std::byte> frames_out;
    };

    using Clients = std::unordered_map<int, Connection>;

    // A live connection in another core's inbox (BL-S2), carrying the
    // statement whose head moved it (`current_line`, the session's sink).
    // The map node, not the `Connection`, travels: a node keeps its
    // element's address, and `KwpSession` holds a reference to the
    // `Session` beside it. Destroyed
    // untaken - the instance stopping mid-move - it is the connection's
    // teardown: its cancel entry leaves the registry and its socket closes.
    // A session moves only between transactions, so there is nothing to
    // roll back.
    struct MovedConnection final : ConnectionHandoff::Transit {
        MovedConnection(Clients::node_type n, CancelRegistry* c) : node(std::move(n)), cancels(c) {}
        ~MovedConnection() override;
        Clients::node_type node;
        CancelRegistry* cancels;
    };

    explicit TcpServer(int fd, bool hosting = false) noexcept : listen_fd_(fd), hosting_(hosting) {}
    void CloseIfOpen() noexcept;

    void OnListenerReadable();
    // One accepted socket's options, then its handoff or adoption.
    void SetUpAccepted(int client_fd);
    void OnClientEvent(int client_fd, const sched::IoEvent& event);
    void OnClientReadable(int client_fd);
    // Starts the next complete command, if the connection has one and is
    // not already running one. Returns false if the connection was closed
    // and must not be touched again. The reply is appended by
    // OnStatementComplete, which then calls this again for the next line.
    bool DrainCommands(int client_fd, Connection& conn);

    // The KWP half of the same loop: hands complete frames to the session
    // one at a time, stopping when one asks for a dispatch. Returns false
    // when the connection was closed.
    bool DrainFrames(int client_fd, Connection& conn);
    // Appends whatever the session wrote and clears it. Returns false when
    // the connection was closed (a channel failure).
    bool FlushFrames(int client_fd, Connection& conn);
    std::uint64_t NextIdentity();

    // The other half: a statement finished, so append its reply, honour
    // STOP, and continue draining.
    void OnStatementComplete(int client_fd);

    // **The transaction lifetime sweep** (AN-R14; docs/spec/txn.md §1 is
    // the contract). Rolls back each idle connection's overdue explicit
    // transaction and leaves its session expired (`Session::Expire`).
    void ExpireOverdueTransactions();
    // Appends one reply line to the outbox, through the wire channel if
    // one is installed. Returns false if the connection was closed (a
    // channel Send failure).
    bool AppendReplyLine(int client_fd, Connection& conn, std::string reply);
    // The same, without the newline: KWP frames carry their own length.
    bool AppendReplyBytes(int client_fd, Connection& conn, std::string_view bytes);
    // Writes as much of conn.outbox as the socket will take and drops what
    // went out. Returns false if the connection was closed (write error).
    bool FlushOutbox(int client_fd, Connection& conn);
    // Keeps the fd's epoll interest in step with whether there is a reply
    // tail still waiting to go out.
    void SyncWriteInterest(int client_fd, Connection& conn);
    void CloseClient(int client_fd);
    void UnregisterCancel(Connection& conn);

    bool logging(LogLevel level) const noexcept {
        return log_ != nullptr && log_->enabled(level);
    }

    // What `Host`'s parked task shares with this server: the server it
    // adopts into, null once detached and re-pointed by a move. Shared with
    // the task's frame, so the task never reads a dead one.
    struct HostState {
        TcpServer* server = nullptr;
    };
    static sched::Coro RunHost(std::shared_ptr<HostState> state, ConnectionHandoff& handoff,
                               std::uint32_t core);

    // Everything after `accept()` returns a socket: the connection's state,
    // its protocol session and its reactor registration.
    void AdoptConnection(int client_fd);
    // BL-S2's two halves: this core gives the connection on `client_fd` to
    // `core` with its statement carried, and a core takes one given to it.
    void MoveConnection(int client_fd, std::uint32_t core);
    void AdoptMoved(std::unique_ptr<ConnectionHandoff::Transit> moved);
    // Starts the statement `conn` holds (`current_line`, the sink) on this
    // core.
    void SubmitStatement(int client_fd, Connection& conn);
    // Moves this core's `CoreLoad::sessions` by `delta` (core_load.hpp).
    void CountSession(int delta) noexcept;

    int listen_fd_;
    // A `Hosting()` server: no socket is not an error at Attach().
    bool hosting_ = false;
    ConnectionHandoff* handoff_ = nullptr;  // set_handoff
    ConnectionHandoff* mover_ = nullptr;    // set_mover
    TransactionPlacer* placer_ = nullptr;   // set_mover
    std::shared_ptr<HostState> host_;  // Host
    sched::Scheduler* scheduler_ = nullptr;
    CommandDispatcher* dispatcher_ = nullptr;
    Logger* log_ = nullptr;
    ChannelFactory channel_factory_;
    AuthGateFactory auth_gate_factory_;
    Protocol protocol_ = Protocol::kKwp;
    wal::DurabilityClass durability_ = wal::DurabilityClass::kGroup;
    std::uint32_t keepalive_s_ = kDefaultTcpKeepaliveS;
    IdentitySource identity_source_;
    // `ClientSetup::cancels`; null keeps `CANCEL` withheld.
    CancelRegistry* cancels_ = nullptr;
    // The counter behind the default identity source. Not a secret; see
    // `set_identity_source`.
    std::uint64_t next_identity_ = 0;
    std::string server_info_ = "kds";
    std::function<void()> stop_handler_;
    // The portal-idle sweep's timer (§10, KW-D3), registered by `Attach` on
    // a KWP listener and cancelled by `Detach`.
    sched::TimerId idle_sweep_ = sched::kInvalidTimerId;
    // The transaction lifetime sweep's (AN-R14), registered by `Attach` on
    // every listener and cancelled by `Detach`.
    sched::MonoTimeNs txn_lifetime_ceiling_ns_ = kDefaultTxnLifetimeCeilingNs;
    sched::TimerId txn_sweep_ = sched::kInvalidTimerId;
    Clients clients_;
    std::uint64_t reply_writes_ = 0;  // `reply_writes()`
};

}  // namespace kds::server
