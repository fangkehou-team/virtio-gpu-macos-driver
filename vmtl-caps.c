/*
 * vmtl-caps.c — P2 caps survey: which of the Metal plugin's 14 required
 * device extensions does the Venus ICD actually expose, and which of the
 * plugin's feature gates light up.
 *
 * Build (in the guest):
 *   clang -O1 -o vmtl-caps vmtl-caps.c -I ~/mesa/include \
 *       ~/mesa/build-venus/src/virtio/vulkan/libvulkan_virtio.dylib
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

typedef VkResult (VKAPI_PTR *PFN_icdNegotiate)(uint32_t *pVersion);
typedef PFN_vkVoidFunction (VKAPI_PTR *PFN_icdGetInstanceProcAddr)(
    VkInstance instance, const char *pName);

/* the plugin's kWant[14] (plugin/vmtl_vk.c) */
static const char *kWant[14] = {
    "VK_EXT_extended_dynamic_state3", "VK_EXT_depth_clip_enable",
    "VK_EXT_memory_budget", "VK_KHR_acceleration_structure",
    "VK_KHR_ray_query", "VK_KHR_deferred_host_operations",
    "VK_EXT_external_memory_host", "VK_EXT_image_drm_format_modifier",
    "VK_KHR_external_memory_fd", "VK_EXT_mutable_descriptor_type",
    "VK_KHR_cooperative_matrix", "VK_EXT_calibrated_timestamps",
    "VK_EXT_sample_locations", "VK_KHR_pipeline_executable_properties"
};

int main(int argc, char **argv)
{
    const char *dylib = argc > 1 ? argv[1]
        : "/Users/lony2003/mesa/build-venus/src/virtio/vulkan/libvulkan_virtio.dylib";
    void *h = dlopen(dylib, RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("dlopen failed: %s\n", dlerror()); return 1; }

    PFN_icdNegotiate negotiate = (PFN_icdNegotiate)dlsym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
    PFN_icdGetInstanceProcAddr getIcd = (PFN_icdGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
    if (!negotiate || !getIcd) { printf("icd entrypoints missing\n"); return 1; }
    uint32_t ver = 5;
    negotiate(&ver);

    PFN_vkGetInstanceProcAddr gpa =
        (PFN_vkGetInstanceProcAddr)getIcd(NULL, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance createInstance =
        (PFN_vkCreateInstance)gpa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "vmtl-caps",
        .apiVersion = VK_API_VERSION_1_4,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = createInstance(&ici, NULL, &inst);
    if (r != VK_SUCCESS) { printf("vkCreateInstance: %d\n", (int)r); return 1; }

    PFN_vkEnumeratePhysicalDevices enumPhys =
        (PFN_vkEnumeratePhysicalDevices)gpa(inst, "vkEnumeratePhysicalDevices");
    uint32_t nphys = 0;
    enumPhys(inst, &nphys, NULL);
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    enumPhys(inst, &nphys, &phys);
    if (!phys) { printf("no physical device\n"); return 1; }

    PFN_vkGetPhysicalDeviceProperties getProps =
        (PFN_vkGetPhysicalDeviceProperties)gpa(inst, "vkGetPhysicalDeviceProperties");
    VkPhysicalDeviceProperties props = {0};
    getProps(phys, &props);
    printf("device: %s  api %u.%u.%u\n", props.deviceName,
           VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
           VK_VERSION_PATCH(props.apiVersion));

    PFN_vkEnumerateDeviceExtensionProperties enumExt =
        (PFN_vkEnumerateDeviceExtensionProperties)
        gpa(inst, "vkEnumerateDeviceExtensionProperties");
    uint32_t na = 0;
    if (enumExt(phys, NULL, &na, NULL) != VK_SUCCESS || !na) {
        printf("no device extensions\n"); return 1;
    }
    VkExtensionProperties *exts = calloc(na, sizeof *exts);
    enumExt(phys, NULL, &na, exts);
    printf("device exposes %u extensions\n", na);
    for (uint32_t i = 0; i < na; i++)
        printf("  %s (spec %u)\n", exts[i].extensionName, exts[i].specVersion);

    printf("\nplugin kWant[14] survey:\n");
    int have[14] = {0};
    for (int w = 0; w < 14; w++)
        for (uint32_t i = 0; i < na; i++)
            if (!strcmp(exts[i].extensionName, kWant[w])) { have[w] = 1; break; }
    for (int w = 0; w < 14; w++)
        printf("  %-40s %s\n", kWant[w], have[w] ? "PRESENT" : "ABSENT");
    int nhit = 0;
    for (int w = 0; w < 14; w++) nhit += have[w];
    printf("  %d/14 present\n", nhit);

    /* plugin feature gates (vmtl_vk.c): g_dyn3, g_clip, g_budget_ext, g_rt */
    printf("\nplugin gates:\n");
    printf("  g_dyn3   (extended_dynamic_state3): %s\n", have[0] ? "on" : "OFF");
    printf("  g_clip   (dyn3 + depth_clip_enable): %s\n",
           (have[0] && have[1]) ? "on" : "OFF");
    printf("  g_budget (memory_budget):            %s\n", have[2] ? "on" : "OFF");
    printf("  g_rt     (accstruct+ray_query+deferred): %s\n",
           (have[3] && have[4] && have[5]) ? "on" : "OFF");

    /* features the plugin checks directly */
    PFN_vkGetPhysicalDeviceFeatures2 getFeatures2 =
        (PFN_vkGetPhysicalDeviceFeatures2)gpa(inst, "vkGetPhysicalDeviceFeatures2");
    if (getFeatures2) {
        VkPhysicalDeviceVulkan12Features v12 = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceAccelerationStructureFeaturesKHR acc = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
            &v12 };
        VkPhysicalDeviceRayQueryFeaturesKHR rq = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, &acc };
        VkPhysicalDeviceFeatures2 f2 = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &rq };
        getFeatures2(phys, &f2);
        printf("\nfeatures:\n");
        printf("  bufferDeviceAddress (vulkan12): %u\n",
               v12.bufferDeviceAddress);
        printf("  accelerationStructure:          %u\n", acc.accelerationStructure);
        printf("  rayQuery:                       %u\n", rq.rayQuery);
    }
    return 0;
}
