/*
 * VMTL virtio-gpu driver for macOS guests
 *
 * VirtIOGPU device (the resource manager, in the role NVRM.kext plays for
 * the NVIDIA card): claims the virtio-gpu-pci device, sets up the control
 * and cursor virtqueues, and answers a Linux-virtgpu-shaped uapi through
 * an IOUserClient so a patched Venus ICD needs only a thin shim on this
 * side (open -> IOServiceOpen, ioctl -> IOConnectCallMethod,
 * mmap -> IOConnectMapMemory64). See docs/PLAN.md section 1.
 *
 * Skeleton stage: device bring-up, queue negotiation and the uapi surface
 * exist; command submission (P1) fills sGoCtl/sGoQ and the hostmem BAR.
 *
 * P1 display: VirtIOFB (IOFramebuffer subclass, same bundle) attaches on top
 * of the VirtIOGPU service and publishes the scanout backing store as the
 * aperture; a 60 Hz vblank timer pushes changed frames to the host with
 * TRANSFER_TO_HOST_2D + RESOURCE_FLUSH.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>   /* kernel_task */

extern "C" {
kern_return_t _start(kmod_info_t *ki, void *data);
kern_return_t _stop(kmod_info_t *ki, void *data);
#ifndef VIRTIOFB_VIRTUAL
extern "C" { KMOD_EXPLICIT_DECL(io.vmtl.VirtIOGPU, "0.1", _start, _stop) }
#endif
__private_extern__ kmod_start_func_t *_realmain = NULL;
__private_extern__ kmod_stop_func_t  *_antimain = NULL;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
}

#define LOG(fmt, ...) do { kprintf("VirtIOGPU: " fmt "\n", ##__VA_ARGS__); IOLog("VirtIOGPU: " fmt "\n", ##__VA_ARGS__); } while (0)

/* ---- virtio PCI layout ----
   Transitional devices expose a legacy I/O BAR0 with fixed offsets; modern
   (virtio 1.0, disable-legacy=on) devices instead declare the common config,
   ISR and device config through virtio_pci_cap entries in the PCI capability
   list, typically all inside one memory BAR (BAR4 on QEMU virtio-gpu). */
#define VIRTIO_PCI_CAP_VENDOR        0x09
#define VIRTIO_PCI_CAP_COMMON_CFG    1
#define VIRTIO_PCI_CAP_NOTIFY_CFG    2
#define VIRTIO_PCI_CAP_ISR_CFG       3
#define VIRTIO_PCI_CAP_DEVICE_CFG    4
#define VIRTIO_PCI_CAP_SHARED_MEMORY_CFG 8

#define VIRTIO_GPU_SHM_ID_HOST_VISIBLE 1

#define VIRTIO_PCI_CAP_VNDR_OFF      0
#define VIRTIO_PCI_CAP_NEXT_OFF      1
#define VIRTIO_PCI_CAP_LEN_OFF       2
#define VIRTIO_PCI_CAP_TYPE_OFF      3
#define VIRTIO_PCI_CAP_BAR_OFF       4
#define VIRTIO_PCI_CAP_ID_OFF        5   /* shared memory caps only */
#define VIRTIO_PCI_CAP_OFFSET_OFF    8   /* 24-bit little-endian (bytes 5-7 are padding) */
#define VIRTIO_PCI_CAP_LENGTH_OFF    12  /* 32-bit little-endian */
#define VIRTIO_PCI_CAP_NOTIFY_OFF_MULTIPLIER 16 /* 32-bit, notify cap only */
#define VIRTIO_PCI_CAP64_OFFSET_HI   16  /* cap64: shared memory caps */
#define VIRTIO_PCI_CAP64_LENGTH_HI   20

/* common config fields (struct virtio_pci_common_cfg, virtio 1.0 spec 4.1.4.3;
   x86 guest is little-endian, matching the device's LE register layout) */
#define CC_DEVICE_FEATURE_SELECT    0x00
#define CC_DEVICE_FEATURE_STATUS    0x04
#define CC_DRIVER_FEATURE_SELECT    0x08
#define CC_DRIVER_FEATURE_STATUS    0x0c
#define CC_MSIX_CONFIG              0x10
#define CC_NUM_QUEUES               0x12
#define CC_DEVICE_STATUS            0x14
#define CC_CONFIG_GENERATION        0x15
#define CC_QUEUE_SELECT             0x16
#define CC_QUEUE_SIZE               0x18
#define CC_QUEUE_MSIX               0x1a
#define CC_QUEUE_ENABLE             0x1c
#define CC_QUEUE_NOTIFY_OFF         0x1e
#define CC_QUEUE_DESC               0x20
#define CC_QUEUE_AVAIL              0x28
#define CC_QUEUE_USED               0x30

#define VIRTIO_STATUS_ACK           0x01
#define VIRTIO_STATUS_DRIVER        0x02
#define VIRTIO_STATUS_DRIVER_OK     0x04
#define VIRTIO_STATUS_FEATURES_OK   0x08

#define VIRTIO_F_VERSION_1          32ULL
#define VIRTIO_F_IN_ORDER           35ULL

/* ---- virtio-gpu device feature bits and config ---- */
#define VIRTIO_GPU_F_VIRGL          0
#define VIRTIO_GPU_F_EDID           1
#define VIRTIO_GPU_F_RESOURCE_UUID  2
#define VIRTIO_GPU_F_RESOURCE_BLOB  3
#define VIRTIO_GPU_F_CONTEXT_INIT   4
#define VIRTIO_GPU_F_HOSTMEM        5

struct virtio_gpu_config {
    uint32_t events_read;
    uint32_t events_clear;
    uint32_t num_scanouts;
    uint32_t num_capsets;
} __attribute__((packed));

/* ---- control queue header types (P1 fills these in; named now so the
        uapi surface and the queue plumbing agree on one vocabulary) ---- */
enum virtio_gpu_ctrl_type {
    /* 2d commands */
    /* command type values from the kernel uapi (standard-headers/linux/virtio_gpu.h).
       Sequential enum starting at 0x0100: CREATE_2D=0x0101, UNREF=0x0102,
       SET_SCANOUT=0x0103, FLUSH=0x0104, TRANSFER_TO_HOST_2D=0x0105,
       ATTACH_BACKING=0x0106, DETACH_BACKING=0x0107, GET_CAPSET_INFO=0x0108,
       GET_CAPSET=0x0109, GET_EDID=0x010a, ASSIGN_UUID=0x010b,
       CREATE_BLOB=0x010c, SET_SCANOUT_BLOB=0x010d.
       (v20/v21 evidence: 0x0107 read as DETACH, 0x0104 as FLUSH, 0x0105 as T2D.) */
    VIRTIO_GPU_CMD_GET_DISPLAY_INFO = 0x0100,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_2D = 0x0101,
    VIRTIO_GPU_CMD_RESOURCE_UNREF   = 0x0102,
    VIRTIO_GPU_CMD_SET_SCANOUT      = 0x0103,
    VIRTIO_GPU_CMD_RESOURCE_FLUSH   = 0x0104,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D = 0x0105,
    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0106,
    VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING = 0x0107,
    VIRTIO_GPU_CMD_GET_CAPSET_INFO  = 0x0108,
    VIRTIO_GPU_CMD_GET_CAPSET       = 0x0109,
    VIRTIO_GPU_CMD_GET_EDID         = 0x010a,
    VIRTIO_GPU_CMD_RESOURCE_ASSIGN_UUID = 0x010b,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB = 0x010c,
    VIRTIO_GPU_CMD_SET_SCANOUT_BLOB = 0x010d,
    /* 3d / context (kernel uapi virtio_gpu.h) */
    VIRTIO_GPU_CMD_CTX_CREATE       = 0x0200,
    VIRTIO_GPU_CMD_CTX_DESTROY      = 0x0201,
    VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE = 0x0202,
    VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE = 0x0203,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_3D = 0x0204,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D = 0x0205,
    VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D = 0x0206,
    VIRTIO_GPU_CMD_SUBMIT_3D        = 0x0207,
    VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB = 0x0208,
    VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB = 0x0209,
    /* cursor */
    VIRTIO_GPU_CMD_UPDATE_CURSOR    = 0x0300,
    VIRTIO_GPU_CMD_MOVE_CURSOR      = 0x0301,
    /* responses */
    VIRTIO_GPU_RESP_OK_NODATA       = 0x1100,
    VIRTIO_GPU_RESP_OK_DISPLAY_INFO = 0x1101,
    VIRTIO_GPU_RESP_OK_CAPSET_INFO  = 0x1102,
    VIRTIO_GPU_RESP_OK_CAPSET       = 0x1103,
    VIRTIO_GPU_RESP_OK_EDID         = 0x1104,
    VIRTIO_GPU_RESP_OK_RESOURCE_UUID = 0x1105,
    VIRTIO_GPU_RESP_OK_MAP_INFO     = 0x1106,
    VIRTIO_GPU_RESP_ERR_UNSPEC      = 0x1200,
};

#define VIRTIO_GPU_FLAG_FENCE  (1 << 0)

struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed));

/* split-ring descriptor (vring 1.0 layout, little-endian in memory) */
struct vring_desc_raw {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

#define VRING_DESC_F_NEXT  1   /* chain continues through next */
#define VRING_DESC_F_WRITE 2   /* device-writable buffer */

struct vring_avail_raw {
    uint16_t flags;
    uint16_t idx;           /* driver-owned: next ring slot to publish */
    uint16_t ring[];        /* idx % size -> head descriptor */
} __attribute__((packed));

struct vring_used_elem_raw {
    uint32_t id;            /* head of the completed chain */
    uint32_t len;           /* bytes the device wrote */
} __attribute__((packed));

struct vring_used_raw {
    uint16_t flags;
    uint16_t idx;           /* device-owned: number of completed chains */
    struct vring_used_elem_raw ring[];
} __attribute__((packed));

/* GET_DISPLAY_INFO request/response (virtio-gpu spec 5.2.2) */
#define VIRTIO_GPU_MAX_SCANOUTS 16

struct virtio_gpu_rect {
    uint32_t x, y, width, height;
} __attribute__((packed));

struct virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    uint32_t enabled;
    uint32_t flags;
} __attribute__((packed));

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} __attribute__((packed));

/* capset enumeration (spec 5.2.2.3) */
#define VIRTIO_GPU_CAPSET_VIRGL             1
#define VIRTIO_GPU_CAPSET_VIRGL2            2
#define VIRTIO_GPU_CAPSET_GFXSTREAM_VULKAN  3
#define VIRTIO_GPU_CAPSET_VENUS             4
#define VIRTIO_GPU_CAPSET_CROSS_DOMAIN      5
#define VIRTIO_GPU_CAPSET_DRM               6

struct virtio_gpu_get_capset {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t capset_id;
    uint32_t capset_version;
} __attribute__((packed));

struct virtio_gpu_get_capset_info {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t capset_index;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resp_capset_info {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t capset_id;
    uint32_t capset_max_version;
    uint32_t capset_max_size;
    uint32_t padding;
} __attribute__((packed));

/* 2D resource / scanout plumbing (spec 5.2.2.5+) */
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM 1

struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;     /* NOTE: format before width/height (kernel uapi order) */
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} __attribute__((packed));

struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

/* ---- 3D protocol (kernel uapi virtio_gpu.h) ---- */
#define VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK 0x000000ff

struct virtio_gpu_ctx_create {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t nlen;
    uint32_t context_init;   /* low 8 bits: capset id */
    char debug_name[64];
} __attribute__((packed));

struct virtio_gpu_ctx_destroy {
    struct virtio_gpu_ctrl_hdr hdr;
} __attribute__((packed));

