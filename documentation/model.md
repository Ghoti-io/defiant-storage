# The entity port

**Status:** describes what is built: the entity port of `model.h`, its
query form, its backend interface and the key store port. It sits beside the
key/bytes API (`store.h`, `projection.h`, `revision.h`), which keeps working
unchanged until that API is removed.

The port speaks entities, revisions, fields and queries, never keys and
encoded bytes, and no SQL. Each backend chooses its physical schema; the rows
the port hands it map one to one onto ordinary tables:

| Rows | Table, per type `T` | Written |
|---|---|---|
| entity | `T`: row, public id, creator, creation time; highest revision, last item id | inserted once; the two counters move forward |
| state | `T_state`: published, deleted, actor, time | appended |
| revision | `T_revision`: number, who, when, why, commit sequence, the languages it changed | appended |
| item version | one table per group: item id, language, parent item, order key, the group's columns, `valid_from`, `valid_to` | appended; `valid_to` set once |
| current item | one current table per group: the open versions | derived, same transaction |
| newest state | columns of `T` or a current table | derived, same transaction |
| declaration | a catalog row per type, `defiant-type/1` text | replaced |
| draft | one per entity, `defiant-draft/1` JSON (a `defiant-tree/1` tree per language) and a base revision | replaced, deleted |
| blind row | the blind table, per type: entity, item, `valid_from`, field, subject, MAC, key id, normalization version, language | inserted with the version it indexes; deleted per subject |

Everything that decides what a write means lives in the port, once, so every
backend stores the same rows: which items changed, item ids, order keys, the
base-revision check, retired fields, as-of trees and differences, and the
reference query engine. A backend keeps what it is handed and answers
queries (`model_backend.h`).

## Types

A type is a machine name (`<module>.<name>`, 3 to 128 bytes: letters, digits,
underscore, one dot, a hyphen inside the local part) and a tree of fields.
A field has a name (`[A-Za-z0-9_]{1,64}`, unique among its siblings ignoring
ASCII case, because some engines fold the case of column names), a logical
type and flags:

| Type | Value | Range |
|---|---|---|
| `text` | UTF-8, no NUL | up to 1 MiB |
| `integer` | int64 | |
| `decimal` | exact: the unscaled integer at the field's scale (0 to 18) | magnitude below 10^18 |
| `float` | double | finite (no NaN, no infinity); -0 is stored as 0 |
| `boolean` | 0 or 1 | |
| `date` | days since 1970-01-01 | years 1 to 9999 |
| `datetime` | milliseconds since 1970-01-01T00:00Z | years 1 to 9999 |
| `reference` | another entity's public id | `[0-9A-Za-z_-]`, the public-id rule |
| `group` | items holding the group's own fields | at least one field |

Money is a decimal, never a float.

- `MULTIPLE`: any number of items, kept in order. Any field may be multiple,
  groups included. A multiple scalar field is a one-column group: each item
  holds one value.
- `REQUIRED`: a save must supply it; for a group or a multiple, at least one
  item. Checked on every save against the declaration as it is then.
- `INDEXED`: a hint for the backend. It never changes an answer. A backend
  may build the index after the declaring write commits, outside its
  transaction (AD-15); until it is built, `gdefstor_type_index_state()` reads
  the field `GDEFSTOR_INDEX_INDEXING`, then `GDEFSTOR_INDEX_READY`, and
  queries on it answer the same throughout.
- `PERSONAL` (AD-19): the value is ciphertext the caller made, stored with
  the subject's public id. The port never sees a key or a plaintext and
  compares the stored bytes, so a caller that re-encrypts an unchanged
  plaintext passes the stored ciphertext through to keep the save a no-op.
  The subject comes from the entity (the caller names it on each value) or
  from a named sibling reference field (single, not personal), in which case
  each value's subject must equal that field's value on the same item, or,
  for an item of a multiple personal field, on its parent. A personal field
  cannot be indexed, sorted or range-filtered; it is matched only by
  blind-index equality (below).

