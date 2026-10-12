/*
 * vmtl-uctest.c — userspace smoke test for the VirtIOGPUUserClient uapi shim.
 *
 * Exercises the same calls the Mesa vdrm_vmtl backend will make:
 *   open -> IOServiceOpen, staging -> IOConnectMapMemory64(type 0),
 *   ioctl -> IOConnectCallStructMethod(SEL_IOCTL, cmd in scalarInput[0]),
 *   bo map -> MAP ioctl + IOConnectMapMemory64(type = bo handle cookie).
 *
 * Build (in the guest): clang -O1 -o vmtl-uctest vmtl-uctest.c \
 *   -framework IOKit -framework CoreFoundation
 * Run as root.
 */
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>

#define SEL_IOCTL 0

#define DRM_COMMAND_BASE 0x40
enum {
    UAPI_MAP = 0x01, UAPI_EXECBUFFER = 0x02, UAPI_GETPARAM = 0x03,
    UAPI_RESOURCE_CREATE = 0x04, UAPI_RESOURCE_INFO = 0x05,
    UAPI_TRANSFER_FROM_HOST = 0x06, UAPI_TRANSFER_TO_HOST = 0x07,
    UAPI_WAIT = 0x08, UAPI_GET_CAPS = 0x09,
    UAPI_RESOURCE_CREATE_BLOB = 0x0a, UAPI_CONTEXT_INIT = 0x0b,
};

#define VIRTGPU_PARAM_3D_FEATURES 1
#define VIRTGPU_PARAM_CAPSET_QUERY_FIX 2
#define VIRTGPU_PARAM_RESOURCE_BLOB 3
#define VIRTGPU_PARAM_HOST_VISIBLE 4
#define VIRTGPU_PARAM_CROSS_DEVICE 5
#define VIRTGPU_PARAM_CONTEXT_INIT 6
#define VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs 7
#define VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME 8
#define VIRTGPU_PARAM_BLOB_ALIGNMENT 9

#define VIRTGPU_BLOB_MEM_HOST3D 0x0002
#define VIRTGPU_BLOB_FLAG_USE_MAPPABLE 0x0001

#define VIRTGPU_CONTEXT_PARAM_CAPSET_ID 0x0001
#define VIRTIO_GPU_CAPSET_VENUS 4

#define DRM_TYPE 'd'
#define DRM_IOC(dir, nr, size) \
    (((uint32_t)(dir) << 30) | ((uint32_t)(size) << 16) | ((uint32_t)(nr) << 8) | DRM_TYPE)
#define DRM_IOWR(nr, size) DRM_IOC(3, DRM_COMMAND_BASE + (nr), size)

struct drm_virtgpu_map { uint64_t offset; uint32_t handle; uint32_t pad; } __attribute__((packed));
struct drm_virtgpu_getparam { uint64_t param; uint64_t value; } __attribute__((packed));
struct drm_virtgpu_execbuffer {
    uint32_t flags; uint32_t size; uint64_t command; uint64_t bo_handles;
    uint32_t num_bo_handles; int32_t fence_fd; uint32_t ring_idx;
    uint32_t syncobj_stride; uint32_t num_in_syncobjs; uint32_t num_out_syncobjs;
    uint64_t in_syncobjs; uint64_t out_syncobjs;
} __attribute__((packed));
struct drm_virtgpu_resource_create_blob {
    uint32_t blob_mem; uint32_t blob_flags; uint32_t bo_handle; uint32_t res_handle;
    uint64_t size; uint32_t pad; uint32_t cmd_size; uint64_t cmd;
    uint64_t blob_id; uint32_t blob_hints; uint32_t pad2;
} __attribute__((packed));
struct drm_virtgpu_get_caps {
    uint32_t cap_set_id; uint32_t cap_set_ver; uint64_t addr; uint32_t size; uint32_t pad;
} __attribute__((packed));
struct drm_virtgpu_context_init { uint32_t num_params; uint32_t pad; uint64_t ctx_set_params; } __attribute__((packed));
struct drm_virtgpu_context_set_param { uint64_t param; uint64_t value; } __attribute__((packed));

static io_connect_t gConnect = 0;
static uint8_t *gStaging = 0;
static mach_vm_size_t gStagingLen = 0;
static int gPass = 0, gFail = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("PASS %s\n", name); gPass++; } \
    else      { printf("FAIL %s  (at line %d)\n", name, __LINE__); gFail++; } \
} while (0)