struct virtio_gpu_ctx_resource {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_cmd_submit {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t size;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_box {
    uint32_t x, y, z, w, h, d;
} __attribute__((packed));

struct virtio_gpu_transfer_host_3d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_box box;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t level;
    uint32_t stride;
    uint32_t layer_stride;
} __attribute__((packed));

struct virtio_gpu_resource_create_blob {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t blob_mem;
    uint32_t blob_flags;
    uint32_t nr_entries;
    uint64_t blob_id;
    uint64_t size;
} __attribute__((packed));

struct virtio_gpu_resource_map_blob {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
    uint64_t offset;   /* offset within the host-visible shm region (guest-chosen) */
} __attribute__((packed));

struct virtio_gpu_resp_map_info {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t map_info;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_unmap_blob {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

#define VIRTIO_GPU_MAP_CACHE_MASK 0x0f
#define VIRTIO_GPU_MAP_CACHE_CACHED 0x01
#define VIRTIO_GPU_MAP_CACHE_WC     0x03

/* ---- Linux virtgpu DRM uapi shapes (the shim passes these verbatim;
        pointer fields are repurposed as inline-data sizes, see uapiIoctl) ---- */
#define DRM_COMMAND_BASE 0x40
enum {   /* nr values 1..11 (DRM_VIRTGPU_*) */
    UAPI_MAP = 0x01,
    UAPI_EXECBUFFER = 0x02,
    UAPI_GETPARAM = 0x03,
    UAPI_RESOURCE_CREATE = 0x04,
    UAPI_RESOURCE_INFO = 0x05,
    UAPI_TRANSFER_FROM_HOST = 0x06,
    UAPI_TRANSFER_TO_HOST = 0x07,
    UAPI_WAIT = 0x08,
    UAPI_GET_CAPS = 0x09,
    UAPI_RESOURCE_CREATE_BLOB = 0x0a,
    UAPI_CONTEXT_INIT = 0x0b,
    UAPI_GEM_CLOSE = 0x0c,
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

#define VIRTGPU_BLOB_MEM_GUEST        0x0001
#define VIRTGPU_BLOB_MEM_HOST3D       0x0002
#define VIRTGPU_BLOB_MEM_HOST3D_GUEST 0x0003
#define VIRTGPU_BLOB_FLAG_USE_MAPPABLE     0x0001
#define VIRTGPU_BLOB_FLAG_USE_SHAREABLE    0x0002
#define VIRTGPU_BLOB_FLAG_USE_CROSS_DEVICE 0x0004
#define VIRTGPU_BLOB_FLAG_USE_MASK 0x0007

struct drm_virtgpu_map {
    uint64_t offset;
    uint32_t handle;
    uint32_t pad;
} __attribute__((packed));

struct drm_virtgpu_getparam {
    uint64_t param;
    uint64_t value;   /* shim protocol: kext writes the int back into this slot */
} __attribute__((packed));

struct drm_virtgpu_execbuffer {
    uint32_t flags;
    uint32_t size;
    uint64_t command;         /* shim protocol: 0, cmd bytes follow the struct */
    uint64_t bo_handles;      /* shim protocol: 0, u32 array follows the cmd bytes */
    uint32_t num_bo_handles;
    int32_t fence_fd;         /* shim protocol: out = already-signaled cookie */
    uint32_t ring_idx;
    uint32_t syncobj_stride;
    uint32_t num_in_syncobjs;
    uint32_t num_out_syncobjs;
    uint64_t in_syncobjs;
    uint64_t out_syncobjs;
} __attribute__((packed));

#define VIRTGPU_EXECBUF_FENCE_FD_OUT 0x02
#define VIRTGPU_EXECBUF_RING_IDX 0x04

struct drm_virtgpu_resource_info {
    uint32_t bo_handle;
    uint32_t res_handle;
    uint32_t size;
    uint32_t blob_mem;
} __attribute__((packed));

struct drm_virtgpu_3d_box {
    uint32_t x, y, z, w, h, d;
} __attribute__((packed));

struct drm_virtgpu_3d_transfer_to_host {
    uint32_t bo_handle;
    struct drm_virtgpu_3d_box box;
    uint32_t level;
    uint32_t offset;
    uint32_t stride;
    uint32_t layer_stride;
} __attribute__((packed));

struct drm_virtgpu_3d_transfer_from_host {
    uint32_t bo_handle;
    struct drm_virtgpu_3d_box box;
    uint32_t level;
    uint32_t offset;
    uint32_t stride;
    uint32_t layer_stride;
} __attribute__((packed));

#define VIRTGPU_WAIT_NOWAIT 1
struct drm_virtgpu_3d_wait {
    uint32_t handle;
    uint32_t flags;
} __attribute__((packed));

struct drm_virtgpu_get_caps {
    uint32_t cap_set_id;
    uint32_t cap_set_ver;
    uint64_t addr;    /* shim protocol: 0, capset data follows the struct */
    uint32_t size;
    uint32_t pad;
} __attribute__((packed));

struct drm_virtgpu_resource_create_blob {
    uint32_t blob_mem;
    uint32_t blob_flags;
    uint32_t bo_handle;    /* out */
    uint32_t res_handle;   /* out */
    uint64_t size;
    uint32_t pad;
    uint32_t cmd_size;
    uint64_t cmd;          /* shim protocol: 0, cmd bytes follow the struct */
    uint64_t blob_id;
    uint32_t blob_hints;
    uint32_t pad2;
} __attribute__((packed));

#define VIRTGPU_CONTEXT_PARAM_CAPSET_ID 0x0001
#define VIRTGPU_CONTEXT_PARAM_NUM_RINGS 0x0002
struct drm_virtgpu_context_set_param {
    uint64_t param;
    uint64_t value;
} __attribute__((packed));

struct drm_virtgpu_context_init {
    uint32_t num_params;
    uint32_t pad;
    uint64_t ctx_set_params;   /* shim protocol: 0, array follows the struct */
} __attribute__((packed));

/* one virtqueue, split-ring layout, pinned in driver memory */
#define VIRTIO_MAX_QUEUES 2   /* controlq + cursorq */
#define VQ_RING_SIZE      256

struct vq {
    uint16_t   idx;         /* queue index in the device */
    uint16_t   size;
    void      *desc;        /* split ring descriptors */
    void      *avail;
    void      *used;
    uint16_t   freeHead, freeCount;
    uint16_t   lastUsed;    /* driver-owned: next used-ring slot to reap */
    uint16_t   availIdx;    /* driver-owned: count of published chains */
    uint16_t   availFlagsShadow;
    uint32_t   notifyOff;   /* common-cfg value; doorbell = notify + off*mult */
    IOBufferMemoryDescriptor *descMem;   /* one allocation holds desc+avail+used, aligned */
    void      *cookie[VQ_RING_SIZE];
};

/* two IOKit personalities in one bundle:
   - VirtIOGPU claims the PCI device (probe score 20000 in the plist)
   - VirtIOFB attaches on the VirtIOGPU service; IOMatchCategory IOFramebuffer
     makes it the one framebuffer candidate on this controller, and a score
     above IONDRVFramebuffer's keeps it in front of the firmware console. */
class VirtIOGPU;
class VirtIOGPUUserClient;

class VirtIOGPU : public IOService {
    OSDeclareDefaultStructors(VirtIOGPU)
    friend class VirtIOGPUUserClient;
public:
    virtual IOService *probe(IOService *provider, SInt32 *score) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual IOWorkLoop *getWorkLoop() const override;

    /* uapi surface (P1): the ioctl-shaped commands. `staging` is the
       connection's shared staging buffer (may be null): uapi pointer fields
       hold either 0 (data follows the struct, small payloads) or a staging
       byte offset (cmd batches up to the staging size). */
    IOReturn uapiIoctl(uint32_t cmd, void *data, uint32_t size, uint32_t *outSize,
                       const void *staging, uint32_t stagingLen, int *ctxIdx);
    /* per-connection teardown: destroys the session's context and its bos
       (Linux semantics: GEM handles and the drm_file's context die together) */
    void uapiClientClosed(int *ctxIdx);
    IOReturn uapiWaitEvent(uint32_t fenceId, uint32_t timeoutUs, uint32_t *signaled);
    /* P2: memory for a mapped blob (type = bo handle) via clientMemoryForType */
    IOMemoryDescriptor *uapiMemoryForType(uint32_t type);

    /* ---- surface VirtIOFB (IOFramebuffer) sits on ---- */
    /* initial state was proven with the checkerboard; now it is the
       framebuffer backing store itself (zeroed black) */
    uint32_t scanoutWidth()  const { return fScanoutW; }
    uint32_t scanoutHeight() const { return fScanoutH; }
    uint32_t scanoutRowBytes() const { return fScanoutW * 4; }
    void *scanoutKva() const { return fFbKva; }
    IOPhysicalAddress scanoutPhys() const { return fFbPhys; }
    /* push (part of) the current frame to the host and repaint the display */
    bool flushFrame(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    /* host-side framebuffer size (dpi etc.) for the registry */
private:
    IOPCIDevice *fPCI = nullptr;
    /* one IOMemoryMap per referenced BAR; fCommon/fIsr/fDevCfg point inside them */
    IOMemoryMap  *fBarMaps[6] = {};
    volatile uint8_t *fCommon = nullptr;
    volatile uint8_t *fIsr = nullptr;
    volatile uint8_t *fDevCfg = nullptr;
    volatile uint32_t *fNotify = nullptr;   /* notify cap: offset + multiplier */
    uint32_t fNotifyMult = 0;
    uint32_t fNotifyBarOff = 0;             /* notify region offset within its BAR */

    struct virtio_gpu_config fCfg = {};
    uint64_t fFeatures = 0;
    bool     fHasBlob = false, fHasEdid = false;

    vq fVq[VIRTIO_MAX_QUEUES] = {};
    /* shared staging buffer for controlq requests/responses:
       [0..2048) request, [2048..8192) response */
    IOBufferMemoryDescriptor *fCtrlBuf = nullptr;
    uint8_t *fCtrlV = nullptr;
    IOPhysicalAddress fCtrlP = 0;
    /* 2D scanout framebuffer */
    IOBufferMemoryDescriptor *fFbMem = nullptr;
    uint8_t *fFbKva = nullptr;
    IOPhysicalAddress fFbPhys = 0;
    uint32_t fScanoutW = 0, fScanoutH = 0;
    uint32_t fScanoutRes = 0;   /* resource id the scanout is (to be) attached to */

    /* ---- P2: virtgpu-uapi state ----
       All uapi ioctls and the vbl flush serialize on fUapiLock (recursive:
       both paths go through ctrlCmd/ctrlCmdData, which share the staging
       buffers and the "one command in flight" sequencing). */
    IORecursiveLock *fUapiLock = nullptr;
    /* host-visible shared memory region (VIRTIO_GPU_SHM_ID_HOST_VISIBLE):
       MAP_BLOB puts host3d blobs here at guest-chosen offsets. We keep only
       the physical address (taken from the PCI BAR config) — the kext never
       dereferences the region itself; userspace maps it per-bo. */
    IOPhysicalAddress fShmPa = 0;
    uint64_t fShmLen = 0;
    uint64_t fShmCursor = 0;    /* bump allocator; freed on bo close (rare) */
    /* second staging buffer: 3D command data / blob cmd payloads */
    IOBufferMemoryDescriptor *fDataBuf = nullptr;
    uint8_t *fDataV = nullptr;
    IOPhysicalAddress fDataP = 0;
    static const uint32_t kDataBufLen = 64 * 1024;

    struct uapiBo {
        bool used = false;
        uint32_t resId = 0;
        uint64_t size = 0;
        uint32_t blobMem = 0, blobFlags = 0;
        uint32_t ctxId = 0;         /* context the resource was created in */
        bool attached = false;      /* resource attached to ctxId on the host */
        bool mapped = false;        /* host3d blob mapped into the shm region */
        uint64_t shmOff = 0;
        IOPhysicalAddress shmPa = 0;
        IOBufferMemoryDescriptor *guestMem = nullptr;   /* GUEST/HOST3D_GUEST backing */
        IOPhysicalAddress guestPa = 0;
    };
    static const uint32_t kMaxBos = 256;
    uapiBo fBo[kMaxBos];
    /* uapi resources start high: ids 1..kMaxScanout are owned by the 2D
       scanout path, and the host keeps one global resource-id namespace */
    uint32_t fNextResId = 0x100, fNextHandle = 1;
    uint32_t fNextFenceCookie = 0x40000000;

    struct uapiCtx { bool used = false; uint32_t ctxId = 0, capsetId = 0; };
    static const uint32_t kMaxCtx = 8;
    uapiCtx fCtx[kMaxCtx];

    /* capsets discovered by probeCapsets, for GET_CAPS + SUPPORTED_CAPSET_IDs */
    struct capsetInfo { uint32_t id, ver, size; };
    capsetInfo fCapsets[16];
    uint32_t fNumCapsets = 0;
    uint32_t fCapsetIdMask = 0;
    bool fHasContextInit = false, fHasHostVisible = false;

    bool uapiInit();
    void uapiCleanup();
    int boLookup(uint32_t handle) const;   /* table index or -1 */
    int capsetIndex(uint32_t id) const;    /* fCapsets index or -1 */
    bool ctrlCmdData(const void *req, uint32_t reqLen,
                     const void *data, uint32_t dataLen,
                     void *resp, uint32_t respLen);
    int uapiCtxCreateLocked(uint32_t capsetId, const char *name);   /* returns fCtx slot or -1 */
    void uapiCtxDestroyLocked(int idx);
    IOReturn uapiExecbufLocked(struct drm_virtgpu_execbuffer *eb, uint32_t size,
                               const void *staging, uint32_t stagingLen, int ctxIdx);
    IOReturn uapiBlobCreateLocked(struct drm_virtgpu_resource_create_blob *rc, uint32_t size, uint32_t *outSize,
                                  const void *staging, uint32_t stagingLen, int ctxIdx);
    IOReturn uapiGetCapsLocked(struct drm_virtgpu_get_caps *gc, uint32_t size, uint32_t *outSize,
                               const void *staging, uint32_t stagingLen);
    IOReturn uapiTransferLocked(bool toHost, struct drm_virtgpu_3d_transfer_to_host *t, uint32_t size);
    bool uapiMapBlobLocked(uapiBo *bo);
    void uapiUnmapBlobLocked(uapiBo *bo);
    IOReturn uapiCloseHandleLocked(uint32_t handle);
    /* the work loop is created lazily by IOService; keep it mutable here */
    mutable IOWorkLoop *fWorkLoop = nullptr;
    IOInterruptEventSource *fIntSrc = nullptr;

    bool negotiateFeatures();
    bool initQueue(uint16_t index, uint16_t size);
    bool readDeviceConfig();
    /* walk the PCI capability list and map the virtio modern regions */
    bool locateModernRegions();
    void dumpCommon(const char *when);
    IOMemoryMap *barMap(uint8_t bar);
    /* P1: kick a queue and reap completions */
    bool allocChain(vq *q, uint16_t *out, int count);   /* count contiguous free descriptors */
    void freeDescChain(vq *q, uint16_t head);
    bool submit(vq *q, uint16_t head);
    void reap(vq *q);
    /* one controlq round trip through the shared staging buffer */
    bool ctrlCmd(const void *req, uint32_t reqLen, void *resp, uint32_t respLen);
    bool probeDisplay();
    bool probeCapsets();
    bool bringUpScanout();
    void interruptOccurred(IOInterruptEventSource *src, int count);
};

#define super IOService
OSDefineMetaClassAndStructors(VirtIOGPU, IOService)

IOService *VirtIOGPU::probe(IOService *provider, SInt32 *score)
{
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return nullptr;
    /* the plist mask is wide on purpose (any virtio display device); this gate
       accepts only virtio-gpu (0x1050) with a virtio-1.0 revision */
    uint16_t deviceId = pci->configRead16(kIOPCIConfigDeviceID);
    if (deviceId != 0x1050) return nullptr;
    uint8_t revision = pci->configRead8(kIOPCIConfigRevisionID);
    if (revision < 1) { kprintf("VirtIOGPU: transitional device (rev 0) not supported\n"); return nullptr; }
    /* must beat AppleVirtIOPCITransport (generic 1af4 personality, score 10000) */
    if (score) *score = 20000;
    return this;
}

bool VirtIOGPU::start(IOService *provider)
{
    if (!super::start(provider)) return false;
    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI) { LOG("no PCI provider"); return false; }

