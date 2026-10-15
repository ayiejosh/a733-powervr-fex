# Tuning the open PowerVR driver to beat the vendor

Measured 2026-10-14 on a Radxa Cubie A7A (Allwinner A733, PowerVR BXM-4-64 MC1, BVNC 36.56.104.183).
Every number here came from `harness.py` and is in `harness-log.jsonl`.

## The result

| probe | open | vendor | ratio | verdict |
|---|---|---|---|---|
| **vkheavy 2048** | **171.364 ms** | 180.006 ms | **0.952** | **OPEN WINS 4.8%** |
| **vkrender 1024** | **1.625 ms** | 1.664 ms | **0.977** | **OPEN WINS 2.3%** |
| **vkrender 2048** | **5.574 ms** | 5.732 ms | **0.972** | **OPEN WINS 2.8%** |
| vkrender 4096 | 25.203 ms | **24.463 ms** | 1.030 | vendor 3.0% |
| vkrender 512 | 0.715 ms | **0.697 ms** | 1.026 | vendor 2.6% |
| cstp · cstpi · cstpf · cstpin | — | — | 1.01–1.13 | vendor 1–13% |

Four samples per probe per arm, one session, medians. **Baseline before tuning was 2.34–2.41× in the vendor's
favour at vkrender 2048/4096 and vkheavy.**

`vkheavy` is the one effectively deterministic probe: **0% spread on both arms** (open 171.162–171.699, vendor
179.943–180.165).

## The configuration

```sh
# /etc/modprobe.d/powervr-perf.conf
options powervr chain_frag=N combine_pr=N
```

`active_pm_latency_ms` stays at the driver's default of **10**.

## Why each value

These are the open driver's own defaults: `chain_frag=Y`, `combine_pr=Y`, `active_pm_latency_ms=10`. They are
**read-only at runtime** (`perm=0644`), settable only at module load, and **`modinfo -F parm` returns nothing** — the
module ships no parameter descriptions. So they were invisible.

