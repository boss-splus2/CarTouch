#!/usr/bin/env python3
"""CI guard: no ct* function may exist only for the tests.

A function named ct<Upper>... that is defined under src/ but never called from
firmware code gives false confidence: its unit tests stay green while the real
code path uses something else (example found in the past:
ctOtaFirmwareHeaderOk was tested, the OTA path used ctOtaHeaderFeed).

A symbol counts as dead when every use outside its own definition is
  - in a test file, or
  - a bare declaration, or
  - inside the body of another dead symbol (checked repeatedly until stable).
Comments, string literals and character literals are ignored.

To keep a symbol on purpose (for example a documented public API that has no
caller yet), add its name to ALLOWED with a reason.
Exit code 0 = ok, 1 = dead symbol(s) found, 2 = nothing to check.
Usage: check_dead_ct_symbols.py [repo-root]
"""
import re
import sys
from pathlib import Path

# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
# □□□□□□□□□□ Configuration
# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

ALLOWED = {
    # "ctExample": "reason",
}

SOURCE_SUFFIXES = {".h", ".cpp"}
NAME = r"ct[A-Z]\w*"
NOT_A_TYPE = {"return", "else", "case", "new", "delete", "goto", "throw", "sizeof"}

LEXEMES = re.compile(
    r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'", re.S)

# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
# □□□□□□□□□□ Parsing helpers
# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

def blank(match):
    """Replace a comment/literal by spaces, keeping newlines (line numbers)."""
    return re.sub(r"[^\n]", " ", match.group(0))

def read_clean(path):
    return LEXEMES.sub(blank, path.read_text(encoding="utf-8", errors="replace"))

def files_under(root, folder):
    base = root / folder
    if not base.is_dir():
        return []
    return sorted(p for p in base.rglob("*") if p.suffix in SOURCE_SUFFIXES)

def matching_brace(text, open_pos):
    depth = 0
    for i in range(open_pos, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    return len(text) - 1

def prev_token(text, pos):
    before = text[:pos].rstrip()
    m = re.search(r"([\w\*&>])$", before)
    if not m:
        return None
    word = re.search(r"(\w+)$", before)
    return word.group(1) if word else m.group(1)

PARAMS = r"\((?:[^;(){}]|\((?:[^()]|\([^()]*\))*\))*\)"
DEFINITION = re.compile(r"\b(" + NAME + r")\s*" + PARAMS + r"\s*(?:const\s*)?(?:noexcept\s*)?\{")
DECLARATION = re.compile(r"\b(" + NAME + r")\s*" + PARAMS + r"\s*(?:const\s*)?;")

def find_definitions(files):
    """name -> (path, name_pos, body_start, body_end, line)"""
    defs = {}
    for path, text in files.items():
        for m in DEFINITION.finditer(text):
            prev = prev_token(text, m.start(1))
            if prev is None or prev in NOT_A_TYPE:
                continue
            open_pos = m.end() - 1
            end = matching_brace(text, open_pos)
            line = text.count("\n", 0, m.start(1)) + 1
            defs.setdefault(m.group(1), (path, m.start(1), open_pos, end, line))
    return defs

# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
# □□□□□□□□□□ Dead symbol search
# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

def find_dead(src, tests):
    defs = find_definitions(src)
    # A declaration has a return type in front of the name ("bool ctFoo(int);");
    # a call such as "x = ctFoo(1);" or "return ctFoo(1);" does not.
    declarations = set()
    for p, t in src.items():
        for m in DECLARATION.finditer(t):
            prev = prev_token(t, m.start(1))
            if prev is not None and prev not in NOT_A_TYPE:
                declarations.add((p, m.start(1)))
    uses = {name: [] for name in defs}
    for path, text in src.items():
        for m in re.finditer(r"\b" + NAME + r"\b", text):
            name = m.group(0)
            if name not in defs:
                continue
            if (path, m.start()) in declarations:
                continue
            if defs[name][0] == path and defs[name][1] == m.start():
                continue  # the definition itself
            uses[name].append((path, m.start()))

    def span(name):
        return defs[name][0], defs[name][2], defs[name][3]

    dead = set()
    changed = True
    while changed:
        changed = False
        spans = [span(n) for n in dead]
        for name, where in uses.items():
            if name in dead or name in ALLOWED:
                continue
            live = [u for u in where
                    if not any(u[0] == s[0] and s[1] <= u[1] <= s[2] for s in spans)]
            if not live:
                dead.add(name)
                changed = True

    test_blob = "\n".join(tests.values())
    result = []
    for name in sorted(dead):
        path, _, _, _, line = defs[name]
        used_by_tests = len(re.findall(r"\b" + name + r"\b", test_blob))
        result.append((name, path, line, used_by_tests))
    return result

# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
# □□□□□□□□□□ Entry point
# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

def main(argv):
    root = Path(argv[1]) if len(argv) > 1 else Path(".")
    src = {p: read_clean(p) for p in files_under(root, "src")}
    tests = {p: read_clean(p) for p in files_under(root, "test")}
    if not src:
        print("No source files found under src/", file=sys.stderr)
        return 2
    dead = find_dead(src, tests)
    total = len(find_definitions(src))
    if not dead:
        print(f"Dead ct* symbol check passed ({total} functions, all reachable from firmware code)")
        return 0
    print("ct* functions that firmware code never calls (only tests or nothing use them):")
    for name, path, line, in_tests in dead:
        kind = f"used by {in_tests} test reference(s)" if in_tests else "not used anywhere"
        print(f"  {path.relative_to(root)}:{line}: {name}  ({kind})")
    print("Remove the function (and move its test to the real code path) or call it from firmware.")
    return 1

if __name__ == "__main__":
    sys.exit(main(sys.argv))
