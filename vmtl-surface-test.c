/*
 * vmtl-surface-test.c — P4.1 acceptance: true shared VRAM.
 *
 *   IOSurface (guest RAM) -> kext UAPI_STAGE_IMPORT -> GUEST blob
 *   -> glue udmabuf -> render server import -> VkDeviceMemory
 *   -> VkImage bound there -> GPU vkCmdClearColorImage
 *   -> CPU reads the IOSurface: the GPU's bytes are already there.
 *
 * Plugin-ABI level: dlsyms vmtl_nvk_stage_import from the ICD and drives
 * vkAllocateMemory with an OPAQUE_FD /dev/null import, exactly like
 * vmtl_vk_surface_vram does.
 *
 * Build (guest): clang -O1 -o vmtl-surface-test vmtl-surface-test.c \
 *   -framework IOKit -framework IOSurface -framework CoreFoundation
 * Run as root.
 */
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

static int gPass, gFail;
#define CHECK(cond, what) do { \
    if (cond) { gPass++; printf("ok   %s\n", what); } \
    else { gFail++; printf("FAIL %s\n", what); } \
} while (0)

#define ICD_LIB "/Library/GPUBundles/vmtl/libvulkan_nouveau.dylib"

typedef struct { uint32_t version, surfaceID, plane, pad; uint64_t size_B; } vmtl_vram_rec;
static void (*p_stage_import)(const vmtl_vram_rec *);

#define W 256
#define H 256
#define PITCH (W * 4)

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    /* ---- 1. IOSurface, CPU-filled with a known pattern ---- */
    const int32_t w = W, h = H, bpe = 4;
    const int64_t alloc = (int64_t)W * H * 4;
    CFNumberRef nw = CFNumberCreate(NULL, kCFNumberSInt32Type, &w);
    CFNumberRef nh = CFNumberCreate(NULL, kCFNumberSInt32Type, &h);
    CFNumberRef nb = CFNumberCreate(NULL, kCFNumberSInt32Type, &bpe);
    CFNumberRef na = CFNumberCreate(NULL, kCFNumberSInt64Type, &alloc);
    const void *keys[] = { kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfaceAllocSize };
    const void *vals[] = { nw, nh, nb, na };
    IOSurfaceRef surf = IOSurfaceCreate(CFDictionaryCreate(NULL, keys, vals, 4,
                                &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    CFRelease(nw); CFRelease(nh); CFRelease(nb); CFRelease(na);
    CHECK(surf != NULL, "IOSurfaceCreate");
    if (!surf) return 1;

    const uint32_t cpuPattern = 0xff20a5de;   /* BGRA: B=255 G=32 R=165 A=255 */
    {
        IOSurfaceLock(surf, kIOSurfaceLockReadOnly | kIOSurfaceLockAvoidSync, NULL);
        uint32_t *p = (uint32_t *)IOSurfaceGetBaseAddress(surf);
        for (unsigned i = 0; i < W * H; i++) p[i] = cpuPattern;
        IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly | kIOSurfaceLockAvoidSync, NULL);
    }
    uint32_t surfID = IOSurfaceGetID(surf);
    printf("     surface id 0x%x pitch %zu bytes %zu\n", surfID,
           IOSurfaceGetBytesPerRow(surf), IOSurfaceGetAllocSize(surf));

    /* ---- 2. Vulkan device through the ICD (direct dlopen, like the plugin) ---- */
    void *lib = dlopen(ICD_LIB, RTLD_NOW | RTLD_LOCAL);
    CHECK(lib != NULL, "dlopen ICD");
    if (!lib) return 1;
    p_stage_import = (void (*)(const vmtl_vram_rec *))dlsym(lib, "vmtl_nvk_stage_import");
    CHECK(p_stage_import != NULL, "dlsym vmtl_nvk_stage_import");
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
    CHECK(gipa != NULL, "dlsym vk_icdGetInstanceProcAddr");

    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vmtl-surface-test", 0, NULL, 0, VK_MAKE_VERSION(1, 3, 0) };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai, 0, NULL, 0, NULL };
    VkInstance inst;
    PFN_vkCreateInstance pvkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkResult cir = pvkCreateInstance ? pvkCreateInstance(&ici, NULL, &inst) : VK_ERROR_INITIALIZATION_FAILED;
    printf("     vkCreateInstance -> %d\n", cir);
    CHECK(cir == VK_SUCCESS, "vkCreateInstance");
    if (cir != VK_SUCCESS || !inst) return 1;

    PFN_vkEnumeratePhysicalDevices epd = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    VkPhysicalDevice pd; uint32_t n = 1;
    int pdOK = epd && epd(inst, &n, &pd) == VK_SUCCESS && n == 1 && pd != NULL;
    CHECK(pdOK, "one physical device");
    if (!pdOK) return 1;

    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
    PFN_vkCreateDevice pfnCreateDevice = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
    PFN_vkDestroyInstance vkDestroyInstance = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
    /* physical-device level entry points come from the instance dispatch */
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties =
       (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    int devLocal = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) { devLocal = i; break; }
    CHECK(devLocal >= 0, "found DEVICE_LOCAL memory type");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL };
    VkDevice dev = VK_NULL_HANDLE;
    VkResult dcr = pfnCreateDevice(pd, &dci, NULL, &dev);
    printf("     vkCreateDevice -> %d\n", dcr);
    CHECK(dcr == VK_SUCCESS, "vkCreateDevice");
    if (dcr != VK_SUCCESS || !dev) return 1;

    /* device level entry points come from the device dispatch */
