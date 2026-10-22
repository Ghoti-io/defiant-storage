#!/bin/sh
#
# Shared by the check-*.sh gates that plant a defect in the library source.
# Sourced, not run. This is the one copy: defiant-storage installs it with its
# conformance suites, and the gates of the other Defiant libraries source it
# from there (pkg-config --variable=conformance_dir), so there is nothing to
# keep in step.
#
# A gate that only shows its test can fail (an `if` inside the test body)
# says nothing about the library. These plants edit a scratch COPY of the
# source by an exact text edit, rebuild, and require that a named test then
# fails. The rules the helpers enforce:
#
#  - the unpatched copy passes the named tests first (the control), and the
#    run really ran tests, so a filter that matches nothing is not a pass;
#  - every anchor must match exactly once, and the edited file must differ
#    from the original. An edit that did not apply leaves a copy that passes
#    for the wrong reason, so it stops the script with a loud error;
#  - the red run must be a test failure BY NAME. A build error, a crash, a
#    timeout or some other test failing does not count;
#  - the copy starts from the finished build (below), so the rebuild after an
#    edit must be shown to have happened: every object that lists the edited
#    file as a dependency has to appear in the build's log as compiled. An
#    object left standing from before the edit would carry no defect and pass
#    for the wrong reason, so it is a failure of its own (STALE OBJECT);
#  - nothing here is in the library: no macro or environment variable in
#    src/ selects a defect.
#
# Speed. A plant's copy takes the main tree's build output, with its
# timestamps (plant_warm), so make recompiles what the edit reaches through the
# .d files and relinks, not the library. That needs the copy's compile flags to
# equal the main tree's, which they do not if they name the copy's own
# directory: the Makefile's gate recipes therefore pass the paths it bakes into
# objects (TEST_DATA, TEST_CORPUS) as make arguments, so the copy compiles
# against the main tree's, and its flags stamp is the same. Plants run at
# once, up to GHOTI_CHECK_JOBS of them (default: the processor count, at most
# four), each in its own copy and each with `make -j1`; the control runs with
# `make -jN`. The lines a gate prints are printed in the order the plants were
# written, whatever order they finish in. GHOTI_CHECK_COLD=1 copies no build
# output, for comparing the two; GHOTI_CHECK_JOBS=1 runs one plant at a time.
#
# Usage, from the library root:
#
#   . tools/plant-lib.sh
#   plant_setup "$MAKE" "$BUILD_DIR" PREFIX=... TEST_DATA=...
#   plant_control testOracle 'Oracle.*'
#   plant NAME src/access/access.c testOracle 'Oracle.Fields' \
#       'old text' 'new text' ['old2' 'new2' ...]
#   plant_stale NAME FILE EXE EXPECT OLD NEW      # the stale-object plant
#   plant_finish
#
# plant_wait waits for every plant started so far and prints what they have to
# say; use it before reading $PLANT_FAIL.
#
# A gate that already had its own wording keeps it, with these (all optional,
# set after plant_setup or before the first plant):
#
#   PLANT_TAG        what each line starts with (default: plant)
#   PLANT_RUN_FILTER a plant runs the tests this filter matches (for example
#                    every test of its program), not only the ones named;
#                    the named tests still have to be among the failures
#   PLANT_NEED_TEXT  set before one plant: the red run's log must also carry
#                    this text (cleared when the plant has started)
#   PLANT_TIMEOUT    seconds a test program may run in a plant. The program
#                    is then built first and run directly under `timeout`
#   PLANT_TREE_DIRS  the directories the copy takes (default: src include
#                    tests tools pkgconfig documentation examples)
#
# and these functions, which a gate may redefine after sourcing this file:
#
#   plant_hook_control EXE FILTER    the line the passing control prints
#   plant_hook_caught NAME EXPECT LOG    what a caught plant prints (stdout)
#   plant_hook_done                  the last line of a gate that passed
#
# plant_spawn runs any function of the gate's in a slot of its own, with the
# same ordered output, for a gate whose plant is not a single copy of the tree
# (plant_acquire and plant_launch are its two halves).

set -u

unset MAKEFLAGS MFLAGS