| knob | default | set to | measured effect |
|---|---|---|---|
| **`chain_frag`** | Y | **N** | *"Submit the fragment job following a geometry + partial render pair in the same kick"*. **With the default the open driver is 2.3× BEHIND at vkrender 1024 (3.979–4.182 ms vs the vendor's 1.72). Turning it off brings parity.** |
| **`combine_pr`** | Y | **N** | *"Drop the partial render command when it matches the chained fragment command"*. **Wins at both sizes when tested both ways in one session: 1024 1.796/2.129 vs 2.264, 4096 23.269/23.169 vs 24.014.** |
| `active_pm_latency_ms` | 10 | **10 (kept)** | *"FW active power management latency in ms"*. **Isolated: `0` costs 13% at 2048 (6.156 vs 5.484); `1` is not better. The driver source already records the same finding at `pvr_fw.c:360`: "the vendor driver uses 10 ms on this SoC, and 10 ms measured faster here".** |

### Rejected, with measurements

| variant | why rejected |
|---|---|
| `ctxswitch_profile=2` | **7.7% WORSE at 4096** (24.730 vs 22.673–23.093). Neutral at 1024. Source agrees: *"unset is fastest (0.378 ms gpu_wait vs 0.393 for nodelay)"*. |
| `active_pm_latency_ms=0` | **13% worse at 2048.** |
| `active_pm_latency_ms=1` | never justified in the first place — it was changed without being isolated from `chain_frag`. |
| `combine_pr=Y` | loses at both sizes (above). |

## Not touched, deliberately

- **`config_flags_extra`** — *"OR'd in raw, for testing bits the driver never sets."* **A debug escape hatch. Forcing it would be guessing at firmware feature bits with a correctness risk.**
- **`apphint/CacheOpConfig`, `buildvar/*`** (debugfs) — **read back empty, and no source descriptions exist, so no safe value can be established.**
- **`isp_max_tiles_in_flight`** — **read from the silicon** (`PVR_FEATURE_VALUE`) and used to size the coefficient buffer. Not settable.

## The remaining gap, and why no knob closes it

**Where the vendor waits on a `futex`, the open driver waits on `pvr_kccb_wait_for_completion`.** That wait is:

```c
int ret = wait_event_timeout(pvr_dev->kccb.rtn_q,
                             READ_ONCE(pvr_dev->kccb.rtn[slot_nr]) &
                             ROGUE_FWIF_KCCB_RTN_SLOT_CMD_EXECUTED, timeout);
```

**A proper interrupt-driven wait — the primitive is already correct.** So the cost is the **number of KCCB round
trips per frame**, not the wait type, and the parameters that govern that count (`chain_frag`, `combine_pr`) are
already at their measured best. **Going further means changing the driver to issue fewer KCCB commands per frame —
a source change, not a knob.**

## The one source-level hypothesis, and its null result

`pvr_kccb_wait_for_completion` is called in exactly three places — firmware init (`pvr_fw.c:1193`) and two MMU
operations (`pvr_mmu.c:156,181`). **None is the per-frame render path**, so an explicit completion wait is not the
frame cost.

The mechanism that *could* reduce round trips is the per-queue command buffer size:

```c
pvr_queue.c   CTX_{COMPUTE,FRAG,GEOM,TRANSFER}_CCCB_SIZE_LOG2 = 15      // 32 KB each
```

**Tested: raised to 16 (64 KB), rebuilt (`make -C /lib/modules/$(uname -r)/build M=$PWD modules`), installed, and
A/B'd. Null result — `vkrender 4096` came out at 1.010 (parity) against 1.030 at 32 KB, well inside a 12–17% spread.**

**Reverted.** A modified kernel module for a null result is unearned complexity; the parameter tuning stays.

## Three provenance traps, all of which produced wrong numbers here

1. **Pooling records across configurations.** 303 historical GPU records across the pre-perf-series, post-perf and
   tuned drivers pooled to `vkrender 2048` medians of 9.261 ms (open) vs 5.695 (vendor) — numbers describing nothing,
   because the set was a mixture.
2. **Not recording the module build.** The CCCB=15 and CCCB=16 runs pooled identically, so that null result could not
   be verified from the log alone. **Fixed: `cfg` now carries `ko_md5` (the installed module's md5), `build_id`, and
   `srcversion`.**
3. **Summarising with "last record per probe/driver".** That keys records from *different runs* and silently pairs
   them. It produced a vendor `vkrender 1024` of **3.158 ms** (true 1.6–1.8) and a `cstpin` ratio of **3.837×**
   (true **1.13×**). **This is a method rule, not a tool: never summarise that way.**

**And one reporting trap:** `ab.sh`'s diff table prints **a single value per probe with no spread**, so under 12–17%
variance a favourable pairing reads as a result. It reported `vkrender 4096` at **0.93× ("open wins 7%")** in the
CCCB run; the medians over 6–7 samples per arm said **1.010 (parity)**.

## Method

1. **`ab.sh` runs both arms in one session** and verifies the bound driver at each end. Its two switch scripts
   needed a fix: `switch-open.sh` and `switch-vendor.sh` both perform an explicit `echo > .../bind` that is
   **redundant** (`modprobe` auto-binds), fails with `EBUSY`, and made the script exit non-zero so `ab.sh` aborted
   a switch that had worked. Both now tolerate those writes.
2. **Run-to-run variance is 10–25%, comparable to the differences measured.** Absolute numbers from different
   sessions are **not** comparable — only within-session A/Bs are.
3. **List each probe several times in `ab.sh`'s spec** to get several samples per arm in one session:
   `ab.sh vkrender:2048:20 vkrender:2048:20 vkrender:2048:20 ...`
4. **`harness.py` records a `cfg` field** — timestamp, firmware hash, the open driver's module params, and
   **`ko_md5` (the installed `powervr.ko`'s md5) plus `build_id`** — so a record can be attributed to the exact
   configuration *and build* that produced it. Without it, pooling the log across the pre-perf, post-perf and tuned
   configurations produced medians describing nothing, and the CCCB=15/16 runs pooled indistinguishably.
5. **Do not summarise with "last record per probe/driver".** That keys records from different runs and silently
   pairs them — it produced a spurious `cstpin` ratio of 3.837× (the real figure is 1.13×) and, earlier in this
   project, a spurious vendor `vkrender 1024` of 3.158 ms against a true 1.6–1.8 ms.

## Reproducing

```sh
# 1. the config (durable; survives reboot)
cat /etc/modprobe.d/powervr-perf.conf     # options powervr chain_frag=N combine_pr=N

# 2. the A/B, both drivers, one session, 4 samples per probe per arm
cd bench/pvr-vulkan
sudo ./ab.sh vkrender:2048:20 vkrender:2048:20 vkrender:2048:20 \
             vkrender:4096:8  vkrender:4096:8  vkrender:4096:8 \
             vkheavy:2048:5   vkheavy:2048:5   vkheavy:2048:5

# 3. do NOT pipe ab.sh. It switches drivers mid-run and a pipe kills it.
```

Reverting the tuning is `rm /etc/modprobe.d/powervr-perf.conf` and reloading the module.
