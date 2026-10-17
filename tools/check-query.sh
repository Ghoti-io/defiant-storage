#!/bin/sh
#
# SPDX-License-Identifier: LGPL-3.0-only
#
# Show that each defect planted in the query form turns a named conformance
# test red. The port's own scan answers queries for a backend with no engine
# of its own (the memory backend is one), and its leaf comparisons and sort
# keys are what the SQLite backend calls, so a defect here is a defect in both.
#
# Usage: check-query.sh <make> <build-dir> [make args]

set -u

MAKE="${1:?usage: check-query.sh <make> <build-dir> [make args]}"
BUILD_DIR="${2:?usage: check-query.sh <make> <build-dir> [make args]}"
shift 2

. tools/plant-lib.sh
plant_setup "$MAKE" "$BUILD_DIR" "$@"
plant_control testConformance 'Query.*'
plant_control testConformance 'QueryEngine.*'

# A clause of an AND ignored: the last one cannot make it false.
plant and-drops-last-clause src/query.c testConformance \
  'Query.AndOrNotCombineEveryClause Query.Generated' \
  '        if (!r) {
          *out = 0;' \
  '        if (!r && i + 1 < n->child_count) {
          *out = 0;'

# `in` off by one: the last member of the list is never tried.
plant in-off-by-one src/query.c testConformance \
  'Query.InListMatchesEveryMember' \
  '  for (k = 0; k < list->n; k++) {' \
  '  for (k = 0; k + 1 < list->n; k++) {'

# A numeric comparison on text that is not an integer: a letter counts as 0.
plant numeric-on-garbage src/query.c testConformance \
  'Query.NumericComparisonOnlyOnIntegers' \
  "    if (p[i] < '0' || p[i] > '9') {
      return 0;
    }
    v = v * 10" \
  "    if (p[i] < '0' || p[i] > '9') {
      continue;
    }
    v = v * 10"

# The offset applied before the sort: the first rows in storage order are
# dropped, and the rest are sorted.
plant offset-before-sort src/query.c testConformance \
  'Query.PagesAreSlicesOfOneOrder Query.Generated' \
  '    rc = hits_sort(alloc, &order, hits, nhits);' \
  '    rc = hits_sort(alloc, &order,
        hits + (plan->query->offset < nhits ? (size_t)plan->query->offset : nhits),
        nhits - (plan->query->offset < nhits ? (size_t)plan->query->offset : nhits));'

# The final tiebreak removed: equal keys come out in storage order.
plant no-tiebreak src/query.c testConformance \
  'Query.TiebreakIsTheEntityArgument' \
  '  /* The entity argument, ascending, is the final tiebreak. */' \
  '  return;
  /* The entity argument, ascending, is the final tiebreak. */'

# The cap on a page removed.
plant no-limit-cap src/query.c testConformance \
  'Query.LimitIsClamped' \
  '(q->limit > GDEFSTOR_QUERY_LIMIT_MAX
                         ? GDEFSTOR_QUERY_LIMIT_MAX
                         : q->limit)' \
  'q->limit'

# The cap on the size of a tree raised past the test.
plant node-cap src/query.c testConformance \
  'Query.LimitsAndShapesAreRefused' \
  '  if (c->nodes > GDEFSTOR_QUERY_MAX_NODES) {' \
  '  if (c->nodes > GDEFSTOR_QUERY_MAX_NODES * 4) {'

# The cap on nesting raised by one.
plant depth-cap src/query.c testConformance \
  'Query.LimitsAndShapesAreRefused' \
  '  if (depth > GDEFSTOR_QUERY_MAX_DEPTH) {' \
  '  if (depth > GDEFSTOR_QUERY_MAX_DEPTH + 1) {'

# `in` against something that is not a constant list accepted.
plant in-against-column src/query.c testConformance \
  'Query.LimitsAndShapesAreRefused' \
  '          n->right.kind != GDEFSTOR_QUERY_CONST) {' \
  '          0) {'

# A repeated key read again: the last one wins as well.
plant json-repeated-key src/query.c testConformance \
  'Query.JsonPathReadsEscapesAndSkipsTheRest' \
  '  if (!pa->found1 && key_is(' \
  '  if (key_is(' \
  '  if (!pa->found2 && key_is(' \
  '  if (key_is('

# Text after the document ignored.
plant json-trailing src/query.c testConformance \
  'Query.DamagedDocumentIsCorruptNeverAGuess' \
  '  return j.i == j.n ? GDEFSTOR_OK : GDEFSTOR_ERR_CORRUPT;' \
  '  return GDEFSTOR_OK;'

# The colon left off the prefix of a type: a type whose hex begins another's
# is read as that type.
plant prefix-without-colon src/query.c testConformance \
  'Query.TypeNamesAreNotPatterns' \
  '"p:", type, type_len, "", 0, 1,
      out_prefix, out_len);' \
  '"p:", type, type_len, "", 0, 0,
      out_prefix, out_len);'

# A backend's answer of "unsupported" taken as the answer, not as a reason to
# scan.
plant no-scan-fallback src/query.c testConformance \
  'QueryEngine.AnEngineThatCannotCarryAQueryIsScanned' \
  '  if (rc == GDEFSTOR_ERR_UNSUPPORTED) {
    memset(rows, 0, sizeof *rows);' \
  '  if (rc == GDEFSTOR_ERR_UNSUPPORTED && store->backend.query == NULL) {
    memset(rows, 0, sizeof *rows);'

# An outer operand read from the row the EXISTS looks at.
plant exists-outer-as-inner src/query.c testConformance \
  'Query.ExistsIsASemiJoinOnTheOuterRow' \
  '            : ((o->flags & GDEFSTOR_QUERY_OUTER) != 0 ? &env->outer : &env->main);' \
  '            : &env->main;'

# The outer row'"'"'s entity read as the inner row'"'"'s.
plant exists-outer-entity src/query.c testConformance \
  'Query.ExistsReadsTheOuterRowsJoinedRow' \
  '    if ((o->flags & GDEFSTOR_QUERY_OUTER) != 0) {
      return texts_add(t, env->outer_entity, env->outer_entity_len);
    }' \
  '    if (0) {
      return texts_add(t, env->outer_entity, env->outer_entity_len);
    }'

# An EXISTS inside an EXISTS accepted.
plant exists-nested-allowed src/query.c testConformance \
  'Query.ExistsHasItsRules' \
  '      if (in_exists || n->child_count != 1 || n->children == NULL ||' \
  '      if (n->child_count != 1 || n->children == NULL ||'

plant_finish
