#!/usr/bin/env python3
"""Generate the guest -> host call bridge for the Windows (x86-64) port.

On Apple silicon the game's ARM64 code calls host C functions directly: both sides
share the AAPCS64 calling convention, so a resolved import is just a host address.
On x86-64 the game's code runs as statically recompiled code with its registers in a
CPU state struct (x[0..31], v[0..31]). A call into the host therefore needs an adapter
that reads the arguments where AAPCS64 puts them, calls the host C function with the
host's own convention, and writes the result back to x0 / v0.

This tool writes those adapters from C prototypes, so nothing is hand-written per
function:

    typedef RET (*NVNT_<name>)(ARGS);      (the NVN layer's nvn_protos.h)

Each imported host function gets a two-instruction guest stub

    svc #(BRIDGE_SVC_BASE + index)
    ret

placed in guest memory; the runtime's SVC handler sends numbers >= BRIDGE_SVC_BASE
to the generated table. Horizon's own SVC numbers end at 0x7f, so the range cannot
collide with a real system call.

Supported argument and return classes (AAPCS64, as Nintendo's toolchain uses it):
  integers, enums, booleans, pointers  -> x0..x7, then 8-byte stack slots
  float, double                        -> v0..v7, then 8-byte stack slots
Anything else (a struct or union passed by value, a variadic function, 128-bit types)
is not generated: it is listed in the report so that a person writes that adapter.
Function-pointer parameters are generated but marked "callback": the host side must
call such a pointer through the recompiler (it is a guest address), never directly.

Usage:
    gen_bridge.py --nvn-header nvn.h --protos nvn_protos.h --out-dir OUT
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import re
import sys
from pathlib import Path

BRIDGE_SVC_BASE = 0x1000
BRIDGE_SVC_MAX = 0xFFFF  # SVC takes a 16-bit immediate
MAX_GPR_ARGS = 8
MAX_FPR_ARGS = 8

# Host C types the classifier knows directly. Value: (class, C type to cast to).
_SCALARS: dict[str, tuple[str, str]] = {
    "void": ("void", "void"),
    "bool": ("gpr", "bool"),
    "_Bool": ("gpr", "bool"),
    "char": ("gpr", "char"),
    "signed char": ("gpr", "signed char"),
    "unsigned char": ("gpr", "unsigned char"),
    "short": ("gpr", "short"),
    "unsigned short": ("gpr", "unsigned short"),
    "int": ("gpr", "int"),
    "unsigned": ("gpr", "unsigned"),
    "unsigned int": ("gpr", "unsigned int"),
    "long": ("gpr", "int64_t"),          # LP64 on the guest: long is 64-bit (not Windows' LLP64)
    "unsigned long": ("gpr", "uint64_t"),
    "long long": ("gpr", "long long"),
    "unsigned long long": ("gpr", "unsigned long long"),
    "int8_t": ("gpr", "int8_t"), "uint8_t": ("gpr", "uint8_t"),
    "int16_t": ("gpr", "int16_t"), "uint16_t": ("gpr", "uint16_t"),
    "int32_t": ("gpr", "int32_t"), "uint32_t": ("gpr", "uint32_t"),
    "int64_t": ("gpr", "int64_t"), "uint64_t": ("gpr", "uint64_t"),
    "size_t": ("gpr", "uint64_t"), "ptrdiff_t": ("gpr", "int64_t"),
    "intptr_t": ("gpr", "int64_t"), "uintptr_t": ("gpr", "uint64_t"),
    "float": ("fpr32", "float"),
    "double": ("fpr64", "double"),
}
_UNSUPPORTED_SCALARS = {"long double", "__int128", "unsigned __int128", "__uint128_t", "__int128_t"}


_INT_SIZES = {"int8_t": 1, "uint8_t": 1, "char": 1, "int16_t": 2, "uint16_t": 2, "int32_t": 4, "uint32_t": 4,
              "int": 4, "unsigned": 4, "int64_t": 8, "uint64_t": 8}


def _integer_struct_size(body: str) -> int | None:
    """Size of a struct whose fields are all integer scalars (arrays and `T a, b;` allowed), naturally aligned; else None."""
    off = align_max = 0
    for field in filter(None, (" ".join(f.split()) for f in body.split(";"))):
        m = re.fullmatch(r"(?:const\s+)?(\w+)\s+(.+)", field)
        if not m or m.group(1) not in _INT_SIZES:
            return None
        size = _INT_SIZES[m.group(1)]
        for decl in m.group(2).split(","):
            d = re.fullmatch(r"\s*(\w+)\s*(?:\[\s*(\d+)\s*\])?\s*", decl)
            if not d:
                return None
            off = (off + size - 1) // size * size + size * int(d.group(2) or 1)
        align_max = max(align_max, size)
    if not align_max:
        return None
    return (off + align_max - 1) // align_max * align_max


@dataclasses.dataclass(frozen=True)
class Kind:
    """How one C type travels across the AAPCS64 boundary."""
    cls: str            # void | gpr | gpr_struct | fpr32 | fpr64 | ptr | callback | unsupported
    ctype: str          # the type as written in the prototype (used for casts)
    reason: str = ""    # why it is unsupported
    size: int = 0       # gpr_struct: size in bytes (at most 16)

    @property
    def regs(self) -> int:
        return (self.size + 7) // 8 if self.cls == "gpr_struct" else 1


@dataclasses.dataclass
class Function:
    name: str
    ret: Kind
    args: list[Kind]
    variadic: bool = False

    def problems(self) -> list[str]:
        out = []
        if self.variadic:
            out.append("variadic")
        for i, a in enumerate([self.ret] + self.args):
            if a.cls == "unsupported":
                where = "return" if i == 0 else f"arg {i - 1}"
                out.append(f"{where}: {a.reason}")
        if self.ret.cls == "callback":
            out.append("returns a function pointer (host address must become a guest stub)")
        return out

    @property
    def callbacks(self) -> list[int]:
        return [i for i, a in enumerate(self.args) if a.cls == "callback"]


class TypeTable:
    """Resolves typedef names from a C header to the classes above."""

    def __init__(self) -> None:
        self.alias: dict[str, str] = {}      # Name -> underlying type text
        self.enums: set[str] = set()
        self.structs: set[str] = set()
        self.fnptrs: set[str] = set()
        self.struct_size: dict[str, int] = {}  # structs made only of integer scalars, with their size in bytes

    @classmethod
    def from_header(cls, text: str) -> "TypeTable":
        t = cls()
        text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
        text = re.sub(r"//[^\n]*", " ", text)
        # typedef enum { ... } Name;   typedef struct [tag] { ... } Name;
        for m in re.finditer(r"typedef\s+(enum|struct|union)\b[^{;]*\{(.*?)\}\s*(\w+)\s*;", text, flags=re.S):
            kind, body, name = m.group(1), m.group(2), m.group(3)
            if kind == "enum":
                t.enums.add(name)
                continue
            t.structs.add(name)
            if kind == "struct":
                size = _integer_struct_size(body)
                if size is not None:
                    t.struct_size[name] = size
        # typedef struct Tag Name;  (opaque)
        for m in re.finditer(r"typedef\s+(struct|union)\s+\w+\s+(\w+)\s*;", text):
            t.structs.add(m.group(2))
        # typedef RET (*Name)(...);
        for m in re.finditer(r"typedef[^;{]*?\(\s*\*\s*(\w+)\s*\)\s*\(", text):
            t.fnptrs.add(m.group(1))
        # typedef <scalar or pointer> Name;
        for m in re.finditer(r"typedef\s+([A-Za-z_][\w\s\*]*?)\s*\b(\w+)\s*;", text):
            under, name = " ".join(m.group(1).split()), m.group(2)
            if under.split()[0] in ("struct", "union", "enum"):
                continue
            t.alias[name] = under
        return t

    def classify(self, ctype: str, depth: int = 0) -> Kind:
        written = " ".join(ctype.replace("*", " * ").split()).replace(" *", "*")
        bare = re.sub(r"\b(const|volatile|restrict|__restrict)\b", " ", written)
        bare = " ".join(bare.split()).replace(" *", "*")
        if depth > 16:
            return Kind("unsupported", written, "typedef loop")
        if bare.endswith("*"):
            pointee = bare[:-1].strip()
            if pointee in self.fnptrs:  # pointer to a function-pointer variable: plain data pointer
                return Kind("ptr", written)
            return Kind("ptr", written)
        if bare in self.fnptrs:
            return Kind("callback", written)
        if bare in _UNSUPPORTED_SCALARS:
            return Kind("unsupported", written, f"{bare} is not supported")
        if bare in _SCALARS:
            cls, _ = _SCALARS[bare]
            return Kind(cls, written)
        if bare in self.enums:
            return Kind("gpr", written)
        if bare in self.struct_size and self.struct_size[bare] <= 16:
            return Kind("gpr_struct", written, size=self.struct_size[bare])
        if bare in self.structs:
            return Kind("unsupported", written, f"struct {bare} passed by value")
        if bare in self.alias:
            k = self.classify(self.alias[bare], depth + 1)
            return dataclasses.replace(k, ctype=written)
        return Kind("unsupported", written, f"unknown type {bare}")


_PROTO = re.compile(r"typedef\s+(?P<ret>[^;(]+?)\s*\(\s*\*\s*NVNT_(?P<name>\w+)\s*\)\s*\((?P<args>[^;]*)\)\s*;")


def _split_args(args: str) -> list[str]:
    out, depth, cur = [], 0, []
    for ch in args:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    tail = "".join(cur).strip()
    if tail:
        out.append(tail)
    return out


def _strip_param_name(arg: str, types: TypeTable) -> str:
    """'NVNdebugCallbackSource source' -> 'NVNdebugCallbackSource'; keeps pure type text unchanged."""
    m = re.match(r"^(.*?[\w\*])\s+([A-Za-z_]\w*)$", arg)
    if not m:
        return arg
    head, last = m.group(1), m.group(2)
    known = set(_SCALARS) | types.enums | types.structs | types.fnptrs | set(types.alias)
    if last in known or last in ("const", "int", "char", "long", "short", "unsigned", "signed"):
        return arg
    return head


def parse_protos(text: str, types: TypeTable) -> list[Function]:
    funcs: list[Function] = []
    for m in _PROTO.finditer(text):
        raw_args = _split_args(m.group("args"))
        variadic = "..." in raw_args
        raw_args = [a for a in raw_args if a not in ("...", "void")]
        args = [types.classify(_strip_param_name(a, types)) for a in raw_args]
        funcs.append(Function(m.group("name"), types.classify(m.group("ret")), args, variadic))
    return funcs


@dataclasses.dataclass(frozen=True)
class Slot:
    where: str   # "x", "v" or "stack"
    index: int   # register number or 8-byte stack slot number


def assign_slots(args: list[Kind]) -> list[Slot]:
    """AAPCS64 argument allocation for scalar and pointer arguments."""
    ngrn = nsrn = nsaa = 0
    slots = []
    for a in args:
        if a.cls in ("gpr", "ptr", "callback", "gpr_struct"):
            n = a.regs
            if ngrn + n <= MAX_GPR_ARGS:
                slots.append(Slot("x", ngrn)); ngrn += n
            else:
                ngrn = MAX_GPR_ARGS  # AAPCS64 C.13: once a composite goes to the stack, no later GPR use
                slots.append(Slot("stack", nsaa)); nsaa += n
        elif a.cls in ("fpr32", "fpr64"):
            if nsrn < MAX_FPR_ARGS:
                slots.append(Slot("v", nsrn)); nsrn += 1
            else:
                slots.append(Slot("stack", nsaa)); nsaa += 1
        else:
            raise ValueError(f"cannot place {a}")
    return slots


def svc_stub_words(index: int) -> tuple[int, int]:
    """The two AArch64 instruction words of guest stub `index`: svc #(base+index); ret."""
    imm = BRIDGE_SVC_BASE + index
    if not 0 <= index or imm > BRIDGE_SVC_MAX:
        raise ValueError(f"bridge index {index} does not fit an SVC immediate")
    return 0xD4000001 | (imm << 5), 0xD65F03C0


