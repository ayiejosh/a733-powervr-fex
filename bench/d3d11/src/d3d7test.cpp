// Minimal offscreen D3D7 smoke test for D7VK: clear + one TL triangle, read back.
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <cstdio>

#define CK(x) do { HRESULT _hr = (x); if (FAILED(_hr)) { printf("FAIL %s hr=0x%08lx\n", #x, (unsigned long)_hr); return 1; } } while (0)

int main() {
    HWND hwnd = CreateWindowExA(0, "STATIC", "d7t", WS_POPUP, 0, 0, 256, 256, nullptr, nullptr, nullptr, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    IDirectDraw7* dd = nullptr;
    CK(DirectDrawCreateEx(nullptr, (void**)&dd, IID_IDirectDraw7, nullptr));
    CK(dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL));

    DDSURFACEDESC2 pd = {};
    pd.dwSize = sizeof(pd);
    pd.dwFlags = DDSD_CAPS;
    pd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_3DDEVICE;
    IDirectDrawSurface7* prim = nullptr;
    CK(dd->CreateSurface(&pd, &prim, nullptr));

    DDSURFACEDESC2 sd = {};
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    sd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    sd.ddpfPixelFormat.dwFlags = DDPF_RGB;
    sd.ddpfPixelFormat.dwRGBBitCount = 32;
    sd.ddpfPixelFormat.dwRBitMask = 0x00FF0000;
    sd.ddpfPixelFormat.dwGBitMask = 0x0000FF00;
    sd.ddpfPixelFormat.dwBBitMask = 0x000000FF;
    sd.dwWidth = 256; sd.dwHeight = 256;
    sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
    IDirectDrawSurface7* rt = prim;

    IDirect3D7* d3d = nullptr;
    CK(dd->QueryInterface(IID_IDirect3D7, (void**)&d3d));
    IDirect3DDevice7* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(IID_IDirect3DTnLHalDevice, prim, &dev);
    if (FAILED(hr)) CK(d3d->CreateDevice(IID_IDirect3DHALDevice, prim, &dev));

    D3DVIEWPORT7 vp = {0, 0, 256, 256, 0.0f, 1.0f};
    CK(dev->SetViewport(&vp));
    CK(dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF0000FF, 1.0f, 0)); // blue

    struct TLV { float x, y, z, rhw; DWORD color; };
    TLV tri[3] = {
        {128, 32, 0.5f, 1.0f, 0xFF00FF00},
        {224, 224, 0.5f, 1.0f, 0xFF00FF00},
        {32, 224, 0.5f, 1.0f, 0xFF00FF00},
    };
    CK(dev->BeginScene());
    CK(dev->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE));
    CK(dev->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE));
    CK(dev->DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, tri, 3, 0));
    CK(dev->EndScene());

    DDSURFACEDESC2 ld = {}; ld.dwSize = sizeof(ld);
    CK(rt->Lock(nullptr, &ld, DDLOCK_READONLY | DDLOCK_WAIT, nullptr));
    DWORD center = *(DWORD*)((BYTE*)ld.lpSurface + 128 * ld.lPitch + 128 * 4);
    DWORD corner = *(DWORD*)((BYTE*)ld.lpSurface + 4 * ld.lPitch + 4 * 4);
    rt->Unlock(nullptr);
    printf("center=0x%08lx corner=0x%08lx\n", (unsigned long)center, (unsigned long)corner);
    bool ok = ((center >> 8) & 0xFF) > 0xC0 && ((corner) & 0xFF) > 0xC0;
    printf(ok ? "D3D7_OK\n" : "D3D7_BAD\n");
    return ok ? 0 : 2;
}
