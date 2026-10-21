# Formats

Writers emit the current version. A reader accepts that version and refuses
a higher one with `GDEFSTOR_ERR_UNSUPPORTED`. An unknown format name is the
same error. A document that is this format but wrong is `GDEFSTOR_ERR_CORRUPT`.
Nothing over a limit is truncated: the call returns `GDEFSTOR_ERR_LIMIT`.

## defiant-revision/1

One JSON object, written canonically by `gdefstor_revision_write`:

```json
{"format":"defiant-revision/1","seq":1,"row":1,"entity":"<public-id>","type":"core.post","lang":{"en":{"title":["Hello"]}},"body":""}
```

`seq` and `row` are positive integers the caller supplies. `row` is the
storage key, written in decimal. `entity` is the public id and is not the
key. A document with no `row`, or with `row` of 0, is `GDEFSTOR_ERR_CORRUPT`
and is not stored. The revision's identity is that sequence together with
`row`. Appending the same identity again leaves the first document and
returns `GDEFSTOR_ERR_INVALID`.

`lang` is the language axis. Each language is an object of fields, and each
field value is an array of strings. This story does not write a
`{"blob":...}` member; a large value of that shape is still accepted by the
port when a caller supplies one.

An empty `body` means the body is inline in `lang`. A non-empty `body` is
the location an archive module already chose; the port stores the string
and does not open it.

A large field value is an object whose only member is `blob`, holding the
caller-supplied hash: `{"blob":"<hash>"}`. The port does not hash the bytes.

## defiant-outbox/1

```json
{"format":"defiant-outbox/1","id":"<public-id>","stream":"<name>","seq":1,"payload":{}}
```

`seq` is assigned at commit, one higher than the highest committed sequence
in that stream. The payload is one JSON value the caller supplied. A log
line is that document plus a newline, written by `gdefstor_outbox_run` after
the commit, never inside it.

## defiant-snapshot/1

The snapshot (AD-23, AD-34): a zip that is a logical export of one tenant's
truth, readable without the framework. Writers write version 1, and a reader
reads version 1 and refuses every other with `GDEFSTOR_ERR_UNSUPPORTED`
(`gdefstor_snapshot_import_explain` says which, in words): until the suite's
first release a format is replaced in place and stays at 1, so no reader keeps
an earlier shape. The member named `manifest` is:

```json
{"format":"defiant-snapshot/1","taken_ms":1234,
 "model":["model/73686f702e6f72646572/type.json","model/73686f702e6f72646572/entities.json",
          "model/73686f702e6f72646572/6f31.json","model/drafts.json","model/records.json",
          "model/counters.json","model/outbox.json"]}
```

`taken_ms` is there when `gdefstor_snapshot_export_at` made it, and nothing
but `format`, `taken_ms` and `model` is. `model` lists the **entity part**,
the truth of the entity port (model.h), in the order the members are written;
it is empty for a tenant that holds none. Every member of the archive is the
manifest, a member `model` names, or `ledger`; anything else is
`GDEFSTOR_ERR_CORRUPT`. Names are never joined to a filesystem path: a name
with an empty segment, a segment `.` or `..`, or a backslash is
`GDEFSTOR_ERR_CORRUPT`, and the tenant is left unchanged. The tenant is an
argument of import, not a member of the zip.

The entity part is read through the backend's `dump` of the truth, so it is
the same whichever backend held it, and it holds no derived data: no current
table, no blind row, no index. Every list is sorted, so the same truth gives
the same bytes. A personal value is there as stored, ciphertext and subject;
no key is ever in a snapshot. Member names spell names in lowercase hex
(`73686f702e6f72646572` is `shop.order`), so no two differ only in case.

**`model/<hex of type>/type.json`**, one per type: the declaration exactly as
stored, a `defiant-type/1` document (model.md, "Formats"). Types come in byte
order of name.

**`model/<hex of type>/entities.json`**, one per type, after its declaration:

```json
{"type":"shop.order","entities":[{"id":"o1","order":3,"creator":"u2","created_ms":120,
  "revision":4,"last_item":9,"states":[{"published":true,"deleted":false,"actor":"u1","when_ms":5000}]}]}
```

The entities of the type in byte order of public id. `order` is the entity's
place among the tenant's entities (1, 2, 3 ... across every type), the order
its internal row had; rows themselves are internal (AD-34) and are given out
afresh on import, in this order. `revision` is its newest revision (0 for an
entity never saved), `last_item` the highest item id it has given out, and
`states` its state history, oldest first.

