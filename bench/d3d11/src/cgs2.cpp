// M2a: VARIABLE-count amplification (real GS emit different vertex counts per prim).
// Compute: triangle i emits (i+1) copies via InterlockedAdd append into one buffer;
// the indirect draw's vertexCount is accumulated atomically. Validates the dynamic case.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
struct Vtx{ float x,y; float r,g,b,pad; };
const char* CS=
"struct Pt{float2 p; float3 c; float n;};\n"
"StructuredBuffer<Pt> pts:register(t0);\n"
"struct V{float2 p; float3 c; float pad;};\n"
"RWStructuredBuffer<V> outv:register(u0);\n"
"RWStructuredBuffer<uint> counter:register(u1);\n"   // counter[0]=vertexCount (atomic)
"[numthreads(1,1,1)]\n"
"void cs(uint3 id:SV_DispatchThreadID){\n"
"  Pt q=pts[id.x]; int n=(int)q.n; float s=0.06;\n"   // emit n triangles
"  float2 tri[3]={float2(0,s*1.5),float2(s,-s),float2(-s,-s)};\n"
"  for(int t=0;t<n;t++){\n"
"    uint base; InterlockedAdd(counter[0], 3, base);\n"     // atomic-append 3 verts, get offset
"    float2 off=q.p+float2(t*0.05-0.05,0);\n"
"    for(int k=0;k<3;k++){ outv[base+k].p=off+tri[k]; outv[base+k].c=q.c; outv[base+k].pad=0; }\n"
"  }\n"
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
  // 4 input prims emitting 1,2,3,4 triangles -> total 10 tris = 30 verts
  struct Pt{float p[2];float c[3];float n;} pts[4]={{{-0.5f,-0.5f},{1,0,0},1},{{0.5f,-0.5f},{0,1,0},2},{{-0.5f,0.5f},{0,0,1},3},{{0.5f,0.5f},{1,1,0},4}};
  D3D11_BUFFER_DESC pb={sizeof(pts),D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,sizeof(Pt)};
  D3D11_SUBRESOURCE_DATA ps_={pts};ID3D11Buffer*ptbuf=0;dev->CreateBuffer(&pb,&ps_,&ptbuf);
  ID3D11ShaderResourceView*ptsrv=0;{D3D11_SHADER_RESOURCE_VIEW_DESC d={};d.Format=DXGI_FORMAT_UNKNOWN;d.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;d.Buffer.NumElements=4;dev->CreateShaderResourceView(ptbuf,&d,&ptsrv);}
  D3D11_BUFFER_DESC ob={(UINT)(sizeof(Vtx)*64),D3D11_USAGE_DEFAULT,D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE,0,D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,sizeof(Vtx)};
  ID3D11Buffer*outbuf=0;dev->CreateBuffer(&ob,0,&outbuf);
  ID3D11UnorderedAccessView*outuav=0;{D3D11_UNORDERED_ACCESS_VIEW_DESC d={};d.Format=DXGI_FORMAT_UNKNOWN;d.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;d.Buffer.NumElements=64;dev->CreateUnorderedAccessView(outbuf,&d,&outuav);}
  ID3D11ShaderResourceView*outsrv=0;{D3D11_SHADER_RESOURCE_VIEW_DESC d={};d.Format=DXGI_FORMAT_UNKNOWN;d.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;d.Buffer.NumElements=64;dev->CreateShaderResourceView(outbuf,&d,&outsrv);}
  // args init {0,1,0,0}: vertexCount accumulated atomically
  unsigned arginit[4]={0,1,0,0};
  D3D11_BUFFER_DESC ab={16,D3D11_USAGE_DEFAULT,D3D11_BIND_UNORDERED_ACCESS,0,D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS|D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS};
  D3D11_SUBRESOURCE_DATA as={arginit};ID3D11Buffer*argbuf=0;dev->CreateBuffer(&ab,&as,&argbuf);
  ID3D11UnorderedAccessView*arguav=0;{D3D11_UNORDERED_ACCESS_VIEW_DESC d={};d.Format=DXGI_FORMAT_R32_TYPELESS;d.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;d.Buffer.NumElements=4;d.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;dev->CreateUnorderedAccessView(argbuf,&d,&arguav);}
  // atomic counter (structured uint) init 0
  unsigned czero=0; D3D11_BUFFER_DESC cbd={4,D3D11_USAGE_DEFAULT,D3D11_BIND_UNORDERED_ACCESS,0,D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,4};
  D3D11_SUBRESOURCE_DATA cz={&czero}; ID3D11Buffer*cntbuf=0; dev->CreateBuffer(&cbd,&cz,&cntbuf);
  ID3D11UnorderedAccessView*cntuav=0;{D3D11_UNORDERED_ACCESS_VIEW_DESC d={};d.Format=DXGI_FORMAT_UNKNOWN;d.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;d.Buffer.NumElements=1;dev->CreateUnorderedAccessView(cntbuf,&d,&cntuav);}
  ID3DBlob*cb=0,*e=0;HRESULT h=D3DCompile(CS,strlen(CS),0,0,0,"cs","cs_5_0",0,0,&cb,&e);if(e)printf("CS:%s\n",(char*)e->GetBufferPointer());printf("CS compile=0x%08lx\n",h);if(FAILED(h))return 2;
  ID3D11ComputeShader*cs=0;dev->CreateComputeShader(cb->GetBufferPointer(),cb->GetBufferSize(),0,&cs);
  ID3DBlob*vb=0,*pbl=0;D3DCompile(GFX,strlen(GFX),0,0,0,"vs","vs_5_0",0,0,&vb,&e);D3DCompile(GFX,strlen(GFX),0,0,0,"ps","ps_5_0",0,0,&pbl,&e);
  ID3D11VertexShader*vs=0;dev->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),0,&vs);ID3D11PixelShader*pps=0;dev->CreatePixelShader(pbl->GetBufferPointer(),pbl->GetBufferSize(),0,&pps);
  ctx->CSSetShader(cs,0,0);ctx->CSSetShaderResources(0,1,&ptsrv);ID3D11UnorderedAccessView*uavs[2]={outuav,cntuav};UINT z[2]={0,0};ctx->CSSetUnorderedAccessViews(0,2,uavs,z);
  ctx->Dispatch(4,1,1);ID3D11UnorderedAccessView*nul[2]={0,0};ctx->CSSetUnorderedAccessViews(0,2,nul,z);
  {D3D11_BOX bx={0,0,0,4,1,1}; ctx->CopySubresourceRegion(argbuf,0,0,0,0,cntbuf,0,&bx);}
  // readback the accumulated vertexCount from args
  D3D11_BUFFER_DESC sb={16,D3D11_USAGE_STAGING,0,D3D11_CPU_ACCESS_READ};ID3D11Buffer*sargs=0;dev->CreateBuffer(&sb,0,&sargs);ctx->CopyResource(sargs,argbuf);
  D3D11_MAPPED_SUBRESOURCE ma;ctx->Map(sargs,0,D3D11_MAP_READ,0,&ma);unsigned vcount=((unsigned*)ma.pData)[0];ctx->Unmap(sargs,0);
  printf("computed vertexCount=%u (expect 30 = (1+2+3+4)*3)\n",vcount);
  float clr[4]={0.05f,0.05f,0.08f,1};ctx->ClearRenderTargetView(rtv,clr);D3D11_VIEWPORT vp={0,0,W,H,0,1};ctx->RSSetViewports(1,&vp);
  ctx->OMSetRenderTargets(1,&rtv,0);ctx->VSSetShader(vs,0,0);ctx->VSSetShaderResources(0,1,&outsrv);ctx->PSSetShader(pps,0,0);
  ctx->IASetInputLayout(0);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->DrawInstancedIndirect(argbuf,0);
  D3D11_TEXTURE2D_DESC sd=rd;sd.BindFlags=0;sd.Usage=D3D11_USAGE_STAGING;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ID3D11Texture2D*stg=0;dev->CreateTexture2D(&sd,0,&stg);ctx->CopyResource(stg,rt);
  D3D11_MAPPED_SUBRESOURCE m;ctx->Map(stg,0,D3D11_MAP_READ,0,&m);
  // count non-background pixels (rendered geometry present)
  int lit=0;for(int y=0;y<H;y+=4)for(int x=0;x<W;x+=4){unsigned char*p=(unsigned char*)m.pData+y*m.RowPitch+x*4;if(p[0]>100||p[1]>100||p[2]>120)lit++;}
  ctx->Unmap(stg,0);printf("lit blocks=%d\n",lit);
  int ok=(vcount==30)&&(lit>20);printf(ok?"DYN_AMPLIFY_OK variable-count append + indirect-from-counter works\n":"DYN_AMPLIFY_FAIL\n");return ok?0:4;
}
