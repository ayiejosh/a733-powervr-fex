// gsbench.cpp — TIMED benchmark of the DXVK compute-GS emulation penalty.
//
// Renders MANY frames of a geometry-shader workload (a GS that consumes a
// triangle and emits it back, tinting green — same GS as gs.exe), measuring
// wall-clock ms/frame. Then times an EQUIVALENT non-GS workload (the same
// triangles drawn directly through VS->PS, no GS) as the baseline. Reports
// both + the ratio (the emulation penalty).
//
// Timing: QueryPerformanceCounter around the per-frame draw loop, and every
// frame ends with CopyResource(staging,rt)+Map+Unmap which forces a full GPU
// finish (the CPU cannot read the staging texture until the GPU has completed
// all prior work). So each measured frame includes real GPU execution time,
// not just command submission.
//
// Headless: renders to an offscreen R8G8B8A8 render target (no swapchain),
// exactly like gs.exe, so it runs with no window/compositor.
//
// Build: x86_64-w64-mingw32-clang++ gsbench.cpp -o gsbench.exe \
//        -ld3d11 -ld3dcompiler -O2
//
// Run (GS path):    DXVK build-ec dll + dxvk.conf d3d11.emulateGeometryShaders=True
//   d3drun gsbench.exe gs   <frames> <drawsPerFrame>
// Run (baseline):   shipping BCn dll, no emulation
//   d3drun gsbench.exe nogs <frames> <drawsPerFrame>
//
// The two modes draw the SAME geometry (one triangle per draw). In "gs" mode
// the triangle passes through the geometry shader (emulated as compute on this
// target); in "nogs" mode it goes VS->PS directly. The per-draw triangle is
// identical so the only difference is the GS-emulation overhead.

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

struct V { float x, y; };

// Same shader family as gs.exe. The GS consumes one triangle and re-emits it,
// tinting the color green so we can verify it actually ran. The VS/PS are
// shared between both modes; only the GS is bound in "gs" mode.
static const char* SH =
"struct VO{float4 p:SV_POSITION;float3 c:COLOR;};\n"
"VO vs(float2 pos:POSITION){VO o;o.p=float4(pos,0,1);o.c=float3(1,0,0);return o;}\n"
"[maxvertexcount(3)]void gs(triangle VO i[3],inout TriangleStream<VO> s){"
"  for(int k=0;k<3;k++){VO o=i[k];o.c=float3(0,1,0);s.Append(o);}}\n"
"float4 ps(VO i):SV_TARGET{return float4(i.c,1);}\n";

static double now_ms(LARGE_INTEGER fr) {
  LARGE_INTEGER t; QueryPerformanceCounter(&t);
  return 1000.0 * (double)t.QuadPart / (double)fr.QuadPart;
}

