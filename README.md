# defiant-storage

The Defiant storage port. It stores immutable revision documents, blobs,
projection rows and outbox rows. A read sees the last commit. Writes to one
tenant are serializable: a second writer on a fiber parks until the first
commits, and a caller that is not a fiber gets `GDEFSTOR_ERR_BUSY`.

It also has the entity port (`model.h`): declared types with typed, nested
and multiple fields; entities with one linear revision history each, in which
only changed items get new rows and `valid_to` is written once; reverts as new
revisions; drafts outside history; current tables that rebuild from the truth;
the query form over fields; and, in the same write as entities, records,
monotonic counters and an ordered outbox with leases. A backend implements
`model_backend.h`; the
port decides what every write means. See `documentation/model.md`. The key
store port (`keystore.h`) holds subject keys and the erasure ledger.

The in-memory backend is a separate library, `defiant-storage-memory`. SQLite
is not this library. The port does not hash plaintext and does not encrypt.

Formats are `defiant-revision/1`, `defiant-outbox/1` and `defiant-snapshot/2`
(which reads `/1` too; `documentation/formats.md`), and `defiant-type/1` and `defiant-tree/1`
(`documentation/model.md`). Queries over projection rows (a predicate tree, a
sort and a page, with a total) are in `documentation/query.md`.

Depends on `cutil`, `text` and `archive`, found only through pkg-config.

```bash
make test PREFIX=/path/to/prefix
make test-valgrind test-asan fuzz FUZZ_TIME=30 PREFIX=/path/to/prefix
```

`make test` also runs `check-symbols`, `check-aliasing`, `check-seams`, `check-defects`, `check-query` and `check-model`.

Backends run the conformance suites installed beside the port:
`suite.inc` (key/bytes), `model.inc` (the entity port, with `system.inc`) and `keystore.inc`.