Declaring is additive. `gdefstor_type_declare()` takes the whole desired tree:

- a new field is added; rows written before it read it as absent;
- a stored field the spec leaves out is **retired**: its data stays readable,
  in history and in the current tree, but no save can change it (a tree may
  restate it unchanged, or leave it out, which keeps it);
- a retired field named again, unchanged, comes back with its data;
- a different type, scale, `MULTIPLE`, `PERSONAL` or subject is
  `GDEFSTOR_ERR_UNSUPPORTED`: that is a new field and a conversion. A retired
  field keeps its name, so it cannot come back as another type either;
- `REQUIRED` and `INDEXED` may change;
- a spec equal to what is stored writes nothing.

Every field gets an id, every group and multiple field a group number, every
single scalar a column in its group. None is ever reused or renumbered, so a
backend can name tables and columns from them; `gdefstor_type_field()`
reports them.

## Trees

An entity's content is one tree. Item 0 is the root item, which holds the
root group's fields; every other item sits under one field of its parent: a
group field (the item holds that group's fields) or a multiple scalar field
(the item holds one value). Every item has an id, stable for the life of the
item, unique within the entity and never reused; the root item is 1.

A tree built for a save (`gdefstor_tree_create()`, `_add()`, `_set()`) names
an existing item by its id and a new one with id 0, and states order by the
order items are added. The root's id is not looked at. A tree read back lists
a parent before its children and siblings in order, with each item's id and
stored order key.

## Languages

Decided 2026-10-19 (`ENTITY-STORAGE-DESIGN.md`, "Languages"; story S6a2).
An entity holds **one tree per language**, and **one revision history covers
all of them**: a revision's number, who, when and why are the entity's, never
a language's. Translations with a history of their own (access spec D5) are
not this; they stay deferred.

- **Tags.** A language is 1 to 35 bytes of `[A-Za-z0-9-]` (the rule core
  holds its languages to), compared byte for byte: `en`, `EN` and `en-GB` are
  three languages, and the port never folds case or matches a prefix.
- **Items.** Every item version carries its language. Each language's tree
  has its own root item; item ids are the entity's, given out across its
  languages, so no two languages share an id and an item belongs to the
  language it was created in for life. A tree names only items of its own
  language.
- **Saves.** `gdefstor_entity_save_languages()` names the languages it writes
  and gives each a whole tree; `gdefstor_entity_save()` is the same with one.
  A language the save leaves out keeps its tree and gets no row. A named
  language whose tree is unchanged gets no row either. A save in which no
  language changed writes no revision. The rows of every language a save
  changes are one revision, and that revision records which languages it
  changed (sorted, joined by `,`: `GDEFSTOR_RevisionInfo::languages`).
- **Removal** is explicit: `gdefstor_entity_remove_language()`, or a
  `GDEFSTOR_LanguageTree` whose tree is NULL in a save, closes every current
  item of the language in a revision. History keeps it: an as-of read of an
  earlier revision finds the tree, and a later save of the language starts a
  new tree with a new root. Removing a language the entity has no tree in
  writes nothing.
- **A revert** brings every language back to its tree at the revision: a
  language changed since is reverted, one removed since comes back under its
  old ids, and one that did not exist then is removed.
- **Reads** name a language: `gdefstor_entity_tree()`, now or as of a
  revision (no tree in that language then is `GDEFSTOR_ERR_INVALID`).
  `gdefstor_entity_languages()` says which languages have a tree, now or as
  of a revision. `gdefstor_entity_history()` takes a language (only the
  revisions that changed it) or none; `gdefstor_entity_diff()` takes a
  language or none, and each item it lists names its language, so the diff
  from one revision to the next says which languages that revision touched.
- **Queries** bind one language (`GDEFSTOR_Query::language`, required; the
  count takes it too). Every field operand and field sort key of the query,
  inside an EXISTS as well, reads that language's current items and no
  other's, so two conditions on one entity are always about the same
  language. An entity with no tree in the language is still an entity of the
  type: it reads as one whose every field is absent, so `=`, `in`, ranges and
  blind equality do not hold on it (`!=` and a NOT around a comparison do, as
  for any absent value), and it sorts as no value. META and ENTITY operands
  are the entity's and read alike in every language.
- **Shared parts.** Blind rows carry the language of the version they index;
  the history lookup and the subject lookup report each version's language.
  A draft holds a tree per language it changes (below).
- **Limits.** An entity holds trees in at most
  `GDEFSTOR_MODEL_MAX_LANGUAGES` (64) languages at once; a save that would
  leave more is `GDEFSTOR_ERR_LIMIT` and writes nothing. That is a cap per
  entity, documented here, and the only one: a language is a column of every
  row, never a table or a column per language, so nothing in the tables'
  shape limits how many languages a tenant uses. Each language's tree is held
  to the tree limits below on its own.

## Entities and states

`gdefstor_entity_create()` makes the entity row: the public id (unused by any
entity of any type and by a draft's reservation), the creator (a principal's
public id, or empty for the system) and the creation time. It has no revision
and no tree until the first save. Rows increase in creation order and are
never reused; they never leave the server.

`gdefstor_entity_set_state()` appends a state row (published, soft-deleted,
actor, time). A state equal to the newest appends nothing. The newest state
is what queries and `gdefstor_entity_get()` see.

## Revisions

`gdefstor_entity_save()` writes revision N+1 from the full new tree:

- **Linear history.** The base revision must be the newest (0 before the
  first). Otherwise `GDEFSTOR_ERR_CONFLICT`, and nothing is written.
- **Only changed rows.** An item whose parent, order key and values are
  unchanged is not touched. A changed item's open version is closed (its
  `valid_to` set to N+1, once, never again) and a new version inserted. An
  item missing from the tree is closed; its descendants go with it. A new
  item gets the next id. Moving an item to another parent of the same group
  is a change of that item.