/* ioctl wrapper: cmd rides in scalarInput[0], buffer in/out in place */
static kern_return_t uioctl(uint32_t cmd, void *buf, uint32_t size)
{
    size_t outCnt = size;
    uint64_t scalarIn = cmd;
    return IOConnectCallMethod(gConnect, SEL_IOCTL, &scalarIn, 1,
                               buf, size, NULL, NULL, buf, &outCnt);
}

static void test_getparam(void)
{
    struct { const char *name; uint64_t param; int expect; } params[] = {
        { "3d_features",   VIRTGPU_PARAM_3D_FEATURES, 1 },
        { "capset_query_fix", VIRTGPU_PARAM_CAPSET_QUERY_FIX, 1 },
        { "resource_blob", VIRTGPU_PARAM_RESOURCE_BLOB, 1 },
        { "host_visible",  VIRTGPU_PARAM_HOST_VISIBLE, 1 },
        { "context_init",  VIRTGPU_PARAM_CONTEXT_INIT, 1 },
        { "blob_alignment", VIRTGPU_PARAM_BLOB_ALIGNMENT, 4096 },
    };
    for (unsigned i = 0; i < sizeof params / sizeof params[0]; i++) {
        struct drm_virtgpu_getparam gp = { params[i].param, 0 };
        kern_return_t kr = uioctl(DRM_IOWR(UAPI_GETPARAM, sizeof gp), &gp, sizeof gp);
        CHECK(kr == KERN_SUCCESS && (int)gp.value == params[i].expect, params[i].name);
    }
    struct drm_virtgpu_getparam gp = { VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs, 0 };
    kern_return_t kr = uioctl(DRM_IOWR(UAPI_GETPARAM, sizeof gp), &gp, sizeof gp);
    printf("     capset mask = %#x\n", (unsigned)gp.value);
    CHECK(kr == KERN_SUCCESS && (gp.value & (1 << VIRTIO_GPU_CAPSET_VENUS)), "supported_capsets has venus");
}

static void test_context_init(void)
{
    uint8_t buf[64];
    memset(buf, 0, sizeof buf);
    struct drm_virtgpu_context_init *ci = (struct drm_virtgpu_context_init *)buf;
    struct drm_virtgpu_context_set_param *sp =
        (struct drm_virtgpu_context_set_param *)(buf + sizeof *ci);
    ci->num_params = 1;
    sp[0].param = VIRTGPU_CONTEXT_PARAM_CAPSET_ID;
    sp[0].value = VIRTIO_GPU_CAPSET_VENUS;
    kern_return_t kr = uioctl(DRM_IOWR(UAPI_CONTEXT_INIT, sizeof buf), buf, sizeof buf);
    CHECK(kr == KERN_SUCCESS, "context_init(venus)");
}

static void test_get_caps(void)
{
    /* staging mode: the small descriptor goes inline, the capset data lands
       in the staging area at gc->addr (structs > 4096 B are sent OOL by
       IOKit, which the kext rejects) */
    struct drm_virtgpu_get_caps gc = {};
    gc.cap_set_id = VIRTIO_GPU_CAPSET_VENUS;
    gc.cap_set_ver = 0;
    gc.addr = 0x8000;           /* staging offset */
    gc.size = 4096;
    uint32_t size = sizeof gc;
    uint64_t scalarIn = DRM_IOWR(UAPI_GET_CAPS, size);
    size_t outCnt = sizeof gc;
    kern_return_t kr = IOConnectCallMethod(gConnect, SEL_IOCTL, &scalarIn, 1,
        &gc, size, NULL, NULL, &gc, &outCnt);
    if (kr == KERN_SUCCESS) {
        printf("     venus capset at staging+%#llx, first 16 bytes: ",
               (unsigned long long)gc.addr);
        for (unsigned i = 0; i < 16; i++)
            printf("%02x ", gStaging[gc.addr + i]);
        printf("\n");
    }
    CHECK(kr == KERN_SUCCESS, "get_caps(venus)");
}