    fPCI->setIOEnable(true);
    fPCI->setMemoryEnable(true);
    fPCI->setBusMasterEnable(true);   /* the device DMAs into the used ring */

    if (!locateModernRegions()) { LOG("modern region location failed"); return false; }

    /* device reset, then acknowledge */
    fCommon[CC_DEVICE_STATUS] = 0;
    fCommon[CC_DEVICE_STATUS] |= VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER;

    if (!negotiateFeatures()) { LOG("feature negotiation failed"); return false; }
    if (!readDeviceConfig()) { LOG("device config read failed"); return false; }

    /* controlq first; the cursorq comes with the framebuffer (P1) */
    if (!initQueue(0, VQ_RING_SIZE)) { LOG("controlq init failed"); return false; }

    fCommon[CC_DEVICE_STATUS] |= VIRTIO_STATUS_DRIVER_OK;
    LOG("virtio-gpu up: scanouts %u capsets %u blob %d edid %d",
        fCfg.num_scanouts, fCfg.num_capsets, fHasBlob, fHasEdid);

    /* shared controlq staging buffer: [0..2048) request, [2048..8192) response */
    fCtrlBuf = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous, 8192, 4096);
    if (fCtrlBuf && fCtrlBuf->prepare() == kIOReturnSuccess) {
        fCtrlV = (uint8_t *)fCtrlBuf->getBytesNoCopy();
        fCtrlP = fCtrlBuf->getPhysicalAddress();
        LOG("ctrl staging at pa %#llx", (unsigned long long)fCtrlP);
    } else {
        LOG("ctrl staging alloc failed");
    }

