# Comics

Illustrated, plain-language summaries of the findings in this repo. Each one is a
self-contained SVG (renders directly on GitHub) plus a PNG export for sharing.

The measurements behind both comics are in
[`../PERFORMANCE-2026-09-22.md`](../PERFORMANCE-2026-09-22.md), and the fixable halves ship
as [`../../overlays/`](../../overlays/) and [`../../system/`](../../system/).

| Comic | About |
|---|---|
| [2026-09-22 — Unlocking the A733](2026-09-22-unlocking-the-a733.svg) ([PNG](2026-09-22-unlocking-the-a733.png)) | Why the PowerVR GPU ran at 600 MHz when the silicon's own spec table says 1008 MHz, why the CPU had been silently capped at 84 % for months, and how a small demand-driven helper gives full speed on demand with low power at idle. |
| [2026-09-22 — How high can it go?](2026-09-22-ceiling-hunt.svg) ([PNG](2026-09-22-ceiling-hunt.png)) | The ceiling hunt: the GPU tops out at **1104 MHz** (the clock generator refuses more), the **L3/DSU fabric was stuck at 780 MHz** because its scaling driver is not compiled into this kernel (raising it to 1027 MHz gave +19 % L3, +38 % memory read, −32 % FEX thread-start), and the **CPU is already at its ceiling** — extra OPPs are ignored because this BSP picks frequencies from the chip's factory speed grade. |

## Regenerating the PNG

The SVG is the source of truth. To export a PNG (no ImageMagick needed — headless
Chromium works):

```sh
# window-size must match the SVG's own width/height
for f in 2026-09-22-unlocking-the-a733:1240x1200 2026-09-22-ceiling-hunt:1240x1380; do
  name=${f%%:*}; size=${f##*:}
  chromium --headless --no-sandbox --disable-gpu --hide-scrollbars \
    --screenshot="$name.png" --window-size="${size/x/,}" \
    "file://$PWD/$name.svg"
done
```

Validating a comic before committing (both are cheap and catch real mistakes):
```sh
python3 -c "import xml.dom.minidom as m; m.parse('2026-09-22-ceiling-hunt.svg')"   # XML well-formed
grep -c 'class=' 2026-09-22-ceiling-hunt.svg   # duplicate class= attributes break strict XML
```

## Style notes

* Flat vector panels, one idea per panel, `Helvetica/Arial`, no external fonts or
  scripts so the file renders identically on GitHub, in browsers and in print.
* Plain language on purpose: each panel should be understandable without the
  companion document.
* Numbers in the comic are the measured ones — see
  [`../BENCHMARKS.md`](../BENCHMARKS.md) and
  [`../../bench/baseline.txt`](../../bench/baseline.txt) for the raw data.