- **A save that changes nothing writes no revision** and reports 0.
- Who, when and why (free text, UTF-8, up to 4096 bytes) are recorded, and
  every revision a write makes carries the tenant's commit sequence of that
  write.

**Order keys** are doubles, per parent and field. The items that stay under
the same parent keep their keys when they lie on the longest run of keys
already in order; every other item takes a key between its neighbours (the
midpoint for one; evenly spaced for several; below the first and above the
last by whole steps). So moving one item changes only that item's key. When
two neighbours would be closer than `GDEFSTOR_MODEL_ORDER_EPSILON` (2^-20),
the whole run is renumbered 1, 2, 3 ... in that same revision; the items
whose key changed get new versions.

**A revert** (`gdefstor_entity_revert()`) is an ordinary save whose trees are
a past revision's, in every language: items keep their ids, an item removed since comes back under
its old id, keys are the old keys, and old rows are never touched, so every
revision in between reads as it did. Retired fields keep their current data;
an item brought back gets none. Reverting to the current tree writes nothing.

**Reads.** `gdefstor_entity_tree()` reads one language's tree: with revision
0 from the current tables; with a number, the versions valid at that
revision. `gdefstor_entity_history()` lists revisions (each with the
languages it changed) and state changes. `gdefstor_entity_diff()` lists the
items added, changed (values, parent, order) and removed between two
revisions, revision 0 being the empty tree, each naming its language.

## Drafts

