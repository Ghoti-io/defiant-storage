#!/bin/sh
#
# SPDX-License-Identifier: LGPL-3.0-only
#
# Fail if one subsystem of defiant-storage includes another's private header.
#
# src/internal.h is shared by every translation unit of the library by design
# (blob, outbox, projection, revision, snapshot, store and util are one
# subsystem each, over one set of helpers). It is private to the library: the
# public headers, the examples, the tests and the fuzzers must not include it,
# and neither may a header or source of another library.
#
# Two rules, both read from the include lines:
#
#  1. A private header (every header under src/) may be included only by the
#     source files listed for it in OWNERS below. Public headers (include/),
#     the app, the tests and every other source file must not.
#  2. Nothing may reach into another library's private code: no angle-bracket
#     include of an *internal.h, no path through src/, and no "../" escape.
#
# The real tree must pass. A clean temporary tree must pass too (the control:
# a scanner that refuses everything proves nothing), and each planted
# violation in a temporary tree must be refused.
#
# Usage: check-seams.sh <root>

set -eu

ROOT="${1:?usage: check-seams.sh <root>}"

owners_of() {
  case "$1" in
    internal.h) printf '%s\n' 'blob.c outbox.c projection.c query.c revision.c snapshot.c store.c util.c core.c' ;;
    *) printf '\n' ;;
  esac
}

# include_lines <dir...>: "file:line:text" for every include directive.
include_lines() {
  for d in "$@"; do
    [ -d "$d" ] || continue
    grep -R -n -E '^[[:space:]]*#[[:space:]]*include' "$d" \
      --include='*.c' --include='*.h' --include='*.cpp' --include='*.hpp' 2>/dev/null || true
  done
}

# scan <tree>: print each violation, return 1 if there were any.
scan() {
  tree=$1
  status=0
  lines=$(include_lines "$tree/src" "$tree/include" "$tree/app" "$tree/tests" "$tree/examples")
  # Rule 1.
  for hdr in $(find "$tree/src" -name '*.h' 2>/dev/null | sed 's|.*/||' | sort -u); do
    allowed=$(owners_of "$hdr")
    pattern="[\"</]${hdr}[\">]"
    hits=$(printf '%s\n' "$lines" | grep -E "$pattern" || true)
    [ -n "$hits" ] || continue
    printf '%s\n' "$hits" | while IFS= read -r hit; do
      [ -n "$hit" ] || continue
      file=${hit%%:*}
      base=${file##*/}
      case "$file" in
        "$tree"/src/*)
          ok=0
          for a in $allowed; do [ "$a" = "$base" ] && ok=1; done
          # A private header may include its own siblings.
          case "$base" in *.h) ok=1 ;; esac
          [ "$ok" -eq 1 ] && continue
          ;;
      esac
      printf 'check-seams: %s includes private %s\n' "$hit" "$hdr" >&2
      echo x >> "$tree/.seam-violation"
    done
  done
  # Rule 2.
  cross=$(printf '%s\n' "$lines" | grep -E '#[[:space:]]*include[[:space:]]*<[^>]*internal\.h>|#[[:space:]]*include[[:space:]]*["<][^">]*/src/|#[[:space:]]*include[[:space:]]*"\.\./' || true)
  if [ -n "$cross" ]; then
    printf '%s\n' "$cross" | sed 's/^/check-seams: reaches into private code: /' >&2
    echo x >> "$tree/.seam-violation"
  fi
  if [ -e "$tree/.seam-violation" ]; then
    status=1
  fi
  return "$status"
}

# Guard: the pieces this check reads must exist, or it measures nothing.
if [ ! -f "$ROOT/src/internal.h" ] || [ ! -f "$ROOT/src/store.c" ] ||
   [ ! -d "$ROOT/include/ghoti.io/defiant-storage" ]; then
  printf 'check-seams: private headers are missing; this gate is measuring nothing\n' >&2
  exit 1
fi

run_scan() {
  rm -f "$1/.seam-violation"
  if scan "$1"; then return 0; fi
  rm -f "$1/.seam-violation"
  return 1
}

if ! run_scan "$ROOT"; then
  exit 1
fi
printf 'check-seams: defiant-storage includes only the headers it is allowed to\n'

plant=$(mktemp -d)
trap 'rm -rf "$plant"' EXIT
mkdir -p "$plant/src" "$plant/include/ghoti.io/defiant-storage" "$plant/tests" "$plant/examples"
printf '%s\n' '/* private */' > "$plant/src/internal.h"
printf '%s\n' '#include "internal.h"' > "$plant/src/store.c"
printf '%s\n' '#include "internal.h"' > "$plant/src/blob.c"
printf '%s\n' '#include <ghoti.io/defiant-storage/core.h>' > "$plant/tests/test_x.cpp"
printf '%s\n' '#include <ghoti.io/defiant-storage/store.h>' > "$plant/examples/demo.c"

if ! run_scan "$plant"; then
  printf 'check-seams: the clean control tree was refused; the scanner proves nothing\n' >&2
  exit 1
fi
printf 'check-seams: control: a clean tree passes\n'

refused() {
  what=$1
  if run_scan "$plant" 2>/dev/null; then
    printf 'check-seams: the planted %s passed\n' "$what" >&2
    exit 1
  fi
  printf 'check-seams: planted %s was refused\n' "$what"
}

mkdir -p "$plant/include/ghoti.io/defiant-storage"
printf '%s\n' '#include "../../../src/internal.h"' > "$plant/include/ghoti.io/defiant-storage/store.h"
refused "public-header include of internal.h"
rm -f "$plant/include/ghoti.io/defiant-storage/store.h"

mkdir -p "$plant/tests"
printf '%s\n' '#include "../src/internal.h"' > "$plant/tests/test_y.cpp"
refused "test include of internal.h"
rm -f "$plant/tests/test_y.cpp"

mkdir -p "$plant/examples"
printf '%s\n' '#include "internal.h"' > "$plant/examples/other.c"
refused "example include of internal.h"
rm -f "$plant/examples/other.c"

mkdir -p "$plant/src"
printf '%s\n' '#include <ghoti.io/defiant-core/subject_internal.h>' > "$plant/src/store.c"
refused "include of another library's internal header"
rm -f "$plant/src/store.c"

mkdir -p "$plant/src"
printf '%s\n' '#include "../../defiant-core/src/subject/subject_internal.h"' > "$plant/src/store.c"
refused "include through another library's src/"
rm -f "$plant/src/store.c"

