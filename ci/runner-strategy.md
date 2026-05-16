# CI runner strategy

Records which tier the cross-platform parity / hardware checks
run in. Stage 15's `replay-parity` and `hil-tolerance` are
conditional gates whose tier depends on the runner infrastructure
available; this file is the source of truth for that tier.

## Current tier: bench, on-demand (no QEMU, no self-hosted runner)

There is no ESP32-C3 RISC-V QEMU path and no self-hosted ESP32-C3
runner attached to the repository. Therefore:

| Check | Where it runs | Gating |
|---|---|---|
| `replay-parity-host` | GitHub Actions, every PR | **PR-gating.** The host replay binary must reproduce `test/parity/replay_parity.csv` byte-for-byte. Locks the host side of cross-platform parity. |
| `replay-parity` (full) | the bench, on-demand | **Release-tag discipline.** `make replay-parity` builds + flashes the ESP32-C3 REPLAY firmware, captures its canonical CSV, and asserts it equals the host golden. Run before cutting a `v*` tag. Not PR-gating. |
| `hil-tolerance` | the bench, on-demand | Behavioral tolerance bands on real hardware. Run on demand / before release. Not PR-gating. |

The host side of parity *is* automated and PR-gating; only the
host-vs-target byte comparison needs the physical board and so is
a bench step.

## Flipping the tier

If a stable ESP32-C3 RISC-V QEMU path appears, or a self-hosted
ESP32-C3 runner is connected to the repository:

1. Add a `replay-parity` CI job that builds + runs the ESP32
   REPLAY firmware (under QEMU, or on the self-hosted runner) and
   runs the host-vs-target byte comparison.
2. Make it PR-gating (or merge-queue-gating).
3. Update the table above and `README.md` to record the new tier.

Until then, the plan does not promise an always-on PR gate for
full host-vs-target parity — `replay-parity-host` is the per-PR
gate, and full `replay-parity` is the release-tag discipline.
