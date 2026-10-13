/*
 * VirtIOAccel — the IOGraphicsAccelerator2 personality that publishes this
 * kext to Metal (MetalPluginName/MetalPluginClassName in the Info.plist).
 *
 * Same shape as the NVAccel reference (kexts/NVRM/accel/nvrm-accel.cpp):
 * implement IOGraphicsAccelerator2's 17 pure virtuals with family-subclass
 * stubs, fill the IOAccelConfig the family expects, register the service so
 * the Metal framework finds the plugin — and nothing more. Actual command
 * submission lives in userspace: the plugin dlopens the Venus ICD and renders
 * through Vulkan (P3); this nub only has to make a Metal device EXIST.
 *
 * Ships inside VirtIOGPU.kext (io.vmtl.VirtIOGPU) — a separate bundle id
 * would need its own UAKM approval, which macOS 13.7 only grants
 * interactively. The personality has its own IOMatchCategory (IOAccelerator,
 * like NVAccel's) so it attaches alongside VirtIOGPU on the same nub, and a
 * boot-arg gate (vioaccel=1) so a bad bring-up can never boot-loop the VM.
 *
 * ABI: the reconstructed headers (NVRM/accel/{re,iofam}) were verified LIVE
 * on this build by the VIOAccelProbe class (IOAccelerator 136 B,
 * IOGraphicsAccelerator2 0xdd8) — see dmesg "vioaccel-probe: MATCH".
 * The vio_fwd.h naked thunks still carry Tahoe-measured vtable slot
 * offsets; if a Ventura vtable slot differs the machine panics ONLY when
 * vioaccel=1 is set, and OpenCore's boot-args are edited host-side.
 */
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IORangeAllocator.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/OSAtomic.h>
#include <sys/sysctl.h>

#include "IOAccelerator.h"
#include "IOGraphicsAccelerator2.h"

#include "IOAccelEventMachineFast2.h"
#include "IOAccelLegacyDisplayMachine.h"
#include "IOAccelTask.h"
#include "IOAccelMemoryMap.h"
#include "IOAccelResource2.h"
#include "IOAccelSysMemory.h"
#include "IOAccelVidMemory.h"
#include "IOAccelLegacySurface.h"
#include "IOAccelLegacyDisplayPipe.h"
#include "IOAccelGLContext2.h"
#include "IOAccel2DContext2.h"
#include "IOAccelCLContext2.h"
#include "IOAccelVideoContext2.h"

#include "vio_fwd.h"

#define ALOG(fmt, ...) do { kprintf("vioaccel: " fmt "\n", ##__VA_ARGS__); IOLog("vioaccel: " fmt "\n", ##__VA_ARGS__); } while (0)

class VirtIOAccel;
static VirtIOAccel *gVioAccelInst = nullptr;
static void vioAccelWriteStamp(int s, unsigned int v);   /* defined after VirtIOAccel */
static int gVioHeadless = 1;   /* scanout belongs to VirtIOFB (2D), never to an accel display pipe:
                                  the plugin reads this to keep WindowServer off our Vulkan device */
SYSCTL_INT(_debug, OID_AUTO, nvaccel_headless, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &gVioHeadless, 0,
           "1 = accel device for apps only: the display is VirtIOFB's, WindowServer must not composite on this GPU");

// kern/thread_call.h and kern/clock.h clash with the family headers here, so the KPIs are declared by hand
extern "C" {
struct thread_call *thread_call_allocate(void (*func)(void *, void *), void *param0);
int thread_call_enter_delayed(struct thread_call *call, uint64_t deadline);
void clock_interval_to_deadline(uint32_t interval, uint32_t scale_factor, uint64_t *result);
}