One per entity, outside the revision tables, for all its languages: a
`defiant-tree/1` JSON tree for each language the draft changes, and the
revision it was made from. `gdefstor_draft_put()` checks only that each is
well formed, and stores them as one `defiant-draft/1` document (Formats);
`gdefstor_draft_tree()` reads each language's tree back exactly as it was put.
`gdefstor_draft_apply()` validates every tree against the type as it is then,
writes them as one save of those languages on its base revision (a language
the draft leaves out keeps its tree; a draft cannot remove a language) (`GDEFSTOR_ERR_CONFLICT` when
the head has moved: nothing is written and the draft stays) and deletes it,
including when the tree equals the current one. A draft for a public id with
no entity reserves the id; applying it creates the entity, with the
revision's who and when as creator and creation time, and writes revision 1,
after checking everything, so a draft that does not validate creates nothing.
Deleting a draft touches nothing else. Scheduled publish is a caller applying
a draft at a time.

## Queries

`gdefstor_entity_query()` and `gdefstor_entity_count()` take the query form
of `query.h` over the current entities of one type, in one language: field
paths into groups,
typed constants, facts of the entity record (published, deleted, creator,
creation time, newest revision) and the public id; equality, `in`, numeric
and date comparison, `and`/`or`/`not`, the `EXISTS` semi-join that relations
are, a sort, an offset and a limit, a total, and the row as the final
tiebreak. `documentation/query.md` says what each means and what is refused.

## The blind index and subjects

AD-14 and AD-19, as `ENTITY-STORAGE-DESIGN.md` ("Personal data", items 2 to
5) decides them. Storage never sees a key or a plaintext: the caller (core)
computes each MAC, an HMAC-SHA-256 under the tenant's per-field blind-index
key over the normalized plaintext, and hands it over as
`GDEFSTOR_BLIND_MAC_BYTES` (32) opaque bytes with the key's id and the
normalization version, which are recorded beside it.

**Writing.** `gdefstor_tree_set_blind()` gives a personal value of a tree its
MAC. A save writes one *blind row* per personal value version that carries a
MAC: the entity, the item and the `valid_from` of the version, the field's
id, the value's subject, the MAC, the key id and the version, in the same
transaction as the version. A value version is the item version that holds
it, so:

- an unchanged value whose item is unchanged writes no version and no row;
- a value carried unchanged into a new version of its item (another column
  of the item changed) keeps its row: the port copies the current version's
  MAC into a row for the new version, unless the tree gives one (same
  ciphertext, same plaintext, same MAC);
- a revert brings old values back with the rows they had, unless their
  subject's rows were deleted since;
- a MAC on a field that is not personal is `GDEFSTOR_ERR_INVALID`, and a MAC
  on a retired field writes nothing (a retired value gets no version of its
  own);
- a tree read back carries no MAC, a draft's JSON holds none, and so a draft
  applied writes none for the values it changes.

The MAC is never a column of a group, revision or current row, and a rebuild
(`gdefstor_model_rebuild()`) leaves the blind table alone: its rows are truth
while their subject exists.

**Blind equality** is the query node `GDEFSTOR_QUERY_BLIND` (`query.h`): a
personal field equals a MAC. It matches an entity when some current item of
the field's group has a value whose current version has a blind row with that
MAC, so an older version's MAC does not match, and it composes with every
other clause. Personal fields stay refused for sorting and ranges.

**The history lookup**, `gdefstor_blind_history()`: every entity of a type
with any version of a personal field that had a given MAC, with the revisions
each version held (`valid_from` up to `valid_to`, 0 while current).

**The subject lookup**, `gdefstor_subject_lookup()`: "everything about this
person", every personal value version of every type that names a subject,
across history, from the subject recorded beside each ciphertext, through the
subject columns' index. It never reads the blind table, and does not see
drafts.

**Deletion**, `gdefstor_blind_delete_subject()`: erasure's data-store step,
after the key store has destroyed the key and appended the ledger. It deletes
every blind row of the subject in every type, is idempotent, and leaves the
ciphertext rows, so the subject lookup still finds every value afterwards and
blind equality and the history lookup find none.

A backend serves this through three optional members (`model_backend.h`);
one that does not refuses every call here `GDEFSTOR_ERR_UNSUPPORTED`.

## Records, counters and the outbox

