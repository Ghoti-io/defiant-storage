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

## Over entities

`gdefstor_entity_query()` and `gdefstor_entity_count()` ask the same tree, a
sort and a page of the current entities of one declared type (the entity
port, `documentation/model.md`), **in one language**: the query names it
(`GDEFSTOR_Query::language`, a language tag; the count takes it as an
argument) and it is required. The operators and the limits are the ones
above; the operands are typed:

| Operand | Its values on an entity | Type |
|---|---|---|
| `FIELD` `lines.taxes.amount` | the field's value on every current item of the query's language that has one | the field's |
| `VALUES` | the listed constants, all of one type | theirs |
| `META` `PUBLISHED`, `DELETED` | the newest state | boolean |
| `META` `CREATOR` | who created it (none for the system) | reference |
| `META` `CREATED` | when | date-time |
| `META` `REVISION` | the newest revision, 0 for none | integer |
| `META` `REVISION_WHO` | who wrote the newest revision (none for no revision or the system's) | reference |
| `ENTITY` | its public id | reference |

What it supports:

- `=`, `!=` and `in` on every type; `<`, `<=`, `>`, `>=` on integers,
  decimals, floats, dates and date-times, compared as numbers and dates.
  Text compares by bytes. Both sides have the same type; a decimal compares
  with a decimal of the same scale (a constant is at the field's scale).
- **Some item matches.** A path through groups and multiples has the values
  of every item it reaches, so `lines.sku = "A-1"` holds when some line has
  that sku, and `!=` holds when none has. Two comparisons are decided on
  their own: `lines.sku = "A-1" and lines.qty = 5` does not say it is the
  same line. An absent value adds nothing; an entity with no revision has no
  field values but still has its facts.
- **One language.** Every `FIELD` operand and field sort key reads the
  current tree of the query's language, inside an `EXISTS` too (an `OUTER`
  field is the outer entity's, in the same language), and no other tree, so
  two conditions on one entity are always about one language. An entity with
  no tree in that language reads as one whose every field is absent: no `=`,
  `in`, range or `BLIND` holds on it, `!=` and a `not` around a comparison
  do, and it sorts as no value. `META` and `ENTITY` read alike in every
  language.
- `and`, `or`, `not`, `true`, `false`, as above.
- `EXISTS` names another declared type and holds when some entity of it
  satisfies the child, whose `OUTER` operands read the entity being decided:
  `EXISTS site.customer (ENTITY = OUTER customer and vip = true)`. One level;
  a relation is one of these.
- `BLIND`, blind-index equality: a personal field (no `OUTER`) and one
  `CONST` text of exactly 32 bytes, a MAC. It holds when some current item of
  the field's group has a value whose current version has a blind row with
  that MAC; a MAC of an older version does not match (`model.md`, "The blind
  index and subjects"). A store whose backend keeps no blind table refuses it
  `GDEFSTOR_ERR_UNSUPPORTED`.
- A sort of up to four keys, each a single scalar field of the root group, a
  `META` fact or the public id, ascending or descending. No value sorts
  before every value. The final tiebreak is the entity's row, ascending even
  under a descending sort, which is creation order.
- An offset, a limit (clamped to 1000; 0 is every match) and the total,
  which counts every match before the offset and the limit.

For a caller whose own values are texts (core's conditions, story S6b),
three flags and one member read the same data the way such a caller does.
Each is refused anywhere else, and a query that names none of them means
exactly what it meant before:

- `GDEFSTOR_QUERY_EMPTY_IF_NONE` on a `FIELD` of a text or reference field:
  with no value on any current item of the language, the operand is the
  single empty text, so `= ""` holds and `!= ""` does not. On a sort key of
  such a field, no value sorts as the empty text and ties with it.
- `GDEFSTOR_QUERY_AS_TEXT` on a reference operand (a reference field,
  `META` `CREATOR` or `REVISION_WHO`, `ENTITY`): its type is text, so it
  compares by bytes with a text field or text constants.
- `GDEFSTOR_QUERY_FIRST` on a sort key: a multiple scalar field of the root
  group, sorted by the value of its first item (lowest order key, then lowest
  item id) that has one; none sorts as no value.
- `GDEFSTOR_QueryNode::exists_language` on an `EXISTS`: the other type's
  `FIELD` operands read that language's items (a relation kept in one
  language, tested from a list in another); its `OUTER` operands read the
  query's. NULL is the query's language.

What it refuses, with `GDEFSTOR_ERR_INVALID` unless a limit says otherwise:
a query with no language, or a tag that is not one; a type never declared; a path that is not an active scalar field (a group, a
retired field, a field that does not exist); a personal field anywhere but
in `BLIND` (its value is ciphertext: never compared, ranged or sorted), and a
`BLIND` on anything else or with a MAC of another length; sides of different
types; an ordered comparison on text, references or booleans; `in` without a
constant list; `COLUMN` and `CONST` (the projection form's operands) outside
`BLIND`; flags other than `OUTER` and the three above, or one of those
where it does not apply; `OUTER` outside an `EXISTS`; an `EXISTS` inside
another, or one naming a language tag that is not one; a sort on a nested
field, on a multiple field without `FIRST`, or with `numeric`; select columns;
`entity_numeric`. The caps above are `GDEFSTOR_ERR_LIMIT`.

A backend answers through `GDEFSTOR_ModelBackend::query`, handed the checked
query and the declaration of every type it names, and must give the answer
`gdefstor_model_scan()` gives: the same entities, in the same order, with
the same total. Nothing in a query reaches a statement but as a bound value,
and every path is a declared field, so a SQL backend can compile each to an
indexed column of a current table (`EXISTS` to a semi-join on the reference
column) without scanning.