class VIOAccelEventMachine : public IOAccelEventMachineFast2 {
    OSDeclareDefaultStructors(VIOAccelEventMachine)
    VIO_FWD_FWD(VIOAccelEventMachine)
public:
    /* stamp memory is a plain array of 4096 4-byte stamps (getStampMemory below) */
    void writeStamp(int s, vendevtCommandRec *r, unsigned int v) APPLE_KEXT_OVERRIDE {
        (void)r;
        vioAccelWriteStamp(s, v);
    }
    void prepareBarrier(vendevtBarrierRec *b) APPLE_KEXT_OVERRIDE { ALOG("EM prepareBarrier(%p)", b); }
    void completeBarrier(vendevtBarrierRec *b) APPLE_KEXT_OVERRIDE { ALOG("EM completeBarrier(%p)", b); }
    void writeBarrierElement(vendevtBarrierRec *b, int i, unsigned int v) APPLE_KEXT_OVERRIDE { ALOG("EM writeBarrierElement(%p, %d, %u)", b, i, v); }
    bool checkGPUProgress() APPLE_KEXT_OVERRIDE { return true; }
    bool checkChannelProgress(int ch) APPLE_KEXT_OVERRIDE { return true; }
    void *eventTimeout(int stamp) APPLE_KEXT_OVERRIDE { ALOG("EM eventTimeout(%d) — no family channels in use", stamp); return nullptr; }
};
OSDefineMetaClassAndStructors(VIOAccelEventMachine, IOAccelEventMachineFast2)

class VIOAccelDisplayMachine : public IOAccelLegacyDisplayMachine {
    OSDeclareDefaultStructors(VIOAccelDisplayMachine)
    VIO_FWD_FWD(VIOAccelDisplayMachine)
public:
    bool displayModeWillChange() APPLE_KEXT_OVERRIDE { ALOG("DM displayModeWillChange"); return true; }
    bool displayModeDidChange() APPLE_KEXT_OVERRIDE { ALOG("DM displayModeDidChange -> true"); return true; }
};
OSDefineMetaClassAndStructors(VIOAccelDisplayMachine, IOAccelLegacyDisplayMachine)

class VIOAccelTask : public IOAccelTask {
    OSDeclareDefaultStructors(VIOAccelTask)
    VIO_FWD_FWD(VIOAccelTask)
public:
    /* one 1 TiB GPU-VA arena per task, first 1 MiB reserved (NULL page) */
    IORangeAllocator *fVa = nullptr;
    bool init(IOGraphicsAccelerator2 *accel, unsigned int kernelTask, IORangeAllocator **ranges) {
        if (!IOAccelTask::init(accel, kernelTask, ranges)) return false;
        fVa = IORangeAllocator::withRange(1ULL << 40, 4096);
        if (fVa) fVa->allocateRange(0, 0x100000);
        return true;
    }
    void free() APPLE_KEXT_OVERRIDE { if (fVa) { fVa->release(); fVa = nullptr; } IOAccelTask::free(); }
    unsigned long long allocate(const IOAccelMemoryMap *map) APPLE_KEXT_OVERRIDE {
        const unsigned long long len = map ? ((IOAccelMemoryMap *)map)->getLength() : 0;
        if (!fVa || !len) return 0;
        unsigned long long addr = 0;
        if (!fVa->allocate(len, &addr, 0x1000)) {
            if (fVaLogged < 8) { fVaLogged++;
                ALOG("VIOAccelTask::allocate: VA space exhausted (%llu bytes)", len); }
            return 0;
        }
        return addr;
    }
    void deallocate(const IOAccelMemoryMap *map, unsigned long long addr) APPLE_KEXT_OVERRIDE {
        if (fVa && addr && map) fVa->deallocate(addr, ((IOAccelMemoryMap *)map)->getLength());
    }
    unsigned fVaLogged = 0;
};
OSDefineMetaClassAndStructors(VIOAccelTask, IOAccelTask)

