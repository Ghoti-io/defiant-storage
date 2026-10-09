# Formats

Writers emit the current version. A reader accepts that version and refuses
a higher one with `GDEFSTOR_ERR_UNSUPPORTED`. An unknown format name is the
same error. A document that is this format but wrong is `GDEFSTOR_ERR_CORRUPT`.
Nothing over a limit is truncated: the call returns `GDEFSTOR_ERR_LIMIT`.

## defiant-revision/1

One JSON object, written canonically by `gdefstor_revision_write`:

```json
{"format":"defiant-revision/1","seq":1,"entity":"<public-id>","type":"core.post","lang":{"en":{"title":["Hello"]}},"body":""}
```

`seq` is a positive integer the caller supplies. The revision's identity is
that sequence together with `entity`. Appending the same identity again
leaves the first document and returns `GDEFSTOR_ERR_INVALID`.

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

A zip. The member named `manifest` is:

```json
{"format":"defiant-snapshot/1","entries":[{"key":"r:...","kind":"revision"}]}
```

`kind` is `revision`, `blob`, `projection` or `outbox`. Every other member
name is a storage key (`r:`, `b:`, `p:` or `o:` plus hex of the caller-supplied
ids). Names are never joined to a filesystem path. A name with an empty
segment, a segment `.` or `..`, or a backslash is `GDEFSTOR_ERR_CORRUPT`,
and the tenant is left unchanged. A slash only separates those segments.
The tenant is an argument of import, not a member of the zip.

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
