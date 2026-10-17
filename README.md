# defiant-storage

The Defiant storage port. It stores immutable revision documents, blobs,
projection rows and outbox rows. A read sees the last commit. Writes to one
tenant are serializable: a second writer on a fiber parks until the first
commits, and a caller that is not a fiber gets `GDEFSTOR_ERR_BUSY`.

The in-memory backend is a separate library, `defiant-storage-memory`. SQLite
is not this library. The port does not hash plaintext, does not encrypt, and
does not know entity-type columns; those arrive in later stories.

Formats are `defiant-revision/1`, `defiant-outbox/1` and `defiant-snapshot/1`.
See `documentation/formats.md`. Queries over projection rows (a predicate tree, a
sort and a page, with a total) are in `documentation/query.md`.

Depends on `cutil`, `text` and `archive`, found only through pkg-config.

```bash
make test PREFIX=/path/to/prefix
make test-valgrind test-asan fuzz FUZZ_TIME=30 PREFIX=/path/to/prefix
```

`make test` also runs `check-symbols`, `check-aliasing`, `check-seams`, `check-defects` and `check-query`.