    /* create the scanout resource, then wire up attach + scanout + flush */
    fScanoutW = 1280; fScanoutH = 800;
    bool created = false;
    {
        struct virtio_gpu_resource_create_2d create = {};
        struct virtio_gpu_ctrl_hdr resp = {};
        create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
        create.resource_id = 1;
        create.width = fScanoutW; create.height = fScanoutH;
        create.format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
        bool ok = ctrlCmd(&create, sizeof(create), &resp, sizeof(resp));
        LOG("CREATE_2D res 1 %ux%u fmt %u: ok %d resp %#x",
            fScanoutW, fScanoutH, create.format, ok, resp.type);
        created = ok && resp.type == VIRTIO_GPU_RESP_OK_NODATA;
    }
    if (created) {
        fScanoutRes = 1;
        probeDisplay();
        probeCapsets();
        bringUpScanout();
        uapiInit();
        /* publish after the scanout is up so the VirtIOFB nub (matching on
           this service) starts against a live backing store. Required for
           the /Library/Extensions runtime-load path: kextd loads us long
           after the PCI bus is done matching, so without registerService()
           the fb personality never gets a second chance to attach. */
        registerService();
    } else {
        probeDisplay();
        probeCapsets();
    }
    return true;
}

void VirtIOGPU::stop(IOService *provider)
{
    if (fIntSrc) { fIntSrc->disable(); fWorkLoop->removeEventSource(fIntSrc); fIntSrc->release(); fIntSrc = nullptr; }
    uapiCleanup();
    if (fFbMem) { fFbMem->complete(); fFbMem->release(); fFbMem = nullptr; }
    fFbKva = nullptr; fFbPhys = 0;
    if (fCtrlBuf) { fCtrlBuf->complete(); fCtrlBuf->release(); fCtrlBuf = nullptr; }
    fCtrlV = nullptr; fCtrlP = 0;
    for (int i = 0; i < VIRTIO_MAX_QUEUES; i++) {
        if (fVq[i].descMem) { fVq[i].descMem->release(); fVq[i].descMem = nullptr; }
        if (fCommon) {
            *(volatile uint16_t *)&fCommon[CC_QUEUE_SELECT] = (uint16_t)i;
            *(volatile uint16_t *)&fCommon[CC_QUEUE_ENABLE] = 0;
        }
    }
    if (fCommon) fCommon[CC_DEVICE_STATUS] = 0;
    fShmPa = 0; fShmLen = 0; fShmCursor = 0;
    for (int i = 0; i < 6; i++) {
        if (fBarMaps[i]) { fBarMaps[i]->release(); fBarMaps[i] = nullptr; }
    }
    fCommon = fIsr = fDevCfg = nullptr; fNotify = nullptr;
    super::stop(provider);
}

IOMemoryMap *VirtIOGPU::barMap(uint8_t bar)
{
    if (bar > 5) return nullptr;
    if (!fBarMaps[bar]) {
        fBarMaps[bar] = fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0 + 4 * bar);
        if (!fBarMaps[bar]) LOG("BAR%u map failed", bar);
    }
    return fBarMaps[bar];
}

void VirtIOGPU::dumpCommon(const char *when)
{
    LOG("common@%s: 00-07 %02x %02x %02x %02x %02x %02x %02x %02x | 08-0f %02x %02x %02x %02x %02x %02x %02x %02x",
        when, fCommon[0], fCommon[1], fCommon[2], fCommon[3],
        fCommon[4], fCommon[5], fCommon[6], fCommon[7],
        fCommon[8], fCommon[9], fCommon[10], fCommon[11],
        fCommon[12], fCommon[13], fCommon[14], fCommon[15]);
    LOG("common@%s: 10-17 %02x %02x %02x %02x %02x %02x %02x %02x | 18-1f %02x %02x %02x %02x %02x %02x %02x %02x",
        when, fCommon[16], fCommon[17], fCommon[18], fCommon[19],
        fCommon[20], fCommon[21], fCommon[22], fCommon[23],
        fCommon[24], fCommon[25], fCommon[26], fCommon[27],
        fCommon[28], fCommon[29], fCommon[30], fCommon[31]);
}

/* virtio 1.0 section 4.1.4: the modern regions are declared by vendor
   capabilities (cap type 9); each names a BAR, a byte offset within it and a
   length. A device without these caps is transitional-only and unsupported. */
bool VirtIOGPU::locateModernRegions()
{
    uint8_t capPtr = fPCI->configRead8(kIOPCIConfigCapabilitiesPtr);
    bool haveCommon = false, haveIsr = false, haveDev = false, haveNotify = false;
    for (int hops = 0; hops < 48 && capPtr; hops++) {
        uint8_t capVendor = fPCI->configRead8(capPtr + VIRTIO_PCI_CAP_VNDR_OFF);
        if (capVendor != VIRTIO_PCI_CAP_VENDOR) {
            capPtr = fPCI->configRead8(capPtr + VIRTIO_PCI_CAP_NEXT_OFF);
            continue;
        }
        uint8_t cfgType = fPCI->configRead8(capPtr + VIRTIO_PCI_CAP_TYPE_OFF);
        uint8_t capLen  = fPCI->configRead8(capPtr + VIRTIO_PCI_CAP_LEN_OFF);
        uint8_t bar     = fPCI->configRead8(capPtr + VIRTIO_PCI_CAP_BAR_OFF);
        uint32_t offset = fPCI->configRead32(capPtr + VIRTIO_PCI_CAP_OFFSET_OFF) & 0xffffffu;
        uint32_t len    = fPCI->configRead32(capPtr + VIRTIO_PCI_CAP_LENGTH_OFF) & 0xffffffu;

        if (cfgType == VIRTIO_PCI_CAP_SHARED_MEMORY_CFG) {
            /* cap64: 64-bit offset/length in hi words (cap_len >= 20) */
            uint64_t off64 = offset, len64 = len;
            if (capLen >= VIRTIO_PCI_CAP64_LENGTH_HI + 4) {
                off64 |= (uint64_t)fPCI->configRead32(capPtr + VIRTIO_PCI_CAP64_OFFSET_HI) << 32;
                len64 |= (uint64_t)fPCI->configRead32(capPtr + VIRTIO_PCI_CAP64_LENGTH_HI) << 32;
            }
            uint8_t shmId = fPCI->configRead8(capPtr + VIRTIO_PCI_CAP_ID_OFF);
            LOG("cap type 8 (shm): id %u bar %u off %#llx len %#llx", shmId, bar,
                (unsigned long long)off64, (unsigned long long)len64);
            if (shmId == VIRTIO_GPU_SHM_ID_HOST_VISIBLE) {
                /* take the region's physical address straight from the BAR
                   config — avoids mapping a multi-GB BAR in-kernel */
                uint64_t barPa = 0;
                uint32_t barLo = fPCI->configRead32(kIOPCIConfigBaseAddress0 + 4 * bar);
                if (barLo & 0x4u) {  /* 64-bit */
                    uint32_t barHi = fPCI->configRead32(kIOPCIConfigBaseAddress0 + 4 * bar + 4);
                    barPa = ((uint64_t)barHi << 32) | (barLo & ~0xfull);
                } else if ((barLo & 0x1u) == 0) {
                    barPa = barLo & ~0xfull;
                }
                if (barPa) {
                    fShmPa = (IOPhysicalAddress)(barPa + off64);
                    fShmLen = len64;
                    fHasHostVisible = true;
                } else {
                    LOG("shm cap: BAR%u unassigned (raw %#x)", bar, barLo);
                }
            }
            capPtr = fPCI->configRead8(capPtr + VIRTIO_PCI_CAP_NEXT_OFF);
            continue;
        }
        if (cfgType > VIRTIO_PCI_CAP_DEVICE_CFG) {
            /* shared-memory and newer cap types: not needed for P1 */
            capPtr = fPCI->configRead8(capPtr + VIRTIO_PCI_CAP_NEXT_OFF);
            continue;
        }
        LOG("cap type %u: bar %u off %#x len %#x", cfgType, bar, offset, len);

        IOMemoryMap *m = barMap(bar);
        if (!m || m->getLength() < offset + len) {
            LOG("cap type %u: BAR%u (len %llu) too small for off %u len %u",
                cfgType, bar, m ? (unsigned long long)m->getLength() : 0ull, offset, len);
        } else {
            volatile uint8_t *base = (volatile uint8_t *)m->getVirtualAddress() + offset;
            switch (cfgType) {
            case VIRTIO_PCI_CAP_COMMON_CFG: fCommon = base; haveCommon = true; break;
            case VIRTIO_PCI_CAP_ISR_CFG:    fIsr    = base; haveIsr    = true; break;
            case VIRTIO_PCI_CAP_DEVICE_CFG: fDevCfg = base; haveDev    = true; break;
            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                fNotify = (volatile uint32_t *)base;
                fNotifyMult = fPCI->configRead32(capPtr + VIRTIO_PCI_CAP_NOTIFY_OFF_MULTIPLIER);
                fNotifyBarOff = offset;
                haveNotify = true;
                break;
            default: break;   /* shared memory / pci alt cfg: ignore for now */
            }
        }
        capPtr = fPCI->configRead8(capPtr + VIRTIO_PCI_CAP_NEXT_OFF);
    }
    LOG("caps: common %d isr %d devcfg %d notify %d (mult %u)",
        haveCommon, haveIsr, haveDev, haveNotify, fNotifyMult);
    if (haveDev) {
        /* virtio_gpu_config: events_read, events_clear, num_scanouts, num_capsets.
           num_scanouts must be >0; this dump also proves the BAR translation
           works for a second, different offset. */
        volatile struct virtio_gpu_config *dc = (volatile struct virtio_gpu_config *)fDevCfg;
        LOG("devcfg: events_read %#x num_scanouts %u num_capsets %u",
            dc->events_read, dc->num_scanouts, dc->num_capsets);
    }
    if (haveCommon) {
        dumpCommon("pre-reset");
        /* device reset, then acknowledge; verify the write lands */
        fCommon[CC_DEVICE_STATUS] = 0;
        dumpCommon("post-reset");
        LOG("status byte after reset-write: %#x (want 0)", fCommon[CC_DEVICE_STATUS]);
    }
    return haveCommon && haveIsr && haveDev && haveNotify;
}

IOWorkLoop *VirtIOGPU::getWorkLoop() const
{
    if (!fWorkLoop) fWorkLoop = IOService::getWorkLoop();  /* real IOKit: lazy, returns the service work loop */
    return fWorkLoop;
}

bool VirtIOGPU::negotiateFeatures()
{
    /* 64-bit feature read, then take what we need */
    *(volatile uint32_t *)&fCommon[CC_DEVICE_FEATURE_SELECT] = 0;
    uint64_t lo = *(volatile uint32_t *)&fCommon[CC_DEVICE_FEATURE_STATUS];
    *(volatile uint32_t *)&fCommon[CC_DEVICE_FEATURE_SELECT] = 1;
    uint64_t hi = *(volatile uint32_t *)&fCommon[CC_DEVICE_FEATURE_STATUS];
    fFeatures = lo | (hi << 32);
    LOG("device features: lo %#llx hi %#llx", (unsigned long long)lo, (unsigned long long)hi);
    if (!(fFeatures & (1ULL << VIRTIO_F_VERSION_1))) { LOG("not a virtio 1.0 device"); return false; }

    uint64_t want = (1ULL << VIRTIO_F_VERSION_1);
    if (fFeatures & (1ULL << VIRTIO_GPU_F_EDID))   { want |= (1ULL << VIRTIO_GPU_F_EDID); }
    if (fFeatures & (1ULL << VIRTIO_GPU_F_RESOURCE_BLOB)) { want |= (1ULL << VIRTIO_GPU_F_RESOURCE_BLOB); }
    if (fFeatures & (1ULL << VIRTIO_GPU_F_CONTEXT_INIT))  { want |= (1ULL << VIRTIO_GPU_F_CONTEXT_INIT); }
    if (fFeatures & (1ULL << VIRTIO_F_IN_ORDER))   { want |= (1ULL << VIRTIO_F_IN_ORDER); }

    *(volatile uint32_t *)&fCommon[CC_DRIVER_FEATURE_SELECT] = 0;
    *(volatile uint32_t *)&fCommon[CC_DRIVER_FEATURE_STATUS] = (uint32_t)(want & 0xffffffffu);
    *(volatile uint32_t *)&fCommon[CC_DRIVER_FEATURE_SELECT] = 1;
    *(volatile uint32_t *)&fCommon[CC_DRIVER_FEATURE_STATUS] = (uint32_t)(want >> 32);
    fFeatures = want;

    fCommon[CC_DEVICE_STATUS] |= VIRTIO_STATUS_FEATURES_OK;
    uint8_t st = fCommon[CC_DEVICE_STATUS];
    if (!(st & VIRTIO_STATUS_FEATURES_OK)) {
        LOG("FEATURES_OK refused (status %#x) - device features %#llx want %#llx",
            st, (unsigned long long)(lo | (hi << 32)), (unsigned long long)want);
        return false;
    }

    fHasBlob = (want >> VIRTIO_GPU_F_RESOURCE_BLOB) & 1;
    fHasEdid = (want >> VIRTIO_GPU_F_EDID) & 1;
    fHasContextInit = (want >> VIRTIO_GPU_F_CONTEXT_INIT) & 1;
    /* fHasHostVisible: set by locateModernRegions from the shm cap (runs
       before this; host-visible is not a virtio feature bit). */
    return true;
}

bool VirtIOGPU::readDeviceConfig()
{
    volatile struct virtio_gpu_config *c = (volatile struct virtio_gpu_config *)fDevCfg;
    fCfg.events_read  = c->events_read;
    fCfg.events_clear = c->events_clear;
    fCfg.num_scanouts = c->num_scanouts;
    fCfg.num_capsets  = c->num_capsets;
    return fCfg.num_scanouts > 0;
}

bool VirtIOGPU::initQueue(uint16_t index, uint16_t size)
{
    *(volatile uint16_t *)&fCommon[CC_QUEUE_SELECT] = index;
    uint16_t devSize = *(volatile uint16_t *)&fCommon[CC_QUEUE_SIZE];
    if (devSize == 0 || devSize > VQ_RING_SIZE) devSize = VQ_RING_SIZE;
    if (devSize != size) {
        LOG("queue %u: device offers %u, using that instead of %u", index, devSize, size);
        size = devSize;
    }
    vq *q = &fVq[index];
    q->idx = index; q->size = size;
    /* desc (16*size) + avail (6 + 2*size + 2) + padding to 4k + used (6 + 8*size) */
    IOByteCount len = 16ull * size + 6ull + 2ull * size + 2ull + 4096ull + 6ull + 8ull * size;
    q->descMem = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous, len, 4096);
    if (!q->descMem) { LOG("queue %u: ring alloc %llu bytes failed", index, (uint64_t)len); return false; }
    if (q->descMem->prepare() != kIOReturnSuccess) { LOG("queue %u: ring prepare failed", index); q->descMem->release(); q->descMem = nullptr; return false; }
    uint8_t *v = (uint8_t *)q->descMem->getBytesNoCopy();
    IOPhysicalAddress pa = q->descMem->getPhysicalAddress();
    q->desc = v;
    q->avail = v + 16ull * size;
    q->used = v + 16ull * size + 6ull + 2ull * size + 2ull
            + (4096ull - (16ull * size + 6ull + 2ull * size + 2ull) % 4096ull);
    bzero(q->desc, len);
    q->freeHead = 0; q->freeCount = size;
    /* chain the free list through the descriptor next fields */
    for (uint16_t i = 0; i < size; i++) {
        struct vring_desc_raw *d = (struct vring_desc_raw *)q->desc + i;
        d->next = i + 1 < size ? i + 1 : 0xffff;
    }
    *(volatile uint16_t *)&fCommon[CC_QUEUE_SELECT] = index;
    /* spec order: select, size, addresses, then enable */
    *(volatile uint64_t *)&fCommon[CC_QUEUE_DESC]  = pa;
    *(volatile uint64_t *)&fCommon[CC_QUEUE_AVAIL] = pa + 16ull * size;
    *(volatile uint64_t *)&fCommon[CC_QUEUE_USED]  = pa + ((uint8_t *)q->used - v);
    q->notifyOff = *(volatile uint16_t *)&fCommon[CC_QUEUE_NOTIFY_OFF];
    *(volatile uint16_t *)&fCommon[CC_QUEUE_ENABLE] = 1;
    LOG("queue %u: enabled, notify off %u (doorbell bar4+%#x)", index, q->notifyOff,
        (unsigned)(fNotifyBarOff + (uint64_t)q->notifyOff * fNotifyMult));
    return true;
}

/* take `count` contiguous free descriptors (split ring; our chains never
   exceed 3, so a contiguous run is always available while freeCount >= count) */
bool VirtIOGPU::allocChain(vq *q, uint16_t *out, int count)
{
    if (q->freeCount < count) return false;
    uint16_t head = q->freeHead;
    uint16_t cur = head;
    for (int i = 1; i < count; i++) cur = ((struct vring_desc_raw *)q->desc)[cur].next;
    q->freeHead = ((struct vring_desc_raw *)q->desc)[cur].next;
    q->freeCount -= count;
    *out = head;
    return true;
}

void VirtIOGPU::freeDescChain(vq *q, uint16_t head)
{
    struct vring_desc_raw *d = (struct vring_desc_raw *)q->desc;
    /* walk to the tail without modifying the chain */
    uint16_t cur = head;
    int n = 1;
    while (n < q->size && d[cur].next != 0xffff) { cur = d[cur].next; n++; }
    /* splice the whole chain in front of the old free list */
    d[cur].next = q->freeHead;
    q->freeHead = head;
    q->freeCount += n;
}

/* publish a chain: fill avail ring entry, bump avail idx, doorbell */
bool VirtIOGPU::submit(vq *q, uint16_t head)
{
    struct vring_avail_raw *a = (struct vring_avail_raw *)q->avail;
    uint16_t slot = q->availIdx % q->size;
    a->ring[slot] = head;
    /* avail.idx must be visible to the device before the doorbell; the common
       cfg doorbell write is ordered vs. our prior stores by UC semantics */
    __asm volatile("sfence" ::: "memory");
    q->availIdx++;
    a->idx = q->availIdx;
    __asm volatile("sfence" ::: "memory");
    /* doorbell: queue's notify offset within the notify region, times multiplier */
    *(volatile uint32_t *)((volatile uint8_t *)fNotify + (uint64_t)q->notifyOff * fNotifyMult) = q->availIdx;
    return true;
}

/* one synchronous controlq round trip through the shared staging buffer.
   req goes in as one descriptor; resp comes back device-writable.
   Strictly one command in flight: wait for the used ring to reach the
   sequence number this submission published. Completion cannot be judged
   by freeCount -- it equals size both before the first completion and
   after the last, so a fresh queue always looks "done" immediately.
   Serialized against the uapi ioctls (which use the same staging). */
bool VirtIOGPU::ctrlCmd(const void *req, uint32_t reqLen, void *resp, uint32_t respLen)
{
    if (fUapiLock) IORecursiveLockLock(fUapiLock);
    bool ok = ctrlCmdData(req, reqLen, nullptr, 0, resp, respLen);
    if (fUapiLock) IORecursiveLockUnlock(fUapiLock);
    return ok;
}

/* ctrlCmd with an optional third descriptor: device-readable data that
   follows the header (3D command buffers, blob cmd payloads, mem entries).
   data is copied into fDataV; the response still uses the fCtrlV tail. */
bool VirtIOGPU::ctrlCmdData(const void *req, uint32_t reqLen,
                            const void *data, uint32_t dataLen,
                            void *resp, uint32_t respLen)
{
    vq *q = &fVq[0];
    if (!fCtrlBuf || reqLen > 2048 || respLen > 6144) return false;
    if (dataLen > kDataBufLen) { LOG("cmd data too big: %u", dataLen); return false; }

    int count = dataLen ? 3 : 2;
    uint16_t head;
    if (!allocChain(q, &head, count)) { LOG("ctrlCmdData: no free descriptors"); return false; }
    struct vring_desc_raw *d = (struct vring_desc_raw *)q->desc;
    uint16_t d0 = head;
    uint16_t d1 = d[d0].next;
    uint16_t d2 = count == 3 ? d[d1].next : 0xffff;
    bzero(fCtrlV, reqLen);   /* clear the request side (hdr padding etc.) */
    bcopy(req, fCtrlV, reqLen);
    d[d0].addr = fCtrlP;
    d[d0].len  = reqLen;
    d[d0].flags = VRING_DESC_F_NEXT;
    d[d0].next = d1;
    if (dataLen) {
        bzero(fDataV, dataLen);
        bcopy(data, fDataV, dataLen);
        d[d1].addr = fDataP;
        d[d1].len  = dataLen;
        d[d1].flags = VRING_DESC_F_NEXT;
        d[d1].next = d2;
        d[d2].addr = fCtrlP + 2048;
        d[d2].len  = respLen;
        d[d2].flags = VRING_DESC_F_WRITE;
        d[d2].next = 0xffff;
    } else {
        d[d1].addr = fCtrlP + 2048;
        d[d1].len  = respLen;
        d[d1].flags = VRING_DESC_F_WRITE;
        d[d1].next = 0xffff;
    }
    q->cookie[head] = fCtrlV + 2048;

    if (!submit(q, head)) return false;
    uint16_t seq = q->availIdx;   /* device's used.idx reaches this when our chain completes */

    bool done = false;
    for (int spin = 0; spin < 5000 && !done; spin++) {
        reap(q);
        done = (q->lastUsed == seq);
        if (!done) IOSleep(1);
    }
    if (!done) { LOG("ctrlCmdData: timeout (type %#x)", ((struct virtio_gpu_ctrl_hdr *)req)->type); return false; }
    bcopy(fCtrlV + 2048, resp, respLen);
    return true;
}

/* allocate the framebuffer backing store and light up scanout 0 with it.
   The 64px checkerboard proved the path end to end; now the buffer is the
   real aperture (black) that VirtIOFB publishes and WindowServer draws in. */
bool VirtIOGPU::bringUpScanout()
{
    const uint32_t w = fScanoutW, h = fScanoutH;
    if (!w || !h) return false;

    /* backing store: one contiguous buffer, B8G8R8A8, 4k aligned */
    IOByteCount fbLen = (IOByteCount)w * h * 4;
    fFbMem = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous, fbLen, 4096);
    if (!fFbMem) { LOG("scanout: fb alloc %u failed", (unsigned)fbLen); return false; }
    if (fFbMem->prepare() != kIOReturnSuccess) { LOG("scanout: fb prepare failed"); fFbMem->release(); fFbMem = nullptr; return false; }
    fFbKva = (uint8_t *)fFbMem->getBytesNoCopy();
    fFbPhys = fFbMem->getPhysicalAddress();
    bzero(fFbKva, fbLen);

    /* the 2D resource already exists (created in start()); attach + scanout */
    const uint32_t resId = fScanoutRes;
    if (!resId) return false;
    struct virtio_gpu_ctrl_hdr resp = {};

    /* attach the backing store (one mem entry) */
    struct {
        struct virtio_gpu_resource_attach_backing ab;
        struct virtio_gpu_mem_entry entry;
    } attach = {};
    attach.ab.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach.ab.resource_id = resId;
    attach.ab.nr_entries = 1;
    attach.entry.addr = fFbPhys;
    attach.entry.length = fbLen;
    if (!ctrlCmd(&attach, sizeof(attach), &resp, sizeof(resp))) return false;
    if (resp.type != VIRTIO_GPU_RESP_OK_NODATA) { LOG("ATTACH_BACKING: resp %#x", resp.type); return false; }

    /* point scanout 0 at it, full frame (resource was created at this size) */
    struct virtio_gpu_set_scanout so = {};
    so.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    so.r.width = w; so.r.height = h;
    so.scanout_id = 0; so.resource_id = resId;
    if (!ctrlCmd(&so, sizeof(so), &resp, sizeof(resp))) return false;
    if (resp.type != VIRTIO_GPU_RESP_OK_NODATA) { LOG("SET_SCANOUT: resp %#x", resp.type); return false; }

    LOG("scanout ready: %ux%u B8G8R8A8 at pa %#llx (resource %u)", w, h,
        (unsigned long long)fFbPhys, resId);
    return true;
}

