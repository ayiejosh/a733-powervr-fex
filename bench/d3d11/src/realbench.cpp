// realbench.cpp — Phase-2 CONFIRM: where does a REALISTIC D3D11 frame's time go on
// the SHIPPING DXVK-Sarek -> PowerVR BXM-4-64 stack? GPU-fill / CPU-record / present.
// MEASUREMENT, not optimization.
//
// A "realistic frame" here = a textured, depth-tested scene drawn as many small
// instanced/separate textured quads (2 tris each) at varying depth & screen pos,
// alternating between 2 PSOs (a few pipeline swaps), with a sampler + SRV + depth
// buffer + per-draw constant-buffer transform. Rendered to an OFFSCREEN RTT at a
// realistic resolution (default 512x512) so the GPU actually fills pixels.
//
// Modes (argv[1]):
//   r  RENDER SWEEP (headless): sweep draw count; for each level measure, per frame,
//      (1) CPU RECORD time = QPC around the draw-record loop ONLY (no GPU wait), and
//      (2) GPU FINISH time = QPC around CopyResource(staging,rt)+Map+Unmap (the wait
//          for the GPU to actually complete the frame). This splits CPU-record from
//          GPU work. Sweeps the draw counts given (default 500 1000 2000 4000) and
//          also reports triangles & us/draw. Linear scaling of GPU-finish with draw/
//          triangle count => GPU fill/vertex bound; flat => CPU/submission bound.
//   p  PRESENT TEST (windowed, DISPLAY=:0, software llvmpipe window): a swapchain
//      Present() loop with a trivial scene; measure ms/present to quantify the
//      software-window present overhead vs offscreen render. ONE small run only.
//
// Build (x86 emulated win64):
//   x86_64-w64-mingw32-clang++ realbench.cpp -o realbench.exe -ld3d11 -ld3dcompiler -O2
// Run:
//   d3drun realbench.exe r [frames] [res] [drawCounts...]      (headless sweep)
//   d3drun realbench.exe p [frames] [W] [H]                    (windowed present)

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>

struct V { float x, y, z, u, v; };

// Two PSOs (different bytecode -> different VkPipeline). Both sample a texture and
// apply a per-draw transform from a constant buffer (offset + depth + tint).
static const char* SH =
"cbuffer C:register(b0){float4 xf;};\n"        // xf = (offX, offY, depth, tint)
"Texture2D tx:register(t0);\n"
"SamplerState smp:register(s0);\n"
"struct VO{float4 p:SV_POSITION;float2 uv:TEXCOORD;float t:TINT;};\n"
"VO vsA(float3 pos:POSITION,float2 uv:TEXCOORD){VO o;o.p=float4(pos.xy*0.06+xf.xy,xf.z,1);o.uv=uv;o.t=xf.w;return o;}\n"
"VO vsB(float3 pos:POSITION,float2 uv:TEXCOORD){VO o;o.p=float4(pos.xy*0.055+xf.xy,xf.z,1);o.uv=uv;o.t=xf.w*0.9;return o;}\n"
"float4 psA(VO i):SV_TARGET{float4 c=tx.Sample(smp,i.uv);return c*i.t;}\n"
"float4 psB(VO i):SV_TARGET{float4 c=tx.Sample(smp,i.uv);return c.bgra*i.t;}\n";

static double now_ms(LARGE_INTEGER fr) {
  LARGE_INTEGER t; QueryPerformanceCounter(&t);
  return 1000.0 * (double)t.QuadPart / (double)fr.QuadPart;
}
static ID3DBlob* compile(const char* sh, const char* entry, const char* prof) {
  ID3DBlob *b = 0, *e = 0;
  D3DCompile(sh, strlen(sh), 0, 0, 0, entry, prof, 0, 0, &b, &e);
  if (e) { printf("compile %s err: %s\n", entry, (char*)e->GetBufferPointer()); }
  return b;
}

