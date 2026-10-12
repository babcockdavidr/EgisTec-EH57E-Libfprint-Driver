#!/usr/bin/env python3
"""Measure textual overlap between the egis057e driver and reference libfprint drivers.

    python3 compare_with_reference.py DRIVER_DIR LIBFPRINT_DRIVERS_DIR

Two measures, both computed on code with comments and whitespace removed:
  * shared lines   - identical normalised lines of >= MIN_LINE characters
  * shingle overlap - share of OUR 12-token windows that also occur in the reference
A line-by-line listing of the shared lines is printed so a human can judge whether each
one is forced by the libfprint API (boilerplate) or is genuinely copied logic.
"""
import os
import re
import sys

MIN_LINE = 24
SHINGLE = 12
OURS = ("egis057e.c", "egis057e.h", "egis057e_image.h")


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    return re.sub(r"//[^\n]*", " ", src)


def norm_lines(src):
    out = []
    for line in strip_comments(src).splitlines():
        line = " ".join(line.split())
        if len(line) >= MIN_LINE:
            out.append(line)
    return out


def tokens(src):
    return re.findall(r"[A-Za-z_]\w*|\d+|\S", strip_comments(src))


def shingles(toks):
    return {tuple(toks[i:i + SHINGLE]) for i in range(len(toks) - SHINGLE + 1)}


def read(path):
    with open(path, errors="replace") as fh:
        return fh.read()


def main():
    ours_dir, ref_dir = sys.argv[1], sys.argv[2]
    our_src = "\n".join(read(os.path.join(ours_dir, f)) for f in OURS)
    our_lines = set(norm_lines(our_src))
    our_sh = shingles(tokens(our_src))
    print(f"our code: {len(our_lines)} distinct non-trivial lines, {len(our_sh)} {SHINGLE}-token windows\n")

    rows = []
    for root, _, files in os.walk(ref_dir):
        for f in files:
            if not f.endswith((".c", ".h")) or f.startswith("egis057e"):
                continue
            path = os.path.join(root, f)
            src = read(path)
            shared = our_lines & set(norm_lines(src))
            sh = len(our_sh & shingles(tokens(src))) / max(1, len(our_sh))
            rows.append((sh, len(shared), os.path.relpath(path, ref_dir), sorted(shared)))
    rows.sort(reverse=True)

    print("closest references (window overlap = share of OUR code found in that file):")
    for sh, n, name, _ in rows[:8]:
        print(f"  {name:38s} windows {sh * 100:5.1f}%   shared lines {n}")

    print("\nshared lines with the Egis 0570 driver (the closest vendor/design relative):")
    for sh, n, name, shared in rows:
        if os.path.basename(name) in ("egis0570.c", "egis0570.h"):
            print(f"  {name}: {n} shared line(s), window overlap {sh * 100:.1f}%")
            for line in shared:
                print(f"      {line}")

    print("\nshared lines with ANY driver, grouped (these are libfprint API idioms):")
    counts = {}
    for _, _, name, shared in rows:
        for line in shared:
            counts.setdefault(line, set()).add(os.path.basename(name))
    for line, names in sorted(counts.items(), key=lambda kv: -len(kv[1]))[:40]:
        print(f"  [{len(names):2d} drivers] {line}")


if __name__ == "__main__":
    main()
