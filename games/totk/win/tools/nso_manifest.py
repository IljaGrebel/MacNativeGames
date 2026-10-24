#!/usr/bin/env python3
"""Turn Horizon modules (NSO) into load manifests for the Windows port's module loader.

The Mac port rewrites the modules' ARM64 code and links them into the app; the Windows port recompiles them instead,
so it needs each module as it lies in memory plus what a dynamic linker needs to place it: segment layout, every
relocation (RELA, JMPREL and RELR), the dynamic symbols, the init array and the known function starts (the hints that
seed the recompiler's function discovery).

One file per module, `<name>.nxm`, little-endian:

    header (NXM_HEADER, 128 bytes)
    relocations   nreloc x {u64 where; u32 type; u32 symbol; i64 addend}        symbol 0xFFFFFFFF = none
    symbols       nsym   x {u32 name; u32 flags; u64 value}                      flags: 1 defined, 2 weak, 4 function
    hints         nhint  x u64                                                    offsets of function starts
    strings       strtab_size bytes (NUL-terminated names)
    image         image_size bytes, at image_offset (16-aligned): text, rodata and data, without bss

All offsets in the tables are module-relative. A summary of every module goes to manifest.json for people and tests.
Runs on the player's own files at build time; nothing it writes is distributed.

usage: nso_manifest.py OUT_DIR main.nso [sdk.nso subsdk0.nso ...]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

import lz4.block

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'third_party' / 'd2r'))
from relr import decode_relr  # noqa: E402  (MIT, from d2r-mobile, as the Mac loader uses it)

MAGIC = b'NXM1'
NXM_HEADER = struct.Struct('<4sIIIQQQQQQQQQQIIIIQ')   # see write_module for the field order
assert NXM_HEADER.size <= 128
HEADER_SIZE = 128
RELOC = struct.Struct('<QIIq')
SYMBOL = struct.Struct('<IIQ')
NO_SYMBOL = 0xFFFFFFFF

R_AARCH64_ABS64, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT, R_AARCH64_RELATIVE = 257, 1025, 1026, 1027
SUPPORTED_RELOCS = {R_AARCH64_ABS64, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT, R_AARCH64_RELATIVE}
DT = {'NEEDED': 1, 'PLTRELSZ': 2, 'HASH': 4, 'STRTAB': 5, 'SYMTAB': 6, 'RELA': 7, 'RELASZ': 8, 'STRSZ': 10,
      'SYMENT': 11, 'INIT': 12, 'FINI': 13, 'JMPREL': 23, 'INIT_ARRAY': 25, 'FINI_ARRAY': 26, 'INIT_ARRAYSZ': 27,
      'FINI_ARRAYSZ': 28, 'GNU_HASH': 0x6FFFFEF5}


class ManifestError(ValueError):
    pass


def check(ok: bool, message: str) -> None:
    if not ok:
        raise ManifestError(message)


def read_nso(raw: bytes, name: str):
    """(image bytes without bss, [(seg addr, size)] x3, bss size, build id, header dynsym (offset, size) or None)."""
    check(raw[:4] == b'NSO0' and len(raw) >= 0x100, f'{name}: not an NSO')
    flags = struct.unpack_from('<I', raw, 12)[0]
    sizes = struct.unpack_from('<III', raw, 0x60)
    segs, datas = [], []
    for i in range(3):
        fileoff, addr, size, extra = struct.unpack_from('<IIII', raw, 0x10 + 16 * i)
        stored = sizes[i] if flags & (1 << i) else size
        check(fileoff + stored <= len(raw) and size <= 1 << 30, f'{name}: segment {i} outside the file')
        payload = raw[fileoff:fileoff + stored]
        data = lz4.block.decompress(payload, uncompressed_size=size) if flags & (1 << i) else payload
        check(len(data) == size, f'{name}: segment {i} size')
        if flags & (8 << i):
            check(hashlib.sha256(data).digest() == raw[0xA0 + 32 * i:0xC0 + 32 * i], f'{name}: segment {i} hash')
        segs.append((addr, size))
        datas.append(data)
    check(segs[0][0] == 0 and segs[0][0] + segs[0][1] <= segs[1][0] and segs[1][0] + segs[1][1] <= segs[2][0],
          f'{name}: segments out of order')
    bss = struct.unpack_from('<I', raw, 0x3C)[0]
    image = bytearray(segs[2][0] + segs[2][1])
    for (addr, size), data in zip(segs, datas):
        image[addr:addr + size] = data
    dynsym = struct.unpack_from('<II', raw, 0x98)
    return bytes(image), segs, bss, raw[0x40:0x60], (segs[1][0] + dynsym[0], dynsym[1]) if dynsym[1] else None


def parse_module(raw: bytes, name: str) -> dict:
    image, segs, bss, build_id, header_dynsym = read_nso(raw, name)

    def u32(o):
        check(0 <= o <= len(image) - 4, f'{name}: read outside image at {o:#x}')
        return struct.unpack_from('<I', image, o)[0]

    mod0 = u32(4)
    check(image[mod0:mod0 + 4] == b'MOD0', f'{name}: no MOD0')
    dyn = mod0 + struct.unpack_from('<i', image, mod0 + 4)[0]
    tags: dict[int, int] = {}
    needed = []
    for i in range(8192):
        check(dyn + 16 * i + 16 <= len(image), f'{name}: dynamic table outside image')
        tag, val = struct.unpack_from('<qQ', image, dyn + 16 * i)
        if tag == 0:
            break
        if tag == DT['NEEDED']:
            needed.append(val)
        tags.setdefault(tag, val)
    else:
        raise ManifestError(f'{name}: unterminated dynamic table')

    strtab, strsz = tags.get(DT['STRTAB'], 0), tags.get(DT['STRSZ'], 0)
    check(strtab + strsz <= len(image), f'{name}: dynstr outside image')

    def cstr(off):
        check(off < strsz, f'{name}: bad string offset')
        end = image.index(b'\0', strtab + off)
        return image[strtab + off:end].decode('utf-8', 'replace')

    symtab = tags.get(DT['SYMTAB'], 0)
    check(tags.get(DT['SYMENT'], 24) == 24, f'{name}: unexpected symbol size')
    if DT['HASH'] in tags:
        nsym = struct.unpack_from('<II', image, tags[DT['HASH']])[1]
    elif header_dynsym:
        check(header_dynsym[0] == symtab and header_dynsym[1] % 24 == 0, f'{name}: header dynsym disagrees')
        nsym = header_dynsym[1] // 24
    else:
        raise ManifestError(f'{name}: cannot count the dynamic symbols')
    symbols = []
    for i in range(nsym):
        nm, info, _other, shndx, value, _size = struct.unpack_from('<IBBHQQ', image, symtab + 24 * i)
        symbols.append({'name': cstr(nm) if nm else '', 'defined': shndx != 0, 'weak': info >> 4 == 2,
                        'function': info & 15 == 2, 'value': value})

    relocs = []
    for where, addend in decode_relr(image, tags, segs[2][0], len(image)):
        relocs.append((where, R_AARCH64_RELATIVE, NO_SYMBOL, addend))
    for table, size in ((tags.get(DT['RELA']), tags.get(DT['RELASZ'], 0)),
                        (tags.get(DT['JMPREL']), tags.get(DT['PLTRELSZ'], 0))):
        if not table:
            continue
        check(table + size <= len(image) and size % 24 == 0, f'{name}: relocation table outside image')
        for i in range(size // 24):
            where, info, addend = struct.unpack_from('<QQq', image, table + 24 * i)
            rtype, sym = info & 0xFFFFFFFF, info >> 32
            check(rtype in SUPPORTED_RELOCS, f'{name}: relocation type {rtype} at {where:#x} is not supported')
            check(sym < nsym, f'{name}: relocation symbol {sym} out of range')
            check(segs[2][0] <= where and where + 8 <= len(image) + bss,
                  f'{name}: relocation at {where:#x} outside the data segment')
            relocs.append((where, rtype, sym if sym else NO_SYMBOL, addend))

    hints = set(func_starts(image, mod0, segs[0][1]))
    hints.update(s['value'] for s in symbols if s['defined'] and s['function'] and s['value'] < segs[0][1])
    hints.add(0)
    if DT['INIT'] in tags:
        hints.add(tags[DT['INIT']])
    init_array = (tags.get(DT['INIT_ARRAY'], 0), tags.get(DT['INIT_ARRAYSZ'], 0) // 8)
    check(init_array[0] + 8 * init_array[1] <= len(image), f'{name}: init array outside image')

    return {'name': name, 'image': image, 'segments': segs, 'bss': bss, 'build_id': build_id.hex(),
            'relocs': relocs, 'symbols': symbols, 'hints': sorted(h for h in hints if h % 4 == 0),
            'init_array': init_array, 'needed': [cstr(n) for n in needed]}


def func_starts(image: bytes, mod0: int, text_end: int) -> list[int]:
    """Function starts from .eh_frame_hdr's search table (MOD0 points at it), as nsoimg.func_starts reads them."""
    start, end = (mod0 + struct.unpack_from('<i', image, mod0 + o)[0] for o in (16, 20))
    if end <= start + 4:
        return []
    version, penc, cenc, tenc = image[start:start + 4]
    if version != 1 or tenc == 0xFF:
        return []

    def rd(o, enc):
        fmt, app = enc & 0x0F, enc & 0x70
        fmts = {0x03: '<I', 0x0B: '<i', 0x04: '<Q', 0x0C: '<q'}
        check(fmt in fmts, f'eh_frame_hdr encoding {enc:#x} not supported')
        v = struct.unpack_from(fmts[fmt], image, o)[0]
        size = struct.calcsize(fmts[fmt])
        if app == 0x10:
            v += o
        elif app == 0x30:
            v += start
        return v, size

    o = start + 4
    _, s = rd(o, penc); o += s
    count, s = rd(o, cenc); o += s
    out = []
    for _ in range(count):
        a, s = rd(o, tenc); o += s
        _, s = rd(o, tenc); o += s
        if 0 <= a < text_end:
            out.append(a)
    return out


