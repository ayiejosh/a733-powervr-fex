#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
struct V{float x,y;};
const char* SH=
"struct VO{float4 p:SV_POSITION;};struct PO{float4 a:SV_TARGET0;float4 b:SV_TARGET1;};\n"
"VO vs(float2 pos:POSITION){VO o;o.p=float4(pos,0,1);return o;}\n"
"PO ps(VO i){PO o;o.a=float4(0,1,0,1);o.b=float4(1,0,1,1);return o;}\n";
int main(){ID3D11Device*dev=0;ID3D11DeviceContext*ctx=0;D3D_FEATURE_LEVEL fl;
 if(FAILED(D3D11CreateDevice(0,D3D_DRIVER_TYPE_HARDWARE,0,0,0,0,D3D11_SDK_VERSION,&dev,&fl,&ctx)))return 1;
 const int W=64,H=64;ID3D11RenderTargetView*rtv[2];ID3D11Texture2D*rt[2];
 D3D11_TEXTURE2D_DESC rd={};rd.Width=W;rd.Height=H;rd.MipLevels=1;rd.ArraySize=1;rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;rd.SampleDesc.Count=1;rd.Usage=D3D11_USAGE_DEFAULT;rd.BindFlags=D3D11_BIND_RENDER_TARGET;
 for(int i=0;i<2;i++){dev->CreateTexture2D(&rd,0,&rt[i]);dev->CreateRenderTargetView(rt[i],0,&rtv[i]);}
 ID3DBlob*vb=0,*pb=0,*e=0;D3DCompile(SH,strlen(SH),0,0,0,"vs","vs_4_0",0,0,&vb,&e);if(e)printf("%s",(char*)e->GetBufferPointer());
 D3DCompile(SH,strlen(SH),0,0,0,"ps","ps_4_0",0,0,&pb,&e);if(e)printf("%s",(char*)e->GetBufferPointer());
 ID3D11VertexShader*vs=0;dev->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),0,&vs);
 ID3D11PixelShader*ps=0;dev->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),0,&ps);
 D3D11_INPUT_ELEMENT_DESC il[]={{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0}};
 ID3D11InputLayout*lay=0;dev->CreateInputLayout(il,1,vb->GetBufferPointer(),vb->GetBufferSize(),&lay);
 V v[6]={{-1,-1},{-1,1},{1,1},{-1,-1},{1,1},{1,-1}};D3D11_BUFFER_DESC bd={sizeof(v),D3D11_USAGE_DEFAULT,D3D11_BIND_VERTEX_BUFFER};D3D11_SUBRESOURCE_DATA s={v};ID3D11Buffer*vbuf=0;dev->CreateBuffer(&bd,&s,&vbuf);
 float clr[4]={0,0,0,1};for(int i=0;i<2;i++)ctx->ClearRenderTargetView(rtv[i],clr);
 D3D11_VIEWPORT vp={0,0,W,H,0,1};ctx->RSSetViewports(1,&vp);ctx->OMSetRenderTargets(2,rtv,0);ctx->IASetInputLayout(lay);
 UINT st=sizeof(V),of=0;ctx->IASetVertexBuffers(0,1,&vbuf,&st,&of);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 ctx->VSSetShader(vs,0,0);ctx->PSSetShader(ps,0,0);ctx->Draw(6,0);
 unsigned char cc[2][4];for(int i=0;i<2;i++){D3D11_TEXTURE2D_DESC sd=rd;sd.BindFlags=0;sd.Usage=D3D11_USAGE_STAGING;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ID3D11Texture2D*stg=0;dev->CreateTexture2D(&sd,0,&stg);ctx->CopyResource(stg,rt[i]);
  D3D11_MAPPED_SUBRESOURCE m;if(FAILED(ctx->Map(stg,0,D3D11_MAP_READ,0,&m)))return 2;unsigned char*c=(unsigned char*)m.pData+32*m.RowPitch+32*4;cc[i][0]=c[0];cc[i][1]=c[1];cc[i][2]=c[2];ctx->Unmap(stg,0);}
 printf("RT0=%d,%d,%d(want green) RT1=%d,%d,%d(want magenta)\n",cc[0][0],cc[0][1],cc[0][2],cc[1][0],cc[1][1],cc[1][2]);
 int ok=(cc[0][1]>200&&cc[0][0]<50)&&(cc[1][0]>200&&cc[1][2]>200&&cc[1][1]<50);printf(ok?"MRT_OK\n":"MRT_FAIL\n");return ok?0:3;}
