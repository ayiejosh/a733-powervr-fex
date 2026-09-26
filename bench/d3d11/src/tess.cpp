#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
struct V{float x,y;};
const char* SH=
"struct VO{float4 p:SV_POSITION;};\n"
"VO vs(float2 pos:POSITION){VO o;o.p=float4(pos,0,1);return o;}\n"
"struct HSC{float e[3]:SV_TessFactor;float i:SV_InsideTessFactor;};\n"
"HSC hsc(InputPatch<VO,3> ip){HSC o;o.e[0]=o.e[1]=o.e[2]=2;o.i=2;return o;}\n"
"[domain(\"tri\")][partitioning(\"integer\")][outputtopology(\"triangle_cw\")][outputcontrolpoints(3)][patchconstantfunc(\"hsc\")]\n"
"VO hs(InputPatch<VO,3> ip,uint id:SV_OutputControlPointID){return ip[id];}\n"
"[domain(\"tri\")]VO ds(HSC c,float3 uvw:SV_DomainLocation,const OutputPatch<VO,3> p){VO o;o.p=p[0].p*uvw.x+p[1].p*uvw.y+p[2].p*uvw.z;return o;}\n"
"float4 ps(VO i):SV_TARGET{return float4(0,1,1,1);}\n";
int main(){ID3D11Device*dev=0;ID3D11DeviceContext*ctx=0;D3D_FEATURE_LEVEL fl;
 if(FAILED(D3D11CreateDevice(0,D3D_DRIVER_TYPE_HARDWARE,0,0,0,0,D3D11_SDK_VERSION,&dev,&fl,&ctx)))return 1;
 const int W=64,H=64;D3D11_TEXTURE2D_DESC rd={};rd.Width=W;rd.Height=H;rd.MipLevels=1;rd.ArraySize=1;rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;rd.SampleDesc.Count=1;rd.Usage=D3D11_USAGE_DEFAULT;rd.BindFlags=D3D11_BIND_RENDER_TARGET;
 ID3D11Texture2D*rt=0;dev->CreateTexture2D(&rd,0,&rt);ID3D11RenderTargetView*rtv=0;dev->CreateRenderTargetView(rt,0,&rtv);
 ID3DBlob*vbl=0,*hb=0,*db=0,*pb=0,*e=0;
 D3DCompile(SH,strlen(SH),0,0,0,"vs","vs_5_0",0,0,&vbl,&e);
 HRESULT hh=D3DCompile(SH,strlen(SH),0,0,0,"hs","hs_5_0",0,0,&hb,&e);if(e)printf("hs:%s",(char*)e->GetBufferPointer());printf("HS compile=0x%08lx\n",hh);
 HRESULT hd=D3DCompile(SH,strlen(SH),0,0,0,"ds","ds_5_0",0,0,&db,&e);if(e)printf("ds:%s",(char*)e->GetBufferPointer());printf("DS compile=0x%08lx\n",hd);
 D3DCompile(SH,strlen(SH),0,0,0,"ps","ps_5_0",0,0,&pb,&e);
 ID3D11VertexShader*vs=0;dev->CreateVertexShader(vbl->GetBufferPointer(),vbl->GetBufferSize(),0,&vs);
 ID3D11HullShader*hs=0;HRESULT ch=dev->CreateHullShader(hb->GetBufferPointer(),hb->GetBufferSize(),0,&hs);printf("CreateHull=0x%08lx\n",ch);
 ID3D11DomainShader*ds=0;HRESULT cd=dev->CreateDomainShader(db->GetBufferPointer(),db->GetBufferSize(),0,&ds);printf("CreateDomain=0x%08lx\n",cd);
 ID3D11PixelShader*ps=0;dev->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),0,&ps);
 D3D11_INPUT_ELEMENT_DESC il[]={{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0}};
 ID3D11InputLayout*lay=0;dev->CreateInputLayout(il,1,vbl->GetBufferPointer(),vbl->GetBufferSize(),&lay);
 V v[3]={{0,0.8f},{0.8f,-0.8f},{-0.8f,-0.8f}};D3D11_BUFFER_DESC bd={sizeof(v),D3D11_USAGE_DEFAULT,D3D11_BIND_VERTEX_BUFFER};D3D11_SUBRESOURCE_DATA s={v};ID3D11Buffer*vbuf=0;dev->CreateBuffer(&bd,&s,&vbuf);
 float clr[4]={0,0,0,1};ctx->ClearRenderTargetView(rtv,clr);D3D11_VIEWPORT vp={0,0,W,H,0,1};ctx->RSSetViewports(1,&vp);
 ctx->OMSetRenderTargets(1,&rtv,0);ctx->IASetInputLayout(lay);UINT st=sizeof(V),of=0;ctx->IASetVertexBuffers(0,1,&vbuf,&st,&of);
 ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
 ctx->VSSetShader(vs,0,0);ctx->HSSetShader(hs,0,0);ctx->DSSetShader(ds,0,0);ctx->PSSetShader(ps,0,0);ctx->Draw(3,0);
 D3D11_TEXTURE2D_DESC sd=rd;sd.BindFlags=0;sd.Usage=D3D11_USAGE_STAGING;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ID3D11Texture2D*stg=0;dev->CreateTexture2D(&sd,0,&stg);ctx->CopyResource(stg,rt);
 D3D11_MAPPED_SUBRESOURCE m;if(FAILED(ctx->Map(stg,0,D3D11_MAP_READ,0,&m)))return 2;unsigned char*c=(unsigned char*)m.pData+40*m.RowPitch+32*4;
 printf("center RGB=%d,%d,%d (cyan=tess ran, black=nothing)\n",c[0],c[1],c[2]);int ok=(c[1]>200&&c[2]>200&&c[0]<50);ctx->Unmap(stg,0);
 printf(ok?"TESS_OK\n":"TESS_FAIL\n");return ok?0:3;}
