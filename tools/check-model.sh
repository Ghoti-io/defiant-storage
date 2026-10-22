#!/bin/sh
#
# SPDX-License-Identifier: LGPL-3.0-only
#
# Show that each defect planted in the entity port (model.h) turns a named
# entity conformance test red. The port decides what every write means - which
# items changed, their order keys, the base revision, retired fields - and
# holds the reference query engine every backend's answer must equal, so a
# defect here is a defect on every backend. The suite runs here against this
# repository's scratch backend (tests/conformance/test_model.cpp); the
# defects that live in a backend are planted in that backend's repository.
#
# Usage: check-model.sh <make> <build-dir> [make args]

set -u

MAKE="${1:?usage: check-model.sh <make> <build-dir> [make args]}"
BUILD_DIR="${2:?usage: check-model.sh <make> <build-dir> [make args]}"
shift 2

. tools/plant-lib.sh
plant_setup "$MAKE" "$BUILD_DIR" "$@"
plant_control testModel 'Model*'

# A save that changes nothing writes a revision anyway.
plant noop-adds-revision src/model.c testModel \
  'ModelSave.SameTreeWritesNothing ModelSweep.RandomHistoriesMatchAnIndependentModel' \
  '  if (rc == GDEFSTOR_OK && touched.n == 0) {
    langs_release(&touched);
    return GDEFSTOR_OK;
  }' \
  '  if (0) {
    langs_release(&touched);
    return GDEFSTOR_OK;
  }'

# An item that did not change gets a new version anyway.
plant unchanged-item-rewritten src/model.c testModel \
  'ModelSave.UnchangedItemIsNotRewritten ModelSweep.RandomHistoriesMatchAnIndependentModel' \
  '      if (item_same(s, x, b)) {' \
  '      if (0 && item_same(s, x, b)) {'

# A write built on a revision that is no longer the newest is accepted.
plant stale-base-accepted src/model.c testModel \
  'ModelSave.StaleBaseIsAConflictAndWritesNothing ModelSweep.RandomHistoriesMatchAnIndependentModel' \
  '  if (e->revision != base_revision) {' \
  '  if (e->revision != base_revision && 0) {'

# A draft is applied on whatever the head is now, not on its base.
plant draft-on-moved-head src/model.c testModel \
  'ModelDraft.AppliedOnAMovedHeadIsRefused' \
  '  if (rc == GDEFSTOR_OK && exists && e.revision != c.base) {' \
  '  if (rc == GDEFSTOR_OK && exists && 0) {'

