#!/usr/bin/env python3
"""Copy the egis057e driver into a libfprint source tree and register it in the build files.

    python3 add_to_libfprint_tree.py PATH/TO/libfprint-source

Idempotent (safe to run twice). Handles the three layouts of libfprint/meson.build found in 1.94.x and 1.95.x.
If a pattern is not found it says exactly which line to add by hand.
"""
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DRIVER_DIR = os.path.join(HERE, "..", "driver")
FILES = ("egis057e.c", "egis057e.h", "egis057e_image.h")


def edit(path, fn, what):
    with open(path) as fh:
        text = fh.read()
    new = fn(text)
    if new is None:
        print(f"  !! {what}: pattern not found in {path} - add it by hand (see README)")
        return False
    if new != text:
        with open(path, "w") as fh:
            fh.write(new)
        print(f"  ok {what}")
    else:
        print(f"  -- {what}: already present")
    return True


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    tree = sys.argv[1]
    drivers = os.path.join(tree, "libfprint", "drivers")
    if not os.path.isdir(drivers):
        sys.exit(f"{tree} does not look like a libfprint source tree (no libfprint/drivers)")

    for f in FILES:
        shutil.copy(os.path.join(DRIVER_DIR, f), os.path.join(drivers, f))
    print(f"copied {', '.join(FILES)} into {drivers}")

    def lib_meson(t):
        if "egis057e" in t:
            return t
        # layout used by 1.95.x:   'egis0570': files('drivers/egis0570.c'),
        m = re.search(r"^(\s*)'egis0570'\s*:\s*files\('drivers/egis0570\.c'\),\s*\n", t, re.M)
        if m:
            ind = m.group(1)
            return t[:m.end()] + f"{ind}'egis057e': files('drivers/egis057e.c'),\n" + t[m.end():]
        # layout used by 1.94.x:   'egis0570' :\n        { 'sources': [ 'drivers/egis0570.c' ] },
        m = re.search(r"^(\s*)'egis0570'\s*:\s*\n\s*\{\s*'sources'\s*:\s*\[\s*'drivers/egis0570\.c'\s*\]\s*\},\s*\n", t, re.M)
        if m:
            ind = m.group(1)
            return (t[:m.end()] + f"{ind}'egis057e' :\n{ind}    {{ 'sources': [ 'drivers/egis057e.c' ] }},\n" + t[m.end():])
        # layout used by 1.94.9:   'egis0570' :\n        [ 'drivers/egis0570.c' ],
        m = re.search(r"^(\s*)'egis0570'\s*:\s*\n\s*\[\s*'drivers/egis0570\.c'\s*\],\s*\n", t, re.M)
        if m:
            ind = m.group(1)
            return t[:m.end()] + f"{ind}'egis057e' :\n{ind}    [ 'drivers/egis057e.c' ],\n" + t[m.end():]
        return None

    def top_meson(t):
        if "egis057e" in t:
            return t
        out, n = re.subn(r"^(\s*)'egis0570',\s*\n", lambda m: m.group(0) + f"{m.group(1)}'egis057e',\n", t, flags=re.M)
        return out if n else None

    ok = edit(os.path.join(tree, "libfprint", "meson.build"), lib_meson, "libfprint/meson.build driver entry")
    ok &= edit(os.path.join(tree, "meson.build"), top_meson, "meson.build driver lists")
    print("\nDone." if ok else "\nFinished with warnings.")
    print("Configure with e.g.:  meson setup build -Ddrivers=all   (or add egis057e to your driver list)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
