#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
const char* CS=
"RWStructuredBuffer<uint> buf:register(u0);\n"
"[numthreads(64,1,1)]\n"
"void cs(uint3 id:SV_DispatchThreadID){ buf[id.x]=id.x*id.x + 7; }\n"; // GPGPU: square+7
int main(){
  ID3D11Device*dev=0; ID3D11DeviceContext*ctx=0; D3D_FEATURE_LEVEL fl;
  HRESULT hr=D3D11CreateDevice(0,D3D_DRIVER_TYPE_HARDWARE,0,0,0,0,D3D11_SDK_VERSION,&dev,&fl,&ctx);
  printf("device hr=0x%08lx FL=0x%x\n",hr,fl); fflush(stdout); if(FAILED(hr))return 1;
  const int N=4096;
  D3D11_BUFFER_DESC bd={}; bd.ByteWidth=N*4; bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
  bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride=4;
  ID3D11Buffer*buf=0; hr=dev->CreateBuffer(&bd,0,&buf); printf("buffer hr=0x%08lx\n",hr);
  D3D11_UNORDERED_ACCESS_VIEW_DESC ud={}; ud.Format=DXGI_FORMAT_UNKNOWN;
  ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements=N;
  ID3D11UnorderedAccessView*uav=0; hr=dev->CreateUnorderedAccessView(buf,&ud,&uav); printf("UAV hr=0x%08lx\n",hr);
  ID3DBlob*cb=0,*e=0; hr=D3DCompile(CS,strlen(CS),0,0,0,"cs","cs_5_0",0,0,&cb,&e);
  if(e)printf("%s\n",(char*)e->GetBufferPointer());
  printf("CS compile hr=0x%08lx\n",hr); fflush(stdout); if(FAILED(hr))return 2;
  ID3D11ComputeShader*cs=0; hr=dev->CreateComputeShader(cb->GetBufferPointer(),cb->GetBufferSize(),0,&cs);
  printf("CreateComputeShader hr=0x%08lx\n",hr); fflush(stdout); if(FAILED(hr))return 3;
  ctx->CSSetShader(cs,0,0); UINT z=0; ctx->CSSetUnorderedAccessViews(0,1,&uav,&z);
  ctx->Dispatch(N/64,1,1);
  D3D11_BUFFER_DESC sd={}; sd.ByteWidth=N*4; sd.Usage=D3D11_USAGE_STAGING; sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
  ID3D11Buffer*stg=0; dev->CreateBuffer(&sd,0,&stg); ctx->CopyResource(stg,buf);
  D3D11_MAPPED_SUBRESOURCE m; hr=ctx->Map(stg,0,D3D11_MAP_READ,0,&m);
  printf("Map hr=0x%08lx\n",hr); fflush(stdout); if(FAILED(hr))return 4;
  unsigned* d=(unsigned*)m.pData; int ok=1; unsigned s0=d[0],s100=d[100],s1023=d[1023];
  for(int i=0;i<N;i++){ if(d[i]!=(unsigned)(i*i+7)){ printf("MISMATCH at %d: got %u want %u\n",i,d[i],i*i+7); ok=0; break; } }
  ctx->Unmap(stg,0);
  printf("GPGPU samples buf[0]=%u buf[100]=%u buf[1023]=%u (want 7,10007,1046536)\n",s0,s100,s1023);
  printf(ok?"COMPUTE_OK all %d values correct\n":"COMPUTE_FAIL\n",N); fflush(stdout);
  return ok?0:5;
}
