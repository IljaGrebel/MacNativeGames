"""Tests for the Windows port's module loading (games/totk/win): synthetic Horizon modules built from C written for the
tests (no game content), turned into load manifests, loaded and linked, and - when the recompiler is installed - run.

Layers, each skipped when its tools are missing:
  * nso_manifest.py on the synthetic NSOs (needs clang and ld.lld that target aarch64, and lz4);
  * platform_test: guest memory and the loader in C, on the host, and as a Windows program under Wine;
  * loader_test: the modules recompiled to x86-64 and run (needs ScarletNX's recompiler library: set
    SNX_RECOMPILER_PREFIX to its install prefix).
"""
from __future__ import annotations

import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WIN = ROOT / "games/totk/win"
FIXTURES = WIN / "tests/fixtures"


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


def _tool(*names: str) -> str | None:
    for n in names:
        p = shutil.which(n)
        if p:
            return p
    return None


def _aarch64_tools() -> tuple[str, str] | None:
    clang = _tool("clang-20", "clang-19", "clang-18", "clang")
    lld = _tool("ld.lld", "ld.lld-20", "ld.lld-19", "ld.lld-18")
    if not clang or not lld:
        return None
    probe = subprocess.run([clang, "--target=aarch64-linux-gnu", "-ffreestanding", "-x", "c", "-c", "-o", os.devnull, "-"],
                           input="int f(void){return 0;}", capture_output=True, text=True)
    return (clang, lld) if probe.returncode == 0 else None


class _Fixtures(unittest.TestCase):
    """Builds the synthetic modules and their manifests once for the module."""
    tmp: Path

    @classmethod
    def setUpClass(cls):
        try:
            import lz4.block  # noqa: F401
        except ImportError:
            raise unittest.SkipTest("lz4 not installed")
        tools = _aarch64_tools()
        if not tools:
            raise unittest.SkipTest("no clang/ld.lld for aarch64")
        cls.tmp = Path(tempfile.mkdtemp())
        mk = _load("make_test_nso", FIXTURES / "make_test_nso.py")
        mk.main([str(cls.tmp / "nso"), "--clang", tools[0], "--lld", tools[1]])
        cls.manifest = _load("nso_manifest", WIN / "tools/nso_manifest.py")
        rc = cls.manifest.main([str(cls.tmp / "nxm"), str(cls.tmp / "nso/main.nso"), str(cls.tmp / "nso/sdk.nso")])
        if rc:
            raise RuntimeError("nso_manifest failed")
        cls.summary = {m["name"]: m for m in json.loads((cls.tmp / "nxm/manifest.json").read_text())["modules"]}

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)


class ManifestTests(_Fixtures):
    def test_relocations_and_imports(self):
        main, sdk = self.summary["main"], self.summary["sdk"]
        self.assertEqual(main["relocs"], {"257": 1, "1025": 2, "1026": 2, "1027": 3})
        self.assertEqual(main["imports"], ["host_add", "sdk_mul", "sdk_ready"])
        self.assertEqual(sdk["relocs"], {"1025": 1, "1027": 1})   # the RELATIVE one came from DT_RELR
        self.assertEqual(sdk["imports"], [])
        self.assertEqual(main["init_array"][1], 1)
        self.assertEqual(sdk["init_array"][1], 1)

    def test_hints_include_eh_frame_starts(self):
        m = self.manifest.parse_module((self.tmp / "nso/main.nso").read_bytes(), "main")
        self.assertGreaterEqual(len(m["hints"]), 4)
        self.assertTrue(all(h % 4 == 0 and h < m["segments"][0][1] for h in m["hints"]))
        entry = next(s["value"] for s in m["symbols"] if s["name"] == "test_entry")
        self.assertIn(entry, m["hints"])

    def test_damaged_nso_is_refused(self):
        raw = bytearray((self.tmp / "nso/main.nso").read_bytes())
        raw[0xA0] ^= 1                                    # text segment digest
        with self.assertRaises(self.manifest.ManifestError):
            self.manifest.parse_module(bytes(raw), "main")
        with self.assertRaises(self.manifest.ManifestError):
            self.manifest.parse_module(b"NSO1" + bytes(raw[4:]), "main")


