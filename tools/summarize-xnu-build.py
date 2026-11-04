#!/usr/bin/env python3
"""Save bounded compiler/linker error context alongside the full CI log."""
from pathlib import Path
import re
import sys

log, output = map(Path, sys.argv[1:])
text = re.sub(r"\x1b\[[0-9;]*[a-zA-Z]", "", log.read_text(errors="replace"))
lines = text.splitlines()
selected = set()
pattern = re.compile(r"error:|fatal:|undefined symbols|ld: (?:error|symbol)|make.*\*\*\*|\bFAILED\b", re.I)
for index, line in enumerate(lines):
    if pattern.search(line):
        selected.update(range(max(0, index - 4), min(len(lines), index + 12)))
indices = sorted(selected)
if len(indices) > 1000:
    indices = indices[:500] + indices[-500:]
result, previous = [], -2
for index in indices:
    if index != previous + 1:
        result.append("...")
    line = lines[index]
    if len(line) > 600:
        line = line[:600] + " ... [long line trimmed; see full log]"
    result.append(f"{index + 1}: {line}")
    previous = index
summary = "\n".join(result) if result else "No recognized compiler/linker errors. Inspect xnu-build-log."
output.write_text(summary + "\n", encoding="utf-8")
print(summary)