# Renumbering spread over revisions: only half the siblings are renumbered in
# the revision that needed it.
plant renumber-partial src/model.c testModel \
  'ModelOrder.RenumberingHappensInOneRevision' \
  '      for (k = 0; k < m; k++) {
        s->t[run[k]].order = (double)(k + 1);' \
  '      for (k = 0; k < m / 2; k++) {
        s->t[run[k]].order = (double)(k + 1);'

# Renumbering put off until keys collide, so revisions hold siblings closer
# than the epsilon and the renumbering lands in a later one.
plant renumber-deferred src/model.c testModel \
  'ModelOrder.RenumberingHappensInOneRevision' \
  '    if (!(gap >= GDEFSTOR_MODEL_ORDER_EPSILON)) {' \
  '    if (!(gap > 0.0)) {'

# A retired field's stored value is not carried into the next revision.
plant retired-data-lost src/model.c testModel \
  'ModelDeclare.RetiredFieldDataStaysReadable ModelSweep.RandomHistoriesMatchAnIndependentModel' \
  '          x->cols[c] = *base_col(b, c);' \
  '          (void)b;'

# A field's type changed in place.
plant type-change-accepted src/type.c testModel \
  'ModelDeclare.ChangingATypeIsRefused' \
  '    if (s->type != o->type || s->scale != o->scale ||' \
  '    if (s->scale != o->scale ||'

# The last clause of an AND ignored.
plant and-drops-last-clause src/model_query.c testModel \
  'ModelQuery.AndUsesEveryClause ModelSweep.RandomHistoriesMatchAnIndependentModel' \
  '      if (!r) {
        size_t j;
        for (j = i + 1; j < n->child_count; j++) {
          e->exists_at += exists_below(n->children[j]);
        }
        *out = 0;' \
  '      if (!r && i + 1 < n->child_count) {
        size_t j;
        for (j = i + 1; j < n->child_count; j++) {
          e->exists_at += exists_below(n->children[j]);
        }
        *out = 0;'

# The total counted before the filter: every entity scanned, not every match.
plant count-before-filter src/model_query.c testModel \
  'ModelQuery.TotalCountsMatchesNotScanned ModelSweep.RandomHistoriesMatchAnIndependentModel' \
  '  if (holds) {
    e->total += 1;' \
  '  e->total += 1;
  if (holds) {'

# The final tiebreak removed: equal keys come out in the backend's order.
plant no-tiebreak src/model_query.c testModel \
  'ModelQuery.TiebreakIsTheRow ModelSweep.RandomHistoriesMatchAnIndependentModel' \
  '  return x->row < y->row ? -1 : (x->row > y->row ? 1 : 0);' \
  '  return 0;'

# A blind row written for an unchanged value: an item the save leaves as it
# is gets a row for a version that is never written (story S5a).
plant blind-row-for-unchanged src/model.c testModel \
  'ModelBlind.AnUnchangedValueWritesNoBlindRow' \
  '        /* Unchanged: no new version, so no blind row either. */
        continue;' \
  '        blinds_of(s, x, 0, NULL);
        continue;'

# Blind equality in the reference engine matching an older version: a row is
# taken for the item, whichever version of it the row indexes.
plant blind-older-version src/model_query.c testModel \
  'ModelBlind.EqualityMatchesOnlyTheCurrentVersion ModelBlind.ARevertBringsBackTheBlindRowsOfTheValuesItRestores' \
  '      if (b->item == it->item && b->from == it->from && b->field == f->id &&' \
  '      if (b->item == it->item && b->field == f->id &&'

# A blind row put back for a version that does not exist: the item's version
# that holds at that revision is taken, whichever revision it began in, so
# the row indexes a valid_from no version has (story S6c).
plant blind-put-any-version src/model.c testModel \
  'ModelBlindPut.APutForNoSuchVersionIsRefused' \
  '    if (x->item == item && x->from == valid_from && x->group == group) {' \
  '    if (x->item == item && x->group == group) {'

# A query reading another language: the reference engine keeps every
# language's items, so a field predicate or a sort sees them all (story S6a2).
plant language-other-read src/model_query.c testModel \
  'ModelLanguage.AQueryReadsOnlyItsLanguage ModelLanguageSweep.SeededHistoriesAgreeWithAModel' \
  '    if (!in_language(e, it->lang, it->lang_len)) {' \
  '    if (0) {'

# A missing language matching: an entity with no tree in the query's language
# falls back to its other languages instead of reading as all absent.
plant language-missing-matches src/model_query.c testModel \
  'ModelLanguage.AnEntityWithNoTreeInTheLanguageMatchesNoField' \
  '  size_t k = 0;
  for (i = 0; i < s->nitems; i++) {' \
  '  size_t k = 0;
  for (i = 0; i < s->nitems &&
       !in_language(e, s->items[i].lang, s->items[i].lang_len); i++) {
  }
  if (i == s->nitems) {
    return;
  }
  for (i = 0; i < s->nitems; i++) {'

# An unchanged language rewritten: a language a save names gets a new version
# of its root whether or not its tree changed.
plant unchanged-language-rewritten src/model.c testModel \
  'ModelLanguage.ChangingOneLanguageWritesOnlyItsItems' \
  '      if (item_same(s, x, b)) {' \
  '      if (item_same(s, x, b) && i != 0) {'

# A current-table row exported: the snapshot also takes the current tables'
# items, which are derived, beside the truth's (story S6a3).
plant snapshot-exports-current src/model_snapshot.c testModel \
  'ModelSnapshot.HoldsTheTruthAndNoDerivedRow' \
  '  rc = store->model.dump(store->model.ctx, tenant, tenant_len,
      GDEFSTOR_MODEL_TRUTH, &v);
  if (rc == GDEFSTOR_OK) {
    rc = link_rows(m);
  }' \
  '  rc = store->model.dump(store->model.ctx, tenant, tenant_len,
      GDEFSTOR_MODEL_TRUTH, &v);
  if (rc == GDEFSTOR_OK) {
    size_t ne = m->nents;
    rc = store->model.dump(store->model.ctx, tenant, tenant_len,
        GDEFSTOR_MODEL_CURRENT, &v);
    while (m->nents > ne) {
      m->nents -= 1;
      gcu_allocator_free(m->alloc, m->ents[m->nents].type);
      gcu_allocator_free(m->alloc, m->ents[m->nents].pid);
      gcu_allocator_free(m->alloc, m->ents[m->nents].creator);
    }
  }
  if (rc == GDEFSTOR_OK) {
    rc = link_rows(m);
  }'

# An item version's valid_from changed on import: a version that revision 1
# wrote and nothing closed comes back as written by revision 2.
plant snapshot-import-moves-valid-from src/model_snapshot.c testModel \
  'ModelSnapshot.EveryRevisionReadsBackAsOfAfterImport ModelSnapshot.ARoundTripKeepsEveryTruthRow' \
  '    rc = read_order(it, &x->order);' \
  '    if (x->from == 1 && x->to == 0 && e->revision > 1) {
      x->from = 2;
    }
    rc = read_order(it, &x->order);'

# A language dropped: the export keeps only the versions in the language of
# the first version it met.
plant snapshot-drops-a-language src/model_snapshot.c testModel \
  'ModelSnapshot.EveryLanguageComesBack' \
  '  /* Trailing no-values are not stored differences (model.md). */' \
  '  if (m->nvers > 0 && !same(i->language, i->language_len, m->vers[0].lang,
                         m->vers[0].lang_len)) {
    return GDEFSTOR_OK;
  }
  /* Trailing no-values are not stored differences (model.md). */'

# An import into a tenant that already holds entity truth accepted.
plant snapshot-import-into-non-empty src/snapshot.c testModel \
  'ModelSnapshot.ImportIntoANonEmptyTenantIsRefusedInWords' \
  '  if (rc == GDEFSTOR_OK && !empty) {' \
  '  if (0) {'

# A snapshot member without its format (story S9): an entity's member is
# written without "format":"defiant-revision/1".
plant snapshot-member-without-format src/model_snapshot.c testModel \
  'ModelSnapshot.EveryEntityMemberSaysItsFormat' \
  '    j_lit(&j, "{\"format\":\"" REVISION_FORMAT "\",\"type\":");' \
  '    j_lit(&j, "{\"type\":");'

# A reader accepting another version (story S9), twice: an entity's member
# read whatever defiant-revision version it says, and a manifest whatever
# defiant-snapshot version.
plant revision-reader-takes-another-version src/model_snapshot.c testModel \
  'ModelSnapshot.ASnapshotOrMemberAtAnotherVersionIsRefusedInWords' \
  '  if (!same(p, n, REVISION_FORMAT, strlen(REVISION_FORMAT))) {' \
  '  if (!(n > strlen(REVISION_PREFIX) &&
          memcmp(p, REVISION_PREFIX, strlen(REVISION_PREFIX)) == 0)) {'

plant snapshot-reader-takes-another-version src/snapshot.c testModel \
  'ModelSnapshot.ASnapshotOrMemberAtAnotherVersionIsRefusedInWords' \
  '  if (len == name_len + 2 && text[name_len + 1] == '"'"'1'"'"') {' \
  '  if (len >= name_len + 2) {'

# A counter reset on import: a caller's counter comes back at 0.
plant snapshot-import-resets-a-counter src/model_snapshot.c testModel \
  'ModelSnapshot.CountersContinueAndOutboxRowsKeepTheirPlace' \
  '        c->name, c->name_len, c->value);' \
  '        c->name, c->name_len,
        c->kind == GDEFSTOR_MODEL_COUNTER ? 0 : c->value);'

