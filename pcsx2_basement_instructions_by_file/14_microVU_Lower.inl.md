# pcsx2/x86/microVU_Lower.inl

**Source:** [`pcsx2/x86/microVU_Lower.inl`](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/microVU_Lower.inl)

> Source-specific instructions only. `P1`/`P2`/`P3` are proposals; `EXISTING` items are already implemented. Match the current source, preserve CPU feature gates and emulation semantics, and verify generated code and performance before accepting changes.

**Related change:** Equivalent EE-side transformations are in [`iFPU.cpp`](17_iFPU.cpp.md).

- **[P1 · LOW01 · L525–534, L752–761](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/microVU_Lower.inl#L525-L534)** — Build SQRT/RSQRT correction bitmap index with `MOV ecx, [ft_raw]; XOR ecx, 0x00800000; AND ecx, 0x00ffffff` instead of eight instructions (8→3); apply at both occurrences.
- **[P1 · LOW02 · L306–317, L802–812](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/microVU_Lower.inl#L306-L317)** — In exact SRT DIV/RSQRT quotient loops, replace `SHL ebp,1; ADD ebp,r8d` with `LEA ebp,[r8+rbp*2]` (2→1 each iteration). Verify downstream flags are overwritten.
- **[P1 · LOW03 · L580–595](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/microVU_Lower.inl#L580-L595)** — In exact SQRT tail, fold `MOV eax,r8d; SHL eax,1; ADD eax,ebp; MOV edi,eax` into `LEA edi,[rbp+r8*2]` (4→1), and the analogous `MOV/SHL 2/ADD ebp` into `LEA ebp,[rbp+r8*4]` (3→1). Check 32-bit overflow behavior.
- **[P2 · LOW04 · L2657–2677](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/microVU_Lower.inl#L2657-L2677)** — For RNEXT bit4 XOR bit22 feedback, test `MOV; AND 0x00400010; POPCNT; AND 1` in place of two shift-and-mask chains plus XOR (7→4). CPU-gate POPCNT; benchmark serial latency versus original parallel chains.
- **[P3 · LOW05 · L1949–1975, L2252–2260, L2327–2337](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/microVU_Lower.inl#L1949-L1975)** — Review guest immediate bitfield sign-extension (`SHL`/`SAR`) for host BMI extract opportunities only if a complete instruction is eliminated. Avoid replacing exact signed extraction with unsigned `BEXTR` alone.