Core writes more than entities in one transaction: system records (installed
and enabled modules, machine-name maps), the config epoch (AD-31) and outbox
rows (AD-33). These three families live in the entity write, so one
`GDEFSTOR_Write` commits an entity save, a record put, a counter's next value
and an outbox append together, or none of them. They are entity operations:
a write that holds key/bytes rows refuses them, and the reverse.

Names are machine-name-like text that compares by bytes: a record's
namespace, a counter's name and an outbox stream are 1 to 128 bytes of
`[A-Za-z0-9_.:-]`, a record's key 1 to 256 bytes of the same, an outbox
format 1 to 128 bytes of the same or `/` (`<name>/<version>`, AD-34).

**Records** are `(namespace, key) -> bytes`, at most 1 MiB, with no history:
`gdefstor_record_put()` inserts or replaces, `gdefstor_record_delete()`
deletes (a missing record deletes nothing and says so), `gdefstor_record_get()`
reads, and a missing record is `GDEFSTOR_OK` with `found` 0, never an error,
and distinct from an empty value. `gdefstor_record_list()` lists one
namespace's records whose key starts with a prefix (empty for all), ordered
by key bytes, with an offset and a limit of 1 to 1000. A record holds no
personal value: it has no subject column, so erasure could not find one;
personal data is a personal field of an entity.

**Counters** are named, per tenant, and monotonic. `gdefstor_counter_next()`
takes the counter's value plus one and stores it in the write; the first
value is 1; `gdefstor_counter_peek()` reads the last committed value (0 for
none). A value is never handed out twice: a write that took a value and then
aborted (an abort, a broken write, a failed commit) leaves the value taken,
so the next write takes the one after it and committed values may have gaps.
A backend keeps a floor apart from the committed value for this.

**The outbox** follows AD-33. `gdefstor_model_outbox_append()` adds a row to a
stream in the write: a public id unique in the tenant, a format name and an
opaque payload of at most 1 MiB, which the port never reads. The row takes
the stream's next sequence (1, 2, 3 ... per tenant and stream) in the write;
it appears when the write commits and never when it aborts, and an aborted
write's sequence numbers are given back with it. A public id already used is
refused, and since the sequence was taken by then the write is broken.

The calls that deliver run each in a write of their own, so they wait for the
tenant's open write as a write does:

- `gdefstor_model_outbox_claim(stream, lease, now, max)` takes rows for
  delivery under a lease ending at `now + lease` (the caller's clock). A
  stream has at most one live claim: while any of its rows that is not
  parked holds a lease that has not ended, a claim takes nothing. Otherwise
  it takes the lowest-sequence rows that are not parked, in order, at most
  `max` (1 to 1000): rows never claimed and rows whose lease ended without a
  delivery. So delivery is at least once and in order within a stream, and
  sinks deduplicate by public id.
- `gdefstor_model_outbox_delivered(ids)` deletes delivered rows; one that is
  gone already is skipped.
- `gdefstor_model_outbox_failed(id, now)` counts a failed delivery and ends the
  row's lease at `now`. The fifth failure (`GDEFSTOR_OUTBOX_PARK_FAILURES`)
  parks the row: it is never claimed again and never deleted, by a delivery or
  anything else, and `gdefstor_model_outbox_parked()` counts parked rows of a
  stream or of every stream, for the admin.

All three families are truth: `gdefstor_model_rebuild()` never touches them,
`gdefstor_model_dump()` lists them, and a snapshot export includes records
(the format is the export's, AD-23).

## Derived data

The current tables, the newest state and the public-id lookup are derived:
written in the same transaction as the truth and rebuildable from it.
`gdefstor_model_rebuild()` drops and rebuilds them in a write. The
conformance suite checks that a rebuild changes nothing and that queries
answer alike before and after.

`gdefstor_model_dump()` is a canonical text of a tenant's stored rows, for
tests and tools (snapshots are a separate, logical export). One line per row,
sorted bytewise, strings in lower-case hex (`-` when empty), doubles as their
64 bits in hex:

```
T <type> <declaration>
E <row> <type> <public id> <creator> <created> <revision> <last item> <published> <deleted>
S <row> <seq> <published> <deleted> <actor> <when>
R <row> <revision> <commit seq> <who> <when> <why> <languages>
I <row> <group> <item> <valid_from> <valid_to> <parent> <order> <n> <value>... <language>
D <public id> <type> <base> <json>
B <row> <item> <valid_from> <field id> <subject> <mac> <key id> <version> <language>
K <namespace> <key> <value>
C <kind> <name> <value>
O <stream> <seq> <public id> <format> <payload> <lease until> <failures> <parked>
```

A value is `v<type>`, then for a set value its integer, its double, its bytes
and its subject. The language is the last word of an `I` and a `B` row, so
the words before it keep their places. Trailing absent columns are left out, so a row written
before a column existed and one written after with it empty read alike.
`GDEFSTOR_MODEL_TRUTH` dumps `T`, `E`, `S`, `R`, every `I` version, `D`,
every blind row `B`, every record `K`, every counter `C` with a committed
value above 0 (kind 1 a counter, kind 2 an outbox stream's last sequence; a
counter's floor is not listed, so an aborted write leaves the dump as it was)
and every outbox row `O`, parked or not;
`GDEFSTOR_MODEL_CURRENT` dumps `E` and the current `I` rows.

## Formats

**`defiant-type/1`**, the stored declaration, written canonically by the
port:

```json
{"format":"defiant-type/1","name":"site.note","next_id":3,"next_group":2,"columns":1,
 "fields":[{"name":"title","id":1,"type":"text","scale":0,"flags":6,"retired":0,"subject":null,"group":-1,"column":0},
           {"name":"tags","id":2,"type":"text","scale":0,"flags":1,"retired":0,"subject":null,"group":1,"column":0}]}
```

A group field adds `"columns"` and `"fields"`. A stored declaration that does
not parse, or whose ids, groups and columns do not fit together, is
`GDEFSTOR_ERR_CORRUPT`.

**`defiant-tree/1`**, a tree in JSON, which a draft holds one of per
language:

```json
{"format":"defiant-tree/1","root":{"@id":1,"title":"Hello","total":"34.99","placed":"2025-10-17",
 "tags":[{"@id":4,"@v":"a"},"b"],
 "lines":[{"@id":5,"sku":"A-1","qty":2}],
 "note":{"@c":"0a1b...","@s":"<subject>"}}}
```

An item is an object: `"@id"` (absent or 0 for a new item), then its fields
by name. A single scalar is its value; a multiple scalar an array of
`{"@id":n,"@v":value}` or bare values (new items); a single group an item;
a multiple group an array of items. Text and references are strings,
integers and floats JSON numbers, booleans `true`/`false`, decimals strings
with at most the field's scale of digits after the point, dates
`YYYY-MM-DD`, date-times `YYYY-MM-DDTHH:MM:SS.mmmZ` (UTC, the milliseconds
optional on input), personal values `{"@c":"<ciphertext in hex>","@s":"<subject>"}`.
A field the type does not have, or a value of the wrong shape, is
`GDEFSTOR_ERR_INVALID`; a newer `defiant-tree` version is
`GDEFSTOR_ERR_UNSUPPORTED`. Parsing needs the declaration
(`gdefstor_tree_from_json()`), which says what each name is.

**`defiant-draft/1`**, a stored draft, written by the port:

```json
{"format":"defiant-draft/1","languages":{"de":{"format":"defiant-tree/1","root":{...}},"fr":{...}}}
```

The languages in ascending byte order, each once, each value the
`defiant-tree/1` document the caller put, byte for byte (spaces around it
included). A draft document this port did not write is `GDEFSTOR_ERR_CORRUPT`;
a newer `defiant-draft` version is `GDEFSTOR_ERR_UNSUPPORTED`.

## Limits

Fixed and documented here, so that no default becomes a capacity ceiling:

| Limit | Value |
|---|---|
| groups nested below the root group | 8 |
| fields of one type, every depth, retired ones included | 512 |
| fields directly in one group, retired ones included | 64 |
| items in one tree, the root included | 65,536 |
| one text, reference or personal value | 1 MiB |
| all values of one tree | 16 MiB |
| the reason of a revision | 4096 bytes |
| one draft, every language together | 32 MiB |
| a decimal's scale | 18 |
| languages one entity holds trees in at once | 64 |
| a language tag | 35 bytes |

Over a limit is `GDEFSTOR_ERR_LIMIT`, never a truncation. Public ids,
creators, actors and subjects are held to `GDEFSTOR_Limits::max_public_id_bytes`.

## Writes and isolation

Entity operations take a `GDEFSTOR_Write`: the tenant's writer queue and one
backend transaction, opened by the first operation. Reads take the store and
see the last commit, also while a write of the same tenant is open; inside a
write, each operation sees the write's own earlier ones. A write holds either
key/bytes rows (blobs, the outbox, projections, revision documents) or entity
operations, not both: the two go to different backends and nothing commits
across two. An operation refused before it reached the backend leaves the
write usable; one that failed after (an I/O error, a full allocator) marks it
broken, and its commit aborts and returns that error. Records, counters and
outbox appends are entity operations; the outbox's claim, delivery and
failure calls each open a write of their own.

## For the authors of a backend

`model_backend.h` is the interface: transactions, the catalog, entity rows,
states, one `apply` per revision (closes, inserts, the revision row and the
counters, all or nothing), reads of history and of items as of a revision or
current, drafts, `query`, `rebuild` and `dump`. The rules a backend keeps are
in that header: truth is append-only and `valid_to` is set once; current
tables are derived and rebuild to the same; a read with no transaction sees
the last commit. `query` must answer exactly as `gdefstor_model_scan()`, the
reference engine, which a backend with no engine of its own calls from its
`query` (the memory backend does). One member is optional and was added
later: `index_state`, called with `index_ctx` (so that a wrapper replacing
`ctx` still reaches the backend), says whether an indexed field's index is
built; a backend that leaves it NULL reads every indexed field as ready, and
one built against the first version of the struct sets `size` to
`GDEFSTOR_MODEL_BACKEND_SIZE_V1`. The blind index came next: `blind_ctx`,
`blind_history`, `subject_lookup` and `blind_delete`, all three functions or
none (`GDEFSTOR_MODEL_BACKEND_SIZE_V2` is the size before them); a backend
that sets them also writes `GDEFSTOR_ModelApply::blinds` in `apply` and visits
blind rows through `GDEFSTOR_ModelVisitor::blind` from `items`, `dump` and
the reference engine's scan. Records, counters and the outbox came after:
`sys_ctx` and eleven functions, all or none (`GDEFSTOR_MODEL_BACKEND_SIZE_V3`
is the size before them). They keep those rows in the same transactions as
the entities, leave them alone in `rebuild`, list them in `dump` through the
visitor's `record`, `counter` and `outbox`, and keep each counter's floor so
that an aborted transaction's value is never returned again
(`model_backend.h` says each member's contract; the port checks names,
values and what each answer holds). With languages (story S6a2) every item
version, close and blind row carries its language and every revision row
the languages it changed, `items` takes a language (or none, every
language), and an optional `languages` member, with `lang_ctx`, lists the
languages an entity has a tree in from its root items; a backend without it
(`GDEFSTOR_MODEL_BACKEND_SIZE_V4` is the size before it) is read through
`items` instead, which reads every language's whole tree. A backend keeps the
language as a column, never as a table or a column per language, and reads
one language's items without the others' where it is asked for one.

Every backend runs the conformance suite, installed beside the port:
`model.inc` (named cases for every rule above, the purchase-order fixture,
and a seeded random sweep of declarations, saves, drafts, reverts, reorders,
conflicts, state changes and queries checked against a model written apart
from the library), `system.inc` (records, counters and the outbox, which
`model.inc` includes, with a seeded sweep of their own), `language.inc` (the
language axis, which `model.inc` includes, with a seeded multilingual sweep)
and `keystore.inc`. `make check-model` here plants a defect
in each rule the port decides and requires the named test to fail; a
backend's own repository plants the defects a backend can make (a `valid_to`
written twice, a revert that reopens old rows, a current table that drifts
from the truth).

## The key store port

`keystore.h` is a port of its own, never part of the data store: subject keys
and the erasure ledger. A subject's key is made inside the key store and
never leaves it; callers wrap and unwrap bytes under it. Erasure destroys the
key and appends the ledger row in one step; afterwards every wrap and unwrap
for the subject reports it erased and no new key can be made for it. A
ledger row learned from elsewhere (a snapshot's copy, replayed on restore) is
appended with the same effect. A key store that seals in process is given a
cipher (`GDEFSTOR_KeyCipher`) by the caller, so this library holds no
cryptography. Nothing is wired to the entity port yet.

Besides the keys it makes, a key store keeps two things a caller seals itself:

- **Sealed material** (`gdefstor_keystore_subject_put()`,
  `gdefstor_keystore_subject_sealed()`): a subject key the caller wrapped under
  its own keyring, kept as that subject's key. The store never opens it. It
  goes in the same step as the ledger row when the subject is erased, and an
  erased subject can never be given it again. A subject has a key the store
  made or sealed material, never both, and a key the store made is never read
  back.
- **Tenant secrets** (`gdefstor_keystore_secret_put()`,
  `gdefstor_keystore_secret_get()`): written once, such as the wrap of a
  tenant's keyring. The same bytes again change nothing; other bytes are
  refused.

**The key set.** `gdefstor_keystore_export()` writes a tenant's secrets, keys,
sealed material and ledger, with the time it was taken, as
`defiant-keyset/1` (text: a header line, `taken <ms>`, then one line per row,
`secret`, `key`, `sealed` and `ledger` in that order, bytes in lower-case
hex). `gdefstor_keystore_import()` merges one back in one step: the set's
ledger rows are replayed as erasures, a key or sealed material for a subject
either ledger names is dropped, an equal row is kept, and a secret or key the
tenant holds with other bytes refuses the whole import. Importing a set twice
changes nothing. A key set holds keys: it is the key half of a backup pair,
never inside a snapshot. The data half is a snapshot taken with
`gdefstor_snapshot_export_at()`, whose manifest records `"taken_ms"`
(`gdefstor_snapshot_taken()` reads it); the restore that pairs the two, and
refuses a key set older than its data set, is core's.

A backend of `GDEFSTOR_KEYSTORE_BACKEND_SIZE_V1` (without these members) is
still accepted; every call that needs them is `GDEFSTOR_ERR_UNSUPPORTED` on it.

## Not built here

- **Translations with their own revision history** (access spec D5): every
  language shares the entity's one history. Designed with the first type
  that needs it.
- **Large values by content hash.** Values are stored inline, up to 1 MiB.
- **Every use of the key store by the entity port**: computing MACs,
  encrypting and erasing are core's (S5b, S6).
- **Blobs in the entity transaction.** Until then a write that holds blobs
  holds no entity operation.
- **Reading a record or a counter inside a write.** Reads see the last
  commit; the backend members take a transaction already, so the port call
  can be added.
- **Select columns** in a query: a caller reads each entity's tree.
- **Correlated conditions on nested fields** ("the same line has sku A and
  quantity 5"): each comparison on a path is decided on its own.
- **Public ids for revisions**: a revision is named by its entity and number.