plant_setup() {
  PLANT_MAKE="${1:?plant_setup <make> <build-dir> [make args]}"
  PLANT_BUILD="${2:?plant_setup <make> <build-dir> [make args]}"
  shift 2
  PLANT_TAG="${PLANT_TAG:-plant}"
  PLANT_NEED_TEXT=
  PLANT_RUN_FILTER="${PLANT_RUN_FILTER:-}"
  PLANT_TIMEOUT="${PLANT_TIMEOUT:-}"
  PLANT_TREE_DIRS="${PLANT_TREE_DIRS:-src include tests tools pkgconfig documentation examples}"
  PLANT_JOBS="${GHOTI_CHECK_JOBS:-$(n=$(nproc 2>/dev/null || echo 4); [ "$n" -gt 4 ] && n=4; echo "$n")}"
  # The control is one run of make and takes every job; a plant is one of
  # several, and a warm one recompiles little, so it takes one.
  PLANT_ARGS="-j$PLANT_JOBS $*"
  PLANT_PLANT_ARGS="-j1 $*"
  PLANT_ROOT=$(pwd)
  PLANT_SCRATCH="$PLANT_ROOT/build/plants-$$"
  PLANT_FAIL=0
  PLANT_CONTROL_DONE=0
  PLANT_SEQ=0
  PLANT_NEXT=0
  case "$(uname -s)" in MINGW* | MSYS* | CYGWIN*) PLANT_EXE_EXT=.exe ;; *) PLANT_EXE_EXT= ;; esac
  rm -rf "$PLANT_SCRATCH"
  mkdir -p "$PLANT_SCRATCH"
  trap 'rm -rf "$PLANT_SCRATCH"' EXIT
  # One token per plant that may run at once, in a pipe. A shell without
  # named pipes runs one at a time.
  PLANT_PAR=$PLANT_JOBS
  if [ "$PLANT_PAR" -gt 1 ]; then
    if mkfifo "$PLANT_SCRATCH/tokens" 2> /dev/null &&
        ( exec 3<> "$PLANT_SCRATCH/tokens" ) 2> /dev/null; then
      exec 3<> "$PLANT_SCRATCH/tokens"
      t=0
      while [ "$t" -lt "$PLANT_PAR" ]; do
        printf 'x\n' >&3
        t=$((t + 1))
      done
    else
      PLANT_PAR=1
    fi
  fi
}

# plant_cp <args>: cp -a, sharing the data blocks where the file system can.
plant_cp() {
  cp -a --reflink=auto "$@" 2> /dev/null || cp -a "$@"
}

plant_copy_tree() {
  dest=$1
  mkdir -p "$dest"
  for d in $PLANT_TREE_DIRS; do
    [ -e "$PLANT_ROOT/$d" ] && plant_cp "$PLANT_ROOT/$d" "$dest/"
  done
  plant_cp "$PLANT_ROOT/Makefile" "$PLANT_ROOT/Doxyfile" "$dest/"
  rm -rf "$dest/tests/fuzz/corpus"
  plant_warm "$PLANT_ROOT" "$dest" "$PLANT_BUILD"
}

