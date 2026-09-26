#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdlib>
struct Vx{float x,y,u,v;};
int main(){
  ID3D11Device*dev=0;ID3D11DeviceContext*ctx=0;D3D_FEATURE_LEVEL fl;
  if(FAILED(D3D11CreateDevice(0,D3D_DRIVER_TYPE_HARDWARE,0,0,0,0,D3D11_SDK_VERSION,&dev,&fl,&ctx)))return 1;
  const int W=64,H=64;
  D3D11_TEXTURE2D_DESC rd={};rd.Width=W;rd.Height=H;rd.MipLevels=1;rd.ArraySize=1;rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;rd.SampleDesc.Count=1;rd.Usage=D3D11_USAGE_DEFAULT;rd.BindFlags=D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D*rt=0;dev->CreateTexture2D(&rd,0,&rt);ID3D11RenderTargetView*rtv=0;dev->CreateRenderTargetView(rt,0,&rtv);
  // REAL BC1 texture: 4x4, color0=red(0xF800) color1=green(0x07E0), idx top=0(red) bottom=1(green)
  unsigned char blk[16]={0xc8,0xc8,0x0,0x0,0x0,0x0,0x0,0x0,0x64,0x64,0x0,0x0,0x0,0x0,0x0,0x0};
  D3D11_TEXTURE2D_DESC td={};td.Width=4;td.Height=4;td.MipLevels=1;td.ArraySize=1;td.Format=DXGI_FORMAT_BC5_UNORM;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA tsd={blk,16,16}; // BC1: pitch=8 bytes per block-row
  ID3D11Texture2D*tex=0;HRESULT h=dev->CreateTexture2D(&td,&tsd,&tex);printf("BC5 Create hr=0x%08lx\n",h);if(FAILED(h))return 2;
  ID3D11ShaderResourceView*srv=0;h=dev->CreateShaderResourceView(tex,0,&srv);printf("BC5 SRV hr=0x%08lx\n",h);if(FAILED(h))return 3;
  D3D11_SAMPLER_DESC smd={};smd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;smd.AddressU=smd.AddressV=smd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
  ID3D11SamplerState*smp=0;dev->CreateSamplerState(&smd,&smp);
  const char*SH="Texture2D t:register(t0);SamplerState s:register(s0);struct VO{float4 p:SV_POSITION;float2 uv:TEXCOORD;};VO vs(float2 pos:POSITION,float2 uv:TEXCOORD){VO o;o.p=float4(pos,0,1);o.uv=uv;return o;}float4 ps(VO i):SV_TARGET{return t.Sample(s,i.uv);}";
  ID3DBlob*vb=0,*pb=0,*e=0;D3DCompile(SH,strlen(SH),0,0,0,"vs","vs_4_0",0,0,&vb,&e);D3DCompile(SH,strlen(SH),0,0,0,"ps","ps_4_0",0,0,&pb,&e);
  ID3D11VertexShader*vs=0;dev->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),0,&vs);
  ID3D11PixelShader*ps=0;dev->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),0,&ps);
  D3D11_INPUT_ELEMENT_DESC il[]={{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA,0}};
  ID3D11InputLayout*lay=0;dev->CreateInputLayout(il,2,vb->GetBufferPointer(),vb->GetBufferSize(),&lay);
  Vx v[6]={{-1,-1,0,1},{-1,1,0,0},{1,1,1,0},{-1,-1,0,1},{1,1,1,0},{1,-1,1,1}};
  D3D11_BUFFER_DESC bd={sizeof(v),D3D11_USAGE_DEFAULT,D3D11_BIND_VERTEX_BUFFER};D3D11_SUBRESOURCE_DATA sr={v};ID3D11Buffer*vbuf=0;dev->CreateBuffer(&bd,&sr,&vbuf);
  float clr[4]={0,0,0,1};ctx->ClearRenderTargetView(rtv,clr);D3D11_VIEWPORT vp={0,0,W,H,0,1};ctx->RSSetViewports(1,&vp);
  ctx->OMSetRenderTargets(1,&rtv,0);ctx->IASetInputLayout(lay);UINT st=sizeof(Vx),of=0;ctx->IASetVertexBuffers(0,1,&vbuf,&st,&of);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->VSSetShader(vs,0,0);ctx->PSSetShader(ps,0,0);ctx->PSSetShaderResources(0,1,&srv);ctx->PSSetSamplers(0,1,&smp);ctx->Draw(6,0);
  D3D11_TEXTURE2D_DESC sd=rd;sd.BindFlags=0;sd.Usage=D3D11_USAGE_STAGING;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ID3D11Texture2D*stg=0;dev->CreateTexture2D(&sd,0,&stg);ctx->CopyResource(stg,rt);
  D3D11_MAPPED_SUBRESOURCE m;if(FAILED(ctx->Map(stg,0,D3D11_MAP_READ,0,&m)))return 4;
  unsigned char*c=(unsigned char*)m.pData+32*m.RowPitch+32*4;
  printf("BC5 sampled=%d,%d,%d (want (200, 100, 0))\n",c[0],c[1],c[2]);
  int ok=(abs(c[0]-200)<25&&abs(c[1]-100)<25&&abs(c[2]-0)<25);ctx->Unmap(stg,0);
  printf(ok?"BC5_OK\n":"BC5_FAIL\n");return ok?0:5;
}
