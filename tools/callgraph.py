#!/usr/bin/env python3
"""Build a static call graph of the recompiled game from generated/default.

Usage:
  callgraph.py build            -> writes tools/callgraph.json
  callgraph.py region LO HI     -> functions in [LO, HI) called from outside it
                                   (the region's entry points), with caller counts

Edges come from direct calls in the generated code (`sub_XXXXXXXX(ctx, base)`)
and imports (`__imp__Name(ctx, base)`). Indirect calls (function pointers,
vtables) are not visible statically.
"""
import collections
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "generated", "default")
OUT = os.path.join(ROOT, "tools", "callgraph.json")

DEF = re.compile(r"^DEFINE_REX_FUNC\((sub_[0-9A-F]{8}|__\w+)\)")
CALL = re.compile(r"\b(sub_[0-9A-F]{8}|__imp__\w+)\(ctx, base\)")


def build():
    edges = collections.defaultdict(collections.Counter)
    for path in sorted(glob.glob(os.path.join(GEN, "forzahorizon2_recomp.*.cpp"))):
        current = None
        with open(path) as f:
            for line in f:
                m = DEF.match(line)
                if m:
                    current = m.group(1)
                    continue
                if current is None:
                    continue
                for callee in CALL.findall(line):
                    if callee != current:
                        edges[current][callee] += 1
    data = {caller: dict(callees) for caller, callees in edges.items()}
    with open(OUT, "w") as f:
        json.dump(data, f)
    print(f"{len(data)} functions with calls -> {OUT}")


def addr(name):
    return int(name[4:], 16) if name.startswith("sub_") else None


def region(lo, hi):
    with open(OUT) as f:
        data = json.load(f)
    inside = lambda n: (a := addr(n)) is not None and lo <= a < hi
    entry_callers = collections.defaultdict(set)
    internal = collections.Counter()
    imports = collections.Counter()
    for caller, callees in data.items():
        for callee in callees:
            if inside(callee):
                if inside(caller):
                    internal[callee] += 1
                else:
                    entry_callers[callee].add(caller)
            if inside(caller) and callee.startswith("__imp__"):
                imports[callee[7:]] += 1
    funcs_in_region = {n for n in data if inside(n)} | set(entry_callers) | set(internal)
    print(f"region {lo:08X}-{hi:08X}: {len(funcs_in_region)} functions, "
          f"{len(entry_callers)} called from outside")
    print("imports used inside:", ", ".join(f"{k}x{v}" for k, v in imports.most_common(40)))
    print("\nentry points (outside callers):")
    for callee, callers in sorted(entry_callers.items(), key=lambda kv: -len(kv[1])):
        print(f"  {callee}  callers={len(callers)}  internal_refs={internal[callee]}")


if __name__ == "__main__":
    if sys.argv[1] == "build":
        build()
    elif sys.argv[1] == "region":
        region(int(sys.argv[2], 16), int(sys.argv[3], 16))
