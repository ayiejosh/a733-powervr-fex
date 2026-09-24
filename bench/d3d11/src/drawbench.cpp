// drawbench.cpp — Phase-2 CHARACTERIZE: per-draw CPU submission overhead (the
// "draw-call wall") on the SHIPPING DXVK-Sarek stack. MEASURE-BEFORE-OPTIMIZE.
//
// Times us/draw under DISTINCT state-change cases, each toggled by argv so they
// run as separate processes (clean isolation, one wedge-risk run at a time):
//
//   a  baseline   : N identical Draw()s, NO state change between them
//   b  pso        : alternate between 2 different PSOs (VS/PS pairs) each draw
//   c  cbuf       : UpdateSubresource a constant buffer each draw (+VSSetCB)
//   d  srv        : rebind an SRV/texture (PSSetShaderResources) each draw
//   e  vbuf       : rebind a vertex buffer (IASetVertexBuffers) each draw
//   f  drawidx    : DrawIndexed each draw (vs baseline Draw)
//   g  drawinst   : DrawInstanced each draw (vs baseline Draw)
//
// Timing harness is identical to gsbench.cpp (proven on this board):
//   QueryPerformanceCounter around the per-frame draw loop; every frame ends
//   with CopyResource(staging,rt)+Map+Unmap to force a full GPU finish (CPU
//   blocks until GPU done). Warmup frame forces pipeline/shader compile off the
//   clock. ~2000 frames x N draws. Reports us/draw + draws/sec.
//
// NOTE: because each frame forces a GPU finish, us/draw includes a per-FRAME
// GPU-finish/submit cost amortized across N draws. With large N (default 256)
// that per-frame fixed cost is amortized down so the per-draw delta between
// cases isolates the per-draw CPU recording/state cost — which is the wall.
//
// Build: x86_64-w64-mingw32-clang++ drawbench.cpp -o drawbench.exe \
//        -ld3d11 -ld3dcompiler -O2
// Run:   d3drun drawbench.exe <case> [frames] [drawsPerFrame]
//        e.g. d3drun drawbench.exe a 2000 256

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

struct V { float x, y; };