class PlatformTests(_Fixtures):
    def _build(self, cc: list[str], out: Path):
        src = [WIN / "runtime/guest_mem.c", WIN / "runtime/module_loader.c", WIN / "tests/platform_test.c"]
        subprocess.run([*cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", f"-I{WIN / 'runtime'}",
                        f"-I{WIN / 'bridge'}", *map(str, src), "-o", str(out)], check=True)

    def _nxm(self, name: str) -> str:
        return str(self.tmp / "nxm" / f"{name}.nxm")

    def test_host(self):
        cc = _tool("cc", "clang", "gcc")
        if not cc:
            self.skipTest("no C compiler")
        exe = self.tmp / "platform_test"
        self._build([cc], exe)
        r = subprocess.run([str(exe), self._nxm("main"), self._nxm("sdk")], capture_output=True, text=True, cwd=self.tmp)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_windows_under_wine(self):
        mingw = _tool("x86_64-w64-mingw32-gcc")
        wine = _tool("wine64", "wine") or ("/usr/lib/wine/wine64" if Path("/usr/lib/wine/wine64").exists() else None)
        if not mingw or not wine:
            self.skipTest("no MinGW or Wine")
        exe = self.tmp / "platform_test.exe"
        self._build([mingw], exe)
        env = dict(os.environ, WINEDEBUG="-all")
        r = subprocess.run([wine, str(exe), "Z:" + self._nxm("main").replace("/", "\\"),
                            "Z:" + self._nxm("sdk").replace("/", "\\")], capture_output=True, text=True, env=env,
                           cwd=self.tmp, timeout=300)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("all passed", r.stdout)
        self.assertIn("16 x 0x10000", r.stdout)   # Windows' 64 KiB reservation granularity was in effect


TOTK = ROOT / "games/totk"
COMPAT = ["-include", str(WIN / "compat/win_compat.h"), f"-I{WIN / 'compat/include'}",
          f"-I{TOTK / 'third_party/d2r_gfx/gfx/nvn/include'}", f"-I{TOTK / 'runtime'}", f"-I{WIN / 'runtime'}"]
PROBE_SOURCES = [TOTK / "scripts/thread_probe.c", TOTK / "runtime/threads.c", WIN / "runtime/thread_enter.c",
                 WIN / "compat/compat.c", WIN / "runtime/guest_mem.c"]


class UpstreamThreadProbeTests(unittest.TestCase):
    """The upstream runtime's own thread test (scripts/thread_probe.c) on the unchanged runtime/threads.c, with this
    port's compatibility layer and thread entry: 32 threads with events, waits, priorities and ExitThread."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def test_host(self):
        cc = _tool("clang-20", "clang", "cc")
        if not cc:
            self.skipTest("no C compiler")
        exe = self.tmp / "thread_probe"
        subprocess.run([cc, "-std=c11", "-O1", "-Wno-format", *COMPAT, *map(str, PROBE_SOURCES), "-lpthread", "-o",
                        str(exe)], check=True, capture_output=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr[-2000:])
        self.assertIn('"status":"pass"', r.stdout)
        self.assertIn('"native_threads":32', r.stdout)

    def test_windows_under_wine(self):
        mingw = _tool("x86_64-w64-mingw32-gcc")
        wine = _tool("wine64", "wine") or ("/usr/lib/wine/wine64" if Path("/usr/lib/wine/wine64").exists() else None)
        if not mingw or not wine:
            self.skipTest("no MinGW or Wine")
        exe = self.tmp / "thread_probe.exe"
        subprocess.run([mingw, "-std=c11", "-O1", *COMPAT, f"-I{WIN / 'compat/include_win'}", *map(str, PROBE_SOURCES),
                        str(WIN / "compat/compat_win.c"), "-lbcrypt", "-static", "-lpthread", "-o", str(exe)],
                       check=True, capture_output=True)
        r = subprocess.run([wine, str(exe)], capture_output=True, text=True, env=dict(os.environ, WINEDEBUG="-all"),
                           cwd=self.tmp, timeout=300)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr[-2000:])
        self.assertIn('"native_threads":32', r.stdout)


class RecompiledTests(_Fixtures):
    @classmethod
    def setUpClass(cls):
        prefix = os.environ.get("SNX_RECOMPILER_PREFIX")
        if not prefix:
            raise unittest.SkipTest("SNX_RECOMPILER_PREFIX not set (ScarletNX recompiler library)")
        super().setUpClass()
        cc = _tool("clang-20", "clang")
        build = cls.tmp / "build"
        subprocess.run(["cmake", "-S", str(WIN), "-B", str(build), f"-DCMAKE_C_COMPILER={cc}",
                        f"-DCMAKE_PREFIX_PATH={prefix}", "-DCMAKE_BUILD_TYPE=Release"], check=True, capture_output=True)
        subprocess.run(["cmake", "--build", str(build), "--target", "loader_test"], check=True, capture_output=True)
        cls.exe = build / "loader_test"

    def _run(self, *extra):
        return subprocess.run([str(self.exe), str(self.tmp / "nxm/main.nxm"), str(self.tmp / "nxm/sdk.nxm"), *extra],
                              capture_output=True, text=True, timeout=600)

    def test_modules_run_recompiled(self):
        r = self._run()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("all passed", r.stdout)

    def test_unresolved_import_stops_with_its_name(self):
        r = self._run("--no-host")
        self.assertEqual(r.returncode, 3, r.stdout + r.stderr)
        self.assertIn("unresolved import called: host_add", r.stdout)


class GameConfigTests(unittest.TestCase):
    """The prebuilt program reads romfs_config.h / shared_fonts_config.h at start (runtime/game_config.c), as the
    preparation scripts write them, including Windows paths with backslashes."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)
        cc = shutil.which("clang") or shutil.which("cc")
        self.exe = self.tmp / "game_config_test"
        subprocess.run([cc, "-std=c11", "-O1", str(WIN / "tests/game_config_test.c"), str(WIN / "runtime/game_config.c"),
                        "-o", str(self.exe)], check=True)

    def run_with(self, romfs, fonts):
        cfg = self.tmp / "cfg"
        cfg.mkdir(exist_ok=True)
        if romfs is not None: (cfg / "romfs_config.h").write_text(romfs)
        if fonts is not None: (cfg / "shared_fonts_config.h").write_text(fonts)
        return subprocess.run([str(self.exe)], capture_output=True, text=True, env={**os.environ, "TOTK_WIN_CONFIG_DIR": str(cfg)})

    def test_reads_the_scripts_headers(self):
        a, b = "a" * 64, "0123456789abcdef" * 4
        romfs = f'#define ROMFS_IMAGE_SIZE 17179869184ULL\n#define ROMFS_IMAGE_SHA256 "{a}"\n#define ROMFS_INDEX_SHA256 "{b}"\n'
        rows = ",".join(f'{{"C:\\Users\\x\\fonts\\{i}-f.bfttf",{100 + i}ULL,{92 + i}ULL,"{b}"}}' for i in range(6))
        r = self.run_with(romfs, "#define SHARED_FONT_COUNT 6\r\n#define SHARED_FONT_ROWS " + rows + "\r\n")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("size 17179869184", r.stdout)
        self.assertIn(f"image {a}", r.stdout)
        self.assertIn(f"index {b}", r.stdout)
        self.assertIn(f"font C:\\Users\\x\\fonts\\5-f.bfttf 105 97 {b}", r.stdout)

    def test_a_missing_value_stops_with_its_name(self):
        r = self.run_with('#define ROMFS_IMAGE_SIZE 4096ULL\n#define ROMFS_IMAGE_SHA256 "short"\n', None)
        self.assertEqual(r.returncode, 3)
        self.assertIn("ROMFS_IMAGE_SHA256", r.stderr)
        (self.tmp / "cfg/romfs_config.h").unlink()
        r = self.run_with(None, None)
        self.assertIn("romfs_config.h", r.stderr)


