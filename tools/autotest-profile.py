#!/usr/bin/env python3
"""Print a profile's boot-args, secs, cores (three lines) for autotest.sh."""
import json, sys, os
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = json.load(open(os.path.join(root, "tools", "autotest-tests.json")))
p = spec["profiles"][sys.argv[1]]
print(p["args"])
print(p.get("secs", 55))
print(p.get("cores", 4))
