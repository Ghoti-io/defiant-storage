# defiant-storage

The Defiant storage port: the entity port (`model.h`). Declared types with
typed, nested and multiple fields; entities with one linear revision history
each, in which only changed items get new rows and `valid_to` is written
once; reverts as new revisions; drafts outside history; current tables that
rebuild from the truth; the query form over fields (`query.h`); and, in the
same write as entities, records, monotonic counters and an ordered outbox
with leases. A read sees the last commit. Writes to one tenant are
serializable: a second writer on a fiber parks until the first commits, and a
caller that is not a fiber gets `GDEFSTOR_ERR_BUSY`. A backend implements
`model_backend.h`; the port decides what every write means. See
`documentation/model.md`. The key store port (`keystore.h`) holds subject keys
and the erasure ledger.

The backends are separate libraries: `defiant-storage-memory` (in memory),
and `defiant-storage-sqlite` and `defiant-storage-postgres`, which share the
SQL compiler and schema of `defiant-storage-sql`. No SQL is in this library.
The port does not hash plaintext and does not encrypt.

Formats are `defiant-snapshot/1`, whose members of one entity's revisions are
`defiant-revision/1` (`documentation/formats.md`), and `defiant-type/1`,
`defiant-tree/1` and `defiant-draft/1` (`documentation/model.md`); every one is
at version 1 and its reader refuses every other. The query form (a predicate
tree, a sort and a page, with a total) is in `documentation/query.md`.

Depends on `cutil`, `text` and `archive`, found only through pkg-config.

```bash
make test PREFIX=/path/to/prefix
make test-valgrind test-asan fuzz FUZZ_TIME=30 PREFIX=/path/to/prefix
```

`make test` also runs `check-symbols`, `check-aliasing`, `check-seams` and `check-model`.

Backends run the conformance suites installed beside the port: `model.inc`
(the entity port, with `system.inc`, `language.inc` and `snapshot.inc`) and
`keystore.inc`.
