/* vkformats - dump per-format feature bits in a diffable form.
 * Vulkan mandates support for specific formats; comparing against llvmpipe (which passes
 * conformance) shows where a driver is missing something it is required to have.
 *   VK_ICD_FILENAMES=<icd.json> ./vkformats > out.txt
 */
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
int main(void){
    VkApplicationInfo ai={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&ai};
    VkInstance inst; if(vkCreateInstance(&ici,NULL,&inst)!=VK_SUCCESS){puts("instance failed");return 1;}
    uint32_t n=0; vkEnumeratePhysicalDevices(inst,&n,NULL);
    VkPhysicalDevice*pd=malloc(n*sizeof(*pd)); vkEnumeratePhysicalDevices(inst,&n,pd);
    VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd[0],&p);
    printf("# device %s\n", p.deviceName);
    for (int f=1; f<=(int)VK_FORMAT_ASTC_12x12_SRGB_BLOCK; f++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(pd[0], (VkFormat)f, &fp);
        if (!fp.linearTilingFeatures && !fp.optimalTilingFeatures && !fp.bufferFeatures) continue;
        printf("fmt %4d lin=0x%08x opt=0x%08x buf=0x%08x\n", f,
               fp.linearTilingFeatures, fp.optimalTilingFeatures, fp.bufferFeatures);
    }
    return 0;
}
