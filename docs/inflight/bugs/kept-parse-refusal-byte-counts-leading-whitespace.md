# A named statement's refusal byte counts its leading whitespace; the same text unnamed does not

**What is wrong.** When a statement's text begins with whitespace, a named
statement (EA's kept parse, `protocol.md` §5) names a refusal byte that
differs from the one the same text names when it is sent unnamed, by the
length of that whitespace. `KeptParse::Of` parses the text as the client
sent it (`src/server/kwp_session.cpp:677`), while the dispatcher parses
`Trim(line)` (`src/server/command_dispatcher.cpp:2151`, `:2154`, `:2194`,
`:2214`). Every byte the kept AST carries is therefore counted from the
untrimmed text, and every byte the text path's parse carries is counted
from the trimmed text. The answer is never different; only the byte in
the refusal's message is.

**Verified at** `b46ac5140` (`v3.5.1-69-gb46ac5140`), on worktree
`em-front-end-reuse`, by EM-S0 (iv)
(`bench/v3.5.1/archive/em-s0-premise-v3.5.1-69-gb46ac5140/s0iv.py`, output
beside it). With two leading spaces, six of the seven statements tried name
a byte 2 higher when named than when unnamed:

| statement (after two spaces) | unnamed | named |
|---|---|---|
| `SELECT * FROM t WHERE nosuch = 1` | byte 22 | byte 24 |
| `SELECT nosuch FROM t WHERE id = 1` | byte 7 | byte 9 |
| `UPDATE t SET nosuch = 1 WHERE id = 1` | byte 13 | byte 15 |
| `DELETE FROM nosuch WHERE id = 1` | byte 12 | byte 14 |
| `INSERT INTO t VALUES (1, 'x', 3)` | byte 25 | byte 27 |
| `SELECT * FROM t WHERE v = 'x'` | byte 26 | byte 28 |

`UPDATE t SET v = 'x' WHERE id = 1` gave the same reply on both paths (`s0iv.out` records `SAME`, not the reply).
Without leading whitespace, all seven agree.

**The smallest reproduction.** Over KWP, `C_PARSE` the name `s` with
`"  SELECT * FROM t WHERE nosuch = 1"`, then `C_BIND` and `C_EXECUTE` `s`:
the refusal names byte 24. Sent unnamed, the same text names byte 22.

**What it costs.** A refusal's byte, not an answer: no row and no refusal
code differs. EA-R3 counts a bound value's byte from "the text the client
sent", which the named path keeps and the text path does not, so neither
path is wrong by its own rule; the two simply disagree.

**Not fixed by EM, by EM-R1.** Making either path count like the other
changes a refusal byte that one path gives today, and EM-R1 forbids that.
EM's C1 parses the trimmed text, so an unnamed statement's bytes stay the
text path's. Which counting is right is the operator's question. A fix
would make `KeptParse::Of` parse `Trim(sql)` and shift every hole's byte by
the trimmed prefix, or make the dispatcher stop trimming. Neither is
scheduled.
