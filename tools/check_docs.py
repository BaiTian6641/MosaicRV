#!/usr/bin/env python3
"""Run the plan documents' own embedded checker, with one declared deviation.

    python3 tools/check_docs.py

`docs/verification.md` embeds a self-contained checker between
`<!-- DOC-CHECK-BEGIN -->` and `<!-- DOC-CHECK-END -->`. Its stated subject is
the *planning documents*: "本轮验证对象是规划文档，不是处理器实现". It asserts that
the repository contains exactly the three byte-frozen source reports plus the
planning documents and nothing else.

That assertion was true when the repository contained no implementation. It is
now false by design, because implementing the plan is the point of the project:
this repository now contains evidence under `results/` and program documentation
under `tests/`, both of which are tracked markdown.

`docs/` is a frozen contract for this project -- append-only, never rewritten.
So the deviation is applied **here**, in this wrapper, where it is visible, and
is limited to exactly one thing: the set of markdown files the inventory check
considers. Everything else in the embedded checker -- source-report hashes,
heading coverage, task-field completeness, the dependency DAG's acyclicity,
reference identifiers, local links, git tracking and whitespace -- runs
unmodified, and any failure of those is reported as a real failure.

This wrapper prints the deviation it applies on every run. If it ever needs to
apply a second one, it refuses rather than widening itself silently.
"""

from __future__ import annotations

import argparse
import os
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
VERIFICATION_DOC = os.path.join(REPO_ROOT, "docs", "verification.md")

BEGIN = "<!-- DOC-CHECK-BEGIN -->\n```python\n"
END = "\n```\n<!-- DOC-CHECK-END -->"

# The single deviation: implementation evidence and firmware documentation are
# tracked markdown that the planning inventory did not anticipate.
ALLOWED_PREFIXES = ("results/", "tests/")

# The exact statement being relaxed, quoted so a change in the frozen plan is
# detected rather than silently worked around.
INVENTORY_STATEMENT = (
    'actual_md = {str(p.relative_to(root)) for p in root.rglob("*.md") if ".git" not in p.parts}'
)
REPLACEMENT = (
    'actual_md = {str(p.relative_to(root)) for p in root.rglob("*.md") if ".git" not in p.parts}\n'
    "# DEVIATION (see tools/check_docs.py): planning-only inventory widened to\n"
    "# implementation evidence; everything else in this checker is unmodified.\n"
    "actual_md = {p for p in actual_md if not p.startswith(%r)}" % (ALLOWED_PREFIXES,)
)


def extract_checker() -> str:
    with open(VERIFICATION_DOC, "r") as handle:
        text = handle.read()
    if BEGIN not in text or END not in text:
        raise SystemExit(
            "docs/verification.md no longer contains the embedded checker between its "
            "DOC-CHECK markers; refusing to guess what it was"
        )
    return text.split(BEGIN, 1)[1].split(END, 1)[0]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--no-deviation", action="store_true",
                        help="run the embedded checker exactly as written")
    args = parser.parse_args()

    source = extract_checker()

    if args.no_deviation:
        print("running the embedded checker unmodified (the planning inventory will fail "
              "while implementation evidence exists)")
    else:
        if INVENTORY_STATEMENT not in source:
            raise SystemExit(
                "the inventory statement in docs/verification.md has changed; refusing to "
                "apply a deviation written against a statement that no longer exists"
            )
        print("DEVIATION: markdown under %s is excluded from the planning inventory"
              % ", ".join(ALLOWED_PREFIXES))
        print("          every other check in docs/verification.md runs unmodified")
        source = source.replace(INVENTORY_STATEMENT, REPLACEMENT, 1)

    namespace = {"__name__": "__main__", "__file__": VERIFICATION_DOC}
    original = os.getcwd()
    try:
        os.chdir(REPO_ROOT)
        exec(compile(source, VERIFICATION_DOC, "exec"), namespace)  # noqa: S102
    except AssertionError as exc:
        print("FAIL plan document check: %r" % (exc.args[0] if exc.args else exc),
              file=sys.stderr)
        return 1
    except SystemExit:
        raise
    finally:
        os.chdir(original)

    print("PASS plan document check")
    return 0


if __name__ == "__main__":
    sys.exit(main())