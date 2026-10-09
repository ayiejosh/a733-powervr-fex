# A clean, non-discard probe confirms the PBE is not the per-surface cost

Earlier PBE conclusions rested on `FRAGDISCARD`, later shown to be an **invalid cross-driver control** (the
vendor optimises discards away). **Re-tested with a method that changes the PBE's work without touching
fragment survival** — the attachment format:

| variant | bytes/pixel | frame |
|---|---|---|
| full precision | 4 | 13.991 ms |
| `FORMAT=r8` | **1** | 14.309 ms |
| `FORMAT=rgba8` | 4 | 13.796 ms |
| `FORMAT=rg16` | 4 | **12.821 ms** |

## What this establishes

**Bytes-per-pixel varies 4× while the frame time moves ~1.5 ms — roughly 10%.**

- **The PBE write is at most ~10% of the render.** The earlier discard-based "PBE write 3.2×" figures were
  inflated by the invalid control; **this clean method puts the PBE's share an order of magnitude lower.**
- **More than 90% of the per-surface cost is FORMAT-INDEPENDENT** — it is **not data movement at all**.
- Combined with the earlier finding that it is also **coverage-independent** (flat against `AREA`), and that
  the **geometry/TA job is *faster* than the vendor's**, the per-surface cost is **per-tile processing work
  whose cost does not depend on what the tile contains or how wide its pixels are**.

## The constraint this creates

It **rules out** the PBE, the store path, the attachment format, the tile load, and any bandwidth term — all
of which would move with format. **What remains is the fixed per-tile work in the fragment job** (the
driver's tile state and the firmware's tile loop) — the "~6.45 ms" residual seen when rasterization was
disabled earlier.

## Probe note

`IO16=1` produced no output (the knob's current form didn't take effect in this binary), so the
half-precision comparison is **not claimed** — only the three format variants, which ran cleanly.
