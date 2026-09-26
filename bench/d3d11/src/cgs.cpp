// Validate the compute-GS ARCHITECTURE: a compute shader AMPLIFIES geometry
// (4 input points -> 4 quads = 24 verts) into a buffer + writes indirect draw args,
// then DrawInstancedIndirect rasterizes them. This is exactly what a GS-via-compute
// lowering does. If it renders on PowerVR, the M1 architecture is feasible+valid.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
struct Vtx{ float x,y; float r,g,b,pad; };
const char* CS=
"struct Pt{float2 p; float3 c;};\n"
"StructuredBuffer<Pt> pts:register(t0);\n"
"struct V{float2 p; float3 c; float pad;};\n"
"RWStructuredBuffer<V> outv:register(u0);\n"
"RWByteAddressBuffer args:register(u1);\n"
"[numthreads(1,1,1)]\n"
"void cs(uint3 id:SV_DispatchThreadID){\n"
"  Pt q=pts[id.x]; float s=0.18;\n"
"  float2 o[6]={float2(-s,-s),float2(-s,s),float2(s,s),float2(-s,-s),float2(s,s),float2(s,-s)};\n"
"  for(int k=0;k<6;k++){uint idx=id.x*6+k; outv[idx].p=q.p+o[k]; outv[idx].c=q.c; outv[idx].pad=0;}\n"
"  if(id.x==0){ args.Store(0, 4*6); args.Store(4,1); args.Store(8,0); args.Store(12,0);} // DrawInstanced args: vtxCount,instCount,startV,startI\n"
"}\n";
const char* GFX=
"struct V{float2 p; float3 c; float pad;};\n"
"StructuredBuffer<V> verts:register(t0);\n"
"struct VO{float4 p:SV_POSITION; float3 c:COLOR;};\n"
"VO vs(uint id:SV_VertexID){VO o; V v=verts[id]; o.p=float4(v.p,0,1); o.c=v.c; return o;}\n"
"float4 ps(VO i):SV_TARGET{return float4(i.c,1);}\n";
int main(){
  ID3D11Device*dev=0;ID3D11DeviceContext*ctx=0;D3D_FEATURE_LEVEL fl;
  if(FAILED(D3D11CreateDevice(0,D3D_DRIVER_TYPE_HARDWARE,0,0,0,0,D3D11_SDK_VERSION,&dev,&fl,&ctx)))return 1;
  const int W=256,H=256;
  D3D11_TEXTURE2D_DESC rd={};rd.Width=W;rd.Height=H;rd.MipLevels=1;rd.ArraySize=1;rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;rd.SampleDesc.Count=1;rd.Usage=D3D11_USAGE_DEFAULT;rd.BindFlags=D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D*rt=0;dev->CreateTexture2D(&rd,0,&rt);ID3D11RenderTargetView*rtv=0;dev->CreateRenderTargetView(rt,0,&rtv);
  // input points (4 quadrants, distinct colors)
  struct Pt{float p[2];float c[3];} pts[4]={{{-0.5f,-0.5f},{1,0,0}},{{0.5f,-0.5f},{0,1,0}},{{-0.5f,0.5f},{0,0,1}},{{0.5f,0.5f},{1,1,0}}};
  D3D11_BUFFER_DESC pb={sizeof(pts),D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,sizeof(Pt)};
  D3D11_SUBRESOURCE_DATA ps_={pts};ID3D11Buffer*ptbuf=0;dev->CreateBuffer(&pb,&ps_,&ptbuf);
  ID3D11ShaderResourceView*ptsrv=0;{D3D11_SHADER_RESOURCE_VIEW_DESC d={};d.Format=DXGI_FORMAT_UNKNOWN;d.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;d.Buffer.NumElements=4;dev->CreateShaderResourceView(ptbuf,&d,&ptsrv);}
  // output verts (24) structured, UAV + SRV
  D3D11_BUFFER_DESC ob={sizeof(Vtx)*24,D3D11_USAGE_DEFAULT,D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE,0,D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,sizeof(Vtx)};
  ID3D11Buffer*outbuf=0;HRESULT h=dev->CreateBuffer(&ob,0,&outbuf);printf("outbuf hr=0x%08lx\n",h);
  ID3D11UnorderedAccessView*outuav=0;{D3D11_UNORDERED_ACCESS_VIEW_DESC d={};d.Format=DXGI_FORMAT_UNKNOWN;d.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;d.Buffer.NumElements=24;dev->CreateUnorderedAccessView(outbuf,&d,&outuav);}
  ID3D11ShaderResourceView*outsrv=0;{D3D11_SHADER_RESOURCE_VIEW_DESC d={};d.Format=DXGI_FORMAT_UNKNOWN;d.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;d.Buffer.NumElements=24;dev->CreateShaderResourceView(outbuf,&d,&outsrv);}
  // indirect args buffer (raw)
  D3D11_BUFFER_DESC ab={16,D3D11_USAGE_DEFAULT,D3D11_BIND_UNORDERED_ACCESS,0,D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS|D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS};
  ID3D11Buffer*argbuf=0;h=dev->CreateBuffer(&ab,0,&argbuf);printf("argbuf hr=0x%08lx\n",h);
  ID3D11UnorderedAccessView*arguav=0;{D3D11_UNORDERED_ACCESS_VIEW_DESC d={};d.Format=DXGI_FORMAT_R32_TYPELESS;d.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;d.Buffer.NumElements=4;d.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;dev->CreateUnorderedAccessView(argbuf,&d,&arguav);}
  // compile + create CS
  ID3DBlob*cb=0,*e=0;h=D3DCompile(CS,strlen(CS),0,0,0,"cs","cs_5_0",0,0,&cb,&e);if(e)printf("CS:%s\n",(char*)e->GetBufferPointer());printf("CS compile=0x%08lx\n",h);if(FAILED(h))return 2;
  ID3D11ComputeShader*cs=0;dev->CreateComputeShader(cb->GetBufferPointer(),cb->GetBufferSize(),0,&cs);
  // GFX shaders
  ID3DBlob*vb=0,*pbl=0;D3DCompile(GFX,strlen(GFX),0,0,0,"vs","vs_5_0",0,0,&vb,&e);if(e)printf("VS:%s\n",(char*)e->GetBufferPointer());D3DCompile(GFX,strlen(GFX),0,0,0,"ps","ps_5_0",0,0,&pbl,&e);
  ID3D11VertexShader*vs=0;dev->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),0,&vs);ID3D11PixelShader*pps=0;dev->CreatePixelShader(pbl->GetBufferPointer(),pbl->GetBufferSize(),0,&pps);
  // PASS 1+2: compute amplifies geometry + writes indirect args
  ctx->CSSetShader(cs,0,0);ctx->CSSetShaderResources(0,1,&ptsrv);ID3D11UnorderedAccessView*uavs[2]={outuav,arguav};UINT z[2]={0,0};ctx->CSSetUnorderedAccessViews(0,2,uavs,z);
  ctx->Dispatch(4,1,1);
  ID3D11UnorderedAccessView*nul[2]={0,0};ctx->CSSetUnorderedAccessViews(0,2,nul,z);
  // PASS 3: indirect draw of the compute-generated geometry
  float clr[4]={0.05f,0.05f,0.08f,1};ctx->ClearRenderTargetView(rtv,clr);D3D11_VIEWPORT vp={0,0,W,H,0,1};ctx->RSSetViewports(1,&vp);
  ctx->OMSetRenderTargets(1,&rtv,0);ctx->VSSetShader(vs,0,0);ctx->VSSetShaderResources(0,1,&outsrv);ctx->PSSetShader(pps,0,0);
  ctx->IASetInputLayout(0);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->DrawInstancedIndirect(argbuf,0);
  // readback: check the 4 quads rendered with their colors
  D3D11_TEXTURE2D_DESC sd=rd;sd.BindFlags=0;sd.Usage=D3D11_USAGE_STAGING;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ID3D11Texture2D*stg=0;dev->CreateTexture2D(&sd,0,&stg);ctx->CopyResource(stg,rt);
  D3D11_MAPPED_SUBRESOURCE m;if(FAILED(ctx->Map(stg,0,D3D11_MAP_READ,0,&m))){printf("map FAIL\n");return 3;}
  auto px=[&](int x,int y){return (unsigned char*)m.pData+y*m.RowPitch+x*4;};
  unsigned char*bl=px(64,192),*br=px(192,192),*tl=px(64,64),*tr=px(192,64);// quadrants (y down)
  printf("BL=%d,%d,%d(redA) BR=%d,%d,%d(greenA) TL=%d,%d,%d(blueA) TR=%d,%d,%d(yellowA)\n",bl[0],bl[1],bl[2],br[0],br[1],br[2],tl[0],tl[1],tl[2],tr[0],tr[1],tr[2]);
  int ok=(bl[0]>180&&bl[1]<60)&&(br[1]>180&&br[0]<60)&&(tl[2]>180&&tl[0]<60)&&(tr[0]>180&&tr[1]>180);
  ctx->Unmap(stg,0);printf(ok?"COMPUTE_GS_ARCH_OK compute amplified 4pts->4quads, indirect-drawn on GPU\n":"COMPUTE_GS_ARCH_FAIL\n");return ok?0:4;
}
