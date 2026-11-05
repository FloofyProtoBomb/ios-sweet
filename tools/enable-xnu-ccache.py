#!/usr/bin/env python3
"""Prefix XNU's target C/C++ compilers with optional DRE_CCACHE.

The compiler, flags, generated headers and linker remain unchanged. An unset
make variable expands to nothing, so local uncached builds still work.
"""
from pathlib import Path
import re
import sys


def enable(path):
    original = path.read_bytes()
    text = original.decode('utf-8')
    pattern = r'(?m)^(\s*export (?:CC|CXX) := )(?!\$\(DRE_CCACHE\))(?=\$\(shell .*?-find clang)'
    updated, count = re.subn(pattern, r'\1$(DRE_CCACHE) ', text)
    if count == 0 and text.count('$(DRE_CCACHE) $(shell ') == 2:
        return
    if count != 2:
        raise ValueError(f'expected two XNU target compiler definitions, found {count}')
    path.write_bytes(updated.encode('utf-8'))


if __name__ == '__main__':
    enable(Path(sys.argv[1]))
