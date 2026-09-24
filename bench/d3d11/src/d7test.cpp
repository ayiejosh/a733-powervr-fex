// Minimal D3D7 smoke test: DirectDraw7 + Direct3D7 device, clear+flip 60 frames.
// Build: x86_64-w64-mingw32-clang++ d7test.cpp -o d7test.exe -lddraw -ldxguid -lgdi32 -luser32
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <stdio.h>

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

int main() {
    WNDCLASSA wc = {}; wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(0);
    wc.lpszClassName = "d7t"; RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d7t", "d7test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              64, 64, 640, 480, 0, 0, wc.hInstance, 0);

    IDirectDraw7* dd = nullptr;
    HRESULT hr = DirectDrawCreateEx(nullptr, (void**)&dd, IID_IDirectDraw7, nullptr);
    printf("DirectDrawCreateEx hr=0x%08lx\n", hr); if (FAILED(hr)) return 1;
    hr = dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
    printf("SetCooperativeLevel hr=0x%08lx\n", hr); if (FAILED(hr)) return 1;

    DDSURFACEDESC2 sd = {}; sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS; sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    IDirectDrawSurface7* prim = nullptr;
    hr = dd->CreateSurface(&sd, &prim, nullptr);
    printf("CreateSurface(primary) hr=0x%08lx\n", hr); if (FAILED(hr)) return 1;

    DDSURFACEDESC2 bd = {}; bd.dwSize = sizeof(bd);
    bd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    bd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE;
    bd.dwWidth = 640; bd.dwHeight = 480;
    IDirectDrawSurface7* back = nullptr;
    hr = dd->CreateSurface(&bd, &back, nullptr);
    printf("CreateSurface(back,3D) hr=0x%08lx\n", hr); if (FAILED(hr)) return 1;

    IDirect3D7* d3d = nullptr;
    hr = dd->QueryInterface(IID_IDirect3D7, (void**)&d3d);
    printf("QI IDirect3D7 hr=0x%08lx\n", hr); if (FAILED(hr)) return 1;

    IDirect3DDevice7* dev = nullptr;
    hr = d3d->CreateDevice(IID_IDirect3DHALDevice, back, &dev);
    printf("CreateDevice(HAL) hr=0x%08lx\n", hr);
    if (FAILED(hr)) { // ponytail: fall back to RGB rasterizer so we still learn something
        hr = d3d->CreateDevice(IID_IDirect3DRGBDevice, back, &dev);
        printf("CreateDevice(RGB fallback) hr=0x%08lx\n", hr);
        if (FAILED(hr)) return 1;
    }

    for (int i = 0; i < 60; i++) {
        D3DRECT r = {0, 0, 640, 480};
        dev->Clear(1, &r, D3DCLEAR_TARGET, 0xFF8000FF, 1.0f, 0);
        RECT rc = {0, 0, 640, 480};
        prim->Blt(nullptr, back, &rc, DDBLT_WAIT, nullptr);
        MSG msg; while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
    }
    printf("D7_OK 60 frames\n");
    dev->Release(); d3d->Release(); back->Release(); prim->Release(); dd->Release();
    return 0;
}