# The copy also takes the main tree's build output for this build directory,
# with its timestamps, so make recompiles only what the plant edited (and,
# through the .d files, whatever includes an edited header) and relinks.
# Without it every plant was a cold rebuild of the whole library.
# GHOTI_CHECK_COLD=1 copies no build output, for comparing the two.
plant_warm() {
  [ -n "${GHOTI_CHECK_COLD:-}" ] && return 0
  case "$3" in /*|*..*|'') return 0 ;; esac
  [ -d "$1/$3" ] || return 0
  mkdir -p "$2/$(dirname "$3")"
  plant_cp "$1/$3" "$2/$3"
}

# ---- what a gate prints, in order ------------------------------------------
#
# Everything a plant has to say goes to a numbered slot, and slots are printed
# in number order as the earlier ones finish, so the output does not depend on
# which plant was quicker.

plant_slot_new() {
  PLANT_SEQ=$((PLANT_SEQ + 1))
  PLANT_SLOT="$PLANT_SCRATCH/slot.$PLANT_SEQ"
}

# plant_say <text>: a line of the gate's own, in its place in the order.
plant_say() {
  plant_slot_new
  printf '%s\n' "$1" > "$PLANT_SLOT.out"
  : > "$PLANT_SLOT.err"
  echo 0 > "$PLANT_SLOT.rc"
  : > "$PLANT_SLOT.done"
}

# plant_drain <all>: print the finished slots from the front. With <all> = 1,
# wait for the unfinished ones.
plant_drain() {
  while [ "$PLANT_NEXT" -lt "$PLANT_SEQ" ]; do
    n=$((PLANT_NEXT + 1))
    s="$PLANT_SCRATCH/slot.$n"
    if [ ! -e "$s.done" ]; then
      [ "${1:-0}" = 1 ] || return 0
      eval "pid=\${PLANT_PID_$n:-}"
      if [ -n "$pid" ]; then
        wait "$pid"
      fi
      if [ ! -e "$s.done" ]; then
        printf '%s: a plant ended without a result (slot %s), which is a failure\n' "$PLANT_TAG" "$n" >&2
        PLANT_FAIL=1
        PLANT_NEXT=$n
        continue
      fi
    fi
    cat "$s.out"
    cat "$s.err" >&2
    rc=1
    read -r rc < "$s.rc"
    [ "$rc" = 0 ] || PLANT_FAIL=1
    rm -f "$s.out" "$s.err"
    PLANT_NEXT=$n
  done
}

plant_wait() {
  plant_drain 1
}

# plant_acquire: wait for a place to run in and take the next slot (PLANT_SLOT).
plant_acquire() {
  if [ "$PLANT_PAR" -gt 1 ]; then
    read -r PLANT_TOKEN <&3
  fi
  plant_slot_new
}

# plant_launch <fn> [args]: run the function in the slot plant_acquire took,
# its standard output and error going to the slot, its status being the slot's
# result. In the background where there is room for more than one.
plant_launch() {
  slot=$PLANT_SLOT
  if [ "$PLANT_PAR" -gt 1 ]; then
    (
      "$@" > "$slot.out" 2> "$slot.err"
      echo $? > "$slot.rc"
      printf 'x\n' >&3
      : > "$slot.done"
    ) &
    eval "PLANT_PID_$PLANT_SEQ=$!"
  else
    (
      "$@" > "$slot.out" 2> "$slot.err"
      echo $? > "$slot.rc"
      : > "$slot.done"
    )
  fi
  plant_drain 0
}

# plant_spawn <fn> [args]: plant_acquire, then plant_launch.
plant_spawn() {
  plant_acquire
  plant_launch "$@"
}

# ---- what the helpers say, unless the gate says it its own way -----------

plant_hook_control() { # exe filter
  plant_say "$PLANT_TAG: control passes: $1 $2"
}

plant_hook_caught() { # name expect log
  for t in $2; do
    printf '%s: %s: caught by %s\n' "$PLANT_TAG" "$1" "$t"
    grep -E "\[  FAILED  \] $t( |\$)" "$3" | head -1 | sed "s/^/$PLANT_TAG:   /"
  done
}

plant_hook_done() {
  printf '%s: every planted defect turned its named test red\n' "$PLANT_TAG"
}

# ---- running one plant --------------------------------------------------------

# plant_run <dir> <exe> <filter> <log> [make args]: build the test program(s)
# in <dir> and run the tests matching <filter>. <exe> may name several
# programs. Exit status is the test run's.
plant_run() {
  dir=$1 exe=$2 filter=$3 log=$4
  shift 4
  paths=
  for e in $exe; do
    paths="$paths $PLANT_BUILD/apps/$e$PLANT_EXE_EXT"
  done
  if [ -n "$PLANT_TIMEOUT" ]; then
    # Built first, then run on its own under a time limit: a plant that
    # makes a test wait for ever is a failure of that plant, not a hung gate.
    # shellcheck disable=SC2086
    ( cd "$dir" && "$PLANT_MAKE" --no-print-directory $paths "$@" ) > "$log" 2>&1 || return 1
    status=0
    for e in $exe; do
      ( cd "$dir" && LD_LIBRARY_PATH="$dir/$PLANT_BUILD/apps${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
          GTEST_FILTER="$filter" timeout "$PLANT_TIMEOUT" \
          "$PLANT_BUILD/apps/$e$PLANT_EXE_EXT" --gtest_brief=1 ) >> "$log" 2>&1 || status=1
    done
    return $status
  fi
  # shellcheck disable=SC2086
  ( cd "$dir" && GTEST_FILTER="$filter" "$PLANT_MAKE" --no-print-directory test \
      TEST_GATES= TEST_EXECUTABLES="$paths" \
      "$@" ) > "$log" 2>&1
}

# plant_control <exe> <filter>: the unpatched copy must pass and run tests.
plant_control() {
  exe=$1 filter=$2
  if [ "$PLANT_CONTROL_DONE" -eq 0 ]; then
    plant_copy_tree "$PLANT_SCRATCH/control"
    PLANT_CONTROL_DONE=1
  fi
  log="$PLANT_SCRATCH/control-$(printf '%s' "$exe" | tr ' ' '-').log"
  # shellcheck disable=SC2086
  if ! plant_run "$PLANT_SCRATCH/control" "$exe" "$filter" "$log" $PLANT_ARGS; then
    plant_wait
    printf '%s: the CONTROL (unpatched) copy fails %s %s, so no plant below would mean anything. Its log:\n' \
      "$PLANT_TAG" "$exe" "$filter" >&2
    tail -40 "$log" >&2
    exit 1
  fi
  if ! grep -Eq '\[  PASSED  \] [1-9][0-9]* test' "$log"; then
    plant_wait
    printf '%s: the CONTROL run of %s %s passed but ran no tests (a filter that matches nothing)\n' \
      "$PLANT_TAG" "$exe" "$filter" >&2
    exit 1
  fi
  plant_hook_control "$exe" "$filter"
  plant_drain 0
}

# plant_depends <dir> <file>...: the objects in <dir>'s build directory whose
# .d file lists one of the files as a dependency, one per line, each as the
# path make builds it by.
plant_depends() {
  dir=$1
  shift
  find "$dir/$PLANT_BUILD" -name '*.d' 2> /dev/null | while read -r d; do
    for f in "$@"; do
      awk -v f="$f" '
        { for (i = 1; i <= NF; i++) {
            if (target == "" && $i ~ /:$/) { target = substr($i, 1, length($i) - 1); continue }
            if ($i == f) hit = 1
          } }
        END { if (hit && target != "") print target }' "$d"
    done
  done | sort -u
}

# plant_stale_objects <dir> <log> <file>...: the objects that depend on an
# edited file and are not among those the build compiled (the compile line
# names its output, `-o <object>`), one per line; or UNLISTED <file> for an edited
# file that no object depends on, which no rebuild could show. A library
# object is in every test program (the archive is linked whole), so it must
# have been compiled; a test object is in the one program that was asked for,
# so it must have been compiled if the link named it, and the other test
# objects were not asked for.
plant_stale_objects() {
  dir=$1 log=$2
  shift 2
  for f in "$@"; do
    objs=$(plant_depends "$dir" "$f")
    if [ -z "$objs" ]; then
      printf 'UNLISTED %s\n' "$f"
      continue
    fi
    for o in $objs; do
      grep -qF -e " -o $o" "$log" && continue
      case "$o" in
        */tests/*) grep -qF -e "$o" "$log" || continue ;;
      esac
      printf '%s\n' "$o"
    done
  done
}

# plant_edit <kind> <dir> <edits...>: apply the exact text edits to the files
# of <dir>; 0 if all applied.
# kind p: <file> <old> <new> [<old> <new> ...]    (plant)
# kind m: <file> <old> <new> [<file> <old> <new> ...]    (plantm)
plant_edit() {
  python3 - "$@" <<'PY'
import sys
kind = sys.argv[1]
root = sys.argv[2]
args = sys.argv[3:]
who = "plant" if kind == "p" else "plantm"
triples = []
if kind == "p":
    f, pairs = args[0], args[1:]
    if len(pairs) == 0 or len(pairs) % 2 != 0:
        sys.stderr.write("plant: edits must come in old/new pairs\n")
        sys.exit(2)
    for i in range(0, len(pairs), 2):
        triples.append((f, pairs[i], pairs[i + 1]))
else:
    if len(args) == 0 or len(args) % 3 != 0:
        sys.stderr.write("plantm: edits must come in file/old/new triples\n")
        sys.exit(2)
    for i in range(0, len(args), 3):
        triples.append((args[i], args[i + 1], args[i + 2]))
texts = {}
for f, old, new in triples:
    path = root + "/" + f
    if f not in texts:
        texts[f] = open(path).read()
    n = texts[f].count(old)
    if n != 1:
        sys.stderr.write("%s: anchor matched %d times in %s: %r\n" % (who, n, path, old))
        sys.exit(3)
    if old == new:
        sys.stderr.write("%s: an edit that changes nothing: %r\n" % (who, old))
        sys.exit(3)
    texts[f] = texts[f].replace(old, new, 1)
for f in texts:
    open(root + "/" + f, "w").write(texts[f])
PY
}

# plant_job <kind> <stale> <name> <exe> <expect> <file/edits...>
# kind p: <file> <old> <new> [<old> <new> ...]    (plant)
# kind m: <file> <old> <new> [<file> <old> <new> ...]    (plantm)
# <stale> = 1: after the edit, make every object that depends on it look up to
# date, so the rebuild does not happen; the plant is then expected to be
# refused as STALE OBJECT, and is a failure if it is not.
# Runs in a subshell of its own, in a copy of the control tree already at
# $PLANT_SCRATCH/<name>; leaves 0 for a plant that behaved, 1 otherwise.
plant_job() {
  kind=$1 stale=$2 name=$3 exe=$4 expect=$5
  shift 5
  dir="$PLANT_SCRATCH/$name"
  log="$dir.log"
  plant_edit "$kind" "$dir" "$@"
  rc=$?
  if [ $rc -ne 0 ]; then
    printf '%s: %s: THE EDIT DID NOT APPLY, nothing was tested\n' "$PLANT_TAG" "$name" >&2
    return 1
  fi
  if [ "$kind" = p ]; then
    files=$1
  else
    files=$(python3 - "$@" <<'PY'
import sys
a = sys.argv[1:]
print(" ".join(sorted(set(a[i] for i in range(0, len(a), 3)))))
PY
)
  fi
  for file in $files; do
    if cmp -s "$PLANT_ROOT/$file" "$dir/$file"; then
      if [ "$kind" = p ]; then
        printf '%s: %s: the edited file equals the original, nothing was tested\n' "$PLANT_TAG" "$name" >&2
      else
        printf '%s: %s: %s equals the original, nothing was tested\n' "$PLANT_TAG" "$name" "$file" >&2
      fi
      return 1
    fi
    # The copy was cp -a'd, so its objects are as new as the source: touch the
    # files that changed, and drop the test program so it relinks.
    touch "$dir/$file"
  done
  if [ "$stale" = 1 ]; then
    # The rebuild that should follow does not: every object that depends on
    # an edited file now looks newer than the edit.
    # shellcheck disable=SC2086
    for o in $(plant_depends "$dir" $files); do
      ( cd "$dir" && touch "$o" )
    done
  fi
  rm -f "$dir/$PLANT_BUILD"/apps/test*
  if [ -n "$PLANT_RUN_FILTER" ]; then
    filter=$PLANT_RUN_FILTER
  else
    filter=$(printf '%s' "$expect" | tr ' ' ':')
  fi
  # shellcheck disable=SC2086
  if plant_run "$dir" "$exe" "$filter" "$log" $PLANT_PLANT_ARGS; then
    ran=0
  else
    ran=1
  fi
  if [ $ran -ne 0 ] && ! grep -Eq '\[==========\] .* ran\.' "$log"; then
    printf '%s: %s: the run failed without finishing (a build error or a crash), which is not a test catching a defect - read %s\n' \
      "$PLANT_TAG" "$name" "$log" >&2
    tail -30 "$log" >&2
    return 1
  fi
  if [ $ran -eq 0 ] && ! grep -Eq '\[==========\] .* ran\.' "$log"; then
    printf '%s: %s: the run finished without running tests - read %s\n' "$PLANT_TAG" "$name" "$log" >&2
    return 1
  fi
  # shellcheck disable=SC2086
  stale_objs=$(plant_stale_objects "$dir" "$log" $files)
  if [ "$stale" = 1 ]; then
    if [ -n "$stale_objs" ]; then
      printf '%s: %s: caught by the stale-object check: %s edited, and not rebuilt:\n' \
        "$PLANT_TAG" "$name" "$(printf '%s' "$files" | tr '\n' ' ' | sed 's/ *$//')"
      printf '%s\n' "$stale_objs" | sed "s/^/$PLANT_TAG:   /"
      return 0
    fi
    printf '%s: %s: every object that depends on the edit was rebuilt anyway, so the stale-object check was not shown to work - read %s\n' \
      "$PLANT_TAG" "$name" "$log" >&2
    return 1
  fi
  if [ -n "$stale_objs" ]; then
    printf '%s: %s: STALE OBJECT: the edit was not rebuilt into the test program, so the defect never reached the tests (read %s):\n' \
      "$PLANT_TAG" "$name" "$log" >&2
    printf '%s\n' "$stale_objs" | sed "s/^/$PLANT_TAG:   /" >&2
    return 1
  fi
  if [ $ran -eq 0 ]; then
    printf '%s: %s: %s PASSED with the defect planted\n' "$PLANT_TAG" "$name" "$expect" >&2
    return 1
  fi
  for t in $expect; do
    if ! grep -Eq "\[  FAILED  \] $t( |\$)" "$log"; then
      printf '%s: %s: the run failed, but %s was not among the failures - read %s\n' \
        "$PLANT_TAG" "$name" "$t" "$log" >&2
      grep -E '\[  FAILED  \]' "$log" >&2
      return 1
    fi
  done
  if [ -n "${PLANT_NEED_TEXT:-}" ] && ! grep -q -- "$PLANT_NEED_TEXT" "$log"; then
    printf '%s: %s: red, but the log does not show "%s" - read %s\n' \
      "$PLANT_TAG" "$name" "$PLANT_NEED_TEXT" "$log" >&2
    grep -E '\[  FAILED  \]' "$log" >&2
    return 1
  fi
  plant_hook_caught "$name" "$expect" "$log"
  return 0
}

# plant_job_in_copy <job args>: plant_job, then the copy is removed.
plant_job_in_copy() {
  plant_job "$@"
  rc=$?
  rm -rf "$PLANT_SCRATCH/$3"
  return $rc
}

# plant_start <kind> <stale> <name> <exe> <expect> ...: start the plant in
# its own copy of the control tree, at once if a place is free.
plant_start() {
  kind=$1 stale=$2 name=$3
  if [ "$PLANT_CONTROL_DONE" -eq 0 ]; then
    printf '%s: %s: no plant_control before the first plant\n' "$PLANT_TAG" "$name" >&2
    PLANT_FAIL=1
    PLANT_NEED_TEXT=
    return
  fi
  plant_acquire
  dir="$PLANT_SCRATCH/$name"
  rm -rf "$dir"
  # Copied here, before the plant is started in the background and while no
  # control is running: the control tree is never copied as it changes.
  plant_cp "$PLANT_SCRATCH/control" "$dir"
  plant_launch plant_job_in_copy "$@"
  PLANT_NEED_TEXT=
}

# plant <name> <file> <exe> <expect> <old> <new> [<old> <new> ...]
# <expect> is one or more gtest names, separated by spaces.
plant() {
  name=$1 file=$2 exe=$3 expect=$4
  shift 4
  plant_start p 0 "$name" "$exe" "$expect" "$file" "$@"
}

# plantm <name> <exe> <expect> <file> <old> <new> [<file> <old> <new> ...]
# A plant whose defect needs edits in more than one file (a variable one file
# sets and another reads). Every anchor must match exactly once in its file;
# every file named must differ from the original afterwards.
plantm() {
  name=$1 exe=$2 expect=$3
  shift 3
  plant_start m 0 "$name" "$exe" "$expect" "$@"
}

# plant_stale <name> <file> <exe> <expect> <old> <new> [<old> <new> ...]
# The stale-object plant: a real plant (give it one the gate already has)
# whose rebuild is made not to happen, which has to be refused by name. If a
# warm copy can ever pass with the edit not compiled in, this is what goes red.
plant_stale() {
  name=$1 file=$2 exe=$3 expect=$4
  shift 4
  if [ -n "${GHOTI_CHECK_COLD:-}" ]; then
    # Nothing is copied, so nothing can be left standing.
    plant_say "$PLANT_TAG: $name: SKIPPED: GHOTI_CHECK_COLD copies no build output, so no object can be stale"
    return
  fi
  plant_start p 1 "$name" "$exe" "$expect" "$file" "$@"
}

plant_finish() {
  plant_wait
  if [ "$PLANT_FAIL" -ne 0 ]; then
    exit 1
  fi
  plant_hook_done
}
