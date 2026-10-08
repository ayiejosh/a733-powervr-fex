# Tile partition matches exactly; shared reservation differs by 8% - neither is the 2.4x

## Measured

Instrumented the open winsys to print the values it takes from the kernel beside what the vendor
winsys computes from the same `dev_info`:

```
[part] kernel common_store_partition_space_size=6144  vendor_formula=6144
       tile=16x16 max_partitions=12 uor=2
       kernel common_store_alloc_region_size=11264
```

| quantity | vendor (computed) | open (kernel) | |
|---|---|---|---|
| `total_reserved_partition_size` | 6144 | **6144** | **match** |
| `reserved_shared_size` | 19456-1024-6144 = **12288** | **11264** | **1024 lower (~8%)** |

The vendor formula `tile_x * tile_y * max_partitions * usc_min_output_registers_per_pix` =
16 * 16 * 12 * 2 = 6144 matches the kernel's value exactly, so **the tile buffer size is correct and
the tile-configuration lead is closed.** The shared reservation is 8% lower in the open driver - real,
worth noting, but not a 2.4x effect.

Instrumentation reverted; vkrender still 296.8 Mpix/s (no regression).

## Where the uniform 2.4x stands

Both drivers: GPU at **1104 MHz**. Tile partition **identical**. Raw fill and a 640-op shader both
**2.4-2.5x** down. Vendor fill improves with area (619 Mpix/s at 1024 -> 743 at 2048) while the open
driver is **flat at ~300 Mpix/s**.

```
vendor 743 Mpix/s  ~= 1.5 cycles/pixel at 1104 MHz
open   300 Mpix/s  ~= 3.7 cycles/pixel
```

Both under 1 pixel/cycle; open ~2.5x worse, uniformly.

## Eliminated for the render half

| hypothesis | evidence |
|---|---|
| PCO codegen | uniform across trivial fill and 640-op shader |
| GPU clock | pll-gpu/gpu0 = 1104000000 under both |
| tile partition size | 6144 both, exactly |
| memory layout / exportability | linear+exportable render at the same rate |
| shared reservation | 8% difference, not 240% |

## Next

A per-pixel cost in how the driver feeds the hardware. No GPU performance counters exist on this SoC,
so the next instrument is comparative: find a configuration knob the open driver exposes that changes
pixel throughput - tile buffer count, MSAA sample count, render-target format - and see whether any
moves the 300 Mpix/s plateau. **A flat rate independent of area is the shape of a serial per-pixel
cost**, so that is what to probe.
