/*
 * vmtl-cube.c — P2 vkcube-equivalent: a real spinning cube rendered by
 * the Venus ICD (graphics pipeline, depth test) into an offscreen
 * host-visible image, verified by pixel readback.
 *
 * Build (in the guest):
 *   clang -O1 -o vmtl-cube vmtl-cube.c -I ~/mesa/include \
 *       ~/mesa/build-venus/src/virtio/vulkan/libvulkan_virtio.dylib
 * Run: ./vmtl-cube  → writes /tmp/cube.ppm, prints PASS/FAIL summary.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <vulkan/vulkan.h>
#include "cube_shaders.h"

typedef VkResult (VKAPI_PTR *PFN_icdNegotiate)(uint32_t *pVersion);
typedef PFN_vkVoidFunction (VKAPI_PTR *PFN_icdGetInstanceProcAddr)(
    VkInstance instance, const char *pName);

static int gPass, gFail;
#define CHECK(cond, name) do { \
    if (cond) { printf("PASS %s\n", name); gPass++; } \
    else      { printf("FAIL %s  (line %d)\n", name, __LINE__); gFail++; } \
} while (0)

#define G(name) ((PFN_##name)gGetDeviceProcAddr(dev, #name))
static PFN_vkGetDeviceProcAddr gGetDeviceProcAddr;

/* ---- tiny column-major mat4 (vk convention: m[col*4 + row]) ---- */
static void mat4_mul(float *o, const float *a, const float *b)
{
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int rw = 0; rw < 4; rw++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[k * 4 + rw] * b[c * 4 + k];
            r[c * 4 + rw] = s;
        }
    memcpy(o, r, sizeof r);
}

static void mat4_persp(float *o, float fovy, float aspect, float n, float f)
{
    const float t = 1.0f / tanf(fovy * 0.5f);
    memset(o, 0, sizeof(float) * 16);
    o[0] = t / aspect;
    o[5] = -t;                 /* vulkan clip space: y down */
    o[10] = f / (n - f);
    o[11] = -1.0f;
    o[14] = f * n / (n - f);
}

static void mat4_rot(float *o, float ax, float ay)
{
    const float cx = cosf(ax), sx = sinf(ax), cy = cosf(ay), sy = sinf(ay);
    /* rotY * rotX, column-major */
    float ry[16] = { cy, 0, -sy, 0,  0, 1, 0, 0,  sy, 0, cy, 0,  0, 0, 0, 1 };
    float rx[16] = { 1, 0, 0, 0,  0, cx, sx, 0,  0, -sx, cx, 0,  0, 0, 0, 1 };
    mat4_mul(o, ry, rx);
}

static void mat4_view(float *o, float z)
{
    memset(o, 0, sizeof(float) * 16);
    o[0] = o[5] = o[10] = o[15] = 1.0f;
    o[14] = z;
}