# An EXISTS read in the query's language, whatever language it names (story
# S6b: relation rows are kept in one language, the outer list in another).
plant exists-language-ignored src/model_query.c testModel \
  'ModelQuery.AnExistsReadsTheLanguageItNames' \
  '    if (x->exists_language != NULL) {' \
  '    if (x->exists_language != NULL && 0) {'

# A flagged field with no value read as no value, not as the empty text.
plant empty-if-none-ignored src/model_query.c testModel \
  'ModelQuery.EmptyIfNoneReadsNoValueAsTheEmptyText' \
  '    if (out->n == 0 && (o->flags & GDEFSTOR_QUERY_EMPTY_IF_NONE) != 0) {' \
  '    if (out->n == 0 && (o->flags & GDEFSTOR_QUERY_EMPTY_IF_NONE) != 0 && 0) {'

# FIRST sorts by whichever item the scan meets first, not the first in order.
plant first-is-any-item src/model_query.c testModel \
  'ModelQuery.FirstSortsAMultipleByItsFirstItem' \
  '    if (best == NULL || it->order < best->order ||
        (it->order == best->order && it->item < best->item)) {' \
  '    if (best == NULL) {'

# REVISION_WHO reads the entity'"'"'s creator instead of the newest author.
plant revision-who-is-the-creator src/model_query.c testModel \
  'ModelQuery.RevisionWhoIsTheNewestRevisionsAuthor' \
  '      out->own.bytes = (const uint8_t *)s->rwho;
      out->own.len = s->rwho_len;' \
  '      out->own.bytes = (const uint8_t *)s->creator;
      out->own.len = s->creator_len;'

# AS_TEXT leaves a reference a reference, so it never meets text.
plant as-text-ignored src/model_query.c testModel \
  'ModelQuery.AsTextComparesAReferenceWithText' \
  '    out->type = GDEFSTOR_FIELD_TEXT;
  }
  return GDEFSTOR_OK;' \
  '  }
  return GDEFSTOR_OK;'

