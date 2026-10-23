"""Tests for the Windows port's guest -> host bridge generator (games/totk/win/bridge/gen_bridge.py).

Three layers:
  * the type classifier and AAPCS64 slot allocation, against fixed expectations;
  * the same allocation against a real AArch64 compiler: clang compiles, for each argument K of a signature, a
    function that returns argument K, and the test reads from its assembly where the argument came from
    (skipped when clang cannot target aarch64);
  * the generated C, compiled and run on the host: a fake guest CPU is filled the way an AArch64 caller would fill
    it, the adapter is called through the table, and the host function checks what it received.
The real NVN headers in the repository are also run through the generator and the result compiled.
"""
from __future__ import annotations

import importlib.util
import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BRIDGE = ROOT / "games/totk/win/bridge"
NVN = ROOT / "games/totk/third_party/d2r_gfx/gfx/nvn"

_spec = importlib.util.spec_from_file_location("gen_bridge", BRIDGE / "gen_bridge.py")
gb = importlib.util.module_from_spec(_spec)
sys.modules["gen_bridge"] = gb   # dataclasses resolve annotations through sys.modules
_spec.loader.exec_module(gb)

HEADER = """
#include <stdint.h>
typedef uint8_t NVNboolean;
typedef uint64_t NVNtextureHandle;
typedef struct { uint64_t value; } H8;
typedef struct { uint64_t a, b; } H16;
typedef struct { uint64_t a, b, c; } H24;
typedef struct { char reserved[64]; } Opaque;
typedef enum { MODE_A = 0, MODE_B = 1 } Mode;
typedef void (*Callback)(void *);
"""


def _types() -> "gb.TypeTable":
    return gb.TypeTable.from_header(HEADER)


def _cc() -> str | None:
    return shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")


def _aarch64_clang() -> str | None:
    clang = shutil.which("clang")
    if not clang:
        return None
    probe = subprocess.run([clang, "--target=aarch64-linux-gnu", "-x", "c", "-S", "-o", "-", "-"],
                           input="int f(int a){return a;}", capture_output=True, text=True)
    return clang if probe.returncode == 0 else None


class ClassifierTests(unittest.TestCase):
    def test_scalars_enums_pointers(self):
        t = _types()
        self.assertEqual(t.classify("int").cls, "gpr")
        self.assertEqual(t.classify("NVNboolean").cls, "gpr")
        self.assertEqual(t.classify("NVNtextureHandle").cls, "gpr")
        self.assertEqual(t.classify("Mode").cls, "gpr")
        self.assertEqual(t.classify("const Opaque*").cls, "ptr")
        self.assertEqual(t.classify("float").cls, "fpr32")
        self.assertEqual(t.classify("double").cls, "fpr64")
        self.assertEqual(t.classify("void").cls, "void")
        self.assertEqual(t.classify("const Callback").cls, "callback")

    def test_structs(self):
        t = _types()
        self.assertEqual((t.classify("H8").cls, t.classify("H8").size), ("gpr_struct", 8))
        self.assertEqual((t.classify("H16").cls, t.classify("H16").regs), ("gpr_struct", 2))
        self.assertEqual(t.classify("H24").cls, "unsupported")   # > 16 bytes: passed by reference, not generated
        self.assertEqual(t.classify("Opaque").cls, "unsupported")
        self.assertEqual(t.classify("long double").cls, "unsupported")

    def test_slots(self):
        t = _types()
        k = [t.classify(x) for x in ["int"] * 9]
        self.assertEqual(gb.assign_slots(k)[-1], gb.Slot("stack", 0))
        k = [t.classify(x) for x in ["int"] * 7 + ["H16", "int"]]
        s = gb.assign_slots(k)
        self.assertEqual(s[7], gb.Slot("stack", 0))   # H16 does not fit x7 alone
        self.assertEqual(s[8], gb.Slot("stack", 2))   # and no later argument goes back to a GPR
        k = [t.classify(x) for x in ["float"] * 9 + ["int"]]
        s = gb.assign_slots(k)
        self.assertEqual(s[8], gb.Slot("stack", 0))
        self.assertEqual(s[9], gb.Slot("x", 0))

    def test_stub_words(self):
        self.assertEqual(gb.svc_stub_words(0), (0xD4020001, 0xD65F03C0))   # svc #0x1000; ret
        self.assertEqual(gb.svc_stub_words(1)[0], 0xD4020021)
        with self.assertRaises(ValueError):
            gb.svc_stub_words(0xF000)