int main(int argc, char** argv) {
  // mode: "gs" (geometry-shader path) or "nogs" (direct baseline)
  const char* mode = argc > 1 ? argv[1] : "gs";
  int useGs = (strcmp(mode, "nogs") != 0);
  int frames = argc > 2 ? atoi(argv[2]) : 2000;
  int drawsPerFrame = argc > 3 ? atoi(argv[3]) : 64;
  if (frames < 1) frames = 1;
  if (drawsPerFrame < 1) drawsPerFrame = 1;

  printf("gsbench mode=%s frames=%d drawsPerFrame=%d\n",
         useGs ? "gs(GS-emulated)" : "nogs(baseline)", frames, drawsPerFrame);
  fflush(stdout);

  ID3D11Device* dev = 0; ID3D11DeviceContext* ctx = 0; D3D_FEATURE_LEVEL fl;
  HRESULT hr = D3D11CreateDevice(0, D3D_DRIVER_TYPE_HARDWARE, 0, 0, 0, 0,
                                 D3D11_SDK_VERSION, &dev, &fl, &ctx);
  printf("device hr=0x%08lx FL=0x%x\n", hr, fl); fflush(stdout);
  if (FAILED(hr)) return 1;

  const int W = 64, H = 64;
  D3D11_TEXTURE2D_DESC rd = {};
  rd.Width = W; rd.Height = H; rd.MipLevels = 1; rd.ArraySize = 1;
  rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1;
  rd.Usage = D3D11_USAGE_DEFAULT; rd.BindFlags = D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D* rt = 0; dev->CreateTexture2D(&rd, 0, &rt);
  ID3D11RenderTargetView* rtv = 0; dev->CreateRenderTargetView(rt, 0, &rtv);

  // staging texture used to force a GPU finish each frame
  D3D11_TEXTURE2D_DESC sd = rd; sd.BindFlags = 0;
  sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D* stg = 0; dev->CreateTexture2D(&sd, 0, &stg);

  ID3DBlob *vb = 0, *gb = 0, *pb = 0, *e = 0;
  D3DCompile(SH, strlen(SH), 0, 0, 0, "vs", "vs_4_0", 0, 0, &vb, &e);
  if (e) { printf("vs err:%s\n", (char*)e->GetBufferPointer()); return 2; }
  HRESULT hg = D3DCompile(SH, strlen(SH), 0, 0, 0, "gs", "gs_4_0", 0, 0, &gb, &e);
  if (e) printf("gs err:%s\n", (char*)e->GetBufferPointer());
  printf("GS compile hr=0x%08lx\n", hg); fflush(stdout);
  D3DCompile(SH, strlen(SH), 0, 0, 0, "ps", "ps_4_0", 0, 0, &pb, &e);
  if (e) { printf("ps err:%s\n", (char*)e->GetBufferPointer()); return 2; }

  ID3D11VertexShader* vs = 0;
  dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), 0, &vs);
  ID3D11GeometryShader* gs = 0;
  if (useGs) {
    HRESULT hcg = dev->CreateGeometryShader(gb->GetBufferPointer(),
                                            gb->GetBufferSize(), 0, &gs);
    printf("CreateGeometryShader hr=0x%08lx\n", hcg); fflush(stdout);
    if (FAILED(hcg)) { printf("GS create FAILED, abort\n"); return 3; }
  }
  ID3D11PixelShader* ps = 0;
  dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), 0, &ps);

  D3D11_INPUT_ELEMENT_DESC il[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
  ID3D11InputLayout* lay = 0;
  dev->CreateInputLayout(il, 1, vb->GetBufferPointer(), vb->GetBufferSize(), &lay);

  // a big triangle so each draw produces real fragment work (overdraw)
  V v[3] = {{0, 0.9f}, {0.9f, -0.9f}, {-0.9f, -0.9f}};
  D3D11_BUFFER_DESC bd = {sizeof(v), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER};
  D3D11_SUBRESOURCE_DATA s = {v};
  ID3D11Buffer* vbuf = 0; dev->CreateBuffer(&bd, &s, &vbuf);

  D3D11_VIEWPORT vp = {0, 0, W, H, 0, 1};
  UINT st = sizeof(V), of = 0;
  float clr[4] = {0, 0, 0, 1};

  // ---- warmup: one full frame to force pipeline/shader compilation off the clock
  ctx->ClearRenderTargetView(rtv, clr);
  ctx->RSSetViewports(1, &vp);
  ctx->OMSetRenderTargets(1, &rtv, 0);
  ctx->IASetInputLayout(lay);
  ctx->IASetVertexBuffers(0, 1, &vbuf, &st, &of);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(vs, 0, 0);
  ctx->GSSetShader(useGs ? gs : 0, 0, 0);
  ctx->PSSetShader(ps, 0, 0);
  for (int d = 0; d < drawsPerFrame; d++) ctx->Draw(3, 0);
  ctx->CopyResource(stg, rt);
  {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(stg, 0, D3D11_MAP_READ, 0, &m))) { printf("map fail\n"); return 4; }
    // verify the workload actually rendered what we expect
    unsigned char* c = (unsigned char*)m.pData + 32 * m.RowPitch + 32 * 4;
    int green = (c[1] > 200 && c[0] < 50), red = (c[0] > 200 && c[1] < 50);
    printf("warmup center RGB=%d,%d,%d -> %s\n", c[0], c[1], c[2],
           green ? "GREEN(GS ran)" : red ? "RED(VS-only)" : "BLACK(nothing)");
    if (useGs && !green) printf("WARN: GS mode but not green — emulation may not be active\n");
    if (!useGs && !red)  printf("WARN: baseline mode but not red\n");
    ctx->Unmap(stg, 0);
  }
  fflush(stdout);

  // ---- timed loop
  LARGE_INTEGER fr; QueryPerformanceFrequency(&fr);
  double tStart = now_ms(fr);
  for (int f = 0; f < frames; f++) {
    ctx->ClearRenderTargetView(rtv, clr);
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &rtv, 0);
    ctx->IASetInputLayout(lay);
    ctx->IASetVertexBuffers(0, 1, &vbuf, &st, &of);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, 0, 0);
    ctx->GSSetShader(useGs ? gs : 0, 0, 0);
    ctx->PSSetShader(ps, 0, 0);
    for (int d = 0; d < drawsPerFrame; d++) ctx->Draw(3, 0);
    // force GPU finish: copy to staging and map (CPU blocks until GPU done)
    ctx->CopyResource(stg, rt);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(stg, 0, D3D11_MAP_READ, 0, &m))) { printf("map fail f=%d\n", f); return 4; }
    ctx->Unmap(stg, 0);
  }
  double tEnd = now_ms(fr);

  double total = tEnd - tStart;
  double perFrame = total / frames;
  printf("RESULT mode=%s frames=%d drawsPerFrame=%d totalMs=%.2f msPerFrame=%.4f usPerDraw=%.3f\n",
         useGs ? "gs" : "nogs", frames, drawsPerFrame, total, perFrame,
         1000.0 * perFrame / drawsPerFrame);
  fflush(stdout);
  return 0;
}