# An import that writes every revision back at one stamp, the entity's
# newest, so history loses the versions it was written at (AD-15).
plant snapshot-import-restamps-history src/model_snapshot.c testModel \
  'ModelSnapshot.EveryRowKeepsItsSchemaStampThroughAnExportAndAnImport' \
  '    rev.schema = sr->schema;
    rev.schema_len = sr->schema_len;' \
  '    rev.schema = m->revs[e->rev0 + e->nrev - 1].schema;
    rev.schema_len = m->revs[e->rev0 + e->nrev - 1].schema_len;'

# A draft applied as data of the apply's stamp, not of the shape the draft
# was written in.
plant draft-applied-at-the-callers-stamp src/model.c testModel \
  'ModelSnapshot.EveryRowKeepsItsSchemaStampThroughAnExportAndAnImport' \
  '  stamped.schema = c.schema;
  stamped.schema_len = c.schema_len;' \
  '  stamped.schema = meta->schema;
  stamped.schema_len = meta->schema_len;'

# The writer queue admitting a second writer of a tenant while the first is
# open (store.h): writes to one tenant stop being serializable.
plant second-writer-admitted src/store.c testModel \
  'ModelWriters.ASecondWriterOnAFiberParksUntilTheFirstCommits' \
  '    if (!gate->held) {
      break;
    }' \
  '    if (1) {
      break;
    }'