// ----------------------------------------------------------------------------
// RENDER SWEEP (headless RTT + depth)
// ----------------------------------------------------------------------------
static int runRender(int frames, int RES, int* counts, int nCounts) {
  ID3D11Device* dev = 0; ID3D11DeviceContext* ctx = 0; D3D_FEATURE_LEVEL fl;
  HRESULT hr = D3D11CreateDevice(0, D3D_DRIVER_TYPE_HARDWARE, 0, 0, 0, 0,
                                 D3D11_SDK_VERSION, &dev, &fl, &ctx);
  printf("device hr=0x%08lx FL=0x%x RES=%dx%d frames=%d\n", hr, fl, RES, RES, frames);
  fflush(stdout);
  if (FAILED(hr)) return 1;

  // RTT + depth
  D3D11_TEXTURE2D_DESC rd = {};
  rd.Width = RES; rd.Height = RES; rd.MipLevels = 1; rd.ArraySize = 1;
  rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1;
  rd.Usage = D3D11_USAGE_DEFAULT; rd.BindFlags = D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D* rt = 0; dev->CreateTexture2D(&rd, 0, &rt);
  ID3D11RenderTargetView* rtv = 0; dev->CreateRenderTargetView(rt, 0, &rtv);

  D3D11_TEXTURE2D_DESC dd = {};
  dd.Width = RES; dd.Height = RES; dd.MipLevels = 1; dd.ArraySize = 1;
  dd.Format = DXGI_FORMAT_D32_FLOAT; dd.SampleDesc.Count = 1;
  dd.Usage = D3D11_USAGE_DEFAULT; dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  ID3D11Texture2D* dt = 0; dev->CreateTexture2D(&dd, 0, &dt);
  ID3D11DepthStencilView* dsv = 0; dev->CreateDepthStencilView(dt, 0, &dsv);
  D3D11_DEPTH_STENCIL_DESC dsd = {}; dsd.DepthEnable = TRUE;
  dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
  ID3D11DepthStencilState* dso = 0; dev->CreateDepthStencilState(&dsd, &dso);

  D3D11_TEXTURE2D_DESC sd = rd; sd.BindFlags = 0;
  sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D* stg = 0; dev->CreateTexture2D(&sd, 0, &stg);

  // shaders
  ID3DBlob* vbA = compile(SH, "vsA", "vs_4_0");
  ID3DBlob* vbB = compile(SH, "vsB", "vs_4_0");
  ID3DBlob* pbA = compile(SH, "psA", "ps_4_0");
  ID3DBlob* pbB = compile(SH, "psB", "ps_4_0");
  if (!vbA || !vbB || !pbA || !pbB) return 2;
  ID3D11VertexShader *vsA = 0, *vsB = 0;
  dev->CreateVertexShader(vbA->GetBufferPointer(), vbA->GetBufferSize(), 0, &vsA);
  dev->CreateVertexShader(vbB->GetBufferPointer(), vbB->GetBufferSize(), 0, &vsB);
  ID3D11PixelShader *psA = 0, *psB = 0;
  dev->CreatePixelShader(pbA->GetBufferPointer(), pbA->GetBufferSize(), 0, &psA);
  dev->CreatePixelShader(pbB->GetBufferPointer(), pbB->GetBufferSize(), 0, &psB);

  D3D11_INPUT_ELEMENT_DESC il[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}};
  ID3D11InputLayout* lay = 0;
  dev->CreateInputLayout(il, 2, vbA->GetBufferPointer(), vbA->GetBufferSize(), &lay);

  // a quad (2 tris) centered at origin; vsA scales it small and offsets via cbuf
  V quad[6] = {
    {-1,-1,0, 0,1},{-1, 1,0, 0,0},{ 1, 1,0, 1,0},
    {-1,-1,0, 0,1},{ 1, 1,0, 1,0},{ 1,-1,0, 1,1}};
  D3D11_BUFFER_DESC bd = {sizeof(quad), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER};
  D3D11_SUBRESOURCE_DATA s = {quad};
  ID3D11Buffer* vbuf = 0; dev->CreateBuffer(&bd, &s, &vbuf);

  // per-draw constant buffer (DYNAMIC, updated each draw with transform)
  D3D11_BUFFER_DESC cbd = {16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE};
  ID3D11Buffer* cbuf = 0; dev->CreateBuffer(&cbd, 0, &cbuf);

  // a real texture (32x32 RGBA checker) + sampler
  const int TW = 32; unsigned int* tpx = (unsigned int*)malloc(TW*TW*4);
  for (int y = 0; y < TW; y++) for (int x = 0; x < TW; x++)
    tpx[y*TW+x] = ((x^y)&4) ? 0xFFFF80FF : 0xFF2080FF;
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = TW; td.Height = TW; td.MipLevels = 1; td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA tsd = {tpx, TW*4, 0};
  ID3D11Texture2D* tex = 0; dev->CreateTexture2D(&td, &tsd, &tex);
  ID3D11ShaderResourceView* srv = 0; dev->CreateShaderResourceView(tex, 0, &srv);
  free(tpx);
  D3D11_SAMPLER_DESC smd = {}; smd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
  ID3D11SamplerState* smp = 0; dev->CreateSamplerState(&smd, &smp);

  D3D11_VIEWPORT vp = {0, 0, (float)RES, (float)RES, 0, 1};
  UINT st = sizeof(V), of = 0;
  float clr[4] = {0.05f, 0.05f, 0.08f, 1};

  // record ONE frame of `draws` textured, depth-tested, PSO-alternating quads.
  // Quads are scattered across the screen and depth so fill + depth test do work.
  auto recordFrame = [&](int draws) {
    ctx->ClearRenderTargetView(rtv, clr);
    ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &rtv, dsv);
    ctx->OMSetDepthStencilState(dso, 0);
    ctx->IASetInputLayout(lay);
    ctx->IASetVertexBuffers(0, 1, &vbuf, &st, &of);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetConstantBuffers(0, 1, &cbuf);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &smp);
    for (int d = 0; d < draws; d++) {
      // alternate PSO every 64 draws -> a "few alternating PSOs" (realistic, not every draw)
      if ((d & 63) == 0) {
        if ((d >> 6) & 1) { ctx->VSSetShader(vsB, 0, 0); ctx->PSSetShader(psB, 0, 0); }
        else              { ctx->VSSetShader(vsA, 0, 0); ctx->PSSetShader(psA, 0, 0); }
      }
      // scatter position + depth (pseudo-random but deterministic)
      float fx = ((d * 2654435761u) & 1023) / 1023.0f * 1.8f - 0.9f;
      float fy = ((d * 40503u)      & 1023) / 1023.0f * 1.8f - 0.9f;
      float fz = ((d * 2246822519u) & 1023) / 1023.0f * 0.9f + 0.05f;
      float xf[4] = {fx, fy, fz, 1.0f};
      D3D11_MAPPED_SUBRESOURCE mp;
      ctx->Map(cbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp);
      memcpy(mp.pData, xf, 16);
      ctx->Unmap(cbuf, 0);
      ctx->Draw(6, 0);
    }
  };

  printf("\n%-7s %-9s %-11s %-11s %-11s %-9s\n",
         "draws", "tris", "cpuRecMs", "gpuFinMs", "totalMs", "usPerDraw");
  fflush(stdout);

  LARGE_INTEGER fr; QueryPerformanceFrequency(&fr);

  for (int ci = 0; ci < nCounts; ci++) {
    int draws = counts[ci];
    // warmup: one full frame (force pipeline compile + first-use off the clock)
    recordFrame(draws);
    ctx->CopyResource(stg, rt);
    D3D11_MAPPED_SUBRESOURCE wmp;
    if (FAILED(ctx->Map(stg, 0, D3D11_MAP_READ, 0, &wmp))) { printf("warmup map fail\n"); return 4; }
    {
      unsigned char* c = (unsigned char*)wmp.pData + (RES/2)*wmp.RowPitch + (RES/2)*4;
      printf("[warmup draws=%d center RGB=%d,%d,%d %s]\n", draws, c[0], c[1], c[2],
             (c[0]>8||c[1]>8||c[2]>8) ? "drew" : "BLACK?!");
    }
    ctx->Unmap(stg, 0);
    fflush(stdout);

    double cpuSum = 0, gpuSum = 0;
    for (int f = 0; f < frames; f++) {
      double t0 = now_ms(fr);
      recordFrame(draws);            // CPU record only (DXVK records, CS thread async)
      double t1 = now_ms(fr);
      ctx->CopyResource(stg, rt);    // forces GPU finish on Map below
      D3D11_MAPPED_SUBRESOURCE mp;
      if (FAILED(ctx->Map(stg, 0, D3D11_MAP_READ, 0, &mp))) { printf("map fail f=%d\n", f); return 4; }
      ctx->Unmap(stg, 0);
      double t2 = now_ms(fr);
      cpuSum += (t1 - t0);
      gpuSum += (t2 - t1);
    }
    double cpuMs = cpuSum / frames;
    double gpuMs = gpuSum / frames;
    double totMs = cpuMs + gpuMs;
    double usPerDraw = 1000.0 * totMs / draws;
    long tris = (long)draws * 2;
    printf("%-7d %-9ld %-11.4f %-11.4f %-11.4f %-9.3f  -> %s\n",
           draws, tris, cpuMs, gpuMs, totMs, usPerDraw,
           gpuMs > cpuMs*1.5 ? "GPU-bound" : (cpuMs > gpuMs*1.5 ? "CPU-record-bound" : "balanced"));
    fflush(stdout);
  }
  printf("RENDER_SWEEP_DONE\n"); fflush(stdout);
  return 0;
}