static void cube_faces(float *v)   /* 36 * (pos3 + normal3) */
{
    static const float p[6][4][3] = {
        {{ 1,-1,-1}, { 1, 1,-1}, { 1, 1, 1}, { 1,-1, 1}},   /* +x */
        {{-1,-1, 1}, {-1, 1, 1}, {-1, 1,-1}, {-1,-1,-1}},   /* -x */
        {{-1, 1,-1}, {-1, 1, 1}, { 1, 1, 1}, { 1, 1,-1}},   /* +y */
        {{-1,-1, 1}, {-1,-1,-1}, { 1,-1,-1}, { 1,-1, 1}},   /* -y */
        {{-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1}},   /* +z */
        {{ 1,-1,-1}, {-1,-1,-1}, {-1, 1,-1}, { 1, 1,-1}},   /* -z */
    };
    static const float n[6][3] = {
        {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1},
    };
    static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
    for (int f = 0; f < 6; f++)
        for (int i = 0; i < 6; i++) {
            const int vi = (f * 6 + i) * 6;
            const int pi = idx[i];
            for (int k = 0; k < 3; k++) {
                v[vi + k] = p[f][pi][k] * 0.7f;
                v[vi + 3 + k] = n[f][k];
            }
        }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *dylib = argc > 1 ? argv[1]
        : "/Users/lony2003/mesa/build-venus/src/virtio/vulkan/libvulkan_virtio.dylib";
    void *h = dlopen(dylib, RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("dlopen failed: %s\n", dlerror()); return 1; }

    PFN_icdNegotiate negotiate = (PFN_icdNegotiate)dlsym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
    PFN_vkGetInstanceProcAddr gpa =
        (PFN_vkGetInstanceProcAddr)((PFN_icdGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr"))(NULL, "vkGetInstanceProcAddr");
    CHECK(negotiate && gpa, "icd entrypoints");

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "vmtl-cube";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = ((PFN_vkCreateInstance)gpa(NULL, "vkCreateInstance"))(&ici, NULL, &inst);
    CHECK(r == VK_SUCCESS, "vkCreateInstance");
    if (r != VK_SUCCESS) return 1;

    PFN_vkEnumeratePhysicalDevices enumPhys =
        (PFN_vkEnumeratePhysicalDevices)gpa(inst, "vkEnumeratePhysicalDevices");
    uint32_t nphys = 0;
    enumPhys(inst, &nphys, NULL);
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    r = enumPhys(inst, &nphys, &phys);
    CHECK(r == VK_SUCCESS && phys, "physical device");
    if (!phys) return 1;

    uint32_t nqf = 0;
    ((PFN_vkGetPhysicalDeviceQueueFamilyProperties)gpa(inst, "vkGetPhysicalDeviceQueueFamilyProperties"))(phys, &nqf, NULL);
    VkQueueFamilyProperties qfp[16] = {0};
    uint32_t qn = nqf > 16 ? 16 : nqf;
    ((PFN_vkGetPhysicalDeviceQueueFamilyProperties)gpa(inst, "vkGetPhysicalDeviceQueueFamilyProperties"))(phys, &qn, qfp);
    uint32_t gfx = (uint32_t)-1;
    for (uint32_t i = 0; i < qn; i++)
        if (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gfx = i; break; }
    CHECK(gfx != (uint32_t)-1, "graphics queue family");

    PFN_vkGetPhysicalDeviceMemoryProperties getMemProps =
        (PFN_vkGetPhysicalDeviceMemoryProperties)gpa(inst, "vkGetPhysicalDeviceMemoryProperties");
    VkPhysicalDeviceMemoryProperties memprops = {0};
    getMemProps(phys, &memprops);
    uint32_t hostIdx = (uint32_t)-1, devIdx = (uint32_t)-1;
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++) {
        const uint32_t f = memprops.memoryTypes[i].propertyFlags;
        if (hostIdx == (uint32_t)-1 &&
            (f & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            hostIdx = i;
        if (devIdx == (uint32_t)-1 && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            devIdx = i;
    }
    CHECK(hostIdx != (uint32_t)-1, "host-visible coherent memory type");

    PFN_vkGetPhysicalDeviceFormatProperties getFmt =
        (PFN_vkGetPhysicalDeviceFormatProperties)gpa(inst, "vkGetPhysicalDeviceFormatProperties");
    VkFormatProperties fp = {0};
    getFmt(phys, VK_FORMAT_D32_SFLOAT, &fp);
    const VkBool32 haveD32 = fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    getFmt(phys, VK_FORMAT_D16_UNORM, &fp);
    const VkBool32 haveD16 = fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    CHECK(haveD32 || haveD16, "depth format");

    const VkFormat depthFmt = haveD32 ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_D16_UNORM;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        NULL, 0, gfx, 1, &prio };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice dev = VK_NULL_HANDLE;
    r = ((PFN_vkCreateDevice)gpa(inst, "vkCreateDevice"))(phys, &dci, NULL, &dev);
    CHECK(r == VK_SUCCESS, "vkCreateDevice");
    if (r != VK_SUCCESS) return 1;

    gGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)gpa(inst, "vkGetDeviceProcAddr");
    if (!gGetDeviceProcAddr) { printf("no vkGetDeviceProcAddr\n"); return 1; }

    PFN_vkGetDeviceQueue getQueue = G(vkGetDeviceQueue);
    VkQueue queue = VK_NULL_HANDLE;
    getQueue(dev, gfx, 0, &queue);
    CHECK(queue != VK_NULL_HANDLE, "queue");

    PFN_vkCreateImage createImage = G(vkCreateImage);
    PFN_vkDestroyImage destroyImage = G(vkDestroyImage);
    PFN_vkGetImageMemoryRequirements2 getImageReq2 = G(vkGetImageMemoryRequirements2);
    PFN_vkAllocateMemory allocMem = G(vkAllocateMemory);
    PFN_vkFreeMemory freeMem = G(vkFreeMemory);
    PFN_vkBindImageMemory2 bindImage2 = G(vkBindImageMemory2);
    PFN_vkCreateImageView createView = G(vkCreateImageView);
    PFN_vkDestroyImageView destroyView = G(vkDestroyImageView);
    PFN_vkCreateRenderPass createPass = G(vkCreateRenderPass);
    PFN_vkCreateFramebuffer createFb = G(vkCreateFramebuffer);
    PFN_vkCreateBuffer createBuffer = G(vkCreateBuffer);
    PFN_vkGetBufferMemoryRequirements getBufReq = G(vkGetBufferMemoryRequirements);
    PFN_vkBindBufferMemory bindBuffer = G(vkBindBufferMemory);
    PFN_vkMapMemory mapMem = G(vkMapMemory);
    PFN_vkUnmapMemory unmapMem = G(vkUnmapMemory);
    PFN_vkInvalidateMappedMemoryRanges invalidate = G(vkInvalidateMappedMemoryRanges);
    PFN_vkCreateCommandPool createPool = G(vkCreateCommandPool);
    PFN_vkAllocateCommandBuffers allocCmds = G(vkAllocateCommandBuffers);
    PFN_vkCreateShaderModule createShader = G(vkCreateShaderModule);
    PFN_vkCreatePipelineLayout createPipeLayout = G(vkCreatePipelineLayout);
    PFN_vkCreateGraphicsPipelines createPipelines = G(vkCreateGraphicsPipelines);
    PFN_vkBeginCommandBuffer beginCmd = G(vkBeginCommandBuffer);
    PFN_vkEndCommandBuffer endCmd = G(vkEndCommandBuffer);
    PFN_vkCmdBeginRenderPass cmdBeginPass = G(vkCmdBeginRenderPass);
    PFN_vkCmdEndRenderPass cmdEndPass = G(vkCmdEndRenderPass);
    PFN_vkCmdBindPipeline cmdBindPipeline = G(vkCmdBindPipeline);
    PFN_vkCmdBindVertexBuffers cmdBindVertex = G(vkCmdBindVertexBuffers);
    PFN_vkCmdPushConstants cmdPush = G(vkCmdPushConstants);
    PFN_vkCmdDraw cmdDraw = G(vkCmdDraw);
    PFN_vkCmdCopyImageToBuffer cmdCopyToBuf = G(vkCmdCopyImageToBuffer);
    PFN_vkQueueSubmit queueSubmit = G(vkQueueSubmit);
    PFN_vkQueueWaitIdle queueWaitIdle = G(vkQueueWaitIdle);

    /* ---- images: color (host-visible) + depth ---- */
    const uint32_t W = 512, H = 512;
    VkImage colorImg, depthImg;
    VkImageCreateInfo iciImg = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    iciImg.imageType = VK_IMAGE_TYPE_2D;
    iciImg.extent = (VkExtent3D){ W, H, 1 };
    iciImg.mipLevels = 1;
    iciImg.arrayLayers = 1;
    iciImg.format = VK_FORMAT_R8G8B8A8_UNORM;
    iciImg.tiling = VK_IMAGE_TILING_OPTIMAL;
    iciImg.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    iciImg.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    iciImg.samples = VK_SAMPLE_COUNT_1_BIT;
    iciImg.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CHECK(createImage(dev, &iciImg, NULL, &colorImg) == VK_SUCCESS, "color image");

    VkImageCreateInfo dciImg = iciImg;
    dciImg.format = depthFmt;
    dciImg.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    CHECK(createImage(dev, &dciImg, NULL, &depthImg) == VK_SUCCESS, "depth image");

    VkMemoryRequirements req, dreq;
    VkImageMemoryRequirementsInfo2 iri = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2 };
    VkMemoryRequirements2 req2 = { VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
    iri.image = colorImg;
    getImageReq2(dev, &iri, &req2);
    req = req2.memoryRequirements;
    iri.image = depthImg;
    getImageReq2(dev, &iri, &req2);
    dreq = req2.memoryRequirements;

    VkDeviceMemory colorMem, depthMem;
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, hostIdx };
    CHECK(allocMem(dev, &mai, NULL, &colorMem) == VK_SUCCESS, "color memory");
    /* depth: prefer device-local, fall back to the host-visible type */
    VkMemoryAllocateInfo dmai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, dreq.size, devIdx };
    if (allocMem(dev, &dmai, NULL, &depthMem) != VK_SUCCESS) {
        dmai.memoryTypeIndex = hostIdx;
        CHECK(allocMem(dev, &dmai, NULL, &depthMem) == VK_SUCCESS, "depth memory");
    }
    VkBindImageMemoryInfo bind[2] = {0};
    bind[0].sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO;
    bind[0].image = colorImg; bind[0].memory = colorMem; bind[0].memoryOffset = 0;
    bind[1].sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO;
    bind[1].image = depthImg; bind[1].memory = depthMem; bind[1].memoryOffset = 0;
    CHECK(bindImage2(dev, 2, bind) == VK_SUCCESS, "bind images");

    VkImageView colorView, depthView;
    VkImageViewCreateInfo vici = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vici.image = colorImg;
    vici.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vici.format = VK_FORMAT_R8G8B8A8_UNORM;
    vici.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    CHECK(createView(dev, &vici, NULL, &colorView) == VK_SUCCESS, "color view");
    vici.image = depthImg;
    vici.format = depthFmt;
    vici.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
    CHECK(createView(dev, &vici, NULL, &depthView) == VK_SUCCESS, "depth view");

    /* ---- renderpass + framebuffer ---- */
    VkAttachmentDescription att[2] = {0};
    att[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    att[1] = att[0];
    att[1].format = depthFmt;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference dref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = { 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL,
        1, &cref, NULL, &dref, 0, NULL };
    VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        NULL, 0, 2, att, 1, &sp, 0, NULL };
    VkRenderPass pass;
    CHECK(createPass(dev, &rpci, NULL, &pass) == VK_SUCCESS, "renderpass");

    VkImageView fbViews[2] = { colorView, depthView };
    VkFramebufferCreateInfo fbci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        NULL, 0, pass, 2, fbViews, W, H, 1 };
    VkFramebuffer fb;
    CHECK(createFb(dev, &fbci, NULL, &fb) == VK_SUCCESS, "framebuffer");

    /* ---- readback buffer (linear, host-visible) ---- */
    VkBufferCreateInfo rbi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0,
        W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkBuffer rbuf;
    CHECK(createBuffer(dev, &rbi, NULL, &rbuf) == VK_SUCCESS, "readback buffer");
    VkMemoryRequirements rreq;
    getBufReq(dev, rbuf, &rreq);
    VkDeviceMemory readMem;
    VkMemoryAllocateInfo rmai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL,
        rreq.size, hostIdx };
    CHECK(allocMem(dev, &rmai, NULL, &readMem) == VK_SUCCESS, "readback memory");
    CHECK(bindBuffer(dev, rbuf, readMem, 0) == VK_SUCCESS, "bind readback buffer");

    /* ---- vertex buffer ---- */
    float verts[36 * 6];
    cube_faces(verts);
    VkBuffer vbuf;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0,
        sizeof verts, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    CHECK(createBuffer(dev, &bci, NULL, &vbuf) == VK_SUCCESS, "vertex buffer");
    VkMemoryRequirements vreq;
    getBufReq(dev, vbuf, &vreq);
    VkDeviceMemory vmem;
    VkMemoryAllocateInfo vmai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL,
        vreq.size, hostIdx };
    CHECK(allocMem(dev, &vmai, NULL, &vmem) == VK_SUCCESS, "vertex memory");
    CHECK(bindBuffer(dev, vbuf, vmem, 0) == VK_SUCCESS, "bind vertex buffer");
    void *vmap = NULL;
    CHECK(mapMem(dev, vmem, 0, VK_WHOLE_SIZE, 0, &vmap) == VK_SUCCESS && vmap, "map vertex");
    memcpy(vmap, verts, sizeof verts);

    /* ---- pipeline ---- */
    VkShaderModule vs, fs;
    VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        NULL, 0, sizeof gCubeVertSpirv, (const uint32_t *)gCubeVertSpirv };
    CHECK(createShader(dev, &smci, NULL, &vs) == VK_SUCCESS, "vertex shader");
    smci.codeSize = sizeof gCubeFragSpirv;
    smci.pCode = (const uint32_t *)gCubeFragSpirv;
    CHECK(createShader(dev, &smci, NULL, &fs) == VK_SUCCESS, "fragment shader");

    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 64 };
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        NULL, 0, 0, NULL, 1, &pcr };
    VkPipelineLayout playout;
    CHECK(createPipeLayout(dev, &plci, NULL, &playout) == VK_SUCCESS, "pipeline layout");

    VkPipelineShaderStageCreateInfo stages[2] = {0};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1] = stages[0];
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;

    VkVertexInputBindingDescription vib = { 0, 24, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription via[2] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
        { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0,
        1, &vib, 2, via };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE };
    VkViewport vp = { 0, 0, (float)W, (float)H, 0.0f, 1.0f };
    VkRect2D sc = (VkRect2D){ { 0, 0 }, { W, H } };
    VkPipelineViewportStateCreateInfo vsi = {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc };
    VkPipelineRasterizationStateCreateInfo rs = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0,
        VK_SAMPLE_COUNT_1_BIT, VK_FALSE, 0.0f, NULL, VK_FALSE, VK_FALSE };
    VkPipelineDepthStencilStateCreateInfo ds = {
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, NULL, 0,
        VK_TRUE, VK_TRUE, VK_COMPARE_OP_LESS, VK_FALSE, VK_FALSE, {}, {}, 0.0f, 1.0f };
    VkPipelineColorBlendAttachmentState cba = {
        VK_FALSE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
        VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
    VkPipelineColorBlendStateCreateInfo cb = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE,
        VK_LOGIC_OP_COPY, 1, &cba, { 0, 0, 0, 0 } };
    VkGraphicsPipelineCreateInfo gpci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        NULL, 0, 2, stages, &vi, &ia, NULL, &vsi, &rs, &ms, &ds, &cb, NULL,
        playout, pass, 0, VK_NULL_HANDLE, -1 };
    VkPipeline pipe;
    CHECK(createPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe) == VK_SUCCESS, "graphics pipeline");

    /* ---- command buffer ---- */
    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, gfx };
    VkCommandPool pool;
    CHECK(createPool(dev, &cpci, NULL, &pool) == VK_SUCCESS, "command pool");
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cmd;
    CHECK(allocCmds(dev, &cbai, &cmd) == VK_SUCCESS, "command buffer");

    /* map color memory once */
    void *cmap = NULL;
    CHECK(mapMem(dev, colorMem, 0, VK_WHOLE_SIZE, 0, &cmap) == VK_SUCCESS && cmap,
          "map color image");

    void *rmap = NULL;
    CHECK(mapMem(dev, readMem, 0, VK_WHOLE_SIZE, 0, &rmap) == VK_SUCCESS && rmap,
          "map readback buffer");
    uint8_t (*img)[4] = (uint8_t (*)[4])rmap;
    int framesColored = 0;
    for (int frame = 0; frame < 8; frame++) {
        float p, y = frame * 0.35f, x = 0.45f;
        float proj[16], rot[16], view[16], vr[16], mvp[16];
        mat4_persp(proj, 1.0f, 1.0f, 0.1f, 10.0f);
        mat4_rot(rot, x, y);
        mat4_view(view, -3.5f);
        mat4_mul(vr, view, rot);
        mat4_mul(mvp, proj, vr);
        (void)p;

        VkCommandBufferBeginInfo bbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
        beginCmd(cmd, &bbi);
        VkClearValue clears[2] = {0};
        clears[0].color = (VkClearColorValue){ { 0.05f, 0.05f, 0.08f, 1.0f } };
        clears[1].depthStencil = (VkClearDepthStencilValue){ 1.0f, 0 };
        VkRenderPassBeginInfo rpbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            NULL, pass, fb, (VkRect2D){ { 0, 0 }, { W, H } }, 2, clears };
        cmdBeginPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        cmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        VkDeviceSize off = 0;
        cmdBindVertex(cmd, 0, 1, &vbuf, &off);
        cmdPush(cmd, playout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, mvp);
        cmdDraw(cmd, 36, 1, 0, 0);
        cmdEndPass(cmd);
        const VkBufferImageCopy bic = { 0, 0, 0,
            { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            { 0, 0, 0 }, { W, H, 1 } };
        cmdCopyToBuf(cmd, colorImg, VK_IMAGE_LAYOUT_GENERAL, rbuf, 1, &bic);
        endCmd(cmd);
        CHECK(queueSubmit(queue, 1, &(VkSubmitInfo){
            VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd, 0, NULL },
            VK_NULL_HANDLE) == VK_SUCCESS, "submit frame");
        queueWaitIdle(queue);

        /* host-coherent: content already visible; else invalidate */
        VkMappedMemoryRange mmr = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL,
            readMem, 0, VK_WHOLE_SIZE };
        invalidate(dev, 1, &mmr);

        /* verify: center pixel non-background, corners background-ish */
        const uint8_t (*c)[4] = img;
        uint32_t lit = 0;
        for (uint32_t i = 0; i < (size_t)W * H; i++)
            if (img[i][0] > 25 || img[i][1] > 25 || img[i][2] > 30) lit++;
        if (frame == 0) printf("     frame0 lit pixels: %u / %u\n", lit, W * H);
        const uint8_t *ctr = c[(H / 2) * W + W / 2];
        const uint8_t *corner = c[8 * W + 8];
        const int ctrOn = ctr[0] > 40 || ctr[1] > 40 || ctr[2] > 40;
        const int bgOk = corner[0] < 25 && corner[1] < 25 && corner[2] < 30;
        if (frame == 0) {
            CHECK(ctrOn, "cube visible (center pixel)");
            CHECK(bgOk, "clear color (corner pixel)");
        }
        if (ctrOn && bgOk) framesColored++;
        char ppm[64];
        snprintf(ppm, sizeof ppm, "/tmp/cube-f%d.ppm", frame);
        if (frame == 0 || frame == 7) {
            FILE *f = fopen(ppm, "wb");
            if (f) {
                fprintf(f, "P6\n%u %u\n255\n", W, H);
                for (uint32_t i = 0; i < (size_t)W * H; i++)
                    fwrite(img[i], 1, 3, f);
                fclose(f);
                printf("     wrote %s\n", ppm);
            }
        }
    }
    CHECK(framesColored == 8, "all 8 frames rendered (rotation visible)");

    printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