class VIOAccelMemoryMap : public IOAccelMemoryMap {
    OSDeclareDefaultStructors(VIOAccelMemoryMap)
    VIO_FWD_FWD(VIOAccelMemoryMap)
public:
    bool fMapped = false;
    /* P3 minimal: the plugin renders through Vulkan blobs, not family page
       tables -- a commit just means "the VA reservation stands". P4 wires
       real host3d blob mapping here. */
    bool commitIntoGPUPageTable() APPLE_KEXT_OVERRIDE {
        fMapped = true;
        ALOG("MM %p commitIntoGPUPageTable -> true (VA reservation only)", this);
        return true;
    }
    void releaseFromGPUPageTable() APPLE_KEXT_OVERRIDE { fMapped = false; }
    bool updateGPUPageTable() APPLE_KEXT_OVERRIDE { return fMapped; }
};
OSDefineMetaClassAndStructors(VIOAccelMemoryMap, IOAccelMemoryMap)

class VIOAccelResource : public IOAccelResource2 {
    OSDeclareDefaultStructors(VIOAccelResource)
    VIO_FWD_FWD(VIOAccelResource)
public:
    void rebuildPagingBuffer() APPLE_KEXT_OVERRIDE { ALOG("Res %p rebuildPagingBuffer -- no page tables (P4)", this); }
    uint64_t getLevelOffset(unsigned char a0, unsigned char a1, int *a2) APPLE_KEXT_OVERRIDE {
        const unsigned char *r = (const unsigned char *)this;
        if (a2) {
            unsigned long long pitch = *(const volatile unsigned long long *)(r + 0xb8);
            unsigned base = r[0xae];
            *a2 = (int)(pitch >> ((a1 >= base) ? (unsigned)(a1 - base) : 0u));
        }
        return (unsigned long long)a0 * *(const volatile unsigned long long *)(r + 0xd0);
    }
    uint64_t getBackingLevelOffset(unsigned char a0, unsigned char a1, int *a2) APPLE_KEXT_OVERRIDE {
        const unsigned char *r = (const unsigned char *)this;
        if (a2) {
            unsigned long long pitch = *(const volatile unsigned long long *)(r + 0xc0);
            unsigned base = r[0xae];
            *a2 = (int)(pitch >> ((a1 >= base) ? (unsigned)(a1 - base) : 0u));
        }
        return (unsigned long long)a0 * *(const volatile unsigned long long *)(r + 0xd0);
    }
    uint64_t calculateIOSurfaceDeviceCacheVRAMBytes(unsigned long long *a0, unsigned long long *a1) APPLE_KEXT_OVERRIDE {
        const unsigned long long size = *(const volatile unsigned long long *)(r8() + 0xd0);
        if (a1) *a1 = 1;
        if (a0) *a0 = size;
        return size;
    }
    bool addToAperture() APPLE_KEXT_OVERRIDE { ALOG("Res %p addToAperture -> false (no BAR aperture)", this); return false; }
    void removeFromAperture() APPLE_KEXT_OVERRIDE { }
    void *getApertureMemoryDescriptor(unsigned long long *a0) APPLE_KEXT_OVERRIDE { if (a0) *a0 = 0; return nullptr; }
    void free() APPLE_KEXT_OVERRIDE { IOAccelResource2::free(); }
    unsigned char *r8() { return (unsigned char *)this; }
};
OSDefineMetaClassAndStructors(VIOAccelResource, IOAccelResource2)

class VIOAccelSysMemory : public IOAccelSysMemory {
    OSDeclareDefaultStructors(VIOAccelSysMemory)
    VIO_FWD_FWD(VIOAccelSysMemory)
public:
};
OSDefineMetaClassAndStructors(VIOAccelSysMemory, IOAccelSysMemory)

