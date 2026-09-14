/*
 * VK_LAYER_PVR_strip — an implicit Vulkan layer that makes zink accept the
 * closed PowerVR BXM-4-64 Vulkan driver, which is missing two features
 * zink hard-requires:
 *
 *   - VkPhysicalDeviceFeatures.geometryShader        (blob reports false)
 *   - VK_EXT_robustness2 / nullDescriptor            (blob doesn't know the
 *                                                      extension at all)
 *
 * It fakes both being present at query time (vkGetPhysicalDeviceFeatures[2],
 * vkEnumerateDeviceExtensionProperties), then strips them back out of the
 * VkDeviceCreateInfo before forwarding to the real driver at vkCreateDevice,
 * so the blob is never actually asked to enable a feature it doesn't have.
 *
 * This is an independent reimplementation of the technique described (but
 * not published as source) in ayiejosh/a733-powervr-fex's zink-trixie.md
 * ("the feature-strip layer") — see ARMBIAN-REPLICATION.md in this repo for
 * the full writeup of why it was needed and what it replaces.
 *
 * Faking real GS pipelines this way would crash the blob — this only fakes
 * the two feature bits zink's *initialization* checks for; GS-using GL
 * content was never a target here (matches the reference's own caveat).
 *
 * Entirely inert unless PVR_FAKE_GS=1 is set (see the .json manifest's
 * enable_environment) — never touches Vulkan apps that don't opt in.
 *
 * Single-instance/single-device only (no attempt at a real dispatch-table
 * map keyed by the dispatchable handle) — sufficient for the off-screen
 * test tools this was built for (eglinfo, glmark2-es2, vkcube-style single
 * instance apps), not general-purpose-correct for multi-instance apps.
 */

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>
#include <stdlib.h>
#include <string.h>

#define VK_LAYER_EXPORT __attribute__((visibility("default")))
#define FAKE_EXT_NAME "VK_EXT_robustness2"

typedef struct {
    VkInstance instance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
    PFN_vkGetPhysicalDeviceFeatures GetPhysicalDeviceFeatures;
    PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2;
    PFN_vkGetPhysicalDeviceFeatures2KHR GetPhysicalDeviceFeatures2KHR;
    PFN_vkCreateDevice CreateDevice;
} InstanceData;

typedef struct {
    VkDevice device;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkDestroyDevice DestroyDevice;
} DeviceData;

static InstanceData g_inst;
static DeviceData g_dev;

static int enabled(void) {
    const char *e = getenv("PVR_FAKE_GS");
    return e && e[0] && e[0] != '0';
}

static VkLayerInstanceCreateInfo *get_instance_chain_info(const VkInstanceCreateInfo *pCreateInfo, VkLayerFunction func) {
    VkLayerInstanceCreateInfo *info = (VkLayerInstanceCreateInfo *)pCreateInfo->pNext;
    while (info) {
        if (info->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && info->function == func) return info;
        info = (VkLayerInstanceCreateInfo *)info->pNext;
    }
    return NULL;
}

static VkLayerDeviceCreateInfo *get_device_chain_info(const VkDeviceCreateInfo *pCreateInfo, VkLayerFunction func) {
    VkLayerDeviceCreateInfo *info = (VkLayerDeviceCreateInfo *)pCreateInfo->pNext;
    while (info) {
        if (info->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && info->function == func) return info;
        info = (VkLayerDeviceCreateInfo *)info->pNext;
    }
    return NULL;
}

/* ---- vkCreateInstance / vkDestroyInstance ---- */

static VKAPI_ATTR VkResult VKAPI_CALL PVRSTRIP_CreateInstance(
    const VkInstanceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkInstance *pInstance)
{
    VkLayerInstanceCreateInfo *chain_info = get_instance_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);
    if (!chain_info) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkCreateInstance fpCreateInstance = (PFN_vkCreateInstance)fpGetInstanceProcAddr(NULL, "vkCreateInstance");
    if (!fpCreateInstance) return VK_ERROR_INITIALIZATION_FAILED;

    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    VkResult result = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS) return result;

    memset(&g_inst, 0, sizeof(g_inst));
    g_inst.instance = *pInstance;
    g_inst.GetInstanceProcAddr = fpGetInstanceProcAddr;
    g_inst.DestroyInstance = (PFN_vkDestroyInstance)fpGetInstanceProcAddr(*pInstance, "vkDestroyInstance");
    g_inst.EnumerateDeviceExtensionProperties = (PFN_vkEnumerateDeviceExtensionProperties)fpGetInstanceProcAddr(*pInstance, "vkEnumerateDeviceExtensionProperties");
    g_inst.GetPhysicalDeviceFeatures = (PFN_vkGetPhysicalDeviceFeatures)fpGetInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceFeatures");
    g_inst.GetPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2)fpGetInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceFeatures2");
    g_inst.GetPhysicalDeviceFeatures2KHR = (PFN_vkGetPhysicalDeviceFeatures2KHR)fpGetInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceFeatures2KHR");
    g_inst.CreateDevice = (PFN_vkCreateDevice)fpGetInstanceProcAddr(*pInstance, "vkCreateDevice");
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL PVRSTRIP_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator) {
    PFN_vkDestroyInstance destroy = g_inst.DestroyInstance;
    memset(&g_inst, 0, sizeof(g_inst));
    if (destroy) destroy(instance, pAllocator);
}