**`model/<hex of type>/<hex of public id>.json`**, one per entity, after its
type's entities, a **`defiant-revision/1`** document: one entity's revisions
and item versions. It says its format first; a member without it is
`GDEFSTOR_ERR_CORRUPT`, and one at another version `GDEFSTOR_ERR_UNSUPPORTED`.

```json
{"format":"defiant-revision/1","type":"shop.order","id":"o1",
 "revisions":[{"revision":1,"commit":1,"who":"u1","when_ms":1000,"why":"first","languages":"en"}],
 "items":[{"lang":"en","group":0,"item":1,"parent":0,"order":"0000000000000000","from":1,"to":2,
           "values":{"number":"A-1","total":"19.99","paid":false,"note":{"@c":"01fe","@s":"subj1"}}},
          {"lang":"en","group":2,"item":4,"parent":1,"order":"3ff0000000000000","from":1,"to":0,
           "values":{"sku":"a","qty":1,"price":"5.00"}}]}
```

`revisions` are revisions 1 to the entity's newest, each with its commit
sequence (`commit`: the same for every revision one write made), who, when,
why and the languages it changed. `items` is every item version of every
language, by group, item and `from`: `group` is the group number of the
declaration (0 the root group), `item` and `parent` the stable item ids,
`order` the order key's IEEE-754 bits in 16 hex digits (exact, and no locale
in it), `from` and `to` the version's `valid_from` and `valid_to` (0 while it
is open). `values` names each value by its field's name in that group and
spells it as `defiant-tree/1` does (model.md); a column with no value is left
out.

**`model/drafts.json`**: `{"drafts":[{"id":"o1","type":"shop.order","base":4,"document":"..."}]}`,
by public id; `document` is the stored `defiant-draft/1` document, byte for
byte, as a JSON string.

**`model/records.json`**: `{"records":[{"namespace":"core.module","key":"alpha","text":"..."}]}`,
by namespace and key. A value that is UTF-8 without NUL is `"text"`; any
other is `"hex"`, lowercase.

**`model/counters.json`**: `{"counters":[{"kind":"counter","name":"core.epoch","value":3}]}`:
every caller's counter (`counter`) and every outbox stream's sequence
(`stream`) at its committed value, by kind and name. A value an aborted write
took above the committed one is not truth and is not exported.

**`model/outbox.json`**: `{"outbox":[{"stream":"core.bus","seq":1,"id":"p1","format":"defiant-bus/1","text":"...","lease_until_ms":150,"failures":0,"parked":false}]}`,
by stream and sequence; the payload is `"text"` or `"hex"` as for records.
The drafts, records, counters and outbox members are left out when empty.

An import is untrusted (AD-37): every member is parsed and checked before
anything is written, and content an export does not write is
`GDEFSTOR_ERR_CORRUPT`: a declaration that is not canonical or not the
member's type, a public id twice, places that are not 1 to n, revisions that
are not 1 to the newest, a version outside them, closed before it opened,
named twice or with an id past `last_item`, a value its field refuses, a
draft of an undeclared type or based past its entity's head, a duplicate
record, counter or outbox row, an outbox row past its stream's counter. It
writes into a tenant that holds no truth (else `GDEFSTOR_ERR_INVALID`), in one
write: the declarations, the entities in their order with their states, each
revision with its commit sequence (current tables rebuilt as it is applied),
the drafts, the records, the counters at their values and the outbox rows
with their leases, failures and parked flags, through the backend's restore
members (model_backend.h). Blind rows are core's to rebuild from decryptable
values (AD-23).

## Limits

`gdefstor_limits_default` sets document 1 MiB, blob and member 16 MiB,
100000 members, snapshot total 64 MiB, zip ratio 100, public id / tenant /
type 256, body location 4096, hash 128, JSON depth 32, 64 columns, column
name 128, column value and outbox payload 1 MiB, stream 256, and 100000
rows of each kind. NULL means these defaults. Zero is a real cap.

The zip ratio is uncompressed size divided by compressed size, integer.
Equal to the cap is allowed. A stored member whose compressed size is 0 and
whose declared size is not is over the cap.

## Planted defects

`src/util.c` holds three switches `tools/check-defects.sh` flips:

- `gdefstor_plant_see_uncommitted` makes a read return an uncommitted write.
  `ReadSnapshot.OpenWriteIsInvisible` goes red.
- `gdefstor_plant_keep_outbox` publishes the outbox row on abort.
  `Abort.NothingVisible` goes red.
- `gdefstor_plant_enforce_limit` cleared skips the document-size cap.
  `Limits.DocumentStopsAtBound` goes red.

The unpatched tree passes.
