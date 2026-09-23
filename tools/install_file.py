#!/usr/bin/env python3
# Cross-platform replacement for `install -m MODE SRC DST`:
# copies SRC to DST, creating DST's parent dirs and applying MODE.
#
# Used by the Makefile's install_file() helper. GNU Make's path escaping
# turns spaces into "\ " (path_shell); undo that. On POSIX the shell
# already unescaped them, so this is a no-op there.
import os
import shutil
import sys


def unescape(path):
    return path.replace("\\ ", " ")


def main():
    if len(sys.argv) != 4:
        print("usage: install_file.py <mode> <src> <dst>", file=sys.stderr)
        return 1
    mode, src, dst = (unescape(a) for a in sys.argv[1:4])
    if not os.path.isfile(src):
        print(f"install_file.py: {src}: no such file", file=sys.stderr)
        return 1
    parent = os.path.dirname(os.path.abspath(dst))
    os.makedirs(parent, exist_ok=True)
    shutil.copyfile(src, dst)
    os.chmod(dst, int(mode, 8))
    return 0


if __name__ == "__main__":
    sys.exit(main())
