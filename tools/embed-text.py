#!/usr/bin/env python3
import pathlib
import sys
import json

src = pathlib.Path(sys.argv[1])
dst = pathlib.Path(sys.argv[2])
name = sys.argv[3]

text = src.read_text()
with dst.open("w") as f:
  f.write(f"static const char {name}[] =\n")
  for line in text.splitlines(True):
    f.write(f"  {json.dumps(line)}\n")
  f.write("  ;\n")