# A backend that gives up a revision, a draft or a record with an empty
# schema stamp is corrupt. Every row is written with a stamp, so none can lack
# one; a port that let the empty one through would hand a reader a row whose
# shape it cannot know. One plant for each place a backend's row is taken: the
# read, and the export that copies it into a snapshot.
plant empty-revision-stamp-accepted src/model.c testModel \
  'ModelBackendStamp.AnEmptyRevisionStampIsCorrupt' \
  '  if (!langs_text_ok(r->languages, r->languages_len) ||
      !gdefstor_schema_ok(r->schema, r->schema_len)) {' \
  '  if (!langs_text_ok(r->languages, r->languages_len) ||
      (r->schema_len != 0 && !gdefstor_schema_ok(r->schema, r->schema_len))) {'

plant empty-draft-stamp-accepted src/model.c testModel \
  'ModelBackendStamp.AnEmptyDraftStampIsCorrupt' \
  '  if (!gdefstor_schema_ok(d->schema, d->schema_len)) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  c->type = ' \
  '  if (d->schema_len != 0 && !gdefstor_schema_ok(d->schema, d->schema_len)) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  c->type = '

plant empty-record-stamp-accepted src/system.c testModel \
  'ModelBackendStamp.AnEmptyRecordStampIsCorrupt' \
  '      !gdefstor_schema_ok(r->schema, r->schema_len)) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  memcpy(c->schema' \
  '      (r->schema_len != 0 && !gdefstor_schema_ok(r->schema, r->schema_len))) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  memcpy(c->schema'

plant empty-record-stamp-listed src/system.c testModel \
  'ModelBackendStamp.AnEmptyRecordStampIsCorrupt' \
  '                       r->key_len) >= 0) ||
      !gdefstor_schema_ok(r->schema, r->schema_len)) {' \
  '                       r->key_len) >= 0) ||
      (r->schema_len != 0 && !gdefstor_schema_ok(r->schema, r->schema_len))) {'

plant empty-revision-stamp-exported src/model_snapshot.c testModel \
  'ModelBackendStamp.AnEmptyRevisionStampIsCorrupt' \
  '  if (!gdefstor_schema_ok(r->schema, r->schema_len)) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  rc = GROW(m, revs);' \
  '  rc = GROW(m, revs);'

plant empty-draft-stamp-exported src/model_snapshot.c testModel \
  'ModelBackendStamp.AnEmptyDraftStampIsCorrupt' \
  '  if (!gdefstor_schema_ok(d->schema, d->schema_len)) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  rc = GROW(m, drafts);' \
  '  rc = GROW(m, drafts);'

plant empty-record-stamp-exported src/model_snapshot.c testModel \
  'ModelBackendStamp.AnEmptyRecordStampIsCorrupt' \
  '  if (r == NULL || (r->bytes == NULL && r->len != 0) ||
      !gdefstor_schema_ok(r->schema, r->schema_len)) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  rc = GROW(m, recs);' \
  '  if (r == NULL || (r->bytes == NULL && r->len != 0)) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  rc = GROW(m, recs);'

# A snapshot row with an empty stamp read as though it were a stamp.
plant empty-stamp-imported src/model_snapshot.c testModel \
  'ModelSnapshot.AStampIsRequiredAndOnlyAStampIsTaken' \
  '         gdefstor_schema_ok(*out, *out_len);' \
  '         (*out_len == 0 || gdefstor_schema_ok(*out, *out_len));'

# The stale-object plant. A plant's copy starts from the finished build, so the
# edit has to be compiled into it; this one is made not to be (the object that
# depends on the edit is left looking up to date), and the gate has to refuse it
# as a STALE OBJECT. If it ever passes instead, a warm copy could pass with the
# defect never built in.
plant_stale stale-object src/model.c testModel \
  'ModelBackendStamp.AnEmptyRevisionStampIsCorrupt' \
  '  if (!langs_text_ok(r->languages, r->languages_len) ||
      !gdefstor_schema_ok(r->schema, r->schema_len)) {' \
  '  if (!langs_text_ok(r->languages, r->languages_len) ||
      (r->schema_len != 0 && !gdefstor_schema_ok(r->schema, r->schema_len))) {'

plant_finish
