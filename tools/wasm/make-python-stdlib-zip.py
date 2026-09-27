#!/usr/bin/env python3
"""Pack an installed CPython stdlib (lib/pythonX.Y) into pythonXY.zip for MEMFS.

    make-python-stdlib-zip.py <prefix>/lib/python3.11 <prefix>/lib/python311.zip

The zip holds .pyc only (compiled by THIS interpreter, which must have the same
minor version as the target - build-deps.sh runs it with the build python), so
nothing has to be compiled in the browser.  zipimport can load neither
extension modules nor anything that needs a real directory on disk, which is
why the few packages that do (none of those FreeCAD's core imports) are left
out along with everything a headless, network-less, single-threaded
interpreter cannot use.
"""
import os
import py_compile
import sys
import tempfile
import zipfile

EXCLUDE_DIRS = {
    "test", "tests", "idlelib", "tkinter", "turtledemo", "ensurepip", "lib2to3",
    "distutils", "venv", "pydoc_data", "site-packages", "__pycache__",
    "config-3.11-wasm32-emscripten", "lib-dynload", "curses", "dbm", "sqlite3",
    "msilib", "wsgiref", "xmlrpc",
}
EXCLUDE_FILES = {"turtle.py", "pydoc.py", "antigravity.py", "this.py"}


def main(src: str, dst: str) -> None:
    n = 0
    with tempfile.TemporaryDirectory() as tmp, \
            zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for root, dirs, files in os.walk(src):
            dirs[:] = sorted(d for d in dirs
                             if d not in EXCLUDE_DIRS and not d.startswith("config-"))
            for f in sorted(files):
                if not f.endswith(".py") or f in EXCLUDE_FILES:
                    continue
                path = os.path.join(root, f)
                rel = os.path.relpath(path, src)
                pyc = os.path.join(tmp, "x.pyc")
                py_compile.compile(path, cfile=pyc, dfile=rel, doraise=True, optimize=0,
                                   invalidation_mode=py_compile.PycInvalidationMode.UNCHECKED_HASH)
                zf.write(pyc, rel + "c")
                n += 1
    print(f"{dst}: {n} modules, {os.path.getsize(dst) / 1e6:.1f} MB")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
