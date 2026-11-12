#!/usr/bin/env python3
"""Flag mutable static/global state and singletons in CrowdSim project code.

CrowdSim forbids singletons and mutable global or static state (including
function-local statics). This script is a fast, dependency-free first line of
defence run in CI; clang-tidy's cppcoreguidelines-avoid-non-const-global-variables
covers namespace-scope globals that a regex cannot reliably see.

Flags:
  * `static` variable declarations that are not const/constexpr
  * any `thread_local`
  * singleton accessors: instance() / getInstance() / Instance()

Suppress a deliberate, reviewed exception by putting
`// global-state-ok: <reason>` on the same line.

Usage: check_global_state.py [root]   (exit code 1 if violations are found)
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

MODULE_DIRS = ["core", "geometry", "snapshot", "sim", "physics", "assets", "script", "commands", "mcp",
               "render", "ui", "platform", "app", "bench"]
EXTENSIONS = {".h", ".hpp", ".cpp", ".cc", ".inl", ".mm"}
SUPPRESS = "global-state-ok:"

STATIC_RE = re.compile(r"\bstatic\b")
THREAD_LOCAL_RE = re.compile(r"\bthread_local\b")
SINGLETON_RE = re.compile(r"\b(?:get_?)?[Ii]nstance\s*\(\s*\)")
IMMUTABLE_RE = re.compile(r"^\s*(?:inline\s+)?(?:constexpr|const|constinit\s+const)\b")


def strip_comments_and_strings(text: str) -> str:
    """Blank out comments and string/char literals, preserving newlines and offsets."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = text.find("\n", i)
            j = n if j == -1 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and nxt == "*":
            j = text.find("*/", i + 2)
            j = n if j == -1 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif c == 'R' and nxt == '"':
            m = re.match(r'R"([^(\s]*)\(', text[i:])
            if m:
                end = text.find(")" + m.group(1) + '"', i)
                end = n if end == -1 else end + len(m.group(1)) + 2
                out.append("".join(ch if ch == "\n" else " " for ch in text[i:end]))
                i = end
            else:
                out.append(c)
                i += 1
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(" " * (j - i))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def strip_angle_brackets(decl: str) -> str:
    """Remove template argument lists so `std::function<void()>` doesn't look like a call."""
    result, depth = [], 0
    for ch in decl:
        if ch == "<":
            depth += 1
        elif ch == ">" and depth > 0:
            depth -= 1
        elif depth == 0:
            result.append(ch)
    return "".join(result)


def is_mutable_static_variable(after_static: str) -> bool:
    if IMMUTABLE_RE.match(after_static):
        return False
    decl = strip_angle_brackets(after_static)
    # The declaration ends at the first of these; a '(' first means a function.
    m = re.search(r"[;{=(]", decl)
    if m is None:
        return False
    if m.group(0) == "(":
        # `static Foo foo(args);` is technically a variable, but functions are far
        # more common; clang-tidy catches the rare direct-init case.
        return False
    head = decl[: m.start()]
    if re.search(r"\b(?:const|constexpr)\b", head):
        # e.g. `static std::string const kName = ...` or `static Foo* const p`.
        # A pointer-to-const (`const Foo* p`) is still mutable, but that is rare enough
        # for review to catch.
        return False
    if re.search(r"\b(?:struct|class|enum|union)\b", head):
        return False
    return True


def check_file(path: Path) -> list[str]:
    raw = path.read_text(encoding="utf-8", errors="replace")
    raw_lines = raw.splitlines()
    code = strip_comments_and_strings(raw)
    problems = []

    def line_of(offset: int) -> int:
        return code.count("\n", 0, offset) + 1

    def suppressed(lineno: int) -> bool:
        return 0 < lineno <= len(raw_lines) and SUPPRESS in raw_lines[lineno - 1]

    for m in STATIC_RE.finditer(code):
        rest = code[m.end(): m.end() + 400]
        if is_mutable_static_variable(rest):
            ln = line_of(m.start())
            if not suppressed(ln):
                problems.append(f"{path}:{ln}: mutable static variable: {raw_lines[ln - 1].strip()}")

    for m in THREAD_LOCAL_RE.finditer(code):
        ln = line_of(m.start())
        if not suppressed(ln):
            problems.append(f"{path}:{ln}: thread_local state: {raw_lines[ln - 1].strip()}")

    for m in SINGLETON_RE.finditer(code):
        ln = line_of(m.start())
        if not suppressed(ln):
            problems.append(f"{path}:{ln}: singleton accessor: {raw_lines[ln - 1].strip()}")

    return problems


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent)
    problems: list[str] = []
    files = 0
    for module in MODULE_DIRS:
        base = root / module
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix in EXTENSIONS and path.is_file():
                files += 1
                problems.extend(check_file(path))

    for p in problems:
        print(p)
    print(f"check_global_state: {files} files scanned, {len(problems)} violation(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
