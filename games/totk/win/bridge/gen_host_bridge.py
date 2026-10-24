#!/usr/bin/env python3
"""Generate guest -> host adapters for the upstream runtime's host functions (the `*_override` tables).

The Mac runtime answers game imports with host C functions: each `runtime_*_override(name)` maps a mangled SDK name to
a static function of its file. On the Mac the game's ARM64 code calls them directly; on x86-64 every one needs an
adapter that reads AAPCS64 arguments from the guest CPU state. This tool finds the tables and the functions' exact
types from clang's AST (after preprocessing, with the build's own defines), classifies every parameter and return value
by AAPCS64, and writes one wrapper translation unit per runtime file:

    #include "<runtime file>.c"        // the upstream file, unchanged: its static functions and types are visible
    static void hb_<n>(GuestCpu *c) { ... call the static function with the guest's arguments ... }
    const HostBridgeEntry host_bridge_<file>[] = { {"<mangled name>", hb_<n>}, ... };

Each wrapper replaces the upstream file in the Windows build. Functions whose signature the generator cannot place
(a struct over 16 bytes returned by value, variadics, ...) are listed in the report, not generated.

usage: gen_host_bridge.py --out-dir DIR --runtime-dir DIR [--cc clang] [-- <compiler flags>] FILE.c ...
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import re
import subprocess
import sys
from pathlib import Path

MAX_GPR, MAX_FPR = 8, 8

INT_SIZES = {
    "_Bool": 1, "bool": 1, "char": 1, "signed char": 1, "unsigned char": 1, "short": 2, "unsigned short": 2,
    "int": 4, "unsigned int": 4, "long": 8, "unsigned long": 8, "long long": 8, "unsigned long long": 8,
    "__int128": 16, "unsigned __int128": 16,
}
FLOAT_SIZES = {"float": 4, "double": 8}


@dataclasses.dataclass
class Kind:
    cls: str                 # void | gpr | ptr | fpr32 | fpr64 | struct | hfa | indirect | unsupported
    spelled: str             # the type as the source spells it (valid inside the wrapped file)
    size: int = 0
    regs: int = 1            # GPRs (struct) or FPRs (hfa) taken
    lane: str = ""           # hfa: float or double
    reason: str = ""


class Records:
    """Struct and union layouts from the AST, to classify records passed by value."""

    def __init__(self) -> None:
        self.fields: dict[str, list[str]] = {}   # record id -> field canonical types
        self.by_name: dict[str, str] = {}        # "struct X" -> record id
        self.union: set[str] = set()
        self.typedefs: dict[str, str] = {}      # typedef name -> underlying type (canonical where clang gives it)
        self.anon: dict[tuple[int, int], str] = {}   # (line, col) of an unnamed record -> record id

    def resolve(self, t: str, depth: int = 0) -> str:
        """Follows typedef names (also inside pointers and arrays are left alone: those are classified by shape)."""
        t = " ".join(re.sub(r"\b(const|volatile|restrict)\b", "", t).split())
        while t in self.typedefs and depth < 16:
            t = " ".join(re.sub(r"\b(const|volatile|restrict)\b", "", self.typedefs[t]).split())
            depth += 1
        return t

    def add(self, node: dict) -> None:
        if node.get("kind") == "TypedefDecl" and node.get("name"):
            ty = node.get("type", {})
            under = ty.get("desugaredQualType", ty.get("qualType", ""))
            if under == node["name"]:            # typedef struct {...} Name: clang desugars it to itself
                under = ty.get("qualType", under)
            self.typedefs[node["name"]] = under
            # typedef struct { ... } Name: clang calls the record "struct Name" without naming its RecordDecl
            stack = list(node.get("inner", []))
            while stack:
                n = stack.pop()
                if n.get("kind") == "RecordType" and isinstance(n.get("decl"), dict):
                    tag = ty.get("qualType", "")
                    if tag.startswith(("struct ", "union ")):
                        self.by_name.setdefault(tag, n["decl"]["id"])
                    break
                stack.extend(n.get("inner", []))
            return
        if node.get("kind") != "RecordDecl" or not node.get("completeDefinition"):
            return
        loc = node.get("loc", {})
        if not node.get("name") and "line" in loc:
            self.anon[(loc["line"], loc.get("col", 0))] = node["id"]
        rid = node["id"]
        self.fields[rid] = [f["type"].get("desugaredQualType", f["type"]["qualType"])
                            for f in node.get("inner", []) if f.get("kind") == "FieldDecl"]
        if node.get("tagUsed") == "union":
            self.union.add(rid)
        if node.get("name"):
            self.by_name[f'{node.get("tagUsed", "struct")} {node["name"]}'] = rid

    def layout(self, canonical: str, depth: int = 0) -> tuple[int, int, list[str]] | None:
        """(size, align, scalar leaf kinds) of a canonical type, or None if unknown."""
        t = self.resolve(canonical)
        if depth > 12:
            return None
        if t.endswith("*") or "(*)" in t:
            return 8, 8, ["int"]
        m = re.fullmatch(r"(.*)\[(\d+)\]", t)
        if m:
            inner = self.layout(m.group(1), depth + 1)
            if not inner:
                return None
            n = int(m.group(2))
            return inner[0] * n, inner[1], inner[2] * n
        if t in INT_SIZES:
            return INT_SIZES[t], INT_SIZES[t], ["int"]
        if t in FLOAT_SIZES:
            return FLOAT_SIZES[t], FLOAT_SIZES[t], [t]
        if t.startswith("enum "):
            return 4, 4, ["int"]
        rid = self.by_name.get(t)
        if rid is None:
            m = re.match(r"(?:struct|union) \((?:unnamed|anonymous) (?:struct|union) at .*:(\d+):(\d+)\)$", t)
            if m:
                rid = self.anon.get((int(m.group(1)), int(m.group(2))))
        if rid is None or rid not in self.fields:
            return None
        size = align = 0
        leaves: list[str] = []
        for ft in self.fields[rid]:
            fl = self.layout(ft, depth + 1)
            if not fl:
                return None
            fs, fa, lv = fl
            align = max(align, fa)
            if rid in self.union:
                size = max(size, fs)
                leaves = lv if len(lv) > len(leaves) else leaves
            else:
                size = (size + fa - 1) // fa * fa + fs
                leaves += lv
        align = align or 1
        return (size + align - 1) // align * align, align, leaves


def classify(qual: dict, records: Records) -> Kind:
    spelled = qual["qualType"]
    canonical = qual.get("desugaredQualType", spelled)
    c = records.resolve(canonical)
    if c == "void":
        return Kind("void", spelled)
    if c.endswith("*") or "(*)" in c:
        return Kind("ptr", spelled, 8)
    if c in FLOAT_SIZES:
        return Kind("fpr32" if c == "float" else "fpr64", spelled, FLOAT_SIZES[c])
    if c in INT_SIZES:
        if INT_SIZES[c] > 8:
            return Kind("unsupported", spelled, reason=f"{c} not supported")
        return Kind("gpr", spelled, INT_SIZES[c])
    if c.startswith("enum "):
        return Kind("gpr", spelled, 4)
    if c.startswith(("struct ", "union ")):
        lay = records.layout(c)
        if not lay:
            return Kind("unsupported", spelled, reason=f"layout of {c} unknown")
        size, _, leaves = lay
        lanes = set(leaves)
        if len(lanes) == 1 and lanes <= {"float", "double"} and 1 <= len(leaves) <= 4:
            return Kind("hfa", spelled, size, regs=len(leaves), lane=leaves[0])
        if size <= 16:
            return Kind("struct", spelled, size, regs=(size + 7) // 8)
        return Kind("indirect", spelled, size)
    return Kind("unsupported", spelled, reason=f"type {c} not supported")


def assign(args: list[Kind]):
    """AAPCS64 slots: ('x', n) / ('v', n) / ('stack', slot) per argument."""
    ngrn = nsrn = nsaa = 0
    out = []
    for a in args:
        if a.cls in ("gpr", "ptr", "indirect"):
            if ngrn < MAX_GPR:
                out.append(("x", ngrn)); ngrn += 1
            else:
                out.append(("stack", nsaa)); nsaa += 1
        elif a.cls == "struct":
            if ngrn + a.regs <= MAX_GPR:
                out.append(("x", ngrn)); ngrn += a.regs
            else:
                ngrn = MAX_GPR
                out.append(("stack", nsaa)); nsaa += a.regs
        elif a.cls in ("fpr32", "fpr64"):
            if nsrn < MAX_FPR:
                out.append(("v", nsrn)); nsrn += 1
            else:
                out.append(("stack", nsaa)); nsaa += 1
        elif a.cls == "hfa":
            if nsrn + a.regs <= MAX_FPR:
                out.append(("v", nsrn)); nsrn += a.regs
            else:
                nsrn = MAX_FPR
                out.append(("stack", nsaa)); nsaa += (a.size + 7) // 8
        else:
            raise ValueError(a)
    return out


def read_arg(a: Kind, slot) -> str:
    where, i = slot
    t = a.spelled
    if a.cls == "gpr":
        return f"({t})BR_X(c, {i})" if where == "x" else f"({t})BR_STACK64(c, {i})"
    if a.cls == "ptr":
        src = f"BR_X(c, {i})" if where == "x" else f"BR_STACK64(c, {i})"
        return f"({t})(uintptr_t){src}"
    if a.cls == "fpr32":
        return f"BR_S(c, {i})" if where == "v" else f"BR_STACK_F32(c, {i})"
    if a.cls == "fpr64":
        return f"BR_D(c, {i})" if where == "v" else f"BR_STACK_F64(c, {i})"
    if a.cls == "indirect":   # AAPCS64 B.4: a copy in memory, its address in the register
        src = f"BR_X(c, {i})" if where == "x" else f"BR_STACK64(c, {i})"
        return f"*({t} *)(uintptr_t){src}"
    if a.cls == "struct":
        fn = "br_load_struct" if where == "x" else "br_load_struct_stack"
        return f"*({t} *){fn}(c, {i}, (uint64_t[2]){{0}}, {a.size})"
    if a.cls == "hfa":
        if where == "v":
            return f"*({t} *)br_load_hfa(c, {i}, {a.regs}, {8 if a.lane == 'double' else 4}, (uint64_t[4]){{0}})"
        return f"*({t} *)br_load_struct_stack(c, {i}, (uint64_t[4]){{0}}, {a.size})"
    raise ValueError(a)


def write_result(r: Kind, call: str) -> str:
    if r.cls == "void":
        return f"{call};"
    if r.cls == "gpr":
        return f"BR_SET_X(c, 0, (uint64_t)({call}));"
    if r.cls == "ptr":
        return f"BR_SET_X(c, 0, (uint64_t)(uintptr_t)({call}));"
    if r.cls == "fpr32":
        return f"BR_SET_S(c, 0, {call});"
    if r.cls == "fpr64":
        return f"BR_SET_D(c, 0, {call});"
    if r.cls == "struct":
        return f"{r.spelled} r_ = {call}; br_store_struct(c, &r_, sizeof r_);"
    if r.cls == "hfa":
        return f"{r.spelled} r_ = {call}; br_store_hfa(c, &r_, {r.regs}, {8 if r.lane == 'double' else 4});"
    if r.cls == "indirect":   # AAPCS64: the caller passes the result's address in x8
        return f"*({r.spelled} *)(uintptr_t)BR_X(c, 8) = {call};"
    raise ValueError(r)


def ast(cc: str, flags: list[str], path: Path) -> dict:
    out = subprocess.run([cc, *flags, "-fsyntax-only", "-Xclang", "-ast-dump=json", str(path)],
                         capture_output=True, text=True)
    if out.returncode != 0:
        raise SystemExit(f"{path}: clang failed:\n{out.stderr[-3000:]}")
    return json.loads(out.stdout)


def walk(node, visit):
    if isinstance(node, dict):
        visit(node)
        for v in node.get("inner", []):
            walk(v, visit)


def scan(tu: dict, source: Path):
    """(functions defined in this file: name -> FunctionDecl, records, [(table owner, mangled, function)])."""
    records = Records()
    funcs: dict[str, dict] = {}
    src = str(source.resolve())

    def visit(n):
        records.add(n)
        if n.get("kind") == "FunctionDecl" and any(i.get("kind") == "CompoundStmt" for i in n.get("inner", [])):
            funcs[n["name"]] = n

    walk(tu, visit)
    pairs = []
    for name, fn in funcs.items():
        if not (name.endswith("_override") or name == "runtime_observer"):
            continue
        seq: list[tuple[str, str]] = []

        def collect(n):
            k = n.get("kind")
            if k == "StringLiteral":
                seq.append(("lit", json.loads(n["value"])))
            elif k == "DeclRefExpr" and n.get("referencedDecl", {}).get("kind") == "FunctionDecl":
                seq.append(("fn", n["referencedDecl"]["name"]))

        walk(fn, collect)
        pending = None
        for kind, val in seq:
            if kind == "lit":
                pending = val
            elif pending is not None and val in funcs and val != name and not val.endswith("_override") \
                    and val != "runtime_observer":
                pairs.append((name, pending, val))
                pending = None
    return funcs, records, pairs


def generate(source: Path, rel_include: str, cc: str, flags: list[str]):
    tu = ast(cc, flags, source)
    funcs, records, pairs = scan(tu, source)
    stem = source.stem
    lines = [f"/* Generated by games/totk/win/bridge/gen_host_bridge.py from runtime/{source.name} - do not edit. */",
             f'#include "{rel_include}"', '#include "bridge_abi.h"', '#include "host_bridge.h"', ""]
    entries, manual, seen = [], {}, {}
    for owner, mangled, fname in pairs:
        decl = funcs[fname]
        ftype = decl["type"]["qualType"]
        if "..." in ftype:
            manual[mangled] = f"{fname}: variadic"
            continue
        ret_q = {"qualType": ftype.split("(")[0].strip()}
        # return type with its desugared form, from the FunctionDecl's type text is not enough: re-derive via a typedef
        params = [p for p in decl.get("inner", []) if p.get("kind") == "ParmVarDecl"]
        try:
            ret = classify(_return_type(decl), records)
            args = [classify(p["type"], records) for p in params]
        except Exception as e:  # pragma: no cover
            manual[mangled] = f"{fname}: {e}"
            continue
        bad = [a for a in [ret] + args if a.cls == "unsupported"]
        if bad:
            manual[mangled] = f"{fname}: {bad[0].reason}"
            continue
        key = fname
        if key not in seen:
            seen[key] = f"hb_{stem}_{len(seen)}"
            slots = assign(args)
            call = f"{fname}({', '.join(read_arg(a, s) for a, s in zip(args, slots))})"
            lines += [f"/* {fname}: {ftype} */", f"static void {seen[key]}(GuestCpu *c) {{",
                      f"    {write_result(ret, call)}", "}"]
        entries.append((mangled, fname, seen[key]))
    lines += ["", f"const HostBridgeEntry host_bridge_{stem}[] = {{"]
    lines += [f'    {{"{m}", (const void *){f}, {fn}}},' for m, f, fn in entries]
    lines += ["    {0, 0},", "};", ""]
    return "\n".join(lines), {"file": source.name, "generated": len(entries), "functions": len(seen), "manual": manual}


def _return_type(decl: dict) -> dict:
    """The function's return type as {qualType, desugaredQualType}, from its type string."""
    t = decl["type"]
    q = t["qualType"]
    d = t.get("desugaredQualType", q)
    return {"qualType": _ret_of(q), "desugaredQualType": _ret_of(d)}


