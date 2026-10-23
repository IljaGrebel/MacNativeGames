# recomp_e2e: recompiler + bridge, end to end

ARM64 code (`guest.c`, built for Cortex-A57 without LSE, as the Switch runs) is recompiled to x86-64 by ScarletNX's
recompiler library and run in this process, with guest addresses equal to host addresses (`cpu.mem = NULL`). It
checks, against the same C compiled natively:

- **calls**: 2,000,000 host calls through a generated bridge adapter (`int, float, 16-byte struct, long`), an
  `LDAXR`/`STLXR` atomic and a `DMB ISH` per call;
- **fp**: an auto-vectorised NEON loop with multiply, add and divide;
- **int**: an integer hash loop over an array.

```sh
./run.sh /path/to/scarletnx-recompiler-install            # exact FP (ScarletNX's default)
SNX_FP_RELAXED=1 ./run.sh /path/to/...                     # needs the relaxed-FP experiment, see below
```

## Results (2026-10-24, ScarletNX 5889bb1, LLVM 20.1.2, cloud Xeon 2.1 GHz, x86-64-v2 reference)

| Test | Result | Recompiled vs native |
| --- | --- | --- |
| calls | identical | 33-36 ns vs 25-32 ns per call: the bridge adds about 3-8 ns |
| int | identical | 1.04-1.12x |
| fp, exact FP | identical | **5.5-7.0x slower** |
| fp, relaxed FP (experiment) | identical, bit for bit | 0.9-1.2x |

The FP cost comes from ScarletNX emulating ARM floating point exactly: every operation checks each operand's class,
tracks FPSR.IXC (a TwoSum or FMA residual until the flag is sticky) and falls back to soft-float for denormals, NaNs
and infinities. Games read FPSR almost never. The relaxed experiment makes `ir_fp_fast` (ir_fp.c) declare the fast
path always valid and IXC never raised, behind an environment variable; it is a local change to the external
recompiler and is not in this repository. Before it can be a default it needs: FPCR.FZ / default-NaN mapped to
MXCSR, a check of NaN propagation where games compare results, and the differential test on real game code.

Not covered here: a real game module, a Windows build of the library, AMD or Intel handheld CPUs.
