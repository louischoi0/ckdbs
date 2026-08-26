# `TlsChannelTest.PlaintextGarbageIsFatal` fails on OpenSSL 3.5.5

**The symptom.** `ctest` is 2707/2708 on the `worktree-clean-docs` tree at
`925f483` (`v2.2.0-11-g925f483`); the one failure is
`tests/tls_channel_test.cpp:216` — `EXPECT_TRUE(out.empty())`, *Actual:
false*. The two assertions before it pass: the status is not ok and no
plaintext surfaces, so **the fatal path itself is intact** and a plaintext
client on a TLS port is still refused and closed on.

**The mechanism.** The failing line is not about the engine. The test pins
a claim about OpenSSL, stated in its own comment: *"No alert goes back:
OpenSSL queues none for a first record that was never TLS, so the peer
gets the close and nothing else."* This host runs **OpenSSL 3.5.5 (27 Jan
2026)**, which *does* queue an alert for a first record that was never
TLS, so the channel drains alert bytes into `wire_out` and `out.empty()`
is false. The engine sends what OpenSSL gives it; what changed is what
OpenSSL gives.

**What it is not.** Not a regression from the docs reorganisation that
found it: the only edit that change made to `tests/tls_channel_test.cpp`
is two comment lines repointing the protocol spec at its new path under
`docs/spec/`, so the failure predates it. Not the neighbouring
`UntrustedServerCertRefused` either — that one asserts the *opposite*
half, that a fatal alert **is** drained, and it passes.

**Reproduction.** `bash scripts/test.sh`, or
`./build/tests/kds_tests --gtest_filter=TlsChannelTest.PlaintextGarbageIsFatal`.
`openssl version` reports the deciding fact.

**What the fix has to decide** — and it is a real decision, not a
loosening: `docs/spec/protocol.md` §2 says a malformed frame is reported
and closed on. Whether an alert accompanies that close is OpenSSL's
choice, not the channel's, so the test should pin **the channel's**
contract — status not ok, no plaintext, and `wire_out` handed over
verbatim whatever it holds — rather than a byte count that a library
upgrade can move. Relaxing the assertion without saying that would leave
the suite green and the contract unstated.