/* copy the current guest frame into the host surface and repaint the display.
   For 2D resources QEMU renders the scanout from its own pixman image, which
   is only filled by TRANSFER_TO_HOST_2D; RESOURCE_FLUSH then flips it to the
   display. A dirty-rect call keeps the idle cost at two 48-byte commands. */
bool VirtIOGPU::flushFrame(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!fScanoutRes || !fFbKva) return false;
    /* clamp to the resource; 0x0 means the whole frame */
    if (x >= fScanoutW || y >= fScanoutH) return true;   /* nothing visible to do */
    if (w == 0 || h == 0) { x = y = 0; w = fScanoutW; h = fScanoutH; }
    if (x + w > fScanoutW) w = fScanoutW - x;
    if (y + h > fScanoutH) h = fScanoutH - y;

    struct virtio_gpu_ctrl_hdr resp = {};

    /* offset into the backing store where the rect starts (linear B8G8R8A8) */
    struct virtio_gpu_transfer_to_host_2d t2d = {};
    t2d.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    t2d.r.x = x; t2d.r.y = y; t2d.r.width = w; t2d.r.height = h;
    t2d.offset = (uint64_t)y * (fScanoutW * 4) + (uint64_t)x * 4;
    t2d.resource_id = fScanoutRes;
    if (!ctrlCmd(&t2d, sizeof(t2d), &resp, sizeof(resp))) return false;
    if (resp.type != VIRTIO_GPU_RESP_OK_NODATA) { LOG("T2D: resp %#x", resp.type); return false; }

    struct virtio_gpu_resource_flush flush = {};
    flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush.r.x = x; flush.r.y = y; flush.r.width = w; flush.r.height = h;
    flush.resource_id = fScanoutRes;
    if (!ctrlCmd(&flush, sizeof(flush), &resp, sizeof(resp))) return false;
    if (resp.type != VIRTIO_GPU_RESP_OK_NODATA) { LOG("FLUSH: resp %#x", resp.type); return false; }
    return true;
}

/* drain the used ring, returning cookies for completed chains */
void VirtIOGPU::reap(vq *q)
{
    struct vring_used_raw *u = (struct vring_used_raw *)q->used;
    __asm volatile("lfence" ::: "memory");
    while (q->lastUsed != u->idx) {
        struct vring_used_elem_raw *e = &u->ring[q->lastUsed % q->size];
        uint16_t head = e->id;
        q->lastUsed++;
        void *cookie = q->cookie[head];
        q->cookie[head] = nullptr;
        freeDescChain(q, head);
        /* P1: dispatch on the completion cookie's first word (ctrl_hdr.type) */
        (void)cookie;
    }
}

/* end-to-end proof: GET_DISPLAY_INFO over controlq */
bool VirtIOGPU::probeDisplay()
{
    struct virtio_gpu_resp_display_info resp = {};
    struct virtio_gpu_ctrl_hdr req = {};
    req.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    if (!ctrlCmd(&req, sizeof(req), &resp, sizeof(resp))) return false;
    if (resp.hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        LOG("GET_DISPLAY_INFO: resp %#x", resp.hdr.type);
        return false;
    }
    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_SCANOUTS && i < fCfg.num_scanouts; i++)
        LOG("scanout %u: %ux%u at (%u,%u) enabled %u", i,
            resp.pmodes[i].r.width, resp.pmodes[i].r.height,
            resp.pmodes[i].r.x, resp.pmodes[i].r.y, resp.pmodes[i].enabled);
    /* pmodes[] is QEMU's *requested* state (xres/yres props), not a hardware
       limit — this boot it reported 640x480 while the firmware GOP ran
       1280x800. Keep fScanoutW/H at the resource size we created. */
    return true;
}

/* enumerate capsets — Venus presence decides the P2 architecture */
bool VirtIOGPU::probeCapsets()
{
    fNumCapsets = 0; fCapsetIdMask = 0;
    for (uint32_t i = 0; i < fCfg.num_capsets && i < 16; i++) {
        struct virtio_gpu_get_capset_info req = {};
        struct virtio_gpu_resp_capset_info resp = {};
        req.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
        req.capset_index = i;
        if (!ctrlCmd(&req, sizeof(req), &resp, sizeof(resp))) return false;
        if (resp.hdr.type != VIRTIO_GPU_RESP_OK_CAPSET_INFO) {
            LOG("GET_CAPSET_INFO %u: resp %#x", i, resp.hdr.type);
            return false;
        }
        LOG("capset %u: id %u version %u size %u%s", i,
            resp.capset_id, resp.capset_max_version, resp.capset_max_size,
            resp.capset_id == VIRTIO_GPU_CAPSET_VENUS ? "  <-- VENUS" : "");
        fCapsets[fNumCapsets++] = { resp.capset_id, resp.capset_max_version, resp.capset_max_size };
        if (resp.capset_id < 32) fCapsetIdMask |= (1u << resp.capset_id);
    }
    if (fCfg.num_capsets == 0) LOG("no capsets (QEMU needs a Venus-enabled build for GL)");
    return true;
}
void VirtIOGPU::interruptOccurred(IOInterruptEventSource *src, int count) { (void)src; (void)count; }

/* ---- P2: the Linux-virtgpu-uapi-shaped surface ----
   The shim (Mesa vdrm backend) passes the full DRM ioctl command in `cmd`;
   the command number is extracted with the standard DRM encoding
   (nr = (cmd & 0xff) - DRM_COMMAND_BASE). Structures are passed in one
   buffer and written back in place; pointer fields are repurposed as
   inline-data markers (0 = data follows the struct) since the kext cannot
   dereference user pointers. */

bool VirtIOGPU::uapiInit()
{
    if (!fUapiLock) fUapiLock = IORecursiveLockAlloc();
    if (fDataBuf || !fHasBlob) return fUapiLock != nullptr;
    fDataBuf = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous, kDataBufLen, 4096);
    if (fDataBuf && fDataBuf->prepare() == kIOReturnSuccess) {
        fDataV = (uint8_t *)fDataBuf->getBytesNoCopy();
        fDataP = fDataBuf->getPhysicalAddress();
        LOG("uapi: data staging at pa %#llx (len %u)%s%s",
            (unsigned long long)fDataP, (unsigned)kDataBufLen,
            fHasContextInit ? " context_init" : "",
            fHasHostVisible ? " host_visible" : "");
    } else {
        LOG("uapi: data staging alloc failed");
    }
    return fUapiLock != nullptr;
}

void VirtIOGPU::uapiCleanup()
{
    for (uint32_t i = 0; i < kMaxBos; i++) {
        if (fBo[i].guestMem) { fBo[i].guestMem->complete(); fBo[i].guestMem->release(); fBo[i].guestMem = nullptr; }
        fBo[i] = {};
    }
    if (fDataBuf) { fDataBuf->complete(); fDataBuf->release(); fDataBuf = nullptr; }
    fDataV = nullptr; fDataP = 0;
    if (fUapiLock) { IORecursiveLockFree(fUapiLock); fUapiLock = nullptr; }
}

int VirtIOGPU::boLookup(uint32_t handle) const
{
    return (handle >= 1 && handle < kMaxBos && fBo[handle].used) ? (int)handle : -1;
}

int VirtIOGPU::capsetIndex(uint32_t id) const
{
    for (uint32_t i = 0; i < fNumCapsets; i++)
        if (fCapsets[i].id == id) return (int)i;
    return -1;
}

int VirtIOGPU::uapiCtxCreateLocked(uint32_t capsetId, const char *name)
{
    if (!fHasContextInit) return -1;
    /* find an unused host context id (ids recycle when sessions close) */
    uint32_t ctxId = 0;
    for (uint32_t id = 1; id <= kMaxCtx; id++) {
        bool taken = false;
        for (uint32_t i = 0; i < kMaxCtx; i++)
            if (fCtx[i].used && fCtx[i].ctxId == id) { taken = true; break; }
        if (!taken) { ctxId = id; break; }
    }
    if (!ctxId) { LOG("out of contexts"); return -1; }
    struct virtio_gpu_ctx_create req = {};
    struct virtio_gpu_ctrl_hdr resp = {};
    req.hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    req.hdr.ctx_id = ctxId;
    req.nlen = name ? (uint32_t)strnlen(name, sizeof(req.debug_name)) : 0;
    req.context_init = capsetId & VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK;
    if (name) strlcpy(req.debug_name, name, sizeof(req.debug_name));
    if (!ctrlCmdData(&req, sizeof(req), nullptr, 0, &resp, sizeof(resp)) ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        LOG("CTX_CREATE %u: resp %#x", ctxId, resp.type);
        return -1;
    }
    for (uint32_t i = 0; i < kMaxCtx; i++) {
        if (!fCtx[i].used) { fCtx[i] = { true, ctxId, capsetId }; return (int)i; }
    }
    return -1;
}

void VirtIOGPU::uapiCtxDestroyLocked(int idx)
{
    if (idx < 0 || (uint32_t)idx >= kMaxCtx || !fCtx[idx].used) return;
    struct virtio_gpu_ctrl_hdr req = {};
    struct virtio_gpu_ctrl_hdr resp = {};
    req.type = VIRTIO_GPU_CMD_CTX_DESTROY;
    req.ctx_id = fCtx[idx].ctxId;
    if (!ctrlCmdData(&req, sizeof(req), nullptr, 0, &resp, sizeof(resp)) ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA)
        LOG("CTX_DESTROY %u: resp %#x", req.ctx_id, resp.type);
    fCtx[idx] = {};
}

/* MAP_BLOB: place the host3d blob in the shared-memory region at a
   guest-chosen offset (bump allocator) and record the physical address. */
bool VirtIOGPU::uapiMapBlobLocked(uapiBo *bo)
{
    if (!fHasHostVisible || !fShmPa || !bo || bo->mapped) return false;
    if (!(bo->blobFlags & VIRTGPU_BLOB_FLAG_USE_MAPPABLE)) return false;

    uint64_t off = (fShmCursor + 4095) & ~4095ull;
    if (off + bo->size > fShmLen) { LOG("shm region exhausted (%llu used)", (unsigned long long)fShmCursor); return false; }
    fShmCursor = off + bo->size;

    struct virtio_gpu_resource_map_blob req = {};
    struct virtio_gpu_resp_map_info resp = {};
    req.hdr.type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB;
    req.resource_id = bo->resId;
    req.offset = off;
    if (!ctrlCmdData(&req, sizeof(req), nullptr, 0, &resp, sizeof(resp)) ||
        (resp.hdr.type != VIRTIO_GPU_RESP_OK_MAP_INFO && resp.hdr.type != VIRTIO_GPU_RESP_OK_NODATA)) {
        LOG("MAP_BLOB res %u: resp %#x", bo->resId, resp.hdr.type);
        return false;
    }
    bo->mapped = true;
    bo->shmOff = off;
    bo->shmPa = (IOPhysicalAddress)((uintptr_t)fShmPa + off);
    LOG("MAP_BLOB res %u: shm off %#llx pa %#llx cache %u", bo->resId,
        (unsigned long long)off, (unsigned long long)bo->shmPa,
        resp.map_info & VIRTIO_GPU_MAP_CACHE_MASK);
    return true;
}

void VirtIOGPU::uapiUnmapBlobLocked(uapiBo *bo)
{
    if (!bo || !bo->mapped) return;
    struct virtio_gpu_resource_unmap_blob req = {};
    struct virtio_gpu_ctrl_hdr resp = {};
    req.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB;
    req.resource_id = bo->resId;
    ctrlCmdData(&req, sizeof(req), nullptr, 0, &resp, sizeof(resp));
    bo->mapped = false;
}

IOReturn VirtIOGPU::uapiCloseHandleLocked(uint32_t handle)
{
    int idx = boLookup(handle);
    if (idx < 0) return kIOReturnBadArgument;
    uapiBo *bo = &fBo[idx];
    uapiUnmapBlobLocked(bo);
    struct { struct virtio_gpu_ctrl_hdr hdr; uint32_t resource_id; uint32_t padding; } unref = {};
    struct virtio_gpu_ctrl_hdr resp = {};
    unref.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    unref.resource_id = bo->resId;
    ctrlCmdData(&unref, sizeof(unref), nullptr, 0, &resp, sizeof(resp));
    if (bo->guestMem) { bo->guestMem->complete(); bo->guestMem->release(); bo->guestMem = nullptr; }
    *bo = {};   /* shm offsets are leaked (bump allocator); 4G is plenty */
    return kIOReturnSuccess;
}

