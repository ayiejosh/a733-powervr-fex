# Vendor vs open PowerVR on the A733 — full comparison

Radxa Cubie A7A, Allwinner A733, PowerVR BXM-4-64 MC1 (BVNC 36.56.104.183).
Both arms measured in **one session** (`full-compare.sh`), because run-to-run variance here is 10–25% between
sessions and 5–17% within one: absolute numbers from different sessions are not comparable.

## Performance

Medians, **n=2 per probe per arm** — indicative, not conclusive.

| probe | size | open | vendor | ratio | verdict |
|---|---|---|---|---|---|
| **vkheavy** | 2048 | **171.250** | 180.016 | **0.951** | **OPEN WINS 4.9%** |
| vkrender | 256 | 0.492 | 0.498 | 0.987 | parity |
| vkrender | 512 | 0.751 | **0.693** | 1.084 | vendor 8.4% |
| vkrender | 1024 | 1.771 | **1.675** | 1.057 | vendor 5.7% |
| vkrender | 2048 | 5.862 | **5.681** | 1.032 | vendor 3.2% |
| vkrender | 4096 | 23.741 | **22.903** | 1.037 | vendor 3.7% |

**`vkheavy` is the only stable result: 0.951 in every run this project has done** (open 171.09–171.56, vendor
179.94–180.83 — 0% spread on both arms).

**The render-size ratios are NOT stable.** `vkrender 2048` measured **0.918 (open wins 8.2%)** in the immediately
preceding run and **1.032 (vendor wins 3.2%)** here — same drivers, same config, minutes apart. At n=2 these are
samples, not measurements; a conclusion needs several samples per arm.

## Compatibility (from `vulkaninfo` per arm)

| field | VENDOR | OPEN |
|---|---|---|
| apiVersion | 1.3.277 | **1.3.363** (newer) |
| deviceName | PowerVR B-Series BXM-4-64 MC1 | same |
| driverName | PowerVR B-Series Vulkan Driver | Imagination open-source Mesa driver |
| driverInfo | **24.2@6603887** (released) | **Mesa 26.3.0-devel (git-fa5832f534)** (dev build) |
| conformanceVersion | 1.3.8.1 | **1.4.3.3** |
| distinct VK_* names | **130** | 129 |
| maxImageDimension2D | 8192 | 8192 |
| maxComputeWorkGroupInvocations | 512 | 512 |
| **maxMemoryAllocationCount** | **4294967295** | **4096** |

**The one hard compatibility gap: `maxMemoryAllocationCount`.** The vendor reports **4294967295** (effectively
unlimited); the open driver reports **4096** — a factor of about a million. An application that allocates many small
`VkDeviceMemory` objects will hit the open driver's ceiling and not the vendor's.

**Extension surface is effectively identical** (130 vs 129 distinct `VK_*` names by crude regex count over the full
dump — a rounded comparison, not an exact extension list).

**The open driver reports a newer API patch version (1.3.363 vs 1.3.277) and a newer conformance claim (1.4.3.3 vs
1.3.8.1), while being a git development build rather than a release.**

## Advantages each way

**Open driver has:**
1. **Source.** `src/imagination` in Mesa — readable, patchable, rebuildable. **Demonstrated this session:** patched
   `pvr_queue.c`, `make -C <kbuild> M=$PWD`, installed, A/B'd, reverted. Impossible with the vendor blob.
2. **Upstream.** Carried as PR #11, 105 commits over `origin/main`; fixes arrive from Mesa, not from a BSP drop.
3. **A newer API patch version and conformance claim** than the vendor's 24.2 release.
4. **A stable ~4.9% win on `vkheavy`** — the most raster-bound probe, and the only result no run has contradicted.

**Vendor driver has:**
1. **`maxMemoryAllocationCount` effectively unlimited vs 4096** — the one hard capability gap.
2. **It is a released, conformance-submitted stack** (`24.2@6603887`), not a `-devel` git build.
3. **It won 4 of 6 render-size probes in this run** — though at n=2 that is not conclusive, and the previous run
   had the open driver winning 2048.

## What was NOT tested

**`glmark2` compatibility was not run.** It needs a windowing system, so it cannot run inside a driver switch, and
the one earlier attempt on the open driver was confounded by a DRM/seat fault (`DRM_IOCTL_MODE_CREATE_DUMB failed:
Permission denied` + `VK_ERROR_OUT_OF_DEVICE_MEMORY`). **So "which driver is more compatible on real workloads" is
NOT established by this comparison** — only the API-level surface above is.

**Also not established:** anything needing more than n=2 samples; the open driver's behaviour under memory pressure
(where its 4096 allocation limit would first bite); and whether the vendor's known `glmark2`
`fragment-complexity=medium:fragment-steps=5` failure (3/3, passes on softpipe) is matched or beaten by the open
driver.

## Reproducing

```sh
cd bench/pvr-vulkan
sudo ./full-compare.sh 3        # 3 samples per probe per arm, both arms, one session
```

It verifies the bound driver at both ends, refuses to run if kwin is alive, and has an `EXIT` trap that restores the
desktop on any abort. Reports land in `/tmp/full-compare/`.

---

## The GL compatibility run (`glmark2-es2`) — vendor obtained, open NOT

**Two batches, one driver each, because `glmark2` needs a display and the desktop is kwin/X on `pvrsrvkm`.**

### Batch A — VENDOR: obtained

```
bound: pvrsrvkm (desktop up)
GL_RENDERER: zink Vulkan 1.3(PowerVR B-Series BXM-4-64 MC1 (IMAGINATION_PROPRIETARY))
GL_VERSION:  OpenGL ES 2.0 Mesa 25.0.7-2+deb13u1
glmark2 Score: 302
```

**Correction to an earlier assumption of mine: the vendor's GL is NOT native — it is `zink` too.** **So the
comparison is apples-to-apples: the same Mesa/zink front end, differing only in the Vulkan driver underneath**
(`IMAGINATION_PROPRIETARY` = `pvrsrvkm` vs `IMAGINATION_OPEN_SOURCE_MESA` = `powervr`).

### Batch B — OPEN: three attempts, all failed, and none of the failures was the driver

| attempt | result | cause |
|---|---|---|
| 1 | `Could not initialize canvas` | `XDG_RUNTIME_DIR is not set` — I hand-rolled weston instead of using `w26x.sh`; **and `env26.sh` is a source-me file, so my `[ -x ]` test was false and it was never sourced** |
| 2 | **ran, but on `softpipe`** — score 11, meaningless | patching in `w26x.sh` removed the `. env26.sh` that set `LD_LIBRARY_PATH` **for my own shell**; weston got the env, glmark2 didn't, so it loaded the system Mesa **24.0.1** |
| 3 | `Could not initialize canvas` | sourcing `env26.sh` before glmark2 restored the lib paths but the surface still failed to initialise |

**Every attempt was restored correctly by the script's `EXIT` trap** — `bound=pvrsrvkm kwin=ALIVE` each time, 0 kernel
warnings. **That part works.**

### What the open-side GL number would be

**Only a stale figure exists: `glmark2-open-final.txt`, dated 2026-10-06, score 22** with
`GL_RENDERER: zink Vulkan 1.3(...IMAGINATION_OPEN_SOURCE_MESA)` — **that is BEFORE the performance series and before
the tuning, so it does not describe the current driver.**

**So the honest statement: the vendor's GL compatibility score is 302; the open driver's is NOT measured.** **The
recipe that produced the 2026-10-06 open capture exists on this board (`glmark2-open-final.txt`) but the exact shell
provisioning that made it work was not captured alongside it — that is the gap.**
