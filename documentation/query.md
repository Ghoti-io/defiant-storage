# Queries over projections

`query.h` adds a backend-neutral way to ask which rows of one projection type
match a condition, in what order, a page at a time (AD-13: the entity layer
never speaks SQL). `gdefstor_projection_query()` returns a page and the total;
`gdefstor_projection_count()` returns only the total.

A query is data. The caller builds a bounded tree of predicates (`and`, `or`,
`not`, `=`, `!=`, `in`, `<`, `<=`, `>`, `>=`, `true`, `false`) over operands that
are sets of texts: constants, a column of the row, a column of another
projection type's row with the same entity argument (a semi-join on identity),
or the entity argument itself. A column can hold a JSON document, an object of
objects whose leaves are arrays of strings, which is what a revision's `lang`
is; the operand then names a language and a field and reads the array there.
The meaning of each operator, of an absent column, of the empty-if-none flag,
of the integer rule and of the sort keys is in the header comment, and the
reference for all of it is `src/query.c`.

## What a backend does

`GDEFSTOR_Backend::query` is optional. A backend that leaves it NULL is
answered by the port's own scan: it lists the keys of the type, reads each row
through `get`, and evaluates the tree row by row. That is the reference
answer, and it is what the memory backend uses. A backend that sets `query`
must give the same rows, in the same order, with the same total, for every
query. The conformance suite (`tests/conformance/query.inc`, installed with
`suite.inc`) checks every backend against a model written apart from the
library, over generated rows and generated queries, and over the named cases
(numeric text, constants that look like SQL, type names that look like
patterns, damaged documents, the caps).

The SQLite backend answers `query` by compiling the tree into one `SELECT`
over `defiant_record` and running it. See its documentation for exactly what
that pushes down and what it does not.

## Limits

| Limit | Value |
|---|---|
| nesting of the tree | 64 |
| nodes, counted as the tree is walked | 8192 |
| texts in one constant operand | 256 |
| bytes in one text of a constant | 65536 |
| sort keys | 4 |
| returned columns | 8 |
| page size | 1000; a larger limit is clamped, 0 means every match |

A tree over a limit is `GDEFSTOR_ERR_LIMIT`; a shape that makes no sense is
`GDEFSTOR_ERR_INVALID`; a row that cannot be read (a damaged JSON document) is
`GDEFSTOR_ERR_CORRUPT` and no partial answer is returned.

## Order

Rows come in the order of the sort keys, then by the entity argument, which is
the final tiebreak, so a page is always a slice of one total order. By default
the entity argument sorts as bytes; with `entity_numeric` it sorts as an
integer first (a row whose entity argument is not an integer sorts ahead of
those that are), then as bytes.

## What it does not do

It checks no permission. It cannot index: projections are stored as encoded
rows under one key each, so a query reads every row of the type. An index
would need the columns to be stored apart from the encoded row, which is a
change to the physical layout of every backend (AD-14's column projection) and
is not part of this form.
