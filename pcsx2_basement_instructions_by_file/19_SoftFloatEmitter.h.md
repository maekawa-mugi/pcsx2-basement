# pcsx2/x86/SoftFloatEmitter.h

**Source:** [`pcsx2/x86/SoftFloatEmitter.h`](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/SoftFloatEmitter.h)

> Source-specific instructions only. `P1`/`P2`/`P3` are proposals; `EXISTING` items are already implemented. Match the current source, preserve CPU feature gates and emulation semantics, and verify generated code and performance before accepting changes.

- **[P3 · SFE01 · L133–218](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/SoftFloatEmitter.h#L133-L218)** — Use `EmitDivCarrySaveStep`/`EmitSqrtCarrySaveStep` as the semantic reference when benchmarking caller-side `LEA` folds. Deeper carry-save rewrites are speculative; do not alter quotient or correction-table behavior.
