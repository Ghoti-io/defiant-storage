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
  '  if (s->nclose == 0 && s->ninsert == 0) {
    return GDEFSTOR_OK;
  }' \
  '  if (0) {
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

plant_finish