IOReturn VirtIOGPU::uapiExecbufLocked(struct drm_virtgpu_execbuffer *eb, uint32_t size,
                                      const void *staging, uint32_t stagingLen, int ctxIdx)
{
    if (size < sizeof(*eb) || eb->size == 0) return kIOReturnBadArgument;
    if (eb->flags & ~(VIRTGPU_EXECBUF_FENCE_FD_OUT | VIRTGPU_EXECBUF_RING_IDX)) return kIOReturnUnsupported;
    if (eb->num_in_syncobjs || eb->num_out_syncobjs) return kIOReturnUnsupported;
    if (eb->size > kDataBufLen) return kIOReturnBadArgument;
    const uint8_t *cmdData;
    const uint32_t *handles;
    if (eb->command) {
        /* cmd bytes live in the connection staging buffer at this offset */
        if (!staging || eb->command > stagingLen || eb->size > stagingLen - eb->command)
            return kIOReturnBadArgument;
        cmdData = (const uint8_t *)staging + eb->command;
        uint64_t inl = size - sizeof(*eb);
        if ((uint64_t)eb->num_bo_handles * 4 > inl) return kIOReturnBadArgument;
        handles = (const uint32_t *)((uint8_t *)eb + sizeof(*eb));
    } else {
        uint64_t avail = size - sizeof(*eb);
        if (eb->size > avail || (uint64_t)eb->num_bo_handles * 4 > avail - eb->size)
            return kIOReturnBadArgument;
        const uint8_t *buf = (const uint8_t *)eb + sizeof(*eb);
        cmdData = buf;
        handles = (const uint32_t *)(buf + eb->size);
    }

    if (ctxIdx < 0 || (uint32_t)ctxIdx >= kMaxCtx || !fCtx[ctxIdx].used)
        return kIOReturnNotOpen;
    uint32_t ctxId = fCtx[ctxIdx].ctxId;

    /* attach any handles not yet on the context (kernel does this on submit) */
    for (uint32_t i = 0; i < eb->num_bo_handles; i++) {
        int idx = boLookup(handles[i]);
        if (idx < 0) { LOG("execbuf: bad handle %u", handles[i]); return kIOReturnBadArgument; }
        uapiBo *bo = &fBo[idx];
        if (bo->ctxId != ctxId) return kIOReturnNotPrivileged;
        if (!bo->attached) {
            struct virtio_gpu_ctx_resource att = {};
            struct virtio_gpu_ctrl_hdr resp = {};
            att.hdr.type = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE;
            att.hdr.ctx_id = ctxId;
            att.resource_id = bo->resId;
            if (!ctrlCmdData(&att, sizeof(att), nullptr, 0, &resp, sizeof(resp)) ||
                resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
                LOG("CTX_ATTACH res %u: resp %#x", bo->resId, resp.type);
                return kIOReturnIOError;
            }
            bo->attached = true;
        }
    }

    /* SUBMIT_3D: header struct + the command buffer as the data descriptor */
    uint8_t submitBuf[sizeof(struct virtio_gpu_cmd_submit)];
    struct virtio_gpu_cmd_submit *cs = (struct virtio_gpu_cmd_submit *)submitBuf;
    bzero(cs, sizeof(*cs));
    cs->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    cs->hdr.ctx_id = ctxId;
    cs->size = eb->size;
    struct virtio_gpu_ctrl_hdr resp = {};
    if (!ctrlCmdData(cs, sizeof(*cs), cmdData, eb->size, &resp, sizeof(resp)) ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        LOG("SUBMIT_3D %u bytes: resp %#x", eb->size, resp.type);
        return kIOReturnIOError;
    }

    /* submissions are synchronous; any requested fence is already complete */
    if (eb->flags & VIRTGPU_EXECBUF_FENCE_FD_OUT)
        eb->fence_fd = (int32_t)(fNextFenceCookie++);
    return kIOReturnSuccess;
}

IOReturn VirtIOGPU::uapiBlobCreateLocked(struct drm_virtgpu_resource_create_blob *rc,
                                         uint32_t size, uint32_t *outSize,
                                         const void *staging, uint32_t stagingLen, int ctxIdx)
{
    if (size < sizeof(*rc)) return kIOReturnBadArgument;
    if (!fHasBlob) return kIOReturnUnsupported;
    if (rc->blob_flags & ~VIRTGPU_BLOB_FLAG_USE_MASK) return kIOReturnBadArgument;

    uint32_t blobMem = rc->blob_mem;
    if (blobMem != VIRTGPU_BLOB_MEM_GUEST &&
        blobMem != VIRTGPU_BLOB_MEM_HOST3D &&
        blobMem != VIRTGPU_BLOB_MEM_HOST3D_GUEST) return kIOReturnBadArgument;
    bool guest = (blobMem != VIRTGPU_BLOB_MEM_HOST3D);
    bool host3d = (blobMem != VIRTGPU_BLOB_MEM_GUEST);
    if (host3d && (rc->cmd_size % 4)) return kIOReturnBadArgument;
    if (!host3d && (rc->blob_id || rc->cmd_size)) return kIOReturnBadArgument;
    if (rc->size == 0 || rc->size > (1ull << 30)) return kIOReturnBadArgument;
    const void *cmdData = nullptr;
    if (rc->cmd_size) {
        if (rc->cmd) {
            if (!staging || rc->cmd > stagingLen || rc->cmd_size > stagingLen - rc->cmd)
                return kIOReturnBadArgument;
            cmdData = (const uint8_t *)staging + rc->cmd;
        } else {
            if (rc->cmd_size > size - sizeof(*rc)) return kIOReturnBadArgument;
            cmdData = (const uint8_t *)rc + sizeof(*rc);
        }
    }

    bool haveCtx = ctxIdx >= 0 && (uint32_t)ctxIdx < kMaxCtx && fCtx[ctxIdx].used;
    if (host3d && !haveCtx) return kIOReturnNotOpen;
    uint32_t ctxId = haveCtx ? fCtx[ctxIdx].ctxId : 0;

    int idx = -1;
    for (uint32_t i = 1; i < kMaxBos; i++) if (!fBo[i].used) { idx = (int)i; break; }
    if (idx < 0) return kIOReturnNoResources;
    uint32_t handle = (uint32_t)idx;
    uint32_t resId = fNextResId++;
    uint64_t boSize = (rc->size + 4095) & ~4095ull;

    uapiBo *bo = &fBo[idx];
    *bo = {};
    bo->used = true;
    bo->resId = resId;
    bo->size = boSize;
    bo->blobMem = blobMem;
    bo->blobFlags = rc->blob_flags;
    bo->ctxId = ctxId;

    /* the creation cmd blob goes to the context first (kernel order) */
    if (rc->cmd_size) {
        struct virtio_gpu_cmd_submit cs = {};
        struct virtio_gpu_ctrl_hdr resp = {};
        cs.hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
        cs.hdr.ctx_id = ctxId;
        cs.size = rc->cmd_size;
        if (!ctrlCmdData(&cs, sizeof(cs), cmdData, rc->cmd_size,
                         &resp, sizeof(resp)) || resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
            LOG("blob cmd submit: resp %#x", resp.type);
            *bo = {}; fNextResId--;
            return kIOReturnIOError;
        }
    }

    /* guest-backed blobs need guest memory + ATTACH_BACKING */
    if (guest) {
        bo->guestMem = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
            kIODirectionInOut | kIOMemoryPhysicallyContiguous, boSize, 4096);
        if (!bo->guestMem || bo->guestMem->prepare() != kIOReturnSuccess) {
            LOG("blob guest alloc %llu failed", (unsigned long long)boSize);
            if (bo->guestMem) { bo->guestMem->release(); bo->guestMem = nullptr; }
            *bo = {}; fNextResId--;
            return kIOReturnNoResources;
        }
        bo->guestPa = bo->guestMem->getPhysicalAddress();
        bzero(bo->guestMem->getBytesNoCopy(), boSize);

        /* CREATE_BLOB hdr + one mem entry as the following data descriptor */
        struct virtio_gpu_resource_create_blob cb = {};
        struct virtio_gpu_mem_entry ent = {};
        cb.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;
        cb.hdr.ctx_id = ctxId;
        cb.resource_id = resId;
        cb.blob_mem = blobMem;
        cb.blob_flags = rc->blob_flags;
        cb.blob_id = rc->blob_id;
        cb.size = boSize;
        cb.nr_entries = 1;
        ent.addr = bo->guestPa;
        ent.length = (uint32_t)boSize;
        struct virtio_gpu_ctrl_hdr resp = {};
        if (!ctrlCmdData(&cb, sizeof(cb), &ent, sizeof(ent), &resp, sizeof(resp)) ||
            resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
            LOG("CREATE_BLOB res %u: resp %#x", resId, resp.type);
            bo->guestMem->complete(); bo->guestMem->release();
            *bo = {}; fNextResId--;
            return kIOReturnIOError;
        }
        bo->attached = true;   /* created with ctx_id: auto-attached host-side */
    } else {
        /* HOST3D: CREATE_BLOB with no entries */
        struct virtio_gpu_resource_create_blob cb = {};
        struct virtio_gpu_ctrl_hdr resp = {};
        cb.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;
        cb.hdr.ctx_id = ctxId;
        cb.resource_id = resId;
        cb.blob_mem = blobMem;
        cb.blob_flags = rc->blob_flags;
        cb.blob_id = rc->blob_id;
        cb.size = boSize;
        cb.nr_entries = 0;
        if (!ctrlCmdData(&cb, sizeof(cb), nullptr, 0, &resp, sizeof(resp)) ||
            resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
            LOG("CREATE_BLOB res %u: resp %#x", resId, resp.type);
            *bo = {}; fNextResId--;
            return kIOReturnIOError;
        }
        bo->attached = true;
        /* NB: the kernel defers MAP_BLOB to the MAP ioctl; doing it here
           would consume the resource's one-shot host-side map state */
    }

    rc->bo_handle = handle;
    rc->res_handle = resId;
    *outSize = sizeof(*rc);
    return kIOReturnSuccess;
}

IOReturn VirtIOGPU::uapiGetCapsLocked(struct drm_virtgpu_get_caps *gc,
                                      uint32_t size, uint32_t *outSize,
                                      const void *staging, uint32_t stagingLen)
{
    if (size < sizeof(*gc) || gc->size == 0) return kIOReturnBadArgument;
    int idx = capsetIndex(gc->cap_set_id);
    if (idx < 0 || fCapsets[idx].ver < gc->cap_set_ver) return kIOReturnBadArgument;
    uint32_t hostSize = fCapsets[idx].size;
    uint32_t n = gc->size < hostSize ? gc->size : hostSize;

    struct virtio_gpu_get_capset req = {};
    req.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET;
    req.capset_id = gc->cap_set_id;
    req.capset_version = gc->cap_set_ver;
    /* response: hdr (24) followed by capset data; staged through fCtrlV tail */
    uint8_t respBuf[24 + 4096];
    if (!ctrlCmdData(&req, sizeof(req), nullptr, 0, respBuf, (uint32_t)sizeof(respBuf)))
        return kIOReturnIOError;
    if (((struct virtio_gpu_ctrl_hdr *)respBuf)->type != VIRTIO_GPU_RESP_OK_CAPSET)
        return kIOReturnIOError;
    void *dst;
    if (gc->addr) {
        if (!staging || gc->addr > stagingLen || n > stagingLen - gc->addr)
            return kIOReturnBadArgument;
        dst = (uint8_t *)staging + gc->addr;
    } else {
        if (n > size - sizeof(*gc)) return kIOReturnBadArgument;
        dst = (uint8_t *)gc + sizeof(*gc);
        *outSize = sizeof(*gc) + n;
    }
    bcopy(respBuf + 24, dst, n);
    return kIOReturnSuccess;
}

IOReturn VirtIOGPU::uapiTransferLocked(bool toHost,
                                       struct drm_virtgpu_3d_transfer_to_host *t,
                                       uint32_t size)
{
    if (size < sizeof(*t)) return kIOReturnBadArgument;
    int idx = boLookup(t->bo_handle);
    if (idx < 0) return kIOReturnBadArgument;
    uapiBo *bo = &fBo[idx];

    struct virtio_gpu_transfer_host_3d req = {};
    req.hdr.type = toHost ? VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D
                          : VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    req.hdr.ctx_id = bo->ctxId;
    req.box.x = t->box.x; req.box.y = t->box.y; req.box.z = t->box.z;
    req.box.w = t->box.w; req.box.h = t->box.h; req.box.d = t->box.d;
    req.offset = t->offset;
    req.resource_id = bo->resId;
    req.level = t->level;
    req.stride = t->stride;
    req.layer_stride = t->layer_stride;
    struct virtio_gpu_ctrl_hdr resp = {};
    if (!ctrlCmdData(&req, sizeof(req), nullptr, 0, &resp, sizeof(resp)) ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        LOG("TRANSFER_%s_3D res %u: resp %#x", toHost ? "TO" : "FROM", bo->resId, resp.type);
        return kIOReturnIOError;
    }
    return kIOReturnSuccess;
}

IOMemoryDescriptor *VirtIOGPU::uapiMemoryForType(uint32_t type)
{
    IOMemoryDescriptor *md = nullptr;
    if (fUapiLock) IORecursiveLockLock(fUapiLock);
    int idx = boLookup(type);
    if (idx >= 0) {
        uapiBo *bo = &fBo[idx];
        if (bo->guestMem) {
            md = bo->guestMem;
            md->retain();
        } else if (bo->mapped) {
            /* physical (MMIO) range: task must be NULL — kernel_task makes
               this a virtual-kernel-address descriptor and the user map fails
               with kIOReturnNoDevice (see NVRM's clientMemoryForType) */
            md = IOMemoryDescriptor::withAddressRange(bo->shmPa, bo->size,
                kIODirectionInOut, nullptr);
            if (!md)
                LOG("uapiMemoryForType %u: withAddressRange failed pa %#llx len %llu",
                    type, (unsigned long long)bo->shmPa, (unsigned long long)bo->size);
        }
    }
    if (fUapiLock) IORecursiveLockUnlock(fUapiLock);
    return md;
}

