#!/usr/bin/env python3
"""Self-test for check_global_state.py: known-bad snippets must be flagged,
known-good ones must not. Run in CI before the real scan."""

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_global_state import check_file  # noqa: E402

BAD = [
    "static int counter = 0;",
    "void f() { static std::vector<int> cache; }",
    "static std::function<void()> callback;",
    "static inline int x = 3;",
    "thread_local int t = 0;",
    "Foo& Foo::instance() { return x; }",
    "auto& s = Registry::getInstance();",
]

GOOD = [
    "static constexpr int kSize = 4;",
    "static const char* const kName = \"x\";",
    "static inline constexpr float kPi = 3.14f;",
    "static int helper(int a);",
    "static std::array<int, 3> make(int a) { return {}; }",
    "static_assert(sizeof(int) == 4);",
    "auto v = static_cast<int>(f);",
    "// static int commentedOut = 0;",
    "const char* s = \"static int inString = 0;\";",
    "static int allowed = 0; // global-state-ok: test fixture",
    "struct S { static S make(); };",
]


def run(snippet: str) -> int:
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "snippet.cpp"
        p.write_text(snippet + "\n", encoding="utf-8")
        return len(check_file(p))


def main() -> int:
    failures = 0
    for s in BAD:
        if run(s) == 0:
            print(f"MISSED (should flag): {s}")
            failures += 1
    for s in GOOD:
        if run(s) != 0:
            print(f"FALSE POSITIVE: {s}")
            failures += 1
    print(f"test_check_global_state: {len(BAD) + len(GOOD)} cases, {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
