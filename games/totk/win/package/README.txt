totk-win - Tears of the Kingdom on Windows, milestone M0 (boot without graphics)
================================================================================

What it does now: prepares your own dump and boots the game's code, recompiled for your CPU, without a window. The goal
of M0 is a log that shows how far the boot gets (its first NVN graphics calls). There is no picture or sound yet.

You need
  - Tears of the Kingdom, base game 1.0.0 (XCI or NSP, no update), dumped from your own console
  - prod.keys from your own console
  - about 40 GB free on the drive this folder is on, 16 GB RAM, Windows 10 or 11 (64-bit)

Run
  1. Unpack the whole zip to a folder on a drive with enough space (a short path without special characters is best).
  2. Double-click First-Run.cmd. It asks for the game file and prod.keys.
     Or in PowerShell:  .\First-Run.ps1 -Game D:\Switch\TotK.xci -Keys D:\Switch\prod.keys
     An NSP has no system fonts inside; add -Firmware <unpacked firmware folder>, or leave it out for M0.
  3. Wait. Extracting the RomFS (about 17 GB) takes a few minutes, the first recompile of the game's code 10-40.
     Later starts reuse both.
  4. Send back first-run.log from this folder. It holds no keys and no game data, but have a look before sending.

Nothing is installed. Everything the run writes stays in this folder (prep\games\totk\extracted, data\). Delete the
folder to remove it all.

Windows SmartScreen may warn about totk-win.exe: it is not signed. "More info" -> "Run anyway".

Contents
  bin\totk-win.exe     the runtime: the Mac port's runtime and ScarletNX's recompiler (LLVM inside)
  python\              portable CPython 3.12 with cryptography and lz4, for the extraction scripts
  prep\                the extraction scripts (from MacNativeGames)
  licenses\            licences; SOURCES.txt names the exact source of every part
