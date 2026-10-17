# pcsx2/x86/ix86-32/iR5900LoadStore.cpp

**Source:** [`pcsx2/x86/ix86-32/iR5900LoadStore.cpp`](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/ix86-32/iR5900LoadStore.cpp)

> Source-specific instructions only. `P1`/`P2`/`P3` are proposals; `EXISTING` items are already implemented. Match the current source, preserve CPU feature gates and emulation semantics, and verify generated code and performance before accepting changes.

- **[P2 · EE01 · L192–222](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/ix86-32/iR5900LoadStore.cpp#L192-L222)** — Fold copied guest-base plus immediate offset into `LEA r32, [base + displacement]` when the guest value is already allocated in a host GPR. For zero immediate, attempt direct register reuse if helper clobbers and 32-bit-address semantics permit.
- **[P3 · EE02 · L207–227](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/ix86-32/iR5900LoadStore.cpp#L207-L227)** — Keep existing constant-address propagation and 128-bit alignment; never remove TLB mapping/exception checks or violate guest register liveness.
