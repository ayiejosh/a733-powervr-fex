#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
int main(void){
  uint32_t n=0; VkResult r;
  r=vkEnumerateInstanceExtensionProperties(NULL,&n,NULL);
  printf("  instance extensions : %u%s\n",n,r?" (err)":"");
  uint32_t nl=0; vkEnumerateInstanceLayerProperties(&nl,NULL);
  printf("  instance layers     : %u\n",nl);
  VkApplicationInfo ai={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_3};
  VkInstanceCreateInfo ci={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&ai};
  VkInstance inst; if(vkCreateInstance(&ci,NULL,&inst)!=VK_SUCCESS){puts("  no instance");return 1;}
  uint32_t nd=0; vkEnumeratePhysicalDevices(inst,&nd,NULL);
  printf("  physical devices    : %u\n",nd);
  if(!nd) return 1;
  VkPhysicalDevice pd[8]; nd=nd>8?8:nd; vkEnumeratePhysicalDevices(inst,&nd,pd);
  VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd[0],&p);
  printf("  device              : %s  api %u.%u.%u  driver %u\n",p.deviceName,
     VK_VERSION_MAJOR(p.apiVersion),VK_VERSION_MINOR(p.apiVersion),VK_VERSION_PATCH(p.apiVersion),p.driverVersion);
  uint32_t ne=0; vkEnumerateDeviceExtensionProperties(pd[0],NULL,&ne,NULL);
  printf("  DEVICE EXTENSIONS   : %u\n",ne);
  VkExtensionProperties *ep=malloc(ne*sizeof(*ep)); vkEnumerateDeviceExtensionProperties(pd[0],NULL,&ne,ep);
  for(uint32_t i=0;i<ne;i++) printf("      %s\n",ep[i].extensionName);
  VkPhysicalDeviceFeatures f; vkGetPhysicalDeviceFeatures(pd[0],&f);
  unsigned on=0,tot=0; unsigned char *b=(unsigned char*)&f;
  for(size_t i=0;i<sizeof(f);i++){ tot+=8; for(int k=0;k<8;k++) on+=(b[i]>>k)&1; }
  printf("  DEVICE FEATURES     : %u of %u VkBool32 bits set\n",on,tot);
  /* named ones that matter for this comparison */
  printf("      shaderFloat64=%d shaderInt64=%d\n",f.shaderFloat64,f.shaderInt64);
  printf("      geometryShader=%d tessellationShader=%d\n",f.geometryShader,f.tessellationShader);
  printf("      depthClamp=%d depthBiasClamp=%d fillModeNonSolid=%d wideLines=%d\n",f.depthClamp,f.depthBiasClamp,f.fillModeNonSolid,f.wideLines);
  printf("      independentBlend=%d dualSrcBlend=%d multiViewport=%d samplerAnisotropy=%d\n",f.independentBlend,f.dualSrcBlend,f.multiViewport,f.samplerAnisotropy);
  printf("      textureCompressionBC=%d textureCompressionETC2=%d textureCompressionASTC=%d\n",f.textureCompressionBC,f.textureCompressionETC2,f.textureCompressionASTC_LDR);
  return 0;
}
