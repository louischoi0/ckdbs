# A pk declared narrower than 64 bits is cut on the wire

**Reproduced** at `17e03da2` on `worktree-bk-keystone-48` (the engine as on
`main` at `5979b5e7`; the commit differs in `instructions/` only), first by
BK-S0's `critics-developer` review by reading, then by BK-S1's cell
`WireRowBatchTest.DISABLED_AnInt32PkAboveItsDeclaredWidthReachesTheClientWhole`
(`tests/wire_row_codec_test.cpp`), run. One core, nothing concurrent. Older
than BK: both sites predate it, and the id's width does not enter.

## What is wrong

`docs/spec/protocol.md` §6: *"Field 0 of every user relation is the
Keystone-derived `id` (u64)."* A relation may declare its pk `int8`, `int16`
or `int32` (`catalog::IsIntegerTypeVal`, `include/kds/catalog/schema.hpp`),
and `docs/spec/heap-and-tuple.md:124` says *"Its declared width is display
metadata"*: the id lives in the Keystone field whatever the column declares,
*"so a narrow declared type does not cap the sequence."* In storage that holds -
`tests/row_codec_test.cpp:159-170` stores id 100000 under an `int8` pk and
reads it back. On the binary wire it does not:

- `wire::DescribeSchema` (`src/wire/row_codec.cpp:117-118`) gives field 0
  `type_len = WireTypeLen(col.type_val)` - 1, 2 or 4 for the narrow types -
  beside the `kFieldFlagKeystone` it sets at `:126`.
- `EncodeValue` (`:183-185`) writes the integer arm as
  `PutLE(out, value.int_val, width)` with that width: the low bytes only.

Observed, schema `(id int32, v int64)`, one row with id 2147483653
(2^31 + 5), through `RowBatchWriter` and `DecodeRowBatch`:

| | expected (§6) | actual |
|---|---|---|
| field 0's length | 8 | 4 |
| field 0 decoded | 2147483653 | -2147483643 |

The cell drives the codec, not a session. The session reaches the same
integer arm: `WireResultSink::EncodeProjectedRow` and `EncodeValueRow`
(`src/server/kwp_session.cpp:240, 258`) call `wire::EncodeValue` with the
column's declared type, and `DescribeFields` derives `type_len` from the
same `wire::WireTypeLen` (`src/server/command_dispatcher.cpp:4806`) beside
the flag it sets at `:4822` - so, by reading, a KWP/1 `SELECT` delivers the
same four bytes. The first id that reads back as another number is 128
under `int8` (one byte `0x80`, sign-extended by the declared type to -128),
32768 under `int16` (-32768) and 2147483648 under `int32` (-2147483648);
from 256, 65536 and 2^32 the upper bytes are dropped as well. Every id
below the first is whole. The debug text surface is unaffected:
`TextResultSink` renders through `exec::FormatValue`
(`include/kds/server/result_sink.hpp:119, 129`), which narrows no integer type.

## What it costs

A **quiet wrong answer**: past the declared width, every row's pk reaches
the client as another number, with no error. `S_ROW_DESC` says `type_len`
4, so a client that trusts the descriptor reads a consistent 4-byte field
holding the wrong value, and one that trusts §6's u64 reads the wrong
bytes. A client that then names the pk it received in an `UPDATE` or
`DELETE` acts on another row or on none.

## The fix

Known and unscheduled. BK-Q6 was marked (a) by the operator on 2026-10-11
(`instructions/v3.0.0/workorder-bk-keystone-48.md` §4,
`raft-marks-2026-10-11.md` §2): this entry now, the fix in its own letter.
The fix CLA recommends is BK-Q6 (b): field 0 is sent as 8 bytes, u64,
whatever the column declares, `DescribeSchema` reports `type_len` 8 for
it, and `protocol.md` §6 and `client-manual.md` say the pk field's width is
fixed - after `xrock`'s reading of the field descriptor has been checked,
since a client that declared its pk narrow receives different bytes. The
cell above is `DISABLED_` until then and the letter drops the prefix.
BK-Q6 (c), refusing the narrow declarations at `CREATE TABLE`, was offered
and not chosen.
