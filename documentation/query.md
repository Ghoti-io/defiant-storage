# Queries over entities

`gdefstor_entity_query()` and `gdefstor_entity_count()` ask a tree of
predicates (`and`, `or`, `not`, `=`, `!=`, `in`, `<`, `<=`, `>`, `>=`, `true`,
`false`, `EXISTS`, `BLIND`), a sort and a page of the current entities of one
declared type (the entity port, `documentation/model.md`), **in one
language**: the query names it (`GDEFSTOR_Query::language`, a language tag;
the count takes it as an argument) and it is required (AD-13: the entity
layer never speaks SQL). A query is data: the caller builds a bounded tree in
its own memory and the port reads it during one call. The operands are
typed:

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
- `and`, `or` and `not` are the usual truth tables; an `and` or `or` with no
  children is true and false.
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
constant list; a `CONST` outside `BLIND`; flags other than `OUTER` and the
three above, or one of those where it does not apply; `OUTER` outside an
`EXISTS`; an `EXISTS` inside another, or one naming a language tag that is not
one; a sort on a nested field or on a multiple field without `FIRST`. The caps
below are `GDEFSTOR_ERR_LIMIT`.

| Limit | Value |
|---|---|
| nesting of the tree | 64 |
| nodes, counted as the tree is walked | 8192 |
| constants in one operand | 256 |
| bytes in one text of a constant | 65536 |
| sort keys | 4 |
| page size | 1000; a larger limit is clamped, 0 means every match |

The query sees the last commit and checks no permission: the caller of the
port is the trusted layer, and a caller that serves a visitor puts the
visitor's access filter into the tree.

A backend answers through `GDEFSTOR_ModelBackend::query`, handed the checked
query and the declaration of every type it names, and must give the answer
`gdefstor_model_scan()` gives: the same entities, in the same order, with
the same total. Nothing in a query reaches a statement but as a bound value,
and every path is a declared field, so a SQL backend can compile each to an
indexed column of a current table (`EXISTS` to a semi-join on the reference
column) without scanning.