SIGNATURES = [
    ["int", "float", "double", "H8", "long", "long", "long", "long", "H16", "long", "float"],
    ["int"] * 10,
    ["double"] * 10 + ["int"],
    ["int"] * 7 + ["H16", "int", "float"],
    ["float", "int", "float", "int", "double", "int", "float", "int", "double", "int", "float", "int"],
    ["H16", "H16", "H16", "H16", "int"],
]


class Aarch64OracleTests(unittest.TestCase):
    """assign_slots must agree with what clang does for aarch64-linux-gnu (standard AAPCS64, as on the Switch)."""

    @classmethod
    def setUpClass(cls):
        cls.clang = _aarch64_clang()
        if not cls.clang:
            raise unittest.SkipTest("clang cannot target aarch64")

    def _where(self, asm: str, fn: str, cls: str) -> gb.Slot:
        body = asm.split(f"{fn}:", 1)[1].split("ret", 1)[0]
        insns = [l.strip() for l in body.splitlines() if l.strip() and not l.strip().startswith(("//", "."))]
        fp = cls in ("fpr32", "fpr64")
        if not insns:
            return gb.Slot("v" if fp else "x", 0)
        last = insns[-1]
        m = re.search(r"\[sp(?:,\s*#(\d+))?\]", last)
        if m:
            return gb.Slot("stack", int(m.group(1) or 0) // 8)
        ops = re.findall(r"\b([xwsdvq])(\d+)\b", last.split(None, 1)[1])
        return gb.Slot("v" if fp else "x", int(ops[-1][1]))

    def test_matches_clang(self):
        t = _types()
        for sig in SIGNATURES:
            kinds = [t.classify(x) for x in sig]
            params = ", ".join(f"{ty} a{i}" for i, ty in enumerate(sig))
            src = [HEADER]
            for i, (ty, k) in enumerate(zip(sig, kinds)):
                if k.cls == "gpr_struct":
                    src.append(f"uint64_t pick{i}({params}) {{ return a{i}.{'value' if k.size == 8 else 'a'}; }}")
                elif k.cls in ("fpr32", "fpr64"):
                    src.append(f"{ty} pick{i}({params}) {{ return a{i}; }}")
                else:
                    src.append(f"uint64_t pick{i}({params}) {{ return (uint64_t)a{i}; }}")
            asm = subprocess.run([self.clang, "--target=aarch64-linux-gnu", "-ffreestanding", "-O2", "-x", "c", "-S", "-o", "-", "-"],
                                 input="\n".join(src), capture_output=True, text=True, check=True).stdout
            expected = gb.assign_slots(kinds)
            for i, k in enumerate(kinds):
                with self.subTest(sig=sig, arg=i):
                    self.assertEqual(self._where(asm, f"pick{i}", k.cls), expected[i])


HOST_TEST = r"""
#include <stdio.h>
#include <string.h>
#include "bridge_abi.h"
extern const BridgeEntry bridge_nvn_table[];
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
void bridge_missing(const char *name) { printf("missing %s\n", name); fails++; }

static uint64_t stack_mem[8];
static double mixed(int a0, float a1, double a2, H8 a3, long a4, long a5, long a6, long a7, H16 a8, long a9, float a10) {
    CHECK(a0 == -7); CHECK(a1 == 1.5f); CHECK(a2 == 2.25); CHECK(a3.value == 0x1122334455667788ull);
    CHECK(a4 == 4 && a5 == 5 && a6 == 6 && a7 == 7); CHECK(a8.a == 80 && a8.b == 81); CHECK(a9 == 9); CHECK(a10 == 10.5f);
    return a2 * 2;
}
static H16 pair(const Opaque *p, Mode m) { CHECK(p == (const Opaque *)(uintptr_t)0x10000000ull); CHECK(m == MODE_B); H16 r = {3, 4}; return r; }
static Opaque *ident(Opaque *p) { return p; }
static void *resolve(const char *name) {
    if (!strcmp(name, "mixed")) return (void *)mixed;
    if (!strcmp(name, "pair")) return (void *)pair;
    if (!strcmp(name, "ident")) return (void *)ident;
    return NULL;
}
static const BridgeEntry *find(const char *n) {
    for (unsigned i = 0; i < bridge_nvn_count; i++) if (!strcmp(bridge_nvn_table[i].name, n)) return &bridge_nvn_table[i];
    return NULL;
}
int main(void) {
    CHECK(bridge_nvn_bind(resolve) == 0);
    GuestCpu c; memset(&c, 0, sizeof c);
    c.x[0] = (uint64_t)(int64_t)-7; float f1 = 1.5f; memcpy(&c.v[0], &f1, 4); double d2 = 2.25; memcpy(&c.v[1], &d2, 8);
    c.x[1] = 0x1122334455667788ull; c.x[2] = 4; c.x[3] = 5; c.x[4] = 6; c.x[5] = 7; c.x[6] = 80; c.x[7] = 81;
    stack_mem[0] = 9; float f10 = 10.5f; memcpy(&c.v[2], &f10, 4);
    c.x[31] = (uint64_t)(uintptr_t)stack_mem;
    find("mixed")->adapter(&c);
    double r; memcpy(&r, &c.v[0], 8); CHECK(r == 4.5);

    memset(&c, 0, sizeof c); c.x[0] = 0x10000000ull; c.x[1] = MODE_B;
    find("pair")->adapter(&c); CHECK(c.x[0] == 3 && c.x[1] == 4);

    memset(&c, 0, sizeof c); c.x[0] = 0x2000; find("ident")->adapter(&c); CHECK(c.x[0] == 0x2000);

    uint32_t w[2]; bridge_stub_words(1, w); CHECK(w[0] == 0xD4020021u && w[1] == 0xD65F03C0u);
    printf(fails ? "FAILED\n" : "ok\n");
    return fails != 0;
}
"""

PROTOS = """
typedef double (*NVNT_mixed)(int, float, double, H8, long, long, long, long, H16, long, float);
typedef H16 (*NVNT_pair)(const Opaque*, Mode);
typedef Opaque* (*NVNT_ident)(Opaque*);
typedef void (*NVNT_takes_big)(H24);
typedef void (*NVNT_vararg)(int, ...);
"""


class GeneratedCodeTests(unittest.TestCase):
    def setUp(self):
        self.cc = _cc()
        if not self.cc:
            self.skipTest("no C compiler")
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def test_adapters_run(self):
        (self.tmp / "protos.h").write_text(HEADER + PROTOS)
        types = gb.TypeTable.from_header(HEADER)
        funcs = gb.parse_protos(PROTOS, types)
        code, report = gb.generate(funcs, "protos.h")
        self.assertEqual(report["generated"], 3)
        self.assertEqual(set(report["manual"]), {"takes_big", "vararg"})
        (self.tmp / "bridge.c").write_text(code)
        (self.tmp / "main.c").write_text('#include "protos.h"\n' + HOST_TEST)
        exe = self.tmp / "bridge_test"
        subprocess.run([self.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        f"-I{BRIDGE}", f"-I{self.tmp}", str(self.tmp / "bridge.c"), str(self.tmp / "main.c"),
                        "-o", str(exe)], check=True)
        out = subprocess.run([str(exe)], capture_output=True, text=True)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)

    def test_real_nvn_headers(self):
        out = self.tmp / "gen"
        rc = gb.main(["--nvn-header", str(NVN / "include/nvn.h"), "--protos", str(NVN / "nvn_protos.h"),
                      "--out-dir", str(out)])
        self.assertEqual(rc, 0)
        report = json.loads((out / "bridge_nvn_report.json").read_text())
        self.assertGreaterEqual(report["generated"], 470)
        self.assertLessEqual(len(report["manual"]), 8)
        subprocess.run([self.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        f"-I{BRIDGE}", f"-I{NVN}", f"-I{NVN / 'include'}", "-c", str(out / "bridge_nvn.c"),
                        "-o", str(out / "bridge_nvn.o")], check=True)


if __name__ == "__main__":
    unittest.main()
