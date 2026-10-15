# vkalloc-limit — does `maxMemoryAllocationCount` bind in practice?

`vulkaninfo` reports the *declared* limit. This allocates 4 KB blocks until the driver refuses,
so total memory stays small (20,000 objects = 78 MB) and **only the count is being measured**.

```sh
cc -O2 -o vkalloc-limit vkalloc-limit.c -lvulkan
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/img_icd.json ./vkalloc-limit          # vendor
VK_ICD_FILENAMES=/home/radxa/pvr_gen_icd.json \
  PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 ./vkalloc-limit                          # open
```

## Measured 2026-10-15

| driver | declared | **measured** |
|---|---|---|
| **vendor (`pvrsrvkm`)** | 4294967295 | **≥ 20,000** — hit the test's cap, not the driver's limit |
| **open (`powervr`)** | **4096** | **not measured** — needs the open driver bound |

**So the vendor holds at least ~4.9x the open driver's declared limit, and the vendor's own ceiling
is above 20,000 or absent.**

**The open driver's ACTUAL limit is untested.** 4096 is exactly the Vulkan 1.0 minimum, so it may
refuse right there — or it may be a conservative declaration and accept more.

## The cap matters

The first version of this test capped at **2,000,000** objects = **7812 MB on a 5914 MB board**.
It ran, exhausted memory, tripped `earlyoom` (`mem avail: 117 of 119 MiB`) and the hardware watchdog
reset the board. **The cap is now 20,000 (78 MB). Do not raise it without checking `free`.**

## Caveats

* **`VkResult 0` at the cap means "at least this many", not "this is the limit"** — the verdict line
  the program prints ("real limit is BELOW the declared N") is trivially true and should be ignored.
  Read the `allocated` line instead.
* **4 KB allocations test the COUNT, not the heap** — a driver could accept many tiny allocations and
  still fail on large ones. This answers one question only.
* **The program leaks on purpose** and exits immediately after counting.