def _ret_of(ftype: str) -> str:
    depth = 0
    for i in range(len(ftype) - 1, -1, -1):
        ch = ftype[i]
        if ch == ")":
            depth += 1
        elif ch == "(":
            depth -= 1
            if depth == 0:
                return ftype[:i].strip()
    return ftype


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    flags = []
    if "--" in argv:
        i = argv.index("--")
        argv, flags = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", type=Path, required=True)
    ap.add_argument("--runtime-dir", type=Path, required=True)
    ap.add_argument("--cc", default="clang")
    ap.add_argument("files", nargs="+")
    a = ap.parse_args(argv)
    a.out_dir.mkdir(parents=True, exist_ok=True)
    report, tables = [], []
    for f in a.files:
        src = (a.runtime_dir / f).resolve()
        rel = Path("..") / src  # absolute include keeps the build simple
        code, rep = generate(src, str(src), a.cc, flags)
        (a.out_dir / f"hb_{src.stem}.c").write_text(code)
        report.append(rep)
        tables.append(src.stem)
    index = ["/* Generated by gen_host_bridge.py - do not edit. */", '#include "host_bridge.h"']
    index += [f"extern const HostBridgeEntry host_bridge_{t}[];" for t in tables]
    index += ["const HostBridgeEntry *const host_bridge_tables[] = {"] + [f"    host_bridge_{t}," for t in tables]
    index += ["    0,", "};", ""]
    (a.out_dir / "hb_index.c").write_text("\n".join(index))
    (a.out_dir / "hb_report.json").write_text(json.dumps(report, indent=1) + "\n")
    gen = sum(r["generated"] for r in report)
    man = sum(len(r["manual"]) for r in report)
    print(f"host bridge: {gen} names generated, {man} left for a person, {len(report)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
