/*
 * vmtl-icdtest.c — minimal Vulkan ICD harness for the Venus dylib.
 *
 * The guest has no Vulkan loader yet, so this drives the ICD entrypoints
 * directly: negotiate -> instance -> physical device -> device -> queue ->
 * a real render (clear a buffer, read it back through a mapped host3d blob).
 *
 * Build (in the guest):
 *   clang -O1 -o vmtl-icdtest vmtl-icdtest.c -I ~/mesa/include \
 *       ~/mesa/build-venus/src/virtio/vulkan/libvulkan_virtio.dylib
 * Run with no arguments.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

#define CHECK(cond, name) do { \
    if (cond) { printf("PASS %s\n", name); gPass++; } \
    else      { printf("FAIL %s  (line %d)\n", name, __LINE__); gFail++; } \
} while (0)

/* ICD interface entrypoints are loader-internal; declare them here. */
typedef VkResult (VKAPI_PTR *PFN_icdNegotiate)(uint32_t *pVersion);
typedef PFN_vkVoidFunction (VKAPI_PTR *PFN_icdGetInstanceProcAddr)(
    VkInstance instance, const char *pName);

static int gPass, gFail;

/* vkGetDeviceProcAddr via the instance table */
static PFN_vkGetDeviceProcAddr gGetDeviceProcAddr;