static void test_blob(void)
{
    struct drm_virtgpu_resource_create_blob rc = {};
    rc.blob_mem = VIRTGPU_BLOB_MEM_HOST3D;
    rc.blob_flags = VIRTGPU_BLOB_FLAG_USE_MAPPABLE;
    rc.size = 256 * 1024;
    uint32_t size = sizeof rc;
    uint64_t scalarIn = DRM_IOWR(UAPI_RESOURCE_CREATE_BLOB, size);
    size_t outCnt = sizeof rc;
    kern_return_t kr = IOConnectCallMethod(gConnect, SEL_IOCTL, &scalarIn, 1,
        &rc, size, NULL, NULL, &rc, &outCnt);
    CHECK(kr == KERN_SUCCESS && rc.bo_handle, "resource_create_blob(host3d)");
    if (!(kr == KERN_SUCCESS && rc.bo_handle)) return;
    printf("     bo_handle %u res_handle %u\n", rc.bo_handle, rc.res_handle);

    struct drm_virtgpu_map m = { .handle = rc.bo_handle };
    kr = uioctl(DRM_IOWR(UAPI_MAP, sizeof m), &m, sizeof m);
    CHECK(kr == KERN_SUCCESS, "map ioctl");

    mach_vm_address_t mapAddr = 0;
    mach_vm_size_t mapLen = 0;
    kr = IOConnectMapMemory64(gConnect, (uint32_t)m.offset, mach_task_self(),
                              &mapAddr, &mapLen, kIOMapAnywhere);
    CHECK(kr == KERN_SUCCESS && mapLen >= 256 * 1024, "IOConnectMapMemory64(hostmem bo)");
    if (kr != KERN_SUCCESS) printf("     IOConnectMapMemory64 kr=%#x\n", kr);
    if (kr == KERN_SUCCESS) {
        volatile uint32_t *p = (volatile uint32_t *)mapAddr;
        p[0] = 0xcafebabe; p[1] = 0x12345678;
        printf("     hostmem mapped at %llx len %llu; wrote pattern\n",
               (unsigned long long)mapAddr, (unsigned long long)mapLen);
        CHECK(p[0] == 0xcafebabe && p[1] == 0x12345678, "hostmem readback");
    }

    struct drm_virtgpu_execbuffer eb = {};
    uint8_t cmd[16];
    memset(cmd, 0, sizeof cmd);   /* dummy venus command */
    eb.size = sizeof cmd;
    uint32_t sz = sizeof eb;
    /* inline protocol: cmd bytes follow the struct in the same buffer */
    uint8_t buf[sizeof eb + sizeof cmd];
    memcpy(buf, &eb, sizeof eb);
    memcpy(buf + sizeof eb, cmd, sizeof cmd);
    uint64_t ebScalarIn = DRM_IOWR(UAPI_EXECBUFFER, sizeof(struct drm_virtgpu_execbuffer));
    size_t ebOutCnt = sizeof buf;
    kr = IOConnectCallMethod(gConnect, SEL_IOCTL, &ebScalarIn, 1,
        buf, sizeof buf, NULL, NULL, buf, &ebOutCnt);
    CHECK(kr == KERN_SUCCESS, "execbuffer(submit 3d)");
    struct drm_virtgpu_execbuffer *ebr = (struct drm_virtgpu_execbuffer *)buf;
    printf("     execbuf fence cookie %#x\n", ebr->fence_fd);
}

int main(void)
{
    CFDictionaryRef match = IOServiceMatching("VirtIOGPU");
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, match);
    if (!svc) { printf("no VirtIOGPU service found\n"); return 1; }
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &gConnect);
    if (kr != KERN_SUCCESS) { printf("IOServiceOpen: %x (root?)\n", kr); return 1; }
    printf("opened user client %x\n", gConnect);

    kr = IOConnectMapMemory64(gConnect, 0, mach_task_self(),
                              (mach_vm_address_t *)&gStaging, &gStagingLen, kIOMapAnywhere);
    CHECK(kr == KERN_SUCCESS && gStagingLen >= 512 * 1024, "staging map (type 0)");
    printf("     staging at %p len %llu\n", gStaging, (unsigned long long)gStagingLen);

    test_getparam();
    test_context_init();
    test_get_caps();
    test_blob();

    printf("\n%d passed, %d failed\n", gPass, gFail);
    IOServiceClose(gConnect);
    return gFail ? 1 : 0;
}