/* ---- feature faking on the query side ---- */

static void patch_robustness2(void *pNextChain, VkBool32 value) {
    VkBaseOutStructure *s = (VkBaseOutStructure *)pNextChain;
    while (s) {
        if ((int)s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_KHR) {
            ((VkPhysicalDeviceRobustness2FeaturesKHR *)s)->nullDescriptor = value;
        }
        s = s->pNext;
    }
}

static VKAPI_ATTR void VKAPI_CALL PVRSTRIP_GetPhysicalDeviceFeatures(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures *pFeatures)
{
    g_inst.GetPhysicalDeviceFeatures(physicalDevice, pFeatures);
    if (enabled()) pFeatures->geometryShader = VK_TRUE;
}

static VKAPI_ATTR void VKAPI_CALL PVRSTRIP_GetPhysicalDeviceFeatures2(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2 *pFeatures)
{
    g_inst.GetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
    if (enabled()) {
        pFeatures->features.geometryShader = VK_TRUE;
        patch_robustness2(pFeatures->pNext, VK_TRUE);
    }
}

static VKAPI_ATTR void VKAPI_CALL PVRSTRIP_GetPhysicalDeviceFeatures2KHR(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2KHR *pFeatures)
{
    g_inst.GetPhysicalDeviceFeatures2KHR(physicalDevice, pFeatures);
    if (enabled()) {
        pFeatures->features.geometryShader = VK_TRUE;
        patch_robustness2(pFeatures->pNext, VK_TRUE);
    }
}

/* ---- vkEnumerateDeviceExtensionProperties: append VK_EXT_robustness2 ---- */

static VKAPI_ATTR VkResult VKAPI_CALL PVRSTRIP_EnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice, const char *pLayerName,
    uint32_t *pPropertyCount, VkExtensionProperties *pProperties)
{
    if (!enabled() || pLayerName != NULL) {
        return g_inst.EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
    }

    uint32_t realCount = 0;
    VkResult res = g_inst.EnumerateDeviceExtensionProperties(physicalDevice, NULL, &realCount, NULL);
    if (res != VK_SUCCESS) return res;

    if (!pProperties) {
        *pPropertyCount = realCount + 1;
        return VK_SUCCESS;
    }

    VkExtensionProperties *real = malloc(sizeof(VkExtensionProperties) * (realCount ? realCount : 1));
    uint32_t got = realCount;
    res = g_inst.EnumerateDeviceExtensionProperties(physicalDevice, NULL, &got, real);
    if (res != VK_SUCCESS && res != VK_INCOMPLETE) { free(real); return res; }

    uint32_t toCopy = got < *pPropertyCount ? got : *pPropertyCount;
    memcpy(pProperties, real, toCopy * sizeof(VkExtensionProperties));
    free(real);

    VkResult finalResult = VK_SUCCESS;
    if (toCopy < *pPropertyCount) {
        VkExtensionProperties fake;
        memset(&fake, 0, sizeof(fake));
        strncpy(fake.extensionName, FAKE_EXT_NAME, VK_MAX_EXTENSION_NAME_SIZE - 1);
        fake.specVersion = 1;
        pProperties[toCopy] = fake;
        toCopy++;
    } else if (got + 1 > *pPropertyCount) {
        finalResult = VK_INCOMPLETE;
    }
    *pPropertyCount = toCopy;
    return finalResult;
}

/* ---- vkCreateDevice: strip the faked bits before forwarding for real ---- */