IOReturn VirtIOGPU::uapiIoctl(uint32_t cmd, void *data, uint32_t size, uint32_t *outSize,
                              const void *staging, uint32_t stagingLen, int *ctxIdx)
{
    /* DRM ioctl encoding: nr in bits 8-15, type ('d') in bits 0-7 */
    uint32_t nr = (cmd >> 8) & 0xff;
    if (nr >= DRM_COMMAND_BASE) nr -= DRM_COMMAND_BASE;
    if (!outSize || !ctxIdx) return kIOReturnBadArgument;
    *outSize = 0;

    if (nr == UAPI_GETPARAM) {
        struct drm_virtgpu_getparam *gp = (struct drm_virtgpu_getparam *)data;
        if (size < sizeof(*gp)) return kIOReturnBadArgument;
        int value = 0;
        switch (gp->param) {
        case VIRTGPU_PARAM_3D_FEATURES:
            value = (fHasContextInit && capsetIndex(VIRTIO_GPU_CAPSET_VENUS) >= 0) ? 1 : 0; break;
        case VIRTGPU_PARAM_CAPSET_QUERY_FIX: value = 1; break;
        case VIRTGPU_PARAM_RESOURCE_BLOB: value = fHasBlob ? 1 : 0; break;
        case VIRTGPU_PARAM_HOST_VISIBLE: value = fHasHostVisible ? 1 : 0; break;
        case VIRTGPU_PARAM_CROSS_DEVICE: value = 0; break;
        case VIRTGPU_PARAM_CONTEXT_INIT: value = fHasContextInit ? 1 : 0; break;
        case VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs: value = (int)fCapsetIdMask; break;
        case VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME: value = fHasContextInit ? 1 : 0; break;
        case VIRTGPU_PARAM_BLOB_ALIGNMENT: value = 4096; break;
        default: return kIOReturnUnsupported;
        }
        gp->value = (uint64_t)value;   /* shim copies this out to the caller's int */
        *outSize = sizeof(*gp);
        return kIOReturnSuccess;
    }

    /* everything else talks to the device */
    if (fUapiLock) IORecursiveLockLock(fUapiLock);
    IOReturn r = kIOReturnUnsupported;
    switch (nr) {
    case UAPI_CONTEXT_INIT: {
        struct drm_virtgpu_context_init *ci = (struct drm_virtgpu_context_init *)data;
        if (size < sizeof(*ci) || ci->num_params == 0 || ci->num_params > 4) { r = kIOReturnBadArgument; break; }
        uint64_t avail = size - sizeof(*ci);
        if ((uint64_t)ci->num_params * sizeof(struct drm_virtgpu_context_set_param) > avail) { r = kIOReturnBadArgument; break; }
        struct drm_virtgpu_context_set_param *params =
            (struct drm_virtgpu_context_set_param *)((uint8_t *)ci + sizeof(*ci));
        uint32_t capsetId = 0;
        for (uint32_t i = 0; i < ci->num_params; i++) {
            switch (params[i].param) {
            case VIRTGPU_CONTEXT_PARAM_CAPSET_ID: capsetId = (uint32_t)params[i].value; break;
            case VIRTGPU_CONTEXT_PARAM_NUM_RINGS: break;   /* single-ring emulation */
            default: break;
            }
        }
        if (!capsetId || capsetIndex(capsetId) < 0) { r = kIOReturnBadArgument; break; }
        if (*ctxIdx >= 0) { r = kIOReturnBusy; break; }   /* one context per connection (drm_file) */
        int slot = uapiCtxCreateLocked(capsetId, "vmtl-venus");
        if (slot >= 0) *ctxIdx = slot;
        r = slot >= 0 ? kIOReturnSuccess : kIOReturnIOError;
        break;
    }
    case UAPI_GET_CAPS:
        r = uapiGetCapsLocked((struct drm_virtgpu_get_caps *)data, size, outSize, staging, stagingLen);
        break;
    case UAPI_RESOURCE_CREATE_BLOB:
        r = uapiBlobCreateLocked((struct drm_virtgpu_resource_create_blob *)data, size, outSize, staging, stagingLen, *ctxIdx);
        break;
    case UAPI_EXECBUFFER:
        r = uapiExecbufLocked((struct drm_virtgpu_execbuffer *)data, size, staging, stagingLen, *ctxIdx);
        break;
    case UAPI_MAP: {
        struct drm_virtgpu_map *m = (struct drm_virtgpu_map *)data;
        if (size < sizeof(*m)) { r = kIOReturnBadArgument; break; }
        int idx = boLookup(m->handle);
        if (idx < 0) { r = kIOReturnBadArgument; break; }
        uapiBo *bo = &fBo[idx];
        if (!bo->guestMem && !bo->mapped) uapiMapBlobLocked(bo);
        if (!bo->guestMem && !bo->mapped) { r = kIOReturnUnsupported; break; }
        m->offset = m->handle;   /* cookie: type for IOConnectMapMemory64 */
        *outSize = sizeof(*m);
        r = kIOReturnSuccess;
        break;
    }
    case UAPI_RESOURCE_INFO: {
        struct drm_virtgpu_resource_info *ri = (struct drm_virtgpu_resource_info *)data;
        if (size < sizeof(*ri)) { r = kIOReturnBadArgument; break; }
        int idx = boLookup(ri->bo_handle);
        if (idx < 0) { r = kIOReturnBadArgument; break; }
        ri->res_handle = fBo[idx].resId;
        ri->size = (uint32_t)fBo[idx].size;
        ri->blob_mem = fBo[idx].blobMem;
        *outSize = sizeof(*ri);
        r = kIOReturnSuccess;
        break;
    }
    case UAPI_TRANSFER_TO_HOST:
        r = uapiTransferLocked(true, (struct drm_virtgpu_3d_transfer_to_host *)data, size);
        break;
    case UAPI_TRANSFER_FROM_HOST:
        r = uapiTransferLocked(false, (struct drm_virtgpu_3d_transfer_to_host *)data, size);
        break;
    case UAPI_WAIT: {
        /* submissions complete synchronously; bos are always idle */
        r = size >= sizeof(struct drm_virtgpu_3d_wait) ? kIOReturnSuccess : kIOReturnBadArgument;
        break;
    }
    case UAPI_GEM_CLOSE: {
        struct GemCloseArgs { uint32_t handle; uint32_t pad; };
        GemCloseArgs *gc = (GemCloseArgs *)data;
        if (size < sizeof(*gc)) { r = kIOReturnBadArgument; break; }
        /* unknown handle: Linux GEM close is a no-op for foreign handles */
        r = boLookup(gc->handle) < 0 ? kIOReturnSuccess : uapiCloseHandleLocked(gc->handle);
        break;
    }
    case UAPI_RESOURCE_CREATE:
        r = kIOReturnUnsupported;   /* virgl path; venus is blob-only */
        break;
    default:
        break;
    }
    if (fUapiLock) IORecursiveLockUnlock(fUapiLock);
    return r;
}

IOReturn VirtIOGPU::uapiWaitEvent(uint32_t fenceId, uint32_t timeoutUs, uint32_t *signaled)
{
    (void)fenceId; (void)timeoutUs;
    if (signaled) *signaled = 1;   /* synchronous submissions: always signaled */
    return kIOReturnSuccess;
}

void VirtIOGPU::uapiClientClosed(int *ctxIdx)
{
    if (!ctxIdx || *ctxIdx < 0) return;
    if (fUapiLock) IORecursiveLockLock(fUapiLock);
    uint32_t ctxId = (uint32_t)*ctxIdx < kMaxCtx ? fCtx[*ctxIdx].ctxId : 0;
    /* release the session's bos (drm_file close drops its GEM handles) */
    for (uint32_t i = 1; i < kMaxBos; i++) {
        if (fBo[i].used && fBo[i].ctxId == ctxId)
            uapiCloseHandleLocked(i);
    }
    uapiCtxDestroyLocked(*ctxIdx);
    *ctxIdx = -1;
    if (fUapiLock) IORecursiveLockUnlock(fUapiLock);
}

/* ---- user client: the Linux-uapi-shaped surface ---- */
enum { SEL_IOCTL = 0, SEL_WAITEVENT = 1 };

class VirtIOGPUUserClient : public IOUserClient {
    OSDeclareDefaultStructors(VirtIOGPUUserClient)
public:
    bool initWithTask(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties) override;
    bool start(IOService *provider) override;
    IOReturn clientClose(void) override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                            IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) override;
    /* type 0: this connection's staging buffer (large payloads; IOConnectCallStructMethod
       itself caps structures at 4096). Other types: bo handles, via the device. */
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override;
    IOMemoryDescriptor *stagingMemory() { if (fStaging) fStaging->retain(); return fStaging; }
private:
    VirtIOGPU *fOwner = nullptr;
    task_t fTask = nullptr;
    /* this connection's context slot in the owner's fCtx[] (-1 = none);
       set by CONTEXT_INIT, cleared by uapiClientClosed */
    int fCtxIdx = -1;
    static const uint32_t kStagingLen = 512 * 1024;
    IOBufferMemoryDescriptor *fStaging = nullptr;
    static IOReturn sIoctl(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sWaitEvent(OSObject *target, void *reference, IOExternalMethodArguments *args);
};

#undef super
#define super IOUserClient
OSDefineMetaClassAndStructors(VirtIOGPUUserClient, IOUserClient)

bool VirtIOGPUUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties)
{
    if (!super::initWithTask(owningTask, securityID, type, properties)) return false;
    fTask = owningTask;
    return true;
}

bool VirtIOGPUUserClient::start(IOService *provider)
{
    fOwner = OSDynamicCast(VirtIOGPU, provider);
    if (!fOwner) return false;
    fStaging = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous, kStagingLen, 4096);
    if (fStaging) fStaging->prepare();
    return super::start(provider);
}

IOReturn VirtIOGPUUserClient::clientClose(void)
{
    if (fStaging) { fStaging->complete(); fStaging->release(); fStaging = nullptr; }
    /* runs on graceful close and on process death (SIGSEGV included):
       without this a crashed client leaves its host context poisoned */
    if (fOwner) fOwner->uapiClientClosed(&fCtxIdx);
    fOwner = nullptr;
    terminate();
    return kIOReturnSuccess;
}

IOReturn VirtIOGPUUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                             IOExternalMethodDispatch *dispatch, OSObject *target, void *reference)
{
    static const IOExternalMethodDispatch tbl[] = {
        { sIoctl,    1, kIOUCVariableStructureSize, 0, kIOUCVariableStructureSize },
        { sWaitEvent, 2, 0, 1, 0 },
    };
    if (selector >= sizeof tbl / sizeof tbl[0]) return kIOReturnBadArgument;
    IOExternalMethodDispatch d = tbl[selector];
    return super::externalMethod(selector, args, &d, this, reference);
}

IOReturn VirtIOGPUUserClient::sIoctl(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
    (void)reference;
    VirtIOGPUUserClient *uc = OSDynamicCast(VirtIOGPUUserClient, target);
    if (!uc || !uc->fOwner || args->structureInputSize > 4096) return kIOReturnBadArgument;
    uint32_t cmd = (uint32_t)args->scalarInput[0];
    uint32_t outLen = 0;
    /* in/out share one buffer: virtgpu uapi structs are written back in place.
       Large payloads go through the connection staging buffer (type 0). */
    IOReturn r = uc->fOwner->uapiIoctl(cmd, (void *)(uintptr_t)args->structureInput,
                                       (uint32_t)args->structureInputSize, &outLen,
                                       uc->fStaging ? uc->fStaging->getBytesNoCopy() : nullptr,
                                       uc->fStaging ? kStagingLen : 0, &uc->fCtxIdx);
    if (r == kIOReturnSuccess) {
        if (outLen) {
            /* uapiIoctl mutates the input copy (getparam->value, map->offset,
               blob->bo_handle, ...). structureInput and structureOutput are
               distinct kernel buffers, so ship the mutation to the caller. */
            if (args->structureOutput && args->structureOutputSize >= outLen)
                memcpy(args->structureOutput, (const void *)(uintptr_t)args->structureInput, outLen);
        }
        /* always publish outLen (0 = staging mode, no copy-back): leaving the
           dispatch's default count would clobber the caller's buffer with
           zeroed output bytes */
        args->structureOutputSize = outLen;
    }
    return r;
}

IOReturn VirtIOGPUUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
    (void)options;
    if (!memory) return kIOReturnBadArgument;
    if (type == 0) {
        if (!fStaging) return kIOReturnNotReady;
        *memory = fStaging;
        fStaging->retain();
        return kIOReturnSuccess;
    }
    if (!fOwner) return kIOReturnBadArgument;
    IOMemoryDescriptor *md = fOwner->uapiMemoryForType((uint32_t)type);
    if (!md) return kIOReturnBadArgument;
    *memory = md;
    return kIOReturnSuccess;
}

IOReturn VirtIOGPUUserClient::sWaitEvent(OSObject *target, void *reference, IOExternalMethodArguments *args)
{
    (void)reference;
    VirtIOGPUUserClient *uc = OSDynamicCast(VirtIOGPUUserClient, target);
    if (!uc || !uc->fOwner) return kIOReturnBadArgument;
    uint32_t signaled = 0;
    IOReturn r = uc->fOwner->uapiWaitEvent((uint32_t)args->scalarInput[0], (uint32_t)args->scalarInput[1], &signaled);
    if (r == kIOReturnSuccess) args->scalarOutput[0] = signaled;
    return r;
}

/* ======================================================================
 * VirtIOFB — the IOFramebuffer that makes this the console display.
 *
 * Attaches on the VirtIOGPU service (same bundle, nub personality with
 * IOMatchCategory IOFramebuffer so IONDRVFramebuffer cannot also take it).
 * The aperture is the VirtIOGPU scanout backing store; WindowServer writes
 * pixels there directly, and a 60 Hz timer pushes changed frames to the
 * host (T2D + FLUSH — see flushFrame). Mode list is the single 1280x800
 * mode the host surface has; display-mode negotiation (EDID, resizing)
 * comes later.
 *
 * The power/vblank/connection plumbing follows NMIntelFB (same controller
 * family answers): without the vblank timer WindowServer never completes
 * its first frame and every display stalls, and without the power
 * acknowledges the first WindowServer restart freezes the machine.
 * ====================================================================== */