// ----------------------------------------------------------------------------
// PRESENT TEST (windowed swapchain, software llvmpipe window)
// ----------------------------------------------------------------------------
static int runPresent(int frames, int W, int H) {
  WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(0);
  wc.lpszClassName = "rb"; RegisterClassA(&wc);
  HWND h = CreateWindowA("rb", "realbench present", WS_OVERLAPPEDWINDOW, 80, 80, W, H, 0, 0, wc.hInstance, 0);
  ShowWindow(h, SW_SHOW);

  DXGI_SWAP_CHAIN_DESC scd = {};
  scd.BufferCount = 2; scd.BufferDesc.Width = W; scd.BufferDesc.Height = H;
  scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; scd.OutputWindow = h;
  scd.SampleDesc.Count = 1; scd.Windowed = TRUE; scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  ID3D11Device* dev = 0; ID3D11DeviceContext* ctx = 0; IDXGISwapChain* sc = 0; D3D_FEATURE_LEVEL fl;
  HRESULT hr = D3D11CreateDeviceAndSwapChain(0, D3D_DRIVER_TYPE_HARDWARE, 0, 0, 0, 0,
                 D3D11_SDK_VERSION, &scd, &sc, &dev, &fl, &ctx);
  printf("present device hr=0x%08lx FL=0x%x %dx%d frames=%d\n", hr, fl, W, H, frames);
  fflush(stdout);
  if (FAILED(hr)) return 1;

  ID3D11Texture2D* bb = 0; sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
  ID3D11RenderTargetView* rtv = 0; dev->CreateRenderTargetView(bb, 0, &rtv);
  D3D11_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1};

  LARGE_INTEGER fr; QueryPerformanceFrequency(&fr);

  // warmup few frames
  for (int i = 0; i < 5; i++) {
    float clr[4] = {0.1f, 0.2f, 0.3f, 1};
    ctx->ClearRenderTargetView(rtv, clr);
    ctx->RSSetViewports(1, &vp); ctx->OMSetRenderTargets(1, &rtv, 0);
    sc->Present(0, 0);
    MSG ms; while (PeekMessageA(&ms, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&ms); DispatchMessageA(&ms); }
  }

  // Split: clear+record vs Present(). Present with vsync OFF (interval 0) to measure
  // raw present path cost, not display refresh wait.
  double clearSum = 0, presentSum = 0;
  for (int i = 0; i < frames; i++) {
    float c = (i & 1) ? 0.2f : 0.4f;
    float clr[4] = {c, 0.2f, 0.3f, 1};
    double t0 = now_ms(fr);
    ctx->ClearRenderTargetView(rtv, clr);
    ctx->RSSetViewports(1, &vp); ctx->OMSetRenderTargets(1, &rtv, 0);
    double t1 = now_ms(fr);
    sc->Present(0, 0);
    double t2 = now_ms(fr);
    clearSum += (t1 - t0);
    presentSum += (t2 - t1);
    MSG ms; while (PeekMessageA(&ms, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&ms); DispatchMessageA(&ms); }
  }
  double clearMs = clearSum / frames, presentMs = presentSum / frames;
  printf("PRESENT result: clear/recordMs=%.4f presentMs=%.4f totalMsPerFrame=%.4f fps=%.1f\n",
         clearMs, presentMs, clearMs + presentMs, 1000.0 / (clearMs + presentMs));
  printf("PRESENT_DONE\n"); fflush(stdout);
  return 0;
}

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "r";
  if (mode[0] == 'p') {
    int frames = argc > 2 ? atoi(argv[2]) : 600;
    int W = argc > 3 ? atoi(argv[3]) : 800;
    int H = argc > 4 ? atoi(argv[4]) : 600;
    return runPresent(frames, W, H);
  }
  // render sweep
  int frames = argc > 2 ? atoi(argv[2]) : 400;
  int RES = argc > 3 ? atoi(argv[3]) : 512;
  int counts[16]; int n = 0;
  for (int i = 4; i < argc && n < 16; i++) counts[n++] = atoi(argv[i]);
  if (n == 0) { counts[n++] = 500; counts[n++] = 1000; counts[n++] = 2000; counts[n++] = 4000; }
  return runRender(frames, RES, counts, n);
}
