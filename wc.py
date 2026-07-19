#!/usr/bin/env python3
# -*- coding:utf-8 -*-

import os
import io

# Directories that should never be counted (build artifacts, VCS, deps caches).
SKIP_DIR_NAMES = {
    "build",
    ".git",
    ".git.bak",
    "__pycache__",
    ".vscode",
    ".cursor",
    "CMakeFiles",
    "_deps",
}

# Vendored / third-party trees under src/ (reported separately).
VENDOR_DIR_NAMES = {
    "ev",
    "kcp",
    "liblmdb",
    "context",
}


def should_skip_dir(dirname):
    return dirname in SKIP_DIR_NAMES or dirname.startswith(".")


def count_source(path, exts, label=None, skip_dirs=None):
    """Count lines for files under path whose names end with any of exts."""
    if isinstance(exts, str):
        exts = (exts,)
    if not os.path.isdir(path):
        return 0

    skip_dirs = set(skip_dirs or ())
    total = 0
    files = 0
    for root, dirs, filenames in os.walk(path):
        dirs[:] = [d for d in dirs if not should_skip_dir(d) and d not in skip_dirs]
        for filename in filenames:
            if not any(filename.endswith(ext) for ext in exts):
                continue
            filepath = os.path.join(root, filename)
            try:
                with io.open(filepath, "r", encoding="utf-8", errors="replace") as f:
                    lines = sum(1 for _ in f)
            except OSError as e:
                print("skip {}: {}".format(filepath, e))
                continue
            print("{}: {}".format(filepath, lines))
            total += lines
            files += 1

    title = label or path
    print("--- {}: {} files, {} lines ---".format(title, files, total))
    return total


def main():
    code_exts = (".cpp", ".h", ".c")
    subtotals = []

    # Root umbrella headers.
    root_headers = 0
    for name in ("qtng.h", "qtcrypto.h"):
        if os.path.isfile(name):
            with io.open(name, "r", encoding="utf-8", errors="replace") as f:
                lines = sum(1 for _ in f)
            print("{}: {}".format(name, lines))
            root_headers += lines
    if root_headers:
        print("--- root headers: {} lines ---".format(root_headers))
        subtotals.append(("root headers", root_headers))

    n = count_source("include", (".h",), "include (headers)")
    subtotals.append(("include", n))

    n = count_source("src", code_exts, "src (first-party)", skip_dirs=VENDOR_DIR_NAMES)
    subtotals.append(("src (first-party)", n))

    vendor_total = 0
    for name in sorted(VENDOR_DIR_NAMES):
        path = os.path.join("src", name)
        if not os.path.isdir(path):
            continue
        n = count_source(path, code_exts + (".S",), "src/{} (vendor)".format(name))
        vendor_total += n
    subtotals.append(("src (vendor)", vendor_total))

    for path, exts, label in (
        ("tests", (".cpp", ".h"), "tests"),
        ("examples", (".cpp", ".h"), "examples"),
        ("docs", (".rst",), "docs"),
    ):
        n = count_source(path, exts, label)
        subtotals.append((label, n))

    print()
    print("summary:")
    total = 0
    for label, n in subtotals:
        print("  {:>20}: {:6d}".format(label, n))
        total += n
    print("  {:>20}: {:6d}".format("total", total))


if __name__ == "__main__":
    main()
