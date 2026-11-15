#!/usr/bin/env python3
"""Invalidate native binaries when build options change, without needless rebuilds."""
import json
from pathlib import Path
import os
import sys
import tempfile

target = Path(sys.argv[1])
encoded = json.dumps(sys.argv[2:], ensure_ascii=True, indent=2) + "\n"
target.parent.mkdir(parents=True, exist_ok=True)
if not target.exists() or target.read_text() != encoded:
    with tempfile.NamedTemporaryFile(mode="w", dir=target.parent, delete=False) as stream:
        stream.write(encoded)
        staged = Path(stream.name)
    try:
        os.replace(staged, target)
    finally:
        staged.unlink(missing_ok=True)