def _read_expr(a: Kind, s: Slot) -> str:
    if a.cls in ("gpr",):
        src = f"BR_X(c, {s.index})" if s.where == "x" else f"BR_STACK64(c, {s.index})"
        return f"({a.ctype}){src}"
    if a.cls in ("ptr", "callback"):
        src = f"BR_X(c, {s.index})" if s.where == "x" else f"BR_STACK64(c, {s.index})"
        if a.cls == "callback":
            # A guest function address, carried as an opaque value. The host must not call it directly.
            return f"({a.ctype})(uintptr_t){src}"
        return f"({a.ctype})BR_G2H({src})"
    if a.cls == "gpr_struct":
        fn = "br_load_struct" if s.where == "x" else "br_load_struct_stack"
        return f"*({a.ctype} *){fn}(c, {s.index}, (uint64_t[2]){{0}}, {a.size})"
    if a.cls == "fpr32":
        return f"BR_S(c, {s.index})" if s.where == "v" else f"BR_STACK_F32(c, {s.index})"
    if a.cls == "fpr64":
        return f"BR_D(c, {s.index})" if s.where == "v" else f"BR_STACK_F64(c, {s.index})"
    raise ValueError(a)


def emit_adapter(f: Function) -> str:
    slots = assign_slots(f.args)
    call_args = ", ".join(_read_expr(a, s) for a, s in zip(f.args, slots))
    call = f"fp({call_args})"
    r = f.ret.cls
    if r == "void":
        body = f"{call};"
    elif r == "gpr":
        body = f"BR_SET_X(c, 0, (uint64_t)({call}));"
    elif r == "ptr":
        body = f"BR_SET_X(c, 0, BR_H2G((const void *)({call})));"
    elif r == "gpr_struct":
        body = f"{f.ret.ctype} r = {call}; br_store_struct(c, &r, sizeof r);"
    elif r == "fpr32":
        body = f"BR_SET_S(c, 0, {call});"
    elif r == "fpr64":
        body = f"BR_SET_D(c, 0, {call});"
    else:
        raise ValueError(f"cannot return {f.ret}")
    sig = ", ".join(a.ctype for a in f.args) or "void"
    return (
        f"/* {f.name}: {f.ret.ctype} ({sig}) */\n"
        f"static void br_{f.name}(GuestCpu *c) {{\n"
        f"    NVNT_{f.name} fp = (NVNT_{f.name})br_slots[BR_IDX_{f.name}];\n"
        f"    if (!fp) {{ bridge_missing(\"{f.name}\"); return; }}\n"
        f"    {body}\n"
        f"}}\n"
    )


