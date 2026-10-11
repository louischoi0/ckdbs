# A SET-value subquery is re-parsed behind padding as long as its offset

**Found by reading** at `5979b5e7` on `worktree-bj-review-fixes`, by the
post-close `critics-developer` review of BJ. Not reproduced by measurement.

## What is wrong

`CommandDispatcher::RunScalarSubquery` (`src/server/command_dispatcher.cpp`)
runs a `SET a = (SELECT ...)` subquery through `HandleSelect` as text, and
pads the text with `node.byte_offset + 1` spaces so that every byte position
the SELECT reports is a byte of the UPDATE:

```
const std::string text = std::string(node.byte_offset + 1, ' ') + node.subquery_sql;
```

Each subquery therefore allocates and lexes as many bytes as its distance from
the statement's start. The work is proportional to the number of subqueries
times the statement's length, so it grows with the square of the statement's
length. The expression cap (4096 nodes per assignment) still admits about 2000
subqueries per assignment, times the relation's columns, inside a 16 MiB
`kMaxFrame`.

## What it costs

CPU and allocation only: no wrong answer. Each subquery also walks a relation,
which costs more than the padding until the statement is long. A client can
still make one statement cost far more than its size.

## The fix it wants

A base byte offset in the lexer (`Lexer(sql, base)`, `Parser(sql, base)`)
that the parser adds to every position it reports, so the subquery text is
passed without padding. The parser slices `sql_` by byte offset once
(`subquery_sql`; a `CREATE ASSERTION` stores the whole input), so that
slice subtracts the base. The other option is to hand `HandleSelect` the
already-parsed `node.subquery` instead of its text. Cells: a subquery at a large
offset reports the UPDATE's byte, and N subqueries at offset L do not allocate
N x L bytes.
