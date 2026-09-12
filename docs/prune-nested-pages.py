#!/usr/bin/env python3
"""Delete breathe-apidoc pages for nested classes/structs.

Breathe renders a nested type inside its enclosing class page already; a page
of its own on top of that makes Sphinx report every member as a duplicate C++
declaration. Run after breathe-apidoc with the api directory as argument.
"""
import re
import sys
from pathlib import Path

api = Path(sys.argv[1])
directive = re.compile(r"^\.\.\s+doxygen(?:class|struct)::\s+(.+?)\s*$", re.M)


def strip_template_args(name: str) -> str:
    out, depth = [], 0
    for ch in name:
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
        elif depth == 0:
            out.append(ch)
    return "".join(out).strip()


pages = {}
for rst in list(api.glob("class/*.rst")) + list(api.glob("struct/*.rst")):
    m = directive.search(rst.read_text())
    if m:
        pages[rst] = strip_template_args(m.group(1))

names = set(pages.values())
removed = 0
for rst, name in pages.items():
    parent = name.rpartition("::")[0]
    if parent in names:
        rst.unlink()
        removed += 1
print(f"prune-nested-pages: removed {removed} nested compound page(s)")