def write_module(m: dict, path: Path) -> dict:
    strings = bytearray(b'\0')
    offsets: dict[str, int] = {}

    def intern(s: str) -> int:
        if not s:
            return 0
        if s not in offsets:
            offsets[s] = len(strings)
            strings.extend(s.encode() + b'\0')
        return offsets[s]

    name_off = intern(m['name'])
    sym_blob = b''.join(SYMBOL.pack(intern(s['name']), (s['defined'] << 0) | (s['weak'] << 1) | (s['function'] << 2),
                                    s['value']) for s in m['symbols'])
    rel_blob = b''.join(RELOC.pack(*r) for r in m['relocs'])
    hint_blob = b''.join(struct.pack('<Q', h) for h in m['hints'])
    tables = HEADER_SIZE + len(rel_blob) + len(sym_blob) + len(hint_blob) + len(strings)
    image_offset = (tables + 15) & ~15
    (t, ts), (r, rs), (d, ds) = m['segments']
    header = NXM_HEADER.pack(MAGIC, 1, name_off, 0, len(m['image']), m['bss'], t, ts, r, rs, d, ds,
                             m['init_array'][0], m['init_array'][1], len(m['relocs']), len(m['symbols']),
                             len(m['hints']), len(strings), image_offset)
    blob = (header.ljust(HEADER_SIZE, b'\0') + rel_blob + sym_blob + hint_blob + bytes(strings)).ljust(image_offset, b'\0')
    path.write_bytes(blob + m['image'])
    by_type: dict[int, int] = {}
    for r_ in m['relocs']:
        by_type[r_[1]] = by_type.get(r_[1], 0) + 1
    return {'name': m['name'], 'file': path.name, 'image_size': len(m['image']), 'bss': m['bss'],
            'segments': [list(s) for s in m['segments']], 'build_id': m['build_id'],
            'relocs': {str(k): v for k, v in sorted(by_type.items())}, 'symbols': len(m['symbols']),
            'imports': sorted(s['name'] for s in m['symbols'] if s['name'] and not s['defined']),
            'exports': len([s for s in m['symbols'] if s['name'] and s['defined']]),
            'hints': len(m['hints']), 'init_array': list(m['init_array']), 'needed': m['needed'],
            'sha256': hashlib.sha256(m['image']).hexdigest()}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('out', type=Path)
    ap.add_argument('modules', nargs='+', type=Path, help='NSO files, main first, in load order')
    a = ap.parse_args(argv)
    a.out.mkdir(parents=True, exist_ok=True)
    summary = []
    for path in a.modules:
        name = path.stem
        try:
            m = parse_module(path.read_bytes(), name)
        except ManifestError as e:
            print(f'error: {e}', file=sys.stderr)
            return 1
        summary.append(write_module(m, a.out / f'{name}.nxm'))
    (a.out / 'manifest.json').write_text(json.dumps({'format': 1, 'modules': summary}, indent=1) + '\n')
    for s in summary:
        print(f"{s['name']}: {s['image_size']:#x} bytes + bss {s['bss']:#x}, {sum(s['relocs'].values())} relocs, "
              f"{s['symbols']} symbols, {len(s['imports'])} imports, {s['hints']} hints")
    return 0


if __name__ == '__main__':
    sys.exit(main())
