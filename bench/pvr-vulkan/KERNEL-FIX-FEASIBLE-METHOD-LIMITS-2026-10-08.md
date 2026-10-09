# The kernel fix is feasible — and two measurements I would not trust

## The prerequisite is met

**The mainline `powervr` module builds** on this host:

```
make -C /lib/modules/6.1.98-5-aw2511/build M=/home/radxa/kernel-src/powervr modules
  -> completes (only a "compiler differs" warning)
```

and a built `powervr.ko` exists from 2026-10-06. **So a kernel-side job-chaining facility — targets (1) and
(3) are the same root issue: a driver-side sync/job-chaining type — is implementable here** (UAPI enum +
`pvr_job.c` handler + Mesa use). It requires a module reload, so weston/Xwayland must be down and the guard
respected.

## The UAPI gap, confirmed

```
enum drm_pvr_job_type {
    DRM_PVR_JOB_TYPE_GEOMETRY = 0,
    DRM_PVR_JOB_TYPE_FRAGMENT,
    DRM_PVR_JOB_TYPE_COMPUTE,
    DRM_PVR_JOB_TYPE_TRANSFER_FRAG,
};
```

**Four job types, no null type**, and the kernel dispatches on exactly those four (`pvr_job.c:280-289`). So
the userspace `pvr_drm_winsys_null_job_submit()` fence-forwarding routine has **no kernel counterpart** —
the structural difference from the vendor's `pvr_srv_sync_type`.

## Two measurements NOT recorded as findings

1. **"GPU 100% busy during the desktop scene."** Computed by unioning job intervals via a
   fence→completion map. **That mapping is already known unreliable above a few thousand jobs** — fences are
   reused, and it earlier produced a physically impossible 168 ms median job for terrain. 9631 jobs were
   involved, so the 100% figure is as likely a mapping artefact. **Discarded rather than reported.**
2. **Per-job duration distributions from long traces** (terrain: median 168 ms against a 200 ms frame —
   inconsistent with 45 jobs/frame). **The fence-pairing method does not scale to these job counts** and must
   not be used for them. It remains valid for short traces (the vkrender/vkheavy work, where paired values
   repeat to within 1%).

## Method note

**The per-job instrument that produced the session's best results degrades with job count**, because it
pairs jobs to completions by fence handle and handles are recycled. **Any per-job timing claim above a few
thousand jobs needs a different method** (per-entity sequencing, or the driver's own timestamps). Recorded
so the same trap is not walked into again.