// Two distinct VS/PS pairs so case (b) really swaps pipelines (different
// bytecode => different VkPipeline in DXVK). Both draw the same triangle.
static const char* SH =
"struct VO{float4 p:SV_POSITION;float3 c:COLOR;};\n"
"VO vsA(float2 pos:POSITION){VO o;o.p=float4(pos,0,1);o.c=float3(1,0,0);return o;}\n"
"VO vsB(float2 pos:POSITION){VO o;o.p=float4(pos*0.99,0,1);o.c=float3(0,0,1);return o;}\n"
"float4 psA(VO i):SV_TARGET{return float4(i.c,1);}\n"
"float4 psB(VO i):SV_TARGET{return float4(i.c.bgr,1);}\n";

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

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "a";
  int frames = argc > 2 ? atoi(argv[2]) : 2000;
  int drawsPerFrame = argc > 3 ? atoi(argv[3]) : 256;
  if (frames < 1) frames = 1;
  if (drawsPerFrame < 1) drawsPerFrame = 1;

  char m = mode[0];
  const char* desc =
    m=='a'?"baseline (no state change)":
    m=='b'?"PSO swap each draw":
    m=='c'?"constant-buffer update each draw":
    m=='d'?"SRV rebind each draw":
    m=='e'?"vertex-buffer rebind each draw":
    m=='f'?"DrawIndexed each draw":
    m=='g'?"DrawInstanced each draw":"UNKNOWN";
  printf("drawbench case=%c (%s) frames=%d drawsPerFrame=%d\n",
         m, desc, frames, drawsPerFrame); fflush(stdout);

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

  D3D11_TEXTURE2D_DESC sd = rd; sd.BindFlags = 0;
  sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D* stg = 0; dev->CreateTexture2D(&sd, 0, &stg);

  // shaders: A and B pairs
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
    {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
  ID3D11InputLayout* lay = 0;
  dev->CreateInputLayout(il, 1, vbA->GetBufferPointer(), vbA->GetBufferSize(), &lay);

  V v[3] = {{0, 0.9f}, {0.9f, -0.9f}, {-0.9f, -0.9f}};
  D3D11_BUFFER_DESC bd = {sizeof(v), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER};
  D3D11_SUBRESOURCE_DATA s = {v};
  // two vertex buffers (identical data) for case (e) rebind
  ID3D11Buffer *vbuf0 = 0, *vbuf1 = 0;
  dev->CreateBuffer(&bd, &s, &vbuf0);
  dev->CreateBuffer(&bd, &s, &vbuf1);

  // index buffer for case (f)
  unsigned short idx[3] = {0, 1, 2};
  D3D11_BUFFER_DESC ibd = {sizeof(idx), D3D11_USAGE_DEFAULT, D3D11_BIND_INDEX_BUFFER};
  D3D11_SUBRESOURCE_DATA is = {idx};
  ID3D11Buffer* ibuf = 0; dev->CreateBuffer(&ibd, &is, &ibuf);

  // constant buffer for case (c)
  struct CB { float v[4]; } cbdata = {{1,1,1,1}};
  D3D11_BUFFER_DESC cbd = {sizeof(CB), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
  D3D11_SUBRESOURCE_DATA cs = {&cbdata};
  ID3D11Buffer* cbuf = 0; dev->CreateBuffer(&cbd, &cs, &cbuf);

  // two textures + SRVs for case (d) rebind
  unsigned int texel = 0xFF00FF00;
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = 1; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA tdata = {&texel, 4, 0};
  ID3D11Texture2D *tx0 = 0, *tx1 = 0;
  dev->CreateTexture2D(&td, &tdata, &tx0);
  dev->CreateTexture2D(&td, &tdata, &tx1);
  ID3D11ShaderResourceView *srv0 = 0, *srv1 = 0;
  dev->CreateShaderResourceView(tx0, 0, &srv0);
  dev->CreateShaderResourceView(tx1, 0, &srv1);

  D3D11_VIEWPORT vp = {0, 0, W, H, 0, 1};
  UINT st = sizeof(V), of = 0;
  float clr[4] = {0, 0, 0, 1};

  // set the constant, fixed pipeline state once outside the timed inner ops
  auto setupFrame = [&]() {
    ctx->ClearRenderTargetView(rtv, clr);
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &rtv, 0);
    ctx->IASetInputLayout(lay);
    ctx->IASetVertexBuffers(0, 1, &vbuf0, &st, &of);
    ctx->IASetIndexBuffer(ibuf, DXGI_FORMAT_R16_UINT, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vsA, 0, 0);
    ctx->PSSetShader(psA, 0, 0);
    ctx->VSSetConstantBuffers(0, 1, &cbuf);
    ctx->PSSetShaderResources(0, 1, &srv0);
  };

  // The per-draw inner body for the selected case.
  auto innerDraw = [&](int d) {
    switch (m) {
      case 'a': // baseline: no state change
        ctx->Draw(3, 0);
        break;
      case 'b': { // PSO swap each draw
        if (d & 1) { ctx->VSSetShader(vsB, 0, 0); ctx->PSSetShader(psB, 0, 0); }
        else       { ctx->VSSetShader(vsA, 0, 0); ctx->PSSetShader(psA, 0, 0); }
        ctx->Draw(3, 0);
        break; }
      case 'c': { // constant-buffer update each draw
        CB cb = {{(float)(d&255)/255.0f,1,1,1}};
        ctx->UpdateSubresource(cbuf, 0, 0, &cb, 0, 0);
        ctx->Draw(3, 0);
        break; }
      case 'd': // SRV rebind each draw
        ctx->PSSetShaderResources(0, 1, (d&1) ? &srv1 : &srv0);
        ctx->Draw(3, 0);
        break;
      case 'e': // vertex-buffer rebind each draw
        ctx->IASetVertexBuffers(0, 1, (d&1) ? &vbuf1 : &vbuf0, &st, &of);
        ctx->Draw(3, 0);
        break;
      case 'f': // DrawIndexed
        ctx->DrawIndexed(3, 0, 0);
        break;
      case 'g': // DrawInstanced
        ctx->DrawInstanced(3, 1, 0, 0);
        break;
      default:
        ctx->Draw(3, 0);
    }
  };

  // ---- warmup: one full frame to force pipeline/shader compile off the clock.
  // Touch BOTH PSOs / both SRVs / both VBs so every variant's pipeline is hot.
  setupFrame();
  ctx->VSSetShader(vsB, 0, 0); ctx->PSSetShader(psB, 0, 0); ctx->Draw(3, 0);
  ctx->VSSetShader(vsA, 0, 0); ctx->PSSetShader(psA, 0, 0); ctx->Draw(3, 0);
  ctx->IASetVertexBuffers(0, 1, &vbuf1, &st, &of); ctx->Draw(3, 0);
  ctx->IASetVertexBuffers(0, 1, &vbuf0, &st, &of);
  ctx->PSSetShaderResources(0, 1, &srv1); ctx->Draw(3, 0);
  ctx->PSSetShaderResources(0, 1, &srv0);
  ctx->DrawIndexed(3, 0, 0);
  ctx->DrawInstanced(3, 1, 0, 0);
  for (int d = 0; d < drawsPerFrame; d++) innerDraw(d);
  ctx->CopyResource(stg, rt);
  {
    D3D11_MAPPED_SUBRESOURCE mp;
    if (FAILED(ctx->Map(stg, 0, D3D11_MAP_READ, 0, &mp))) { printf("map fail\n"); return 4; }
    unsigned char* c = (unsigned char*)mp.pData + 32 * mp.RowPitch + 32 * 4;
    printf("warmup center RGB=%d,%d,%d %s\n", c[0], c[1], c[2],
           (c[0]>10||c[1]>10||c[2]>10) ? "(drew OK)" : "(BLACK?!)");
    ctx->Unmap(stg, 0);
  }
  fflush(stdout);

  // ---- timed loop
  LARGE_INTEGER fr; QueryPerformanceFrequency(&fr);
  double tStart = now_ms(fr);
  for (int f = 0; f < frames; f++) {
    setupFrame();
    for (int d = 0; d < drawsPerFrame; d++) innerDraw(d);
    ctx->CopyResource(stg, rt);
    D3D11_MAPPED_SUBRESOURCE mp;
    if (FAILED(ctx->Map(stg, 0, D3D11_MAP_READ, 0, &mp))) { printf("map fail f=%d\n", f); return 4; }
    ctx->Unmap(stg, 0);
  }
  double tEnd = now_ms(fr);

  double total = tEnd - tStart;
  double perFrame = total / frames;
  double usPerDraw = 1000.0 * perFrame / drawsPerFrame;
  double drawsPerSec = usPerDraw > 0 ? 1e6 / usPerDraw : 0;
  printf("RESULT case=%c frames=%d drawsPerFrame=%d totalMs=%.2f msPerFrame=%.4f "
         "usPerDraw=%.3f drawsPerSec=%.0f\n",
         m, frames, drawsPerFrame, total, perFrame, usPerDraw, drawsPerSec);
  fflush(stdout);
  return 0;
}
