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

#define VIRTIO_PCI_CAP_VNDR_OFF      0
#define VIRTIO_PCI_CAP_NEXT_OFF      1
#define VIRTIO_PCI_CAP_LEN_OFF       2
#define VIRTIO_PCI_CAP_TYPE_OFF      3
#define VIRTIO_PCI_CAP_BAR_OFF       4
#define VIRTIO_PCI_CAP_OFFSET_OFF    8   /* 24-bit little-endian (bytes 5-7 are padding) */
#define VIRTIO_PCI_CAP_LENGTH_OFF    12  /* 32-bit little-endian */
#define VIRTIO_PCI_CAP_NOTIFY_OFF_MULTIPLIER 16 /* 32-bit, notify cap only */

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
    /* cursor */
    VIRTIO_GPU_CMD_UPDATE_CURSOR    = 0x0300,
    VIRTIO_GPU_CMD_MOVE_CURSOR      = 0x0301,
    /* responses */
    VIRTIO_GPU_RESP_OK_NODATA       = 0x1100,
    VIRTIO_GPU_RESP_OK_DISPLAY_INFO = 0x1101,
    VIRTIO_GPU_RESP_OK_CAPSET_INFO  = 0x1102,
    VIRTIO_GPU_RESP_OK_CAPSET       = 0x1103,
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

    /* uapi surface (P1): the ioctl-shaped commands */
    IOReturn uapiIoctl(uint32_t cmd, void *data, uint32_t size, uint32_t *outSize);
    IOReturn uapiWaitEvent(uint32_t fenceId, uint32_t timeoutUs, uint32_t *signaled);

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
        uint8_t bar     = fPCI->configRead8(capPtr + VIRTIO_PCI_CAP_BAR_OFF);
        uint32_t offset = fPCI->configRead32(capPtr + VIRTIO_PCI_CAP_OFFSET_OFF) & 0xffffffu;
        uint32_t len    = fPCI->configRead32(capPtr + VIRTIO_PCI_CAP_LENGTH_OFF) & 0xffffffu;

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
   after the last, so a fresh queue always looks "done" immediately. */
bool VirtIOGPU::ctrlCmd(const void *req, uint32_t reqLen, void *resp, uint32_t respLen)
{
    vq *q = &fVq[0];
    if (!fCtrlBuf || reqLen > 2048 || respLen > 6144) return false;

    uint16_t head;
    if (!allocChain(q, &head, 2)) { LOG("ctrlCmd: no free descriptors"); return false; }
    struct vring_desc_raw *d = (struct vring_desc_raw *)q->desc;
    uint16_t d0 = head, d1 = d[d0].next;
    bzero(fCtrlV, reqLen);   /* clear the request side (hdr padding etc.) */
    bcopy(req, fCtrlV, reqLen);
    d[d0].addr = fCtrlP;
    d[d0].len  = reqLen;
    d[d0].flags = VRING_DESC_F_NEXT;
    d[d0].next = d1;
    d[d1].addr = fCtrlP + 2048;
    d[d1].len  = respLen;
    d[d1].flags = VRING_DESC_F_WRITE;
    d[d1].next = 0xffff;
    q->cookie[head] = fCtrlV + 2048;

    if (!submit(q, head)) return false;
    uint16_t seq = q->availIdx;   /* device's used.idx reaches this when our chain completes */

    bool done = false;
    for (int spin = 0; spin < 1000 && !done; spin++) {
        reap(q);
        done = (q->lastUsed == seq);
        if (!done) IOSleep(1);
    }
    if (!done) { LOG("ctrlCmd: timeout (type %#x)", ((struct virtio_gpu_ctrl_hdr *)req)->type); return false; }
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
    for (uint32_t i = 0; i < fCfg.num_capsets; i++) {
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
    }
    if (fCfg.num_capsets == 0) LOG("no capsets (QEMU needs a Venus-enabled build for GL)");
    return true;
}
void VirtIOGPU::interruptOccurred(IOInterruptEventSource *src, int count) { (void)src; (void)count; }

IOReturn VirtIOGPU::uapiIoctl(uint32_t cmd, void *data, uint32_t size, uint32_t *outSize)
{
    /* P1: switch on the DRM_IOCTL_VIRTGPU_* command numbers, dispatch into
       controlq submissions. The shim in Venus uses the same numbering. */
    (void)cmd; (void)data; (void)size; (void)outSize;
    return kIOReturnUnsupported;
}

IOReturn VirtIOGPU::uapiWaitEvent(uint32_t fenceId, uint32_t timeoutUs, uint32_t *signaled)
{
    (void)fenceId; (void)timeoutUs; (void)signaled;
    return kIOReturnUnsupported;
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
private:
    VirtIOGPU *fOwner = nullptr;
    task_t fTask = nullptr;
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
    return super::start(provider);
}

IOReturn VirtIOGPUUserClient::clientClose(void)
{
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
    /* in/out share one buffer: virtgpu uapi structs are written back in place */
    IOReturn r = uc->fOwner->uapiIoctl(cmd, (void *)(uintptr_t)args->structureInput, (uint32_t)args->structureInputSize, &outLen);
    if (r == kIOReturnSuccess && outLen) args->structureOutputSize = outLen;
    return r;
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