class BootTests(unittest.TestCase):
    """totk-win (the upstream runtime on the recompiler) boots synthetic modules that walk the real SDK's startup:
    nn::init::Start with the host's callbacks, module constructors, Horizon SVCs through the upstream dispatcher, a
    guest thread through threads.c, a host replacement and the NVN entry. Needs SNX_RECOMPILER_PREFIX."""

    @classmethod
    def setUpClass(cls):
        prefix = os.environ.get("SNX_RECOMPILER_PREFIX")
        if not prefix:
            raise unittest.SkipTest("SNX_RECOMPILER_PREFIX not set (ScarletNX recompiler library)")
        tools = _aarch64_tools()
        if not tools:
            raise unittest.SkipTest("no clang/ld.lld for aarch64")
        cls.tmp = Path(tempfile.mkdtemp())
        _load("make_test_nso", FIXTURES / "make_test_nso.py").main(
            [str(cls.tmp / "nso"), "--clang", tools[0], "--lld", tools[1], "--set", "boot"])
        if _load("nso_manifest", WIN / "tools/nso_manifest.py").main(
                [str(cls.tmp / "nxm"), str(cls.tmp / "nso/main.nso"), str(cls.tmp / "nso/sdk.nso")]):
            raise RuntimeError("nso_manifest failed")
        build = cls.tmp / "build"
        subprocess.run(["cmake", "-S", str(WIN), "-B", str(build), f"-DCMAKE_C_COMPILER={tools[0]}",
                        f"-DCMAKE_PREFIX_PATH={prefix}", "-DCMAKE_BUILD_TYPE=Release", "-DTOTK_WIN_RUNTIME_CONFIG=ON",
                        f"-DPython3_EXECUTABLE={sys.executable}"], check=True, capture_output=True)
        subprocess.run(["cmake", "--build", str(build), "--target", "totk-win"], check=True, capture_output=True)
        cls.exe = build / "totk-win"

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_boot_walks_the_sdk_startup(self):
        r = subprocess.run([str(self.exe), str(self.tmp / "nxm/main.nxm"), str(self.tmp / "nxm/sdk.nxm")],
                           capture_output=True, text=True, timeout=300, cwd=self.tmp)
        self.assertEqual(r.returncode, 0, r.stderr[-3000:])
        self.assertIn("BOOTTEST ok=7f", r.stderr)          # all seven steps
        self.assertIn("rtld callback: modules linked", r.stderr)
        self.assertIn('nvnBootstrapLoader(', r.stderr)
        self.assertIn('"nvnDeviceInitialize"', r.stderr)


if __name__ == "__main__":
    unittest.main()