#define DEV(name) PFN_##name name = (PFN_##name)gdpa(dev, #name)
    DEV(vkAllocateMemory); DEV(vkFreeMemory); DEV(vkCreateImage); DEV(vkDestroyImage);
    DEV(vkBindImageMemory); DEV(vkGetImageSubresourceLayout);
    DEV(vkGetDeviceQueue); DEV(vkCreateCommandPool); DEV(vkAllocateCommandBuffers);
    DEV(vkBeginCommandBuffer); DEV(vkEndCommandBuffer); DEV(vkQueueSubmit);
    DEV(vkQueueWaitIdle); DEV(vkDestroyCommandPool); DEV(vkDestroyDevice);
    DEV(vkCmdClearColorImage); DEV(vkCmdPipelineBarrier);
#undef DEV

    /* ---- 3. staged import + AllocateMemory (plugin ABI) ---- */
    const size_t need = W * H * 4;
    vmtl_vram_rec rec = { 3, surfID, 0, 0, need };
    VkImportMemoryFdInfoKHR imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, NULL,
                                    VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
                                    open("/dev/null", O_RDONLY | O_CLOEXEC) };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, need, (uint32_t)devLocal };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    p_stage_import(&rec);
    VkResult ar = vkAllocateMemory(dev, &mai, NULL, &mem);
    p_stage_import(NULL);
    printf("     vkAllocateMemory -> %d mem %p\n", ar, (void *)mem);
    CHECK(ar == VK_SUCCESS && mem != VK_NULL_HANDLE, "staged import vkAllocateMemory");
    if (ar != VK_SUCCESS) return 1;

    /* ---- 4. linear VkImage bound to the imported memory ---- */
    VkImageCreateInfo ici2 = {
        VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D,
        VK_FORMAT_B8G8R8A8_UNORM, { W, H, 1 }, 1, 1,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_LINEAR,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE,
        0, NULL, VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage img;
    CHECK(vkCreateImage(dev, &ici2, NULL, &img) == VK_SUCCESS, "vkCreateImage linear");
    CHECK(vkBindImageMemory(dev, img, mem, 0) == VK_SUCCESS, "vkBindImageMemory offset 0");

    VkImageSubresource isr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout lay;
    vkGetImageSubresourceLayout(dev, img, &isr, &lay);
    printf("     image layout: offset %llu rowPitch %llu size %llu (surface pitch %zu)\n",
           (unsigned long long)lay.offset, (unsigned long long)lay.rowPitch,
           (unsigned long long)lay.size, IOSurfaceGetBytesPerRow(surf));
    CHECK(lay.rowPitch == IOSurfaceGetBytesPerRow(surf),
          "rowPitch matches IOSurface pitch");

    /* ---- 5. GPU writes: clear to teal ---- */
    VkQueue queue; vkGetDeviceQueue(dev, 0, 0, &queue);
    VkCommandPool pool;
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0 };
    CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool) == VK_SUCCESS, "command pool");
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    vkAllocateCommandBuffers(dev, &cai, &cb);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, 0, NULL };
    vkBeginCommandBuffer(cb, &bi);

    VkImageMemoryBarrier bar = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL,
        0, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img,
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &bar);
    VkClearColorValue color = { .float32 = { 0.0f, 0.5f, 1.0f, 1.0f } };  /* R G B A */
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    bar.oldLayout = bar.newLayout;
    bar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, NULL, 0, NULL, 1, &bar);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL };
    CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS, "queue submit");
    CHECK(vkQueueWaitIdle(queue) == VK_SUCCESS, "queue idle");

    /* ---- 6. CPU reads the IOSurface: expect the GPU's teal ---- */
    {
        IOSurfaceLock(surf, kIOSurfaceLockReadOnly | kIOSurfaceLockAvoidSync, NULL);
        const uint32_t *p = (const uint32_t *)IOSurfaceGetBaseAddress(surf);
        /* expected B8G8R8A8 bytes for (R=0, G=0.5, B=1, A=1) */
        const uint32_t expectB = 0xff, expectG = 0x80, expectR = 0x00;
        unsigned teal = 0, cpu = 0, other = 0;
        for (unsigned i = 0; i < W * H; i++) {
            uint32_t v = p[i];
            uint32_t b = v & 0xff, g = (v >> 8) & 0xff, r = (v >> 16) & 0xff, a = (v >> 24) & 0xff;
            if (b >= expectB - 2 && g >= expectG - 2 && g <= expectG + 2 && r <= 2 && a == 0xff) teal++;
            else if (v == cpuPattern) cpu++;
            else other++;
        }
        printf("     pixels: teal %u, cpu-pattern %u, other %u (of %d)\n", teal, cpu, other, W * H);
        CHECK(teal == W * H, "GPU clear visible in the IOSurface (zero-copy works)");
        IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly | kIOSurfaceLockAvoidSync, NULL);
    }

    /* ---- 7. teardown: free memory must release the kext bo cleanly ---- */
    vkDestroyImage(dev, img, NULL);
    vkFreeMemory(dev, mem, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    CFRelease(surf);
    printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
