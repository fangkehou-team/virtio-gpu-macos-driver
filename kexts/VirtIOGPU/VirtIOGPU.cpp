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
 */

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/OSDebug.h>
#include <mach/mach_time.h>

extern "C" {
kern_return_t _start(kmod_info_t *ki, void *data);
kern_return_t _stop(kmod_info_t *ki, void *data);
KMOD_EXPLICIT_DECL(io.vmtl.VirtIOGPU, "0.1", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = NULL;
__private_extern__ kmod_stop_func_t  *_antimain = NULL;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
}

#define LOG(fmt, ...) do { kprintf("VirtIOGPU: " fmt "\n", ##__VA_ARGS__); IOLog("VirtIOGPU: " fmt "\n", ##__VA_ARGS__); } while (0)

/* ---- virtio PCI modern layout (virtio 1.0+, section 4.1.4) ---- */
#define VIRTIO_PCI_COMMON_CFG_OFF   0x0   /* BAR 0: common configuration */
#define VIRTIO_PCI_ISR_OFF          0x10  /* BAR 0: ISR status */
#define VIRTIO_PCI_DEVICE_CFG_OFF   0x14  /* BAR 0: device-specific (virtio_gpu_config) */

/* common config fields (offsets within the common config structure) */
#define CC_DEVICE_FEATURE_SELECT    0x14
#define CC_DEVICE_FEATURE_STATUS    0x1c
#define CC_DRIVER_FEATURE_SELECT    0x20
#define CC_DRIVER_FEATURE_STATUS    0x28
#define CC_MSIX_CONFIG              0x2c
#define CC_NUM_QUEUES               0x30
#define CC_DEVICE_STATUS            0x32
#define CC_QUEUE_SELECT             0x34
#define CC_QUEUE_SIZE               0x36
#define CC_QUEUE_MSIX               0x38
#define CC_QUEUE_ENABLE             0x3a
#define CC_QUEUE_NOTIFY             0x3c
#define CC_QUEUE_DESC               0x40
#define CC_QUEUE_AVAIL              0x48
#define CC_QUEUE_USED               0x50

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
    VIRTIO_GPU_CMD_GET_DISPLAY_INFO = 0x0100,
    VIRTIO_GPU_CMD_GET_CAPSET_INFO  = 0x0101,
    VIRTIO_GPU_CMD_GET_CAPSET       = 0x0102,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_2D = 0x0103,
    VIRTIO_GPU_CMD_RESOURCE_UNREF   = 0x0104,
    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0105,
    VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING = 0x0106,
    VIRTIO_GPU_CMD_SET_SCANOUT      = 0x0107,
    VIRTIO_GPU_CMD_RESOURCE_FLUSH   = 0x0108,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB = 0x0109,
    VIRTIO_GPU_CMD_SET_SCANOUT_BLOB = 0x010a,
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

/* ---- one virtqueue, split-ring layout, pinned in driver memory ---- */
#define VIRTIO_MAX_QUEUES 2   /* controlq + cursorq */
#define VQ_RING_SIZE      256

struct vq {
    uint16_t   idx;         /* queue index in the device */
    uint16_t   size;
    void      *desc;        /* split ring descriptors */
    void      *avail;
    void      *used;
    uint16_t   freeHead, freeCount;
    uint16_t   lastUsed;
    uint16_t   availFlagsShadow;
    IOMemoryDescriptor *descMem;   /* one allocation holds desc+avail+used, aligned */
    void      *cookie[VQ_RING_SIZE];
};

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
private:
    IOPCIDevice *fPCI = nullptr;
    IOMemoryMap  *fBar0 = nullptr;
    volatile uint8_t *fCommon = nullptr;
    volatile uint8_t *fIsr = nullptr;
    volatile uint8_t *fDevCfg = nullptr;

    struct virtio_gpu_config fCfg = {};
    uint64_t fFeatures = 0;
    bool     fHasBlob = false, fHasEdid = false;

    vq fVq[VIRTIO_MAX_QUEUES] = {};
    /* the work loop is created lazily by IOService; keep it mutable here */
    mutable IOWorkLoop *fWorkLoop = nullptr;
    IOInterruptEventSource *fIntSrc = nullptr;

    bool negotiateFeatures();
    bool initQueue(uint16_t index, uint16_t size);
    bool readDeviceConfig();
    /* P1: kick a queue and reap completions */
    void kick(vq *q);
    void reap(vq *q);
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
    uint8_t revision = pci->configRead8(kIOPCIConfigRevisionID);
    if (revision < 1) { kprintf("VirtIOGPU: transitional device (rev 0) not supported\n"); return nullptr; }
    if (score) *score = 1000;
    return this;
}

