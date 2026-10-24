#!/usr/bin/env python3
"""Build synthetic Horizon modules (NSO) for the Windows loader tests: no game content, only code written here.

Each module is a position-independent AArch64 shared object linked with module.ld (text / rodata / data at page
offsets, MOD0 in crt0.S) and converted to an NSO with LZ4-compressed segments and their SHA-256 digests, the way the
Switch toolchain lays modules out. Needs clang and ld.lld that can target aarch64, and the lz4 Python package.

usage: make_test_nso.py OUT_DIR [--clang clang] [--lld ld.lld]
writes OUT_DIR/sdk.nso and OUT_DIR/main.nso
"""
import argparse, hashlib, struct, subprocess, sys
from pathlib import Path

import lz4.block

HERE = Path(__file__).resolve().parent
CFLAGS = ['--target=aarch64-linux-gnu', '-mcpu=cortex-a57', '-ffreestanding', '-fPIC', '-O2',
          '-fno-stack-protector', '-funwind-tables', '-fasynchronous-unwind-tables']


def build_elf(name, clang, lld, out, relr):
    objs = []
    for src in ('crt0.S', f'{name}.c'):
        obj = out / f'{name}.{Path(src).stem}.o'
        subprocess.run([clang, *CFLAGS, '-c', str(HERE / src), '-o', str(obj)], check=True)
        objs.append(str(obj))
    elf = out / f'{name}.elf'
    args = [lld, '-shared', '-nostdlib', '--eh-frame-hdr', '--hash-style=sysv', '-z', 'notext',
            '-z', 'max-page-size=0x1000', '-T', str(HERE / 'module.ld'), '--unresolved-symbols=ignore-all',
            '-soname', name, *(['-z', 'pack-relative-relocs'] if relr else []), *objs, '-o', str(elf)]
    subprocess.run(args, check=True)
    return elf


def load_segments(elf: bytes):
    """The three PT_LOAD segments as (vaddr, data, memsz), and section headers by name."""
    assert elf[:4] == b'\x7fELF' and elf[4] == 2 and elf[5] == 1
    phoff, shoff = struct.unpack_from('<QQ', elf, 0x20)
    phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from('<HHHHH', elf, 0x36)
    loads = []
    for i in range(phnum):
        ptype, flags, off, vaddr, _, filesz, memsz, _ = struct.unpack_from('<IIQQQQQQ', elf, phoff + i * phentsize)
        if ptype == 1:
            loads.append((vaddr, elf[off:off + filesz], memsz))
    assert len(loads) == 3, f'expected 3 PT_LOAD segments, got {len(loads)}'
    sections = {}
    stroff = struct.unpack_from('<Q', elf, shoff + shstrndx * shentsize + 0x18)[0]
    for i in range(shnum):
        nm, _, _, addr, _, size = struct.unpack_from('<IIQQQQ', elf, shoff + i * shentsize)
        name = elf[stroff + nm:elf.index(b'\0', stroff + nm)].decode()
        sections[name] = (addr, size)
    return loads, sections


def to_nso(elf_path: Path, nso_path: Path):
    elf = elf_path.read_bytes()
    loads, sections = load_segments(elf)
    (tx, tdata, _), (rx, rdata, _), (dx, ddata, dmem) = loads
    assert tx == 0 and rx % 0x1000 == 0 and dx % 0x1000 == 0
    segs = [(tx, tdata), (rx, rdata), (dx, ddata)]
    payloads = [lz4.block.compress(d, store_size=False) for _, d in segs]
    header = bytearray(0x100)
    struct.pack_into('<4sIII', header, 0, b'NSO0', 0, 0, 0b111111)       # compressed + hash-checked segments
    off = 0x100
    for i, ((addr, data), payload) in enumerate(zip(segs, payloads)):
        extra = (dmem - len(ddata)) if i == 2 else 0                        # bss size for the data segment
        struct.pack_into('<IIII', header, 0x10 + 16 * i, off, addr, len(data), extra)
        struct.pack_into('<I', header, 0x60 + 4 * i, len(payload))
        header[0xA0 + 32 * i:0xC0 + 32 * i] = hashlib.sha256(data).digest()
        off += len(payload)
    header[0x40:0x60] = hashlib.sha256(elf).digest()                       # module (build) id
    for slot, sect in ((0x88, None), (0x90, '.dynstr'), (0x98, '.dynsym')):  # api info, dynstr, dynsym: rodata-relative
        if sect:
            a, s = sections[sect]
            struct.pack_into('<II', header, slot, a - rx, s)
    nso_path.write_bytes(bytes(header) + b''.join(payloads))


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('out', type=Path)
    ap.add_argument('--clang', default='clang')
    ap.add_argument('--lld', default='ld.lld')
    a = ap.parse_args(argv)
    a.out.mkdir(parents=True, exist_ok=True)
    # sdk packs its RELATIVE relocations as DT_RELR, as the 1.4.3 SDK modules do; main keeps RELA
    for name, relr in (('sdk', True), ('main', False)):
        to_nso(build_elf(name, a.clang, a.lld, a.out, relr), a.out / f'{name}.nso')
    return 0


if __name__ == '__main__':
    sys.exit(main())
