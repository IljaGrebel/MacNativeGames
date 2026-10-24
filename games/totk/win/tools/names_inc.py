#!/usr/bin/env python3
"""names.txt (one NVN function name per line) -> a C initializer list of string literals."""
import sys
from pathlib import Path

names = [l.strip() for l in Path(sys.argv[1]).read_text().splitlines() if l.strip() and not l.startswith('#')]
Path(sys.argv[2]).write_text(''.join(f'    "{n}",\n' for n in names))
