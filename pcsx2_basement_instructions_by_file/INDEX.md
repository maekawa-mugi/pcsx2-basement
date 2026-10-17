# PCSX2 Basement — File-by-File Optimization Tasks

Repository: [https://github.com/maekawa-mugi/pcsx2-basement](https://github.com/maekawa-mugi/pcsx2-basement)

Use this index to open **only the source-file task you are editing**. The exception is the related IPU AVX-512 implementation/dispatch pair, kept in a single file.

**Status:** `P1` = simple/high-priority; `P2` = experiment; `P3` = conditional/complex; `EXISTING` = already implemented, **not** a new task. All suggestions are unimplemented proposals unless marked `EXISTING`; not all previous rejection histories have been exhaustively checked.

**Validation:** Preserve CPU ISA dispatch, PS2 integer wraparound, masks/partial writes, guest flags, FP rounding/NaN/signed-zero behavior, ABI, and register aliasing. Compare generated code and benchmark by microarchitecture. Some lines and proposed instruction counts need verification; lower instruction count is not automatically faster.

## Source-file tasks

| Target source file | Task MD | Proposed | Existing | MD size |
|---|---|---:|---:|---:|
| `GSDrawScanlineCodeGenerator.all.cpp` | [01_GSDrawScanlineCodeGenerator.all.cpp.md](01_GSDrawScanlineCodeGenerator.all.cpp.md) | 16 | 2 | 6.8 KiB |
| `GSScanlineEnvironment.h` | [02_GSScanlineEnvironment.h.md](02_GSScanlineEnvironment.h.md) | 1 | 1 | 1.3 KiB |
| `GSSetupPrimCodeGenerator.all.cpp` | [03_GSSetupPrimCodeGenerator.all.cpp.md](03_GSSetupPrimCodeGenerator.all.cpp.md) | 3 | 0 | 1.5 KiB |
| `GSRasterizer.cpp` | [04_GSRasterizer.cpp.md](04_GSRasterizer.cpp.md) | 1 | 0 | 0.8 KiB |
| `GSRendererSW.cpp` | [05_GSRendererSW.cpp.md](05_GSRendererSW.cpp.md) | 1 | 1 | 1.0 KiB |
| `GSVertexTraceFMM.cpp` | [06_GSVertexTraceFMM.cpp.md](06_GSVertexTraceFMM.cpp.md) | 1 | 1 | 1.1 KiB |
| `GSBlock.h` | [07_GSBlock.h.md](07_GSBlock.h.md) | 6 | 1 | 2.8 KiB |
| `GSLocalMemoryMultiISA.cpp` | [08_GSLocalMemoryMultiISA.cpp.md](08_GSLocalMemoryMultiISA.cpp.md) | 2 | 0 | 1.0 KiB |
| `GSClut.cpp` | [09_GSClut.cpp.md](09_GSClut.cpp.md) | 1 | 0 | 0.7 KiB |
| `iR5900LoadStore.cpp` | [10_iR5900LoadStore.cpp.md](10_iR5900LoadStore.cpp.md) | 2 | 0 | 1.1 KiB |
| `iMMI.cpp` | [11_iMMI.cpp.md](11_iMMI.cpp.md) | 2 | 1 | 1.3 KiB |
| `microVU_UpperSoft.inl` | [12_microVU_UpperSoft.inl.md](12_microVU_UpperSoft.inl.md) | 5 | 1 | 2.5 KiB |
| `microVU_Upper.inl` | [13_microVU_Upper.inl.md](13_microVU_Upper.inl.md) | 1 | 1 | 1.0 KiB |
| `microVU_Lower.inl` | [14_microVU_Lower.inl.md](14_microVU_Lower.inl.md) | 5 | 0 | 2.2 KiB |
| `microVU_Misc.inl` | [15_microVU_Misc.inl.md](15_microVU_Misc.inl.md) | 2 | 0 | 1.0 KiB |
| `microVU_IR.h` | [16_microVU_IR.h.md](16_microVU_IR.h.md) | 1 | 0 | 0.7 KiB |
| `iFPU.cpp` | [17_iFPU.cpp.md](17_iFPU.cpp.md) | 3 | 0 | 1.3 KiB |
| `iFPUd.cpp` | [18_iFPUd.cpp.md](18_iFPUd.cpp.md) | 0 | 1 | 0.7 KiB |
| `SoftFloatEmitter.h` | [19_SoftFloatEmitter.h.md](19_SoftFloatEmitter.h.md) | 1 | 0 | 0.8 KiB |
| `Vif_Dynarec.cpp` | [20_Vif_Dynarec.cpp.md](20_Vif_Dynarec.cpp.md) | 1 | 0 | 0.8 KiB |
| `Vif_UnpackSSE.cpp` | [21_Vif_UnpackSSE.cpp.md](21_Vif_UnpackSSE.cpp.md) | 1 | 1 | 1.0 KiB |
| `InterpreterAVX512.cpp` + `IPU_MultiISA.cpp` | [22_InterpreterAVX512__IPU_MultiISA.md](22_InterpreterAVX512__IPU_MultiISA.md) | 0 | 2 | 1.2 KiB |

## Related tasks (load together only when needed)

- GS scanline ternary/mask constants: tasks **01** + **02**.
- Exact SoftFloat DIV/SQRT recurrence: tasks **14** + **17**; task **19** is helper-reference only.
- GS masked VRAM store optimization: task **07**, with call-site benchmarks in task **08**.
- GS static LOD specialization: task **01**, with per-draw state in task **05**.
- IPU AVX-512 kernel + feature dispatch: already combined as task **22**.

## Do not redo / unresolved directions

- **EE MMI comparisons** — Most compare results must end as a guest 128-bit vector, so converting to k-mask and back is unlikely to help unless a full producer-consumer chain stays masked.
- **microVU standard flags/MAX/MIN** — Existing k-mask specializations cover the obvious cases; do not blanket-convert vector comparisons.
- **TLB bitset (~48 entries)** — Keep the compact GPR bitset; k-mask conversion is not justified by instruction count alone.
- **GS SW per-pixel store masks** — Already AVX-512 optimized; performance may depend on hardware internal read-modify-write and sparse-byte store costs.
- **GSBlock 4HL/4HH** — Preserve the other nibble in the same byte; `0x88888888` byte stores are **not** a valid drop-in replacement.
- **All proposed `VPTERNLOGD` immediates** — Verify the operand-order truth table against Xbyak's `(dest, src1, src2, imm)` convention and test 128-/256-bit lanes separately.

## Recommended execution order

1. **Cheap correctness-safe patches:** GS06 (skip no-op ZClamp), US01 (remove redundant MADD AND), LOW01 (SQRT bitmap index), MMI01 (PCPYUD SSE), LOW02/LOW03 and FPU01/FPU02 (LEA folds).
2. **Short JIT instruction reductions:** GS01–GS13, SET01, LOW04; group each by CPU ISA and benchmark actual emitted code.
3. **Potentially high-impact memory work:** BLK01/BLK02, VIF01/VIF02, BLK04, VTX01, RAS01.
4. **Larger structural projects:** GS14/GS15, US02–US05, IR01; validate intermediate guest-visible flags and timing-sensitive dependencies.
5. **Do not redo implemented features:** entries marked `EXISTING`. Compare and validate them before modifying.
