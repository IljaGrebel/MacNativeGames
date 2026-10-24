#!/usr/bin/env python3
"""Prepare the player's own Tears of the Kingdom 1.0.0 dump for totk-win (games/totk/win).

Runs the upstream extraction steps the Mac build runs (games/totk/Makefile, base game 1.0.0), in games/totk:

  1. ExeFS        scripts/extract_exefs.py            -> extracted/v0/exefs, extracted/v0/extraction-manifest.json
  2. RomFS        scripts/extract_romfs.py            -> extracted/v0/romfs/romfs.bin + index.json (about 17 GB)
  3. RomFS index  scripts/prepare_romfs_backend.py    -> extracted/v0/romfs/runtime-index.bin, build/romfs_config.h
  4. fonts        scripts/extract_shared_fonts.py     -> extracted/firmware/shared-fonts
                  scripts/prepare_shared_fonts.py     -> build/shared_fonts_config.h
  5. modules      win/tools/nso_manifest.py           -> extracted/v0/nxm/{main,subsdk0,sdk}.nxm

Every step is skipped when its output exists. Keys stay in memory (the upstream scripts never print them). Nothing it
writes is distributed: it is made from the player's files for this machine.

usage: prepare_game.py GAME.xci|GAME.nsp --keys prod.keys [--firmware DIR] [--skip-romfs]
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

TOTK = Path(__file__).resolve().parents[2]


def run(*args: str | Path) -> None:
    cmd = [sys.executable, *map(str, args)]
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=TOTK, check=True)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("game", type=Path, help="base game 1.0.0 (.xci or .nsp), dumped from your own console")
    ap.add_argument("--keys", type=Path, required=True, help="prod.keys dumped from your own console")
    ap.add_argument("--firmware", type=Path, help="unpacked firmware NCA directory (for an NSP: the shared fonts)")
    ap.add_argument("--skip-romfs", action="store_true", help="modules only (the game stops where it first reads RomFS)")
    a = ap.parse_args(argv)
    game, keys = a.game.resolve(), a.keys.resolve()
    if not game.is_file() or not keys.is_file():
        print("error: the game file and prod.keys must exist", file=sys.stderr)
        return 2
    v0 = TOTK / "extracted/v0"
    if not (v0 / "exefs/main").exists():
        run("scripts/extract_exefs.py", game, "--keys", keys, "--output", v0 / "exefs")
    if not a.skip_romfs:
        if not (v0 / "romfs/index.json").exists():
            run("scripts/extract_romfs.py", "--keys", keys, "--manifest", v0 / "extraction-manifest.json",
                "--output", v0 / "romfs")
        if not (TOTK / "build/romfs_config.h").exists():
            run("scripts/prepare_romfs_backend.py", "--version", "v0")
        fonts = TOTK / "extracted/firmware/shared-fonts"
        if not (fonts / "manifest.json").exists():
            source = ["--firmware", a.firmware.resolve()] if a.firmware else ["--xci", game]
            run("scripts/extract_shared_fonts.py", "--keys", keys, *source, "--output", fonts)
        if not (TOTK / "build/shared_fonts_config.h").exists():
            run("scripts/prepare_shared_fonts.py")
    nxm = v0 / "nxm"
    if not (nxm / "sdk.nxm").exists():
        run("win/tools/nso_manifest.py", nxm, v0 / "exefs/main", v0 / "exefs/subsdk0", v0 / "exefs/sdk")
    print(f"\nready: modules in {nxm}" + ("" if a.skip_romfs else f", RomFS in {v0 / 'romfs'}, configs in {TOTK / 'build'}"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