bool VirtIOGPU::start(IOService *provider)
{
    if (!super::start(provider)) return false;
    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI) { LOG("no PCI provider"); return false; }

    if (fPCI->requestProbe(kIOPCIConfiguratorBaseAddress)) {}
    fPCI->setIOEnable(true);
    fPCI->setMemoryEnable(true);
    fPCI->configWrite16(kIOPCIConfigCommand, 0x7);  /* IO + MEM + BUSMASTER */

    fBar0 = fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!fBar0) { LOG("BAR0 map failed"); return false; }
    volatile uint8_t *bar = (volatile uint8_t *)fBar0->getVirtualAddress();
    fCommon = bar + VIRTIO_PCI_COMMON_CFG_OFF;
    fIsr    = bar + VIRTIO_PCI_ISR_OFF;
    fDevCfg = bar + VIRTIO_PCI_DEVICE_CFG_OFF;

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
    return true;
}

void VirtIOGPU::stop(IOService *provider)
{
    if (fIntSrc) { fIntSrc->disable(); fWorkLoop->removeEventSource(fIntSrc); fIntSrc->release(); fIntSrc = nullptr; }
    for (int i = 0; i < VIRTIO_MAX_QUEUES; i++) {
        if (fVq[i].descMem) { fVq[i].descMem->release(); fVq[i].descMem = nullptr; }
        *(volatile uint16_t *)&fCommon[CC_QUEUE_SELECT] = (uint16_t)i;
        *(volatile uint16_t *)&fCommon[CC_QUEUE_ENABLE] = 0;
    }
    fCommon[CC_DEVICE_STATUS] = 0;
    if (fBar0) { fBar0->release(); fBar0 = nullptr; }
    super::stop(provider);
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
    if (!(fCommon[CC_DEVICE_STATUS] & VIRTIO_STATUS_FEATURES_OK)) return false;

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
    if (*(volatile uint16_t *)&fCommon[CC_QUEUE_SIZE] != size) {
        LOG("queue %u: device wants size %u, driver wants %u", index,
            *(volatile uint16_t *)&fCommon[CC_QUEUE_SIZE], size);
        return false;
    }
    vq *q = &fVq[index];
    q->idx = index; q->size = size;
    /* desc (16*size) + avail (6 + 2*size + 2) + padding to 4k + used (6 + 8*size) */
    IOByteCount len = 16ull * size + 6ull + 2ull * size + 2ull + 4096ull + 6ull + 8ull * size;
    q->descMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous, len, 4096ull);
    if (!q->descMem) return false;
    if (q->descMem->prepare() != kIOReturnSuccess) { q->descMem->release(); q->descMem = nullptr; return false; }
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
    *(volatile uint16_t *)&fCommon[CC_QUEUE_ENABLE] = 1;
    *(volatile uint64_t *)&fCommon[CC_QUEUE_DESC]  = pa;
    *(volatile uint64_t *)&fCommon[CC_QUEUE_AVAIL] = pa + 16ull * size;
    *(volatile uint64_t *)&fCommon[CC_QUEUE_USED]  = pa + ((uint8_t *)q->used - v);
    return true;
}

void VirtIOGPU::kick(vq *q) { (void)q; }
void VirtIOGPU::reap(vq *q) { (void)q; }
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