#define G(name) ((PFN_##name)gGetDeviceProcAddr(dev, #name))

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* keep PASS/FAIL visible across crashes */
    const char *dylib = argc > 1 ? argv[1]
        : "/Users/lony2003/mesa/build-venus/src/virtio/vulkan/libvulkan_virtio.dylib";
    void *h = dlopen(dylib, RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("dlopen failed: %s\n", dlerror()); return 1; }

    PFN_icdNegotiate negotiate =
        (PFN_icdNegotiate)
        dlsym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
    PFN_icdGetInstanceProcAddr getIcd =
        (PFN_icdGetInstanceProcAddr)
        dlsym(h, "vk_icdGetInstanceProcAddr");
    CHECK(negotiate && getIcd, "icd entrypoints present");

    uint32_t ver = 5;
    CHECK(negotiate(&ver) == VK_SUCCESS, "negotiate");
    printf("     interface version %u\n", ver);

    PFN_vkGetInstanceProcAddr gpa =
        (PFN_vkGetInstanceProcAddr)getIcd(NULL, "vkGetInstanceProcAddr");
    CHECK(gpa != NULL, "vkGetInstanceProcAddr");

    PFN_vkCreateInstance createInstance =
        (PFN_vkCreateInstance)gpa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "vmtl-icdtest",
        .apiVersion = VK_API_VERSION_1_4,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = createInstance(&ici, NULL, &inst);
    CHECK(r == VK_SUCCESS, "vkCreateInstance");
    if (r != VK_SUCCESS) printf("     vkCreateInstance returned %d\n", (int)r);
    if (r != VK_SUCCESS) { printf("%d passed, %d failed\n", gPass, gFail); return 1; }

    PFN_vkEnumeratePhysicalDevices enumPhys =
        (PFN_vkEnumeratePhysicalDevices)gpa(inst, "vkEnumeratePhysicalDevices");
    uint32_t nphys = 0;
    CHECK(enumPhys(inst, &nphys, NULL) == VK_SUCCESS && nphys >= 1,
          "enumerate physical devices");
    printf("     %u physical device(s)\n", nphys);

    VkPhysicalDevice phys = VK_NULL_HANDLE;
    r = enumPhys(inst, &nphys, &phys);
    if (r != VK_SUCCESS)
        printf("     second enum failed: %d\n", (int)r);
    VkPhysicalDeviceProperties props = {0};
    PFN_vkGetPhysicalDeviceProperties getProps =
        (PFN_vkGetPhysicalDeviceProperties)gpa(inst, "vkGetPhysicalDeviceProperties");
    getProps(phys, &props);
    printf("     device: %s  api %u.%u.%u  vendor %#x device %#x\n",
           props.deviceName,
           VK_VERSION_MAJOR(props.apiVersion),
           VK_VERSION_MINOR(props.apiVersion),
           VK_VERSION_PATCH(props.apiVersion),
           props.vendorID, props.deviceID);
    CHECK(props.apiVersion >= VK_API_VERSION_1_1, "api version >= 1.1");

    /* pick a graphics queue */
    uint32_t nqf = 0;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties getQfp =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
        gpa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    getQfp(phys, &nqf, NULL);
    VkQueueFamilyProperties *qfp = calloc(nqf, sizeof *qfp);
    getQfp(phys, &nqf, qfp);
    uint32_t gfx = (uint32_t)-1;
    for (uint32_t i = 0; i < nqf; i++)
        if (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gfx = i; break; }
    CHECK(gfx != (uint32_t)-1, "graphics queue family");
    printf("     %u queue families, graphics=%u\n", nqf, gfx);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = gfx,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
    };
    VkDevice dev = VK_NULL_HANDLE;
    PFN_vkCreateDevice createDevice =
        (PFN_vkCreateDevice)gpa(inst, "vkCreateDevice");
    r = createDevice(phys, &dci, NULL, &dev);
    CHECK(r == VK_SUCCESS, "vkCreateDevice");
    if (r != VK_SUCCESS) { printf("%d passed, %d failed\n", gPass, gFail); return 1; }

    PFN_vkGetDeviceProcAddr gdpa =
        (PFN_vkGetDeviceProcAddr)gpa(inst, "vkGetDeviceProcAddr");
    gGetDeviceProcAddr = gdpa;

    PFN_vkGetDeviceQueue getQueue = G(vkGetDeviceQueue);
    VkQueue queue = VK_NULL_HANDLE;
    getQueue(dev, gfx, 0, &queue);
    CHECK(queue != VK_NULL_HANDLE, "vkGetDeviceQueue");

    /* ---- real render: fill a 16KB host-visible buffer via a clear ---- */
    PFN_vkGetPhysicalDeviceMemoryProperties getMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)
        gpa(inst, "vkGetPhysicalDeviceMemoryProperties");
    VkPhysicalDeviceMemoryProperties memprops = {0};
    getMemProps(phys, &memprops);
    uint32_t hostIdx = (uint32_t)-1;
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        if ((memprops.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            hostIdx = i;
            break;
        }
    CHECK(hostIdx != (uint32_t)-1, "host-visible memory type");
    printf("     %u memory types, %llu heaps\n",
           memprops.memoryTypeCount,
           (unsigned long long)memprops.memoryHeapCount);

    PFN_vkAllocateMemory allocMem = G(vkAllocateMemory);
    PFN_vkFreeMemory freeMem = G(vkFreeMemory);
    PFN_vkMapMemory mapMem = G(vkMapMemory);
    PFN_vkUnmapMemory unmapMem = G(vkUnmapMemory);

    VkMemoryRequirements req = {0};
    const VkDeviceSize bufSize = 16 * 1024;

    /* use a buffer object so requirements don't depend on an image path */
    PFN_vkCreateBuffer createBuffer = G(vkCreateBuffer);
    PFN_vkDestroyBuffer destroyBuffer = G(vkDestroyBuffer);
    PFN_vkBindBufferMemory bindBuffer = G(vkBindBufferMemory);
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bufSize,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buf = VK_NULL_HANDLE;
    CHECK(createBuffer(dev, &bci, NULL, &buf) == VK_SUCCESS, "vkCreateBuffer");

    PFN_vkGetBufferMemoryRequirements getBufReq = G(vkGetBufferMemoryRequirements);
    getBufReq(dev, buf, &req);
    printf("     buffer req size %llu align %llu\n",
           (unsigned long long)req.size, (unsigned long long)req.alignment);

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = hostIdx,
    };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    CHECK(allocMem(dev, &mai, NULL, &mem) == VK_SUCCESS, "vkAllocateMemory");
    CHECK(bindBuffer(dev, buf, mem, 0) == VK_SUCCESS, "vkBindBufferMemory");

    void *p = NULL;
    CHECK(mapMem(dev, mem, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS && p != NULL,
          "vkMapMemory");
    if (p) {
        memset(p, 0xAB, bufSize);
        printf("     wrote 0xAB pattern through mapped host3d blob\n");
        unmapMem(dev, mem);
    }

    /* command buffer: clear the buffer via vkCmdFillBuffer, submit, read back */
    PFN_vkCreateCommandPool createPool = G(vkCreateCommandPool);
    PFN_vkAllocateCommandBuffers allocCmds = G(vkAllocateCommandBuffers);
    PFN_vkBeginCommandBuffer beginCmd = G(vkBeginCommandBuffer);
    PFN_vkEndCommandBuffer endCmd = G(vkEndCommandBuffer);
    PFN_vkCmdFillBuffer cmdFill = G(vkCmdFillBuffer);
    PFN_vkQueueSubmit queueSubmit = G(vkQueueSubmit);
    PFN_vkQueueWaitIdle queueWaitIdle = G(vkQueueWaitIdle);
    PFN_vkCreateFence createFence = G(vkCreateFence);
    PFN_vkDestroyFence destroyFence = G(vkDestroyFence);

    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = gfx,
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    CHECK(createPool(dev, &cpci, NULL, &pool) == VK_SUCCESS, "vkCreateCommandPool");

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    CHECK(allocCmds(dev, &cbai, &cmd) == VK_SUCCESS, "vkAllocateCommandBuffers");

    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    CHECK(beginCmd(cmd, &cbbi) == VK_SUCCESS, "vkBeginCommandBuffer");
    cmdFill(cmd, buf, 0, bufSize, 0x5A5A5A5A);
    CHECK(endCmd(cmd) == VK_SUCCESS, "vkEndCommandBuffer");

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    CHECK(createFence(dev, &fci, NULL, &fence) == VK_SUCCESS, "vkCreateFence");

    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    r = queueSubmit(queue, 1, &si, fence);
    CHECK(r == VK_SUCCESS, "vkQueueSubmit (fill buffer)");
    if (r == VK_SUCCESS) {
        CHECK(queueWaitIdle(queue) == VK_SUCCESS, "vkQueueWaitIdle");
        VkResult q = mapMem(dev, mem, 0, VK_WHOLE_SIZE, 0, &p);
        CHECK(q == VK_SUCCESS && p != NULL, "re-map after submit");
        if (p) {
            uint32_t *w = (uint32_t *)p;
            CHECK(w[0] == 0x5A5A5A5A && w[bufSize / 4 - 1] == 0x5A5A5A5A,
                  "GPU fill visible through readback");
            if (w[0] != 0x5A5A5A5A)
                printf("     got %#x expected 0x5a5a5a5a\n", w[0]);
            unmapMem(dev, mem);
        }
    }

    destroyFence(dev, fence, NULL);
    destroyBuffer(dev, buf, NULL);
    freeMem(dev, mem, NULL);

    printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