def generate(funcs: list[Function], header_include: str) -> tuple[str, dict]:
    ok = [f for f in funcs if not f.problems()]
    manual = [f for f in funcs if f.problems()]
    if BRIDGE_SVC_BASE + len(ok) - 1 > BRIDGE_SVC_MAX:
        raise ValueError("too many bridged functions for the SVC immediate range")
    lines = [
        "/* Generated by games/totk/win/bridge/gen_bridge.py - do not edit by hand. */",
        "#include <stdint.h>",
        "#include <stddef.h>",
        f'#include "{header_include}"',
        '#include "bridge_abi.h"',
        "",
        f"#define BR_COUNT {len(ok)}",
        "enum {",
    ]
    lines += [f"    BR_IDX_{f.name} = {i}," for i, f in enumerate(ok)]
    lines += ["};", "", "static void *br_slots[BR_COUNT];", ""]
    lines += [emit_adapter(f) for f in ok]
    lines += ["const BridgeEntry bridge_nvn_table[BR_COUNT] = {"]
    lines += [f'    {{"{f.name}", br_{f.name}, {1 if f.callbacks else 0}}},' for f in ok]
    lines += [
        "};",
        "const unsigned bridge_nvn_count = BR_COUNT;",
        "",
        "/* Fills every host function pointer through the NVN layer's own lookup (runtime_gfx_override on the Mac).",
        "   Returns how many names the host does not implement; their adapters report bridge_missing() when called. */",
        "unsigned bridge_nvn_bind(void *(*resolve)(const char *name)) {",
        "    unsigned missing = 0;",
        "    for (unsigned i = 0; i < BR_COUNT; i++) {",
        "        br_slots[i] = resolve(bridge_nvn_table[i].name);",
        "        missing += br_slots[i] == NULL;",
        "    }",
        "    return missing;",
        "}",
        "",
    ]
    report = {
        "svc_base": BRIDGE_SVC_BASE,
        "total": len(funcs),
        "generated": len(ok),
        "with_callback_args": sorted(f.name for f in ok if f.callbacks),
        "manual": {f.name: f.problems() for f in manual},
    }
    return "\n".join(lines), report


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--nvn-header", type=Path, required=True)
    ap.add_argument("--protos", type=Path, required=True)
    ap.add_argument("--out-dir", type=Path, required=True)
    ap.add_argument("--include", default="nvn_protos.h", help="header the generated C includes for NVNT_ typedefs")
    a = ap.parse_args(argv)
    types = TypeTable.from_header(a.nvn_header.read_text(encoding="utf-8"))
    funcs = parse_protos(a.protos.read_text(encoding="utf-8"), types)
    if not funcs:
        print("no prototypes found", file=sys.stderr)
        return 1
    code, report = generate(funcs, a.include)
    a.out_dir.mkdir(parents=True, exist_ok=True)
    (a.out_dir / "bridge_nvn.c").write_text(code, encoding="utf-8")
    (a.out_dir / "bridge_nvn_report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"bridge: {report['generated']}/{report['total']} generated, {len(report['manual'])} manual, "
          f"{len(report['with_callback_args'])} with callback args")
    return 0


if __name__ == "__main__":
    sys.exit(main())
