#!/bin/sh
#
# Fail if a struct with a leading `size` member is not accepted at exactly its
# own size, in the one way a grep can tell, and fail if the source reads one by
# its prefix again.
#
# Three things are checked, in a repository root:
#
#  1. Every public header struct that begins with `size_t size;` is in the
#     list file (a C++ header of X("Struct", "home") lines, which the tests
#     generate their table from), and the list names no struct the headers do
#     not declare. A struct added without a line in the list fails here, by
#     name, before any test could have missed it.
#  2. Each home the list names exists and runs its structs: it contains
#     `Run(..., "<home>", ...)`, the call that holds every struct listed for
#     it to a probe.
#  3. Nothing under src/ reads a struct by a prefix of its size: no
#     field_covers, no take_prefix, no offsetof(...) + sizeof(...) coverage,
#     no `size` compared with < or >= against a size, no offsetof in the same
#     statement as a size member.
#
# The script then plants each defect in the text it has just read (a struct
# added to a header, an entry dropped from the list, an entry for a struct that
# is not there, a home that does not run its structs, and each removed
# pattern put into a real source file) and requires every one to fail by name.
#
# Usage: check-size-structs.sh <root> <list-file>
#   <list-file> is relative to <root>.

set -eu

ROOT="${1:?usage: check-size-structs.sh <root> <list-file>}"
LIST="${2:?usage: check-size-structs.sh <root> <list-file>}"

exec python3 -I - "$ROOT" "$LIST" <<'PY'
import os
import re
import sys

root, list_rel = sys.argv[1], sys.argv[2]

# The removed patterns: (name, regex). A statement is the text between ; { }.
PATTERNS = [
    ("field_covers", re.compile(r"\bfield_covers\b")),
    ("take_prefix", re.compile(r"\btake_prefix\b")),
    ("size prefix read through a size_t pointer",
     re.compile(r"\*\s*sizep\b|\*\s*\(\s*(?:const\s+)?size_t\s*\*\s*\)")),
    ("offsetof(...) + sizeof(...) coverage",
     re.compile(r"offsetof\s*\([^;{}]*?\)\s*\+\s*sizeof", re.S)),
    ("size compared with < or >= against a size",
     re.compile(r"(?:->|\.)\s*size\s*(?:<|>=)\s*(?:sizeof\b|offsetof\b|\w*_SIZE\b)")),
    ("offsetof in a statement with a size member",
     re.compile(r"[^;{}]*\boffsetof\s*\([^;{}]*", re.S)),
]


def read_tree(base, exts):
    files = {}
    for d, _dirs, names in os.walk(base):
        for n in sorted(names):
            if n.endswith(exts):
                p = os.path.join(d, n)
                with open(p, encoding="utf-8", errors="replace") as fh:
                    files[os.path.relpath(p, root)] = fh.read()
    return files


def strip_comments(text):
    # One left-to-right pass: a `//` comment can hold `/*` (a path pattern) and
    # a block comment can hold `//`, and whichever starts first wins.
    return re.sub(r"//[^\n]*|/\*.*?\*/",
                  lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)


def header_structs(headers):
    names = {}
    lines = 0
    for path, text in headers.items():
        body = strip_comments(text)
        lines += len(re.findall(r"^\s*size_t\s+size\s*;", body, re.M))
        for m in re.finditer(r"typedef\s+struct\s*\w*\s*\{(.*?)\}\s*(\w+)\s*;", body, re.S):
            if re.search(r"^\s*size_t\s+size\s*;", m.group(1), re.M):
                names[m.group(2)] = path
    return names, lines


def listed(text):
    return {m.group(1): m.group(2)
            for m in re.finditer(r'\bX\(\s*(\w+)\s*,\s*"([^"]+)"\s*\)', text)}


def check(headers, list_text, homes, sources):
    """Return the list of failures (empty: the tree is as it must be)."""
    out = []
    names, lines = header_structs(headers)
    if lines != len(names):
        out.append("%d `size_t size;` members in the headers, %d structs read: "
                   "one is in a form this gate does not read" % (lines, len(names)))
    table = listed(list_text)
    for n in sorted(names):
        if n not in table:
            out.append("%s has a size member (%s) and is not in %s"
                       % (n, names[n], list_rel))
    for n in sorted(table):
        if n not in names:
            out.append("%s is in %s and no public header declares it with a "
                       "size member" % (n, list_rel))
    for n, home in sorted(table.items()):
        text = homes.get(home)
        if text is None:
            out.append("%s: its home %s does not exist" % (n, home))
        elif not re.search(r'\bRun\([^;]*"%s"' % re.escape(home), text):
            out.append("%s: its home %s does not call Run(..., \"%s\", ...), "
                       "so nothing probes it" % (n, home, home))
    for path, text in sorted(sources.items()):
        body = strip_comments(text)
        for pname, rx in PATTERNS:
            if pname == "offsetof in a statement with a size member":
                for m in rx.finditer(body):
                    if re.search(r"\bsize\b", m.group(0)):
                        line = body.count("\n", 0, m.start()) + 1
                        out.append("%s: %s (near line %d)" % (path, pname, line))
                continue
            for m in rx.finditer(body):
                line = body.count("\n", 0, m.start()) + 1
                out.append("%s:%d: %s" % (path, line, pname))
    return out


