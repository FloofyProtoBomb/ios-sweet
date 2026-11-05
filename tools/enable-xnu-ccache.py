#!/usr/bin/env python3
"""Wrap XNU's selected target C/C++ compilers with optional DRE_CCACHE.

darwin-xnu-build exports CC/CXX, so XNU's default-only compiler assignments
may never execute. Wrap after those guards instead, retaining the selected
compiler and arguments, including make command-line overrides. Recursive
makes must not add a second prefix. An unset/empty DRE_CCACHE changes nothing
about compiler selection or arguments.
"""
from pathlib import Path
import re
import sys


BEGIN = '# BEGIN DRE optional target compiler cache'
END = '# END DRE optional target compiler cache'
BLOCK = '''
# BEGIN DRE optional target compiler cache
ifneq ($(strip $(DRE_CCACHE)),)
# MIG executes its preprocessor as one quoted pathname. Preserve the actual
# selected compiler before CC becomes a cache+compiler command, including
# recursive makes that already inherited a cache prefix. Explicit MIGCC wins.
ifeq ($(origin MIGCC),undefined)
ifneq ($(notdir $(firstword $(CC))),$(notdir $(firstword $(DRE_CCACHE))))
export MIGCC := $(firstword $(CC))
else
export MIGCC := $(word 2,$(CC))
endif
endif
ifneq ($(notdir $(firstword $(CC))),$(notdir $(firstword $(DRE_CCACHE))))
override CC := $(strip $(DRE_CCACHE)) $(CC)
endif
ifneq ($(notdir $(firstword $(CXX))),$(notdir $(firstword $(DRE_CCACHE))))
override CXX := $(strip $(DRE_CCACHE)) $(CXX)
endif
export CC CXX
endif
# END DRE optional target compiler cache
'''


def enable(path):
    original = path.read_bytes()
    text = original.decode('utf-8')
    lines = text.splitlines(keepends=True)
    definitions = {}
    legacy = 0
    for index, line in enumerate(lines):
        match = re.fullmatch(
            r'([ \t]*export (CC|CXX) := )(\$\(DRE_CCACHE\) )?'
            r'(\$\(shell [^\r\n]*?-find (clang(?:\+\+)?)[ \t]*\))',
            line.rstrip('\r\n'))
        if not match:
            continue
        name, compiler = match[2], match[5]
        if name in definitions or compiler != {'CC':'clang', 'CXX':'clang++'}[name]:
            raise ValueError('unexpected duplicate or mismatched XNU compiler definition')
        if (not index or index + 1 >= len(lines) or
                lines[index - 1].strip() != f'ifeq ($(origin {name}),default)' or
                lines[index + 1].strip() != 'endif'):
            raise ValueError('expected default-only XNU compiler guards')
        definitions[name] = index
        legacy += bool(match[3])
        if match[3]:
            # Migrate the previous hook, which wrapped only default-origin CC.
            ending = line[len(line.rstrip('\r\n')):]
            lines[index] = match[1] + match[4] + ending
    if set(definitions) != {'CC','CXX'} or definitions['CXX'] != definitions['CC'] + 3:
        raise ValueError('expected adjacent XNU target CC/CXX default definitions')
    if legacy not in (0, 2):
        raise ValueError('refusing a partially applied legacy compiler cache hook')
    newline = '\r\n' if lines[definitions['CXX']].endswith('\r\n') else '\n'
    block = BLOCK.replace('\n', newline)
    insert = definitions['CXX'] + 2
    if text.count(BEGIN) or text.count(END):
        if (legacy or text.count(BEGIN) != 1 or text.count(END) != 1 or
                not ''.join(lines[insert:]).startswith(block)):
            raise ValueError('unexpected or modified DRE compiler cache block')
        return
    if not lines[insert - 1].endswith(('\r\n', '\n')):
        raise ValueError('unterminated XNU compiler guard')
    lines.insert(insert, block)
    path.write_bytes(''.join(lines).encode('utf-8'))


if __name__ == '__main__':
    enable(Path(sys.argv[1]))