class VirtIOFB : public IOFramebuffer {
    OSDeclareDefaultStructors(VirtIOFB)
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

    IOReturn enableController() override { return kIOReturnSuccess; }
    bool isConsoleDevice() override { return true; }
    IODeviceMemory *getApertureRange(IOPixelAperture aperture) override;
    const char *getPixelFormats() override { return IO32BitDirectPixels "\0"; }
    IOItemCount getDisplayModeCount() override { return 1; }
    IOReturn getDisplayModes(IODisplayModeID *modes) override { modes[0] = 1; return kIOReturnSuccess; }
    IOReturn getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info) override;
    UInt64 getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) override { return 0; }
    IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *pi) override;
    IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) override { *mode = 1; *depth = 0; return kIOReturnSuccess; }
    IOReturn getStartupDisplayMode(IODisplayModeID *mode, IOIndex *depth) override { return getCurrentDisplayMode(mode, depth); }
    /* WindowServer shows the display at 0.00 Hz without a timing answer (NMIntelFB, same symptom) */
    IOReturn getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info) override;
    IOReturn setDisplayMode(IODisplayModeID mode, IOIndex depth) override { return (mode == 1 && depth == 0) ? kIOReturnSuccess : kIOReturnUnsupported; }
    IOItemCount getConnectionCount() override { return 1; }
    IOReturn registerForInterruptType(IOSelect type, IOFBInterruptProc proc, OSObject *target, void *ref, void **iref) override {
        if (type == kIOFBVBLInterruptType) { fVbl = { proc, target, ref }; if (iref) *iref = &fVbl; return kIOReturnSuccess; }
        if (type == kIOFBConnectInterruptType) { fConnect = { proc, target, ref }; if (iref) *iref = &fConnect; return kIOReturnSuccess; }
        return kIOReturnUnsupported;
    }
    IOReturn unregisterInterrupt(void *) override { return kIOReturnSuccess; }
    IOReturn setInterruptState(void *, UInt32) override { return kIOReturnSuccess; }
    IOReturn setGammaTable(UInt32, UInt32, UInt32, void *) override { return kIOReturnSuccess; }
    IOReturn setGammaTable(UInt32, UInt32, UInt32, void *, bool) override { return kIOReturnSuccess; }
    IOReturn setCLUTWithEntries(IOColorEntry *, UInt32, UInt32, IOOptionBits) override { return kIOReturnSuccess; }
    IOReturn setApertureEnable(IOPixelAperture, IOOptionBits) override { return kIOReturnSuccess; }
    IOReturn setStartupDisplayMode(IODisplayModeID, IOIndex) override { return kIOReturnSuccess; }
    /* Power acknowledges copied from NMIntelFB/NVRMFB: left to the base class a
       power change queues on the shared controller thread and waits for an
       acknowledge (45 s timeout), and the first WindowServer restart then
       freezes every display. */
    IOReturn setPowerState(unsigned long ordinal, IOService *) override {
        handleEvent(ordinal ? kIOFBNotifyDidPowerOn : kIOFBNotifyWillPowerOff);
        return kIOPMAckImplied;
    }
    IOReturn setAttribute(IOSelect attribute, uintptr_t value) override {
        if (attribute == kIOWindowServerActiveAttribute) return kIOReturnSuccess;
        if (attribute == kIOPowerAttribute) {
            if (value) { IOReturn r = IOFramebuffer::setAttribute(attribute, value); handleEvent(kIOFBNotifyDidPowerOn);
                         return r == kIOReturnUnsupported ? kIOReturnSuccess : r; }
            handleEvent(kIOFBNotifyWillPowerOff);
            IOReturn r = IOFramebuffer::setAttribute(attribute, value);
            return r == kIOReturnUnsupported ? kIOReturnSuccess : r;
        }
        return IOFramebuffer::setAttribute(attribute, value);
    }
    IOReturn setAttributeForConnection(IOIndex idx, IOSelect attribute, uintptr_t value) override {
        switch (attribute) {
        case kConnectionPower: case kConnectionColorModesSupported: case kConnectionColorDepthsSupported:
        case kConnectionControllerColorDepth: case kConnectionControllerDepthsSupported: case kConnectionColorMode:
        case kConnectionDisplayFlags: case kConnectionFlags:
            return kIOReturnSuccess;
        default: return IOFramebuffer::setAttributeForConnection(idx, attribute, value);
        }
    }
    IOReturn connectFlags(IOIndex, IODisplayModeID, IOOptionBits *flags) override {
        if (flags) *flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
        return kIOReturnSuccess;
    }
    IODeviceMemory *getVRAMRange() override { return getApertureRange(kIOFBSystemAperture); }
    IOReturn getAttribute(IOSelect attribute, uintptr_t *value) override {
        if (attribute == kIOWindowServerActiveAttribute) { if (value) *value = 1; return kIOReturnSuccess; }
        return IOFramebuffer::getAttribute(attribute, value);
    }
    IOReturn getAttributeForConnection(IOIndex idx, IOSelect attr, uintptr_t *value) override {
        if (idx != 0 || !value) return kIOReturnBadArgument;
        switch (attr) {
        case kConnectionEnable: case kConnectionCheckEnable: *value = 1; return kIOReturnSuccess;
        case kConnectionFlags: *value = kIOConnectionBuiltIn; return kIOReturnSuccess;
        /* the colour answers WindowServer asks before compositing (NMIntelFB) */
        case kConnectionColorModesSupported: case kConnectionColorMode: *value = 0x00000001; return kIOReturnSuccess;
        case kConnectionColorDepthsSupported: case kConnectionControllerColorDepth: case kConnectionControllerDepthsSupported:
            *value = 0x00000002; return kIOReturnSuccess;
        default: return IOFramebuffer::getAttributeForConnection(idx, attr, value);
        }
    }

private:
    VirtIOGPU *fDev = nullptr;
    uint32_t fWidth = 0, fHeight = 0, fRowBytes = 0;
    IODeviceMemory *fAperture = nullptr;
    /* vblank: WindowServer paces display updates on it; without one the first
       frame never completed and WindowServer stalled (NMIntelFB studio 10-10).
       A 60 Hz timer is the vblank, as NVRMFB's fallback is. */
    struct Irq { IOFBInterruptProc proc = nullptr; OSObject *target = nullptr; void *ref = nullptr; };
    Irq fVbl, fConnect;
    thread_call_t fVblTimer = nullptr;
    uint64_t fVblNext = 0;
    volatile bool fStopping = false;
    uint64_t fFrameNs = 16666667ull;   /* 60 Hz period */
    static void vblFire(thread_call_param_t p0, thread_call_param_t) {
        VirtIOFB *me = (VirtIOFB *)p0;
        if (me->fStopping) return;
        /* push the framebuffer to the host every vsync: QEMU's 2D path draws
           the scanout from its own pixman image, fed only by
           TRANSFER_TO_HOST_2D (full frame — partial rects re-tile rows), then
           RESOURCE_FLUSH flips it to the display. WindowServer writes pixels
           into the aperture between vblanks; this is the only thing that
           makes its output visible. */
        me->fDev->flushFrame(0, 0, me->fWidth, me->fHeight);
        if (me->fVbl.proc) me->fVbl.proc(me->fVbl.target, me->fVbl.ref);
        uint64_t period; nanoseconds_to_absolutetime(me->fFrameNs, &period);
        uint64_t now = mach_absolute_time();
        me->fVblNext = (me->fVblNext && me->fVblNext + period > now) ? me->fVblNext + period : now + period;
        thread_call_enter_delayed(me->fVblTimer, me->fVblNext);
    }
    void armVbl() {
        fVblTimer = thread_call_allocate(vblFire, this);
        if (fVblTimer) {
            uint64_t d; nanoseconds_to_absolutetime(fFrameNs, &d);
            fVblNext = mach_absolute_time() + d;
            thread_call_enter_delayed(fVblTimer, fVblNext);
        }
        LOG("fb: vbl timer %s", fVblTimer ? "armed (60 Hz)" : "ALLOCATE FAILED");
    }
    /* the two power states: off, and on + usable */
    static IOPMPowerState *powerStates();
};

/* the two power states NVRMFB registers: off, and on + usable */
static IOPMPowerState sPowerStates[2] = {
    { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { kIOPMPowerStateVersion1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
};
IOPMPowerState *VirtIOFB::powerStates() { return sPowerStates; }

#define super IOFramebuffer
OSDefineMetaClassAndStructors(VirtIOFB, IOFramebuffer)

bool VirtIOFB::start(IOService *provider)
{
    fDev = OSDynamicCast(VirtIOGPU, provider);
    if (!fDev || !fDev->scanoutKva()) { LOG("fb: no VirtIOGPU scanout behind me"); return false; }
    fWidth = fDev->scanoutWidth(); fHeight = fDev->scanoutHeight();
    fRowBytes = fDev->scanoutRowBytes();

    /* IOFBDependentID MUST be set before super::start(): IOFramebuffer::start
       -> copyController(kDepIDCreate) creates the shared IOFBController only
       when the property is already present (IOFramebuffer.cpp kDepIDCreate
       branch returns NULL without it). With the controller NULL, the first PM
       notification dies in FBLOCK (controller->fWl) -- exactly the panic
       IOFramebuffer::powerStateWillChangeTo +0x25 at +5.5 s in v36. NVRMFB
       sets it before super::start for the same reason (nvrm-fb.cpp). */
    setProperty("IOFBDependentID", 0x56544C47ull /* 'VTLG' */, 64);
    setProperty("IOFBDependentIndex", 0ull, 32);
    setProperty("built-in", kOSBooleanTrue);

    if (!IOFramebuffer::start(provider)) { LOG("fb: IOFramebuffer::start failed"); return false; }

    /* the aperture is the device's backing store, already DMA-mapped */
    fAperture = IODeviceMemory::withRange(fDev->scanoutPhys(), (IOPhysicalLength)fRowBytes * fHeight);
    if (!fAperture) { LOG("fb: aperture IODeviceMemory failed"); return false; }
    if (IOWorkLoop *wl = getWorkLoop()) { (void)wl; }
    armVbl();
    registerPowerDriver(this, powerStates(), 2);
    LOG("fb: published %ux%u B8G8R8A8 rowbytes %u", fWidth, fHeight, fRowBytes);
    return true;
}

void VirtIOFB::stop(IOService *provider)
{
    fStopping = true;
    /* thread_call_cancel_wait is not exported to kexts: cancel and retry the
       free until the callback is out (as NVRMFB/NMIntelFB) */
    if (fVblTimer) { thread_call_cancel(fVblTimer); while (!thread_call_free(fVblTimer)) { thread_call_cancel(fVblTimer); IOSleep(5); } fVblTimer = nullptr; }
    if (fAperture) { fAperture->release(); fAperture = nullptr; }
    IOFramebuffer::stop(provider);
}

IODeviceMemory *VirtIOFB::getApertureRange(IOPixelAperture aperture)
{
    if (aperture != kIOFBSystemAperture) return nullptr;
    if (fAperture) { fAperture->retain(); return fAperture; }
    return IODeviceMemory::withRange(fDev->scanoutPhys(), (IOPhysicalLength)fRowBytes * fHeight);
}

IOReturn VirtIOFB::getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info)
{
    if (mode != 1 || !info) return kIOReturnBadArgument;
    bzero(info, sizeof(*info));
    info->maxDepthIndex = 0; info->nominalWidth = fWidth; info->nominalHeight = fHeight;
    info->refreshRate = 60 << 16;
    info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
    return kIOReturnSuccess;
}

IOReturn VirtIOFB::getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *pi)
{
    if (mode != 1 || depth != 0 || aperture != kIOFBSystemAperture || !pi) return kIOReturnUnsupportedMode;
    bzero(pi, sizeof(*pi));
    pi->bytesPerRow = fRowBytes; pi->bytesPerPlane = 0; pi->bitsPerPixel = 32; pi->pixelType = kIORGBDirectPixels;
    pi->componentCount = 3; pi->bitsPerComponent = 8;
    pi->componentMasks[0] = 0x000000FF; pi->componentMasks[1] = 0x0000FF00; pi->componentMasks[2] = 0x00FF0000;
    strlcpy(pi->pixelFormat, IO32BitDirectPixels, sizeof(pi->pixelFormat));
    pi->activeWidth = fWidth; pi->activeHeight = fHeight;
    return kIOReturnSuccess;
}

IOReturn VirtIOFB::getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info)
{
    if (mode != 1 || !info) return kIOReturnUnsupportedMode;
    bzero(info, sizeof(*info));
    info->appleTimingID = kIOTimingIDInvalid;
    /* CEA-style 1280x800@60: pixel clock 83.5 MHz, totals 1440x823 */
    IODetailedTimingInformationV2 &d = info->detailedInfo.v2;
    d.pixelClock = d.minPixelClock = d.maxPixelClock = 83500000ull;
    d.horizontalActive = fWidth; d.horizontalBlanking = 160;
    d.horizontalSyncOffset = 48; d.horizontalSyncPulseWidth = 32;
    d.verticalActive = fHeight; d.verticalBlanking = 23;
    d.verticalSyncOffset = 3; d.verticalSyncPulseWidth = 6;
    info->flags = kIODetailedTimingValid;
    return kIOReturnSuccess;
}
