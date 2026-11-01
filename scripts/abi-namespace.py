#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Put every tanh-lib declaration into the ABI namespace, thl::inline THL_ABI_NAMESPACE.

In every C++ and Objective-C++ file under include/, src/, test/ and examples/:

  - an opener ``namespace thl[::a::b] {`` (definitions and forward declarations alike)
    becomes ``namespace thl::inline THL_ABI_NAMESPACE[::a::b] {``;
  - a closing comment ``// namespace thl[::a::b]`` becomes
    ``// namespace thl::inline THL_ABI_NAMESPACE[::a::b]``, the spelling clang-format's
    namespace comment fixer expects;
  - a file with such an opener gets ``#include <tanh/core/AbiNamespace.h>``, the
    generated header that defines it (cmake/core-library.cmake), unless it has it already.

Namespaces nested in an opener (``namespace detail {`` inside ``thl::core``) inherit the
inline namespace and are left alone, as are ``using namespace thl::...`` lines and every
qualified name: code keeps spelling ``thl::core::Logger``.

Idempotent: a rewritten opener does not match again, so the script can be re-run at any
time. Re-run it after a rebase onto a branch that adds files or openers, then clang-format
the files it lists. ``--check`` changes nothing and exits 1 when a file would change.

Usage: scripts/abi-namespace.py [--check] [--root <tanh-lib>]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

DIRECTORIES = ("include", "src", "test", "examples")
SUFFIXES = {".h", ".hpp", ".cpp", ".cc", ".mm"}

MACRO = "THL_ABI_NAMESPACE"
INCLUDE = "#include <tanh/core/AbiNamespace.h>"

# `namespace thl {` and `namespace thl::a::b {`; a rewritten opener has `::inline X` next,
# which the `\s*\{` after the identifiers refuses.
OPENER = re.compile(
    r"^(?P<lead>\s*namespace\s+)thl(?P<rest>(?:::[A-Za-z_]\w*)*)(?P<tail>\s*\{)"
)
CLOSER = re.compile(
    r"^(?P<lead>\s*\}\s*//\s*namespace\s+)thl(?P<rest>(?:::[A-Za-z_]\w*)*)\s*$"
)
NAMESPACED_OPENER = re.compile(rf"^\s*namespace\s+thl::inline\s+{MACRO}\b")
INCLUDE_LINE = re.compile(r"^\s*#\s*(?:include|import)\s*[<\"]")
# clang-format's Google categories put angle-bracket .h includes (ours) in the first block,
# after only the main include of a source file ("Foo.h" for Foo.cpp).
ANGLE_H_INCLUDE = re.compile(r"^#include <[^>]*\.h>")
MAIN_INCLUDE = re.compile(r'^#include "(?:[^"]*/)?(?P<stem>[^"/]+)\.h"')


def rewrite_namespaces(lines: list[str]) -> list[str]:
    out = []
    for line_with_end in lines:
        line = line_with_end.rstrip("\r\n")
        end = line_with_end[len(line) :]
        line = OPENER.sub(
            lambda m: f"{m['lead']}thl::inline {MACRO}{m['rest']}{m['tail']}",
            line,
            count=1,
        )
        line = CLOSER.sub(
            lambda m: f"{m['lead']}thl::inline {MACRO}{m['rest']}", line, count=1
        )
        out.append(line + end)
    return out


def add_include(lines: list[str], stem: str) -> list[str]:
    """Insert the include where clang-format would sort it.

    Into the first block of angle-bracket .h includes, in sorted order, if there is one;
    otherwise as a block of its own before the first include (after the main include of a
    source file); in a file without includes, after `#pragma once` (or at the top).
    """
    if any(line.strip() == INCLUDE for line in lines):
        return lines

    first_include = next(
        (i for i, l in enumerate(lines) if INCLUDE_LINE.match(l)), None
    )
    if first_include is not None:
        # The first contiguous run of `#include <....h>` lines.
        start = next(
            (
                i
                for i in range(first_include, len(lines))
                if ANGLE_H_INCLUDE.match(lines[i])
            ),
            None,
        )
        if start is not None and all(
            INCLUDE_LINE.match(l) or not l.strip() for l in lines[first_include:start]
        ):
            end = start
            while end < len(lines) and ANGLE_H_INCLUDE.match(lines[end]):
                end += 1
            at = start
            while at < end and lines[at].rstrip("\n") < INCLUDE:
                at += 1
            return lines[:at] + [INCLUDE + "\n"] + lines[at:]
        main = MAIN_INCLUDE.match(lines[first_include])
        if main and main["stem"] == stem:
            at = first_include + 1
            return lines[:at] + ["\n", INCLUDE + "\n"] + lines[at:]
        return lines[:first_include] + [INCLUDE + "\n", "\n"] + lines[first_include:]

    pragma = next((i for i, l in enumerate(lines) if l.strip() == "#pragma once"), None)
    at = 0 if pragma is None else pragma + 1
    return lines[:at] + ["\n", INCLUDE + "\n"] + lines[at:]


def process(path: Path) -> list[str] | None:
    """The file's new lines, or None when it is unchanged."""
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines(keepends=True)
    new = rewrite_namespaces(lines)
    if any(NAMESPACED_OPENER.match(l) for l in new):
        new = add_include(new, path.stem)
    return None if new == lines else new


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--check", action="store_true", help="change nothing, exit 1 on a change"
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="tanh-lib",
    )
    args = parser.parse_args()

    changed: list[Path] = []
    for directory in DIRECTORIES:
        base = args.root / directory
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in SUFFIXES or not path.is_file():
                continue
            new = process(path)
            if new is None:
                continue
            changed.append(path)
            if not args.check:
                path.write_text("".join(new), encoding="utf-8")

    for path in changed:
        print(path.relative_to(args.root))
    if args.check and changed:
        print(
            f"{len(changed)} file(s) open a thl namespace without THL_ABI_NAMESPACE or lack "
            f"{INCLUDE}: run scripts/abi-namespace.py, then clang-format them.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
