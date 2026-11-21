"""Add the imports flatc leaves out of its generated Python modules.

flatc's Python output names classes of other modules without importing them:
the object API (`XT._UnPack`, the union `...Creator` functions) refers to
`PricingT`, `PointCreator`, ... with no import at all, and a reader imports a
nested table class only inside the first accessor that uses it, so a second
accessor of the same type (`BasisSwap.Leg2`) raises NameError. Packing never
needs these names; unpacking (`InitFromBuf`, `InitFromPackedBuf`) and the
second accessors do.

For every reference to a class of another generated module (`X`, `XT`,
`XCreator`) this script inserts `from quantra.X import ...` on the line before
it, at the same indentation, unless the same block already imported it.
Imports stay inside the functions, as flatc's own ones do, so modules that
refer to each other never import each other at load time. Running it twice
changes nothing.

Usage: python3 fix_imports.py <generated quantra package directory>
"""

import os
import re
import sys

DEFINITION = re.compile(r"^(?:class|def) (\w+)")
IMPORT = re.compile(r"^\s*from quantra\.(\w+) import (\w+)\s*$")
# A capitalised name not preceded by a dot (attribute) or a word character.
NAME = re.compile(r"(?<![\w.])([A-Z]\w*)")


def exported_names(directory):
    """Map every class / union creator defined in the package's top-level
    modules to the module that defines it."""
    names = {}
    for filename in sorted(os.listdir(directory)):
        if not filename.endswith(".py") or filename == "__init__.py":
            continue
        module = filename[:-3]
        with open(os.path.join(directory, filename)) as f:
            for line in f:
                m = DEFINITION.match(line)
                if m and m.group(1) in (module, module + "T", module + "Creator"):
                    names[m.group(1)] = module
    return names


def _indent(line):
    return len(line) - len(line.lstrip(" "))


def fix_lines(lines, names):
    """Return the lines of one module with the missing imports inserted."""
    own = {m.group(1) for m in (DEFINITION.match(l) for l in lines) if m}
    out = []
    imported = []  # (indent, name): imports visible in the current block
    in_function = False
    for line in lines:
        code = line.split("#", 1)[0]
        if not code.strip() or '"""' in code:
            out.append(line)
            continue
        indent = _indent(line)
        imported = [(i, n) for i, n in imported if i <= indent]
        if line.lstrip().startswith("def "):
            in_function = True
            out.append(line)
            continue
        if indent == 0:
            in_function = False
        m = IMPORT.match(line)
        if m:
            imported.append((indent, m.group(2)))
            out.append(line)
            continue
        if not in_function or indent == 0:
            # Class bodies hold only constants (a union's member names are
            # its enum values, not references).
            out.append(line)
            continue
        visible = {n for _, n in imported}
        for name in dict.fromkeys(NAME.findall(code)):
            target = names.get(name)
            if target is None or name in own or name in visible:
                continue
            out.append(f"{' ' * indent}from quantra.{target} import {name}\n")
            imported.append((indent, name))
            visible.add(name)
        out.append(line)
    return out


def fix_imports(directory):
    names = exported_names(directory)
    fixed = 0
    for filename in sorted(os.listdir(directory)):
        if not filename.endswith(".py") or filename == "__init__.py":
            continue
        path = os.path.join(directory, filename)
        with open(path) as f:
            lines = f.readlines()
        new_lines = fix_lines(lines, names)
        if new_lines != lines:
            with open(path, "w") as f:
                f.writelines(new_lines)
            fixed += 1
    print(f"  Added missing imports to {fixed} generated modules.")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 fix_imports.py <directory>")
        sys.exit(1)
    fix_imports(sys.argv[1])
