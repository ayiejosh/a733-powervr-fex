# IMG PowerVR (Rogue B-series) architecture notes - and where the DM2/TA stall sits

Compiled 2026-10-27 from: IMG's PowerVR Architecture Guide (docs.imgtec.com starter guides), PVRTune hardware
terms, IMG's "Pipelined Data Masters" blog, Mesa's PowerVR driver docs (CSBGEN hardware interfaces, open
kernel driver), the vendor DDK source in `~/ddk242`, and our own RE of the firmware/MTS on this board.

## 1. The graphics pipeline (TBDR)

PowerVR is tile-based deferred rendering: geometry is processed once, the picture is split into tiles, and
each tile is shaded only after hidden-surface removal.

- **Geometry pass - Tiler / Tile Accelerator (TA, DM2 here)**: vertex shader runs per vertex (USC), then the
  tiler clips/culls and bins triangles into tiles, writing the **parameter buffer / display list**: region
  headers (one per tile, with a link pointer), per-tile control streams, tail pointer cache (TPC), and the
  parameter stream generator (PSG). The tiler is `VDM` (Vertex Data Master) in the register/stream docs; its
  input is a VDM control stream (built by the driver, see CSBGEN "VDM control stream" structures).
- **Pixel pass - Renderer**: the ISP does per-tile HSR (depth/stencil), then the TSP/USC run the fragment
  shaders only for visible fragments, and the PBE does blending/format/twiddle writes. Counters group this
  as Renderer; the TA/Tiler is DM2, the pixel/3D DM is DM3.
- **Memory**: on-chip SLC cache (size matters - vendor disables DM overlap on tiny-SLC parts), parameter
  buffer in system memory, region headers fetched per tile by the ISP.

## 2. Firmware and Data Masters

- A **firmware processor** (MIPS on this B-series; RISC-V on Volcanic) does high-level scheduling and
  priority: it consumes kernel/firmware CCB commands, sets up DM state between jobs, handles power and
  interrupts. Fixed-function **Data Masters** run the job types: GP (firmware/general), TDM/2D?,
  **DM2 = geometry/TA**, **DM3 = 3D/pixel**, **CDM = compute**, plus transfer; kernel logs here print
  "Data Master=2 (Geometry)".
- On B-series the DMs are single-tasking: switching job means firmware setup work, which is why the TA->3D
  handoff inside a render pass costs ~176 us of firmware time on this board (measured earlier). D-series
  added **pipelined DMs** so firmware prepares the next job while the previous runs.
- DM **context switching** is register + program driven: per-DM context state base
  (`RGX_CR_VDM_CONTEXT_STATE_BASE`, `RGX_CR_CDM_CONTEXT_STATE_BASE`), context store/resume **PDS tasks**,
  low-latency scheduling (LLS) structures and context-switch profile. The driver supplies the PDS programs
  and static context state; the firmware executes the switch.

## 3. Kick protocol (who tells the GPU to start a DM)

1. Userspace writes command streams (CCCB / control streams) and submits via DRM.
2. Kernel builds a KCCB command and **kicks the firmware** by writing the documented `MTS_SCHEDULE` with
   `DM_GP` (`pvr_fw.c`, vendor `rgxfwutils.c`: "Kick MTS"), with a read-back for ordering.
3. Firmware validates the job, sets up the DM's context, then writes the **firmware-private kick request**
   `thread<<19 + 0xCE0 = 1` (not present in any host CR definition) and clears the pending bit at
   `thread<<19 + 0xD20 = 0`. Both clients write exactly these values.
4. **MTS** (a hardware scheduler block) decides to start the DM: it context-switches, loads the DM context,
   starts the DM, and routes DM interrupts back to firmware. Host-visible MTS state: `MTS_SCHEDULE`
   (documented GP kick), `MTS_BGCTX`/`MTS_INTCTX` + per-thread DM association, `SCHEDULE_ENABLE`, per-DM
   interrupt enables. MTS also has virtualisation sideband registers (OSid).
5. DM completion raises event `EVENT_STATUS` 0x20 (TA) etc.; firmware finishes the render pass, updates
   fences.

## 4. The open stack mapping

- Open kernel `drm/imagination`: allocates/maps firmware objects, builds KCCB/kernel CCB commands, kicks the
  firmware via `MTS_SCHEDULE`, programs firmware runtime config/sysdata, handles firmware CCB replies. It
  does **not** program client-DM dispatch - that is firmware/MTS.
- Mesa's open Vulkan driver (`pvr`/`pco`): builds CCCB job streams (shared with the closed vendor userspace
  layout), control streams via generated CSBGEN code, PDS programs, static context state, HWRT datasets.
- Native Gallium driver (`pvr_native`): same kernel interface, its own stream builder. It reaches the same
  firmware state (identical kick, identical traces) but DM2 never starts.

## 5. Where the stall sits in this architecture

Evidence says the native client's job gets all the way to the firmware kick: FCCtx/RTData/streams are
identical modulo addresses, firmware prints "Kick TA", and the kick request register `0xCE0` reads back 1
(a stable level) while `0xD20` is pending. MTS then never starts DM2; firmware times out, tears the context
down and HWRs. Only working-DM activity seen in MTS during the window is background DM0 scheduling.
The host cannot influence this: re-writing `0xCE0`, poking documented MTS registers, firmware config flags
(system + per-OS), priority/job flags/sync shapes, and the vendor-vs-open firmware comparison all changed
nothing. The refusal is inside MTS hardware, after everything software-visible has been accepted.

## 6. Open questions that only IMG can answer

- What preconditions does MTS check before starting a DM (context loaded? DMA fence? OSid? DM association
  state? SLC/cache state?) and is there any documented/undocumented status register that records a rejection?
- Does the firmware validate the client's DM2 context differently from what Mesa's Vulkan client produces
  (e.g., expected fields in FWCtx/HWRT that the native builder leaves zero - our comparisons found no
  difference, but the check may be hardware-side)?
- Is there a firmware debug build/log group that traces the MTS accept/refuse path?
- Is this a known erratum for non-Mesa clients on B-series? (The public docs and forums show no such report.)