def load():
    headers = read_tree(os.path.join(root, "include"), (".h",))
    sources = read_tree(os.path.join(root, "src"), (".c", ".h"))
    with open(os.path.join(root, list_rel), encoding="utf-8") as fh:
        list_text = fh.read()
    homes = {}
    for home in set(listed(list_text).values()):
        p = os.path.join(root, home)
        if os.path.isfile(p):
            with open(p, encoding="utf-8", errors="replace") as fh:
                homes[home] = fh.read()
    return headers, list_text, homes, sources


headers, list_text, homes, sources = load()
if not headers or not sources:
    sys.stderr.write("check-size-structs: no headers or no sources under %s; "
                     "this gate is measuring nothing\n" % root)
    sys.exit(1)
failures = check(headers, list_text, homes, sources)
if failures:
    for f in failures:
        sys.stderr.write("check-size-structs: %s\n" % f)
    sys.exit(1)
names, _ = header_structs(headers)
print("check-size-structs: %d structs, each listed and run by its home; "
      "no size-prefix read in src" % len(names))
if not names:
    print("check-size-structs: no struct with a size member here; the source "
          "patterns are still held")

# ---- plants: each defect put in the text just read, seen failing by name ----


def must_fail(label, needle, failures):
    if not failures:
        sys.stderr.write("check-size-structs: the %s plant was accepted\n" % label)
        sys.exit(1)
    if not any(needle in f for f in failures):
        sys.stderr.write("check-size-structs: the %s plant failed, but not by "
                         "name (%s):\n  %s\n" % (label, needle, "\n  ".join(failures)))
        sys.exit(1)
    print("check-size-structs: planted %s was refused (%s)" % (label, needle))


first_header = sorted(headers)[0]
planted = dict(headers)
planted[first_header] = headers[first_header] + (
    "\ntypedef struct GPLANT_Planted {\n  size_t size;\n  int member;\n} "
    "GPLANT_Planted;\n")
must_fail("a struct with a size member and no line in the list", "GPLANT_Planted",
          check(planted, list_text, homes, sources))

if listed(list_text):
    dropped = sorted(listed(list_text))[0]
    must_fail("a listed struct dropped from the list", dropped,
              check(headers, re.sub(r'[^\n]*\bX\(\s*%s\s*,[^\n]*\n?' % dropped, "", list_text, count=1),
                    homes, sources))

    must_fail("a list entry for a struct no header declares", "GPLANT_Stale",
              check(headers, list_text + '\n  X(GPLANT_Stale, "%s")\n'
                    % sorted(set(listed(list_text).values()))[0], homes, sources))

    some_home = sorted(set(listed(list_text).values()))[0]
    quiet = dict(homes)
    quiet[some_home] = re.sub(r"\bRun\(", "Skip(", quiet[some_home])
    must_fail("a home that no longer runs its structs", some_home,
              check(headers, list_text, quiet, sources))

target = next(p for p in sorted(sources) if p.endswith(".c"))
for label, text, needle in (
    ("field_covers", "\nstatic int planted_a(size_t size) { return field_covers(size, 8); }\n",
     "field_covers"),
    ("take_prefix", "\nstatic void planted_b(void * d, const void * s) { take_prefix(d, 8, s); }\n",
     "take_prefix"),
    ("an offsetof(...) + sizeof(...) coverage check",
     "\nstatic int planted_c(size_t size) { return size >= offsetof(struct planted, tail) + sizeof(int); }\n",
     "offsetof(...) + sizeof(...) coverage"),
    ("a size compared with < against a struct size",
     "\nstatic int planted_d(const struct planted * p) { return p->size < sizeof *p; }\n",
     "size compared with < or >="),
):
    bad = dict(sources)
    bad[target] = sources[target] + text
    must_fail("%s in a real source file" % label, needle,
              check(headers, list_text, homes, bad))
PY
