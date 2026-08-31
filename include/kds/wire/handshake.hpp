#pragma once

#include <cstdint>
#include <string>

#include "kds/wire/error_registry.hpp"
#include "kds/wire/kwp.hpp"
#include "kds/wire/kwp_types.hpp"

// The KWP/1 handshake (`docs/spec/protocol.md` §3,
// `docs/inflight/in-progress/protocol-wp.md` P07): what a `C_HELLO` has to
// carry for the server to answer `S_HELLO`, and what the answer says.
//
// ---- A pure function, on purpose -----------------------------------------
//
// `Negotiate` reads two structs and writes one. It opens no socket, mints
// no identifier, consults no clock and holds no state - the session id and
// the cancel key are **arguments**, because randomness is an injected
// concern (rules.md §4) and because a negotiation that generated its own
// could not be pinned byte-for-byte, which is precisely what P16's golden
// sessions need of it.
//
// The state *machine* the workplan's row names is therefore small enough to
// be a rule rather than a class: a connection is pre-hello or post-hello,
// and the endpoint holds that one bit beside the session it built.
//
// ---- What the handshake decides, and what it refuses ---------------------
//
// **Version.** The chosen version is `min(client.max, server.max)`, and it
// must be at least `max(client.min, server.min)`. Empty intersection is
// fatal (§3): there is no version both sides can speak, and the client has
// to reconnect knowing this server's range - which the message states, so
// the refusal is actionable rather than merely correct.
//
// **Capabilities.** The intersection, `client & server`. Bits this server
// does not know are dropped by the AND, which is §3's "unknown capability
// bits ignored, not rejected" implemented rather than checked for: a client
// built against a later version offers bits this build has never heard of,
// and the whole point of the bitset is that it survives that.
//
// One bit is not a capability but a **demand**: `kTlsRequired` is "a KWP
// client's way to demand the transport it is on" (§1). A client that sets
// it on a plaintext connection is refused, and refused *fatally* - it asked
// not to be served in the clear, and answering anything else on that
// connection would be serving it in the clear.
//
// **Authentication.** `auth_method` is negotiated here and the exchange
// runs after, in `C_AUTH`/`S_AUTH` frames the endpoint drives through the
// same `server::AuthGate` the newline protocol uses. Two refusals, both
// fatal and both in the same direction - the party that asked for less
// security never gets it silently:
//
//   - a server that requires authentication refuses `kAuthNone`;
//   - a client that offered SCRAM is refused by a server with no
//     credential store, rather than admitted unauthenticated.
//
// This layer never sees a credential. It answers *whether* an exchange is
// owed; `server/kwp_endpoint` runs it.

namespace kds::wire {

// What this server offers. Every field is the server's own side of a
// negotiation - nothing here is derived from the client.
struct HandshakeConfig {
    std::uint16_t min_version = kKwpVersion;
    std::uint16_t max_version = kKwpVersion;

    // The bits this build implements. Not a config knob: a capability the
    // server offers and cannot honour is worse than one it never offered.
    std::uint64_t capabilities = 0;

    // Reported to the client for telemetry, never interpreted by it - the
    // mirror of `ClientHello::client_name`.
    std::string server_info;

    // Whether this connection's bytes are already inside TLS. The
    // transport knows; the protocol above it does not, so it is told
    // (`server/wire_channel.hpp` is where the seam sits).
    bool tls_active = false;
};

// What the negotiation decided.
struct HandshakeOutcome {
    // False means `error` is what the client gets and, since every
    // handshake refusal is fatal (§11), the connection closes after it.
    bool accepted = false;

    // Valid when accepted. The endpoint frames this as `S_HELLO`, but only
    // once any owed exchange has succeeded: an unauthenticated connection
    // must not learn a session id or a cancel key, which are the two
    // things §10's out-of-band cancel is built on.
    ServerHello hello;

    // Valid when accepted: the client must send `C_AUTH` before anything
    // else, and the endpoint must not answer `S_HELLO`/`S_READY` until the
    // gate admits it.
    bool auth_owed = false;

    // Valid when not accepted.
    WireError error;
};

// `auth_required` is a **parameter and not a config field**: it is derived
// from whether the connection holds a credential gate, so a field would be
// a setting nobody sets and two places for one fact to disagree.
HandshakeOutcome Negotiate(const ClientHello& hello, const HandshakeConfig& config,
                           bool auth_required, std::uint64_t session_id,
                           std::uint64_t cancel_key);

// The bits this build implements, as one named constant so the server and
// the reference client cannot disagree about what is on offer.
//
// **A bit is offered when the frame behind it is answered, and not before.**
//
// `kStreaming` is here because portals are built (§7). `kCapBulkLoad` is
// here because the v0 load endpoint answers its block.
//
// `kCancel` is **not**, and that is the rule above applied to this build
// rather than an oversight: `C_CANCEL` has no handler, so a server that
// offered the bit would refuse the one frame it had just advertised, which
// is worse for a client than the absence - an absent capability is a
// branch it does not take, and a lying one is a branch that fails. The
// session half exists (`KwpSession::RequestCancel` and its observation
// point); the connection half is P14's remainder, in `known-gaps.md`.
//
// `kCompression` is not offered either: KW-D5 deferred it, the bit stays
// reserved in the enum, and offering a shape nobody has specified is a
// promise this build cannot keep.
//
// `kTlsRequired` is not offered, and that is not an omission - it is a
// demand a *client* makes, never a service a server advertises, so a server
// that listed it would be claiming to require TLS of itself.
inline constexpr std::uint64_t kServerCapabilities =
    static_cast<std::uint64_t>(Capability::kStreaming) | kCapBulkLoad;

}  // namespace kds::wire
