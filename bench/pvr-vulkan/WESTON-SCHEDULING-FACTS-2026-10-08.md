# weston's scheduling facts, and two discriminators that do not work

## Facts from `weston --debug`

```
[19:04:19.381] Output repaint window is 7 ms maximum.
[19:04:19.391] DRM: does not support Atomic async page flip
```

* **Repaint window is 7 ms** - the same order as the ~7 ms floor measured earlier in the release wait
  (7.10 ms at 160x120, 7.20 ms at 320x240, area-independent). **That floor is very likely weston's
  repaint scheduling window, not a driver cost.**
* **No atomic async page flip** - weston's flips are synchronous. Same for the vendor configuration
  (same weston, same DRM), so it cannot explain a difference between the two drivers.

## Discriminator that fails: weston `--renderer=pixman`

Removing weston's GPU work with the software renderer breaks the GL client:

```
MESA-EGL: warning: egl: failed to create dri2 screen
Error: eglInitialize() failed with error: 0x3001
```

weston comes up and Xwayland connects, but the client cannot get GL. Not usable as a "cheap
compositor" control with a GPU client. Recorded so it is not retried.

## Discriminator rejected as unsafe

Client on the open driver + weston on the vendor Vulkan would require `powervr` and `pvrsrvkm` loaded
**simultaneously** - exactly the configuration behind the recorded `SyncCheckpointUnref` NULL-deref
Oops (`[#13]`). Not attempted.

## Where the ~8 ms stands

Unattributed. Known: it is in the release path; not the client's render (separate 4.4x); not weston's
composite render time (~4 ms of 12.5 ms); and weston's repaint window contributes ~7 ms of scheduling
the vendor config shares. Next instrument needs weston-side timing `--debug` does not provide -
`weston-debug timeline` is already known lossy, so this wants a real counter in weston's repaint path
rather than more outside inference.