class VIOAccelVidMemory : public IOAccelVidMemory {
    OSDeclareDefaultStructors(VIOAccelVidMemory)
    VIO_FWD_FWD(VIOAccelVidMemory)
public:
    /* No dedicated VRAM behind this accelerator: physical backing arrives in
       P4 via host3d blobs. A false return is the family's "no vidmem" signal
       and callers fall back to sys memory. */
    bool allocPhysical() APPLE_KEXT_OVERRIDE {
        const unsigned long long want = *(const volatile unsigned long long *)((const unsigned char *)this + 0x40);
        ALOG("VidMem %p allocPhysical(%llu bytes) -> false (no VRAM until P4)", this, want);
        return false;
    }
    void deallocPhysical() APPLE_KEXT_OVERRIDE { }
    uint64_t getPhysicalSegment(unsigned long long a0, unsigned long long *a1) APPLE_KEXT_OVERRIDE {
        if (a1) *a1 = 0;
        return 0;
    }
};
OSDefineMetaClassAndStructors(VIOAccelVidMemory, IOAccelVidMemory)

class VIOAccelSurface : public IOAccelLegacySurface {
    OSDeclareDefaultStructors(VIOAccelSurface)
    VIO_FWD_FWD(VIOAccelSurface)
public:
    void setSyncCommand(unsigned int *a0, unsigned int &a1, unsigned int &a2, _AMDSurfaceSwapSyncOptions const &a3) const APPLE_KEXT_OVERRIDE { (void)a0; (void)a3; a1 = 0; a2 = 0; }
    void _pure335() APPLE_KEXT_OVERRIDE { ALOG("Surface %p _pure335", this); }
    bool copyFromBuffer(int a0, int a1, int a2, int a3, unsigned int a4, unsigned int a5, IOAccelEvent *a6, IOAccelResource2 *a7, IOAccelSysMemory *a8, unsigned int a9, unsigned long long a10, unsigned int a11) APPLE_KEXT_OVERRIDE {
        (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
        ALOG("Surface %p copyFromBuffer -> false", this); return false; }
    bool copyToBuffer(int a0, int a1, int a2, int a3, unsigned int a4, unsigned int a5, IOAccelEvent *a6, IOAccelResource2 *a7, IOAccelSysMemory *a8, unsigned int a9, unsigned long long a10, unsigned int a11) APPLE_KEXT_OVERRIDE {
        (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
        ALOG("Surface %p copyToBuffer -> false", this); return false; }
    void submitSwapCopy(IOAccelEvent *a0, IOAccelResource2 *a1, IOAccelResource2 *a2) APPLE_KEXT_OVERRIDE {
        (void)a0; (void)a1; (void)a2;
        ALOG("Surface %p submitSwapCopy — no accel scanout, ignored", this); }
    void shapeSurface(unsigned int a0, unsigned short a1, unsigned short a2) APPLE_KEXT_OVERRIDE {
        ALOG("Surface %p shapeSurface(mode 0x%x, %u x %u) — logged only", this, a0, a1, a2); }
    uint32_t getDirtyBufferBitsFromPrivateModeBits(unsigned long long a0) APPLE_KEXT_OVERRIDE { (void)a0; return 0; }
};
OSDefineMetaClassAndStructors(VIOAccelSurface, IOAccelLegacySurface)

class VIOAccelDisplayPipe : public IOAccelLegacyDisplayPipe {
    OSDeclareDefaultStructors(VIOAccelDisplayPipe)
    VIO_FWD_FWD(VIOAccelDisplayPipe)
public:
    IOAccelMemory *initFramebufferResource(unsigned int index, IOAccelResource2 *res) APPLE_KEXT_OVERRIDE {
        (void)index; (void)res;
        ALOG("Pipe %p initFramebufferResource(%u) -> NULL (no accel scanout)", this, index);
        return nullptr;
    }
    IOReturn validateTransaction(IOAccelDisplayPipeTransaction2 *txn) APPLE_KEXT_OVERRIDE { (void)txn; return kIOReturnUnsupported; }
    IOReturn performTransaction(IOAccelDisplayPipeTransaction2 *txn) APPLE_KEXT_OVERRIDE { (void)txn; return kIOReturnUnsupported; }
    bool isTransactionComplete(IOAccelDisplayPipeTransaction2 *txn) APPLE_KEXT_OVERRIDE { (void)txn; return true; }
};
OSDefineMetaClassAndStructors(VIOAccelDisplayPipe, IOAccelLegacyDisplayPipe)

class VIOAccelGLContext : public IOAccelGLContext2 {
    OSDeclareDefaultStructors(VIOAccelGLContext)
    VIO_FWD_FWD(VIOAccelGLContext)
public:
    void _pure362() APPLE_KEXT_OVERRIDE { ALOG("GLCtx %p _pure362", this); }
    void _pure363() APPLE_KEXT_OVERRIDE { ALOG("GLCtx %p _pure363", this); }
    void _pure364() APPLE_KEXT_OVERRIDE { ALOG("GLCtx %p _pure364", this); }
    void _pure367() APPLE_KEXT_OVERRIDE { ALOG("GLCtx %p _pure367", this); }
};
OSDefineMetaClassAndStructors(VIOAccelGLContext, IOAccelGLContext2)

class VIOAccel2DContext : public IOAccel2DContext2 {
    OSDeclareDefaultStructors(VIOAccel2DContext)
    VIO_FWD_FWD(VIOAccel2DContext)
public:
    void _pure362() APPLE_KEXT_OVERRIDE { ALOG("2DCtx %p _pure362", this); }
    void _pure363() APPLE_KEXT_OVERRIDE { ALOG("2DCtx %p _pure363", this); }
};
OSDefineMetaClassAndStructors(VIOAccel2DContext, IOAccel2DContext2)

class VIOAccelCLContext : public IOAccelCLContext2 {
    OSDeclareDefaultStructors(VIOAccelCLContext)
    VIO_FWD_FWD(VIOAccelCLContext)
public:
};
OSDefineMetaClassAndStructors(VIOAccelCLContext, IOAccelCLContext2)

class VIOAccelVideoContext : public IOAccelVideoContext2 {
    OSDeclareDefaultStructors(VIOAccelVideoContext)
    VIO_FWD_FWD(VIOAccelVideoContext)
public:
};
OSDefineMetaClassAndStructors(VIOAccelVideoContext, IOAccelVideoContext2)

class VirtIOAccel : public IOGraphicsAccelerator2 {
    OSDeclareDefaultStructors(VirtIOAccel)
    VIO_FWD_FWD(VirtIOAccel)
public:
    IOBufferMemoryDescriptor *fStamps = nullptr;
    IORangeAllocator *fRanges[32] = {};

    void storeStamp(int s, unsigned int v) {
        if (!fStamps || s < 0 || s >= 4096) return;
        ((volatile unsigned int *)fStamps->getBytesNoCopy())[s] = v;
    }

    uint64_t getStampMemory(unsigned int *p) APPLE_KEXT_OVERRIDE {
        if (!fStamps) {
            fStamps = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, 16384, 4096);
            if (fStamps) { fStamps->prepare(); bzero(fStamps->getBytesNoCopy(), 16384); }
        }
        if (p) *p = fStamps ? 4096 : 0;
        ALOG("getStampMemory(%p) -> %p (4096 stamps, 16 KB)", p, fStamps);
        return (uint64_t)fStamps;
    }
    OSObject *newEventMachine() APPLE_KEXT_OVERRIDE {
        VIOAccelEventMachine *m = OSTypeAlloc(VIOAccelEventMachine);
        ALOG("newEventMachine -> %p (family inits it)", m);
        return m;
    }
    OSObject *newGPUTask(const char *who) {
        for (unsigned i = 0; i < 32; i++) if (!fRanges[i]) {
            fRanges[i] = IORangeAllocator::withRange(1ULL << 40, 4096);
            if (fRanges[i]) fRanges[i]->allocateRange(0, 0x100000);
        }
        VIOAccelTask *t = OSTypeAlloc(VIOAccelTask);
        bool ok = t && t->init(this, 1, fRanges);
        ALOG("%s -> %p init=%d", who, t, ok);
        if (t && !ok) { t->release(); t = nullptr; }
        return t;
    }
    OSObject *createUserGPUTask() APPLE_KEXT_OVERRIDE { return newGPUTask("createUserGPUTask"); }
    OSObject *createKernelGPUTask() APPLE_KEXT_OVERRIDE { return newGPUTask("createKernelGPUTask"); }

    void populateAccelConfig(IOAccelConfig *c) APPLE_KEXT_OVERRIDE {
        uint8_t *p = reinterpret_cast<uint8_t *>(c);
        if (!p) { ALOG("populateAccelConfig(NULL) — nothing to do"); return; }
        if (((uintptr_t)p & 0xFFF) + 0x70 > 0x1000) {
            ALOG("populateAccelConfig(%p) — 0x70 bytes would cross out of this page; REFUSING", p);
            return;
        }
        *(uint64_t *)(p + 0x00) = (uint64_t)(uintptr_t)"VirtIOAccelerator";
        *(uint32_t *)(p + 0x08) = 0x30844300u;      /* family capability flags (as NVAccel) */
        *(uint32_t *)(p + 0x0c) = 15u;              /* AccelCaps: GL|CL|2D|video bits */
        *(uint32_t *)(p + 0x18) = 8u;
        *(uint32_t *)(p + 0x28) = 1u;               /* maxFB: 1 — no accel scanout, but >0 keeps the family sane */
        *(uint32_t *)(p + 0x30) = 0x40004000u;      /* max texture 16384 x 16384 */
        *(uint32_t *)(p + 0x34) = 0x00008000u;
        *(uint32_t *)(p + 0x38) = 0x00008000u;
        *(uint8_t  *)(p + 0x44) = 0;
        *(uint8_t  *)(p + 0x46) = 1;
        *(uint8_t  *)(p + 0x47) = 1;
        *(uint64_t *)(p + 0x48) = 0x0000001F0000000FULL;
        *(uint64_t *)(p + 0x50) = 0x7FFFFFFF0000001FULL;
        *(uint64_t *)(p + 0x58) = 0x0000000C00000000ULL;
        *(uint32_t *)(p + 0x64) = 0x4Bu;
        ALOG("populateAccelConfig(%p) — filled: caps=15, flags=0x30844300, maxtex=16384x16384", p);
    }

    bool configureDevice(IOPCIDevice *pci) APPLE_KEXT_OVERRIDE {
        ALOG("configureDevice(%p) %04x:%04x — bus mastered by VirtIOGPU; nothing to do",
             pci, pci ? pci->configRead16(0) : 0, pci ? pci->configRead16(2) : 0);
        return true;
    }
    void teardownDevice(IOPCIDevice *pci) APPLE_KEXT_OVERRIDE { ALOG("teardownDevice(%p)", pci); }

    OSObject *newDisplayMachine() APPLE_KEXT_OVERRIDE {
        VIOAccelDisplayMachine *d = OSTypeAlloc(VIOAccelDisplayMachine);
        ALOG("newDisplayMachine -> %p", d);
        return d;
    }
    OSObject *newDisplayPipe() APPLE_KEXT_OVERRIDE {
        VIOAccelDisplayPipe *p = OSTypeAlloc(VIOAccelDisplayPipe);
        if (!p) { ALOG("newDisplayPipe: OSTypeAlloc FAILED — the family stores this NULL unchecked"); return NULL; }
        ALOG("newDisplayPipe -> %p", p);
        return p;
    }
    OSObject *newGLContext() APPLE_KEXT_OVERRIDE { VIOAccelGLContext *c = OSTypeAlloc(VIOAccelGLContext); ALOG("newGLContext -> %p", c); return c; }
    OSObject *newCLContext() APPLE_KEXT_OVERRIDE { VIOAccelCLContext *c = OSTypeAlloc(VIOAccelCLContext); ALOG("newCLContext -> %p", c); return c; }
    OSObject *newSurface() APPLE_KEXT_OVERRIDE { VIOAccelSurface *o = OSTypeAlloc(VIOAccelSurface); ALOG("newSurface -> %p", o); return o; }
    OSObject *new2DContext() APPLE_KEXT_OVERRIDE { VIOAccel2DContext *c = OSTypeAlloc(VIOAccel2DContext); ALOG("new2DContext -> %p", c); return c; }
    OSObject *newVideoContext() APPLE_KEXT_OVERRIDE { VIOAccelVideoContext *c = OSTypeAlloc(VIOAccelVideoContext); ALOG("newVideoContext -> %p", c); return c; }
    OSObject *newSysMemory() APPLE_KEXT_OVERRIDE { VIOAccelSysMemory *m = OSTypeAlloc(VIOAccelSysMemory); ALOG("newSysMemory -> %p", m); return m; }
    OSObject *newVidMemory() APPLE_KEXT_OVERRIDE { VIOAccelVidMemory *m = OSTypeAlloc(VIOAccelVidMemory); ALOG("newVidMemory -> %p", m); return m; }
    OSObject *newResource() APPLE_KEXT_OVERRIDE { VIOAccelResource *r = OSTypeAlloc(VIOAccelResource); ALOG("newResource -> %p", r); return r; }
    OSObject *newMemoryMap() APPLE_KEXT_OVERRIDE { VIOAccelMemoryMap *m = OSTypeAlloc(VIOAccelMemoryMap); ALOG("newMemoryMap -> %p", m); return m; }

    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE {
        ALOG("probe(%s) score %d", provider ? provider->getName() : "?", score ? (int)*score : 0);
        if (IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider)) {
            const uint16_t deviceId = pci->configRead16(kIOPCIConfigDeviceID);
            if (deviceId != 0x1050) { ALOG("device 0x%04x is not virtio-gpu — not claiming", deviceId); return NULL; }
            const uint8_t revision = pci->configRead8(kIOPCIConfigRevisionID);
            if (revision < 1) { ALOG("transitional device (rev 0) not supported — not claiming"); return NULL; }
            ALOG("virtio-gpu 1af4:1050 rev %u — claiming the accelerator slot", revision);
        }
        return IOGraphicsAccelerator2::probe(provider, score);
    }

    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE {
        uint32_t gate = 0;
        if (!PE_parse_boot_argn("vioaccel", &gate, sizeof gate) || !gate) {
            kprintf("vioaccel: boot-arg vioaccel=1 absent: NOT starting (boot-loop guard)\n");
            return false;
        }
        ALOG("start(%s): calling IOGraphicsAccelerator2::start — the family's own bring-up", provider ? provider->getName() : "?");
        bool r = IOGraphicsAccelerator2::start(provider);
        ALOG("IOGraphicsAccelerator2::start -> %d", r);
        if (r) {
            registerService();
            ALOG("registerService() — Metal can now find MetalPluginName=%s", "VMTLDriver");
            gVioAccelInst = this;
            sysctl_register_oid(&sysctl__debug_nvaccel_headless);
        }
        return r;
    }
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE {
        ALOG("stop");
        sysctl_unregister_oid(&sysctl__debug_nvaccel_headless);
        if (gVioAccelInst == this) gVioAccelInst = nullptr;
        IOGraphicsAccelerator2::stop(provider);
    }
};
OSDefineMetaClassAndStructors(VirtIOAccel, IOGraphicsAccelerator2)

static void vioAccelWriteStamp(int s, unsigned int v) {
    if (!gVioAccelInst) return;
    gVioAccelInst->storeStamp(s, v);
}

/* naked thunks for the inherited slots Apple stopped exporting (vio_fwd.h) */
VIO_FWD_FWD_DEFS