static VKAPI_ATTR VkResult VKAPI_CALL PVRSTRIP_CreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkDevice *pDevice)
{
    VkLayerDeviceCreateInfo *chain_info = get_device_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);
    if (!chain_info) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr fpGetDeviceProcAddr = chain_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    PFN_vkCreateDevice fpCreateDevice = (PFN_vkCreateDevice)fpGetInstanceProcAddr(g_inst.instance, "vkCreateDevice");
    if (!fpCreateDevice) return VK_ERROR_INITIALIZATION_FAILED;

    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    VkDeviceCreateInfo modCreateInfo = *pCreateInfo;
    const char **extNames = NULL;
    VkPhysicalDeviceFeatures modFeatures;

    if (enabled()) {
        /* drop the fake extension name so the real driver never sees it */
        if (pCreateInfo->enabledExtensionCount > 0) {
            extNames = malloc(sizeof(char *) * pCreateInfo->enabledExtensionCount);
            uint32_t j = 0;
            for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
                if (strcmp(pCreateInfo->ppEnabledExtensionNames[i], FAKE_EXT_NAME) == 0) continue;
                extNames[j++] = pCreateInfo->ppEnabledExtensionNames[i];
            }
            modCreateInfo.ppEnabledExtensionNames = extNames;
            modCreateInfo.enabledExtensionCount = j;
        }

        /* legacy pEnabledFeatures path */
        if (pCreateInfo->pEnabledFeatures) {
            modFeatures = *pCreateInfo->pEnabledFeatures;
            modFeatures.geometryShader = VK_FALSE;
            modCreateInfo.pEnabledFeatures = &modFeatures;
        }

        /* pNext-chain path: mutate the app's structs for the duration of
         * this call only, then restore them below. */
        VkBaseOutStructure *s = (VkBaseOutStructure *)modCreateInfo.pNext;
        while (s) {
            if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
                ((VkPhysicalDeviceFeatures2 *)s)->features.geometryShader = VK_FALSE;
            } else if ((int)s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_KHR) {
                ((VkPhysicalDeviceRobustness2FeaturesKHR *)s)->nullDescriptor = VK_FALSE;
            }
            s = s->pNext;
        }
    }

    VkResult result = fpCreateDevice(physicalDevice, &modCreateInfo, pAllocator, pDevice);
    free(extNames);

    if (enabled()) {
        VkBaseOutStructure *s = (VkBaseOutStructure *)pCreateInfo->pNext;
        while (s) {
            if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
                ((VkPhysicalDeviceFeatures2 *)s)->features.geometryShader = VK_TRUE;
            } else if ((int)s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_KHR) {
                ((VkPhysicalDeviceRobustness2FeaturesKHR *)s)->nullDescriptor = VK_TRUE;
            }
            s = s->pNext;
        }
    }

    if (result != VK_SUCCESS) return result;

    memset(&g_dev, 0, sizeof(g_dev));
    g_dev.device = *pDevice;
    g_dev.GetDeviceProcAddr = fpGetDeviceProcAddr;
    g_dev.DestroyDevice = (PFN_vkDestroyDevice)fpGetDeviceProcAddr(*pDevice, "vkDestroyDevice");
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL PVRSTRIP_DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator) {
    PFN_vkDestroyDevice destroy = g_dev.DestroyDevice;
    memset(&g_dev, 0, sizeof(g_dev));
    if (destroy) destroy(device, pAllocator);
}

/* ---- proc addr dispatch ---- */

static PFN_vkVoidFunction VKAPI_CALL PVRSTRIP_GetDeviceProcAddr(VkDevice device, const char *pName) {
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) return (PFN_vkVoidFunction)PVRSTRIP_GetDeviceProcAddr;
    if (strcmp(pName, "vkDestroyDevice") == 0) return (PFN_vkVoidFunction)PVRSTRIP_DestroyDevice;
    if (!g_dev.GetDeviceProcAddr) return NULL;
    return g_dev.GetDeviceProcAddr(device, pName);
}

static PFN_vkVoidFunction VKAPI_CALL PVRSTRIP_GetInstanceProcAddr(VkInstance instance, const char *pName) {
    if (strcmp(pName, "vkGetInstanceProcAddr") == 0) return (PFN_vkVoidFunction)PVRSTRIP_GetInstanceProcAddr;
    if (strcmp(pName, "vkCreateInstance") == 0) return (PFN_vkVoidFunction)PVRSTRIP_CreateInstance;
    if (strcmp(pName, "vkDestroyInstance") == 0) return (PFN_vkVoidFunction)PVRSTRIP_DestroyInstance;
    if (strcmp(pName, "vkCreateDevice") == 0) return (PFN_vkVoidFunction)PVRSTRIP_CreateDevice;
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) return (PFN_vkVoidFunction)PVRSTRIP_GetDeviceProcAddr;
    if (strcmp(pName, "vkEnumerateDeviceExtensionProperties") == 0) return (PFN_vkVoidFunction)PVRSTRIP_EnumerateDeviceExtensionProperties;
    if (strcmp(pName, "vkGetPhysicalDeviceFeatures") == 0) return (PFN_vkVoidFunction)PVRSTRIP_GetPhysicalDeviceFeatures;
    if (strcmp(pName, "vkGetPhysicalDeviceFeatures2") == 0)
        return g_inst.GetPhysicalDeviceFeatures2 ? (PFN_vkVoidFunction)PVRSTRIP_GetPhysicalDeviceFeatures2 : NULL;
    if (strcmp(pName, "vkGetPhysicalDeviceFeatures2KHR") == 0)
        return g_inst.GetPhysicalDeviceFeatures2KHR ? (PFN_vkVoidFunction)PVRSTRIP_GetPhysicalDeviceFeatures2KHR : NULL;
    if (!g_inst.GetInstanceProcAddr) return NULL;
    return g_inst.GetInstanceProcAddr(instance, pName);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct) {
    if (pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
    if (pVersionStruct->loaderLayerInterfaceVersion < 2) return VK_ERROR_INITIALIZATION_FAILED;
    pVersionStruct->loaderLayerInterfaceVersion = 2;
    pVersionStruct->pfnGetInstanceProcAddr = PVRSTRIP_GetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = PVRSTRIP_GetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}
