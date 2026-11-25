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

#include <IOKit/graphics/IOFramebuffer.h>   /* complete type for found_framebuffer() logging */

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
static bool vioAccelFindVramDescriptor();
static void vioAccelWriteStamp(int s, unsigned int v);   /* defined after VirtIOAccel */
static int gVioHeadless = 1;   /* scanout belongs to VirtIOFB (2D), never to an accel display pipe:
                                  the plugin reads this to keep WindowServer off our Vulkan device */
SYSCTL_INT(_debug, OID_AUTO, nvaccel_headless, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &gVioHeadless, 0,
           "1 = accel device for apps only: the display is VirtIOFB's, WindowServer must not composite on this GPU");

/* --- EXPERIMENT: prove the "no accelerated scanout -> CompositorSW" premise ---
   Baseline (measured): WindowServer never even asks for a framebuffer resource
   (initFramebufferResource is called 0 times), so the gate sits upstream of it
   -- in IOAccelLegacyDisplayPipe::isCurrentModeAcceleratorBacked() /
   initScanoutResource(), which this port never answers. Both are real Ventura
   13.7.8 symbols (verified in SystemKernelExtensions.kc), so overriding them is
   a plain virtual override, not a vtable guess.

   Default 0 = behave exactly as before. `sysctl -w debug.vioscanout=1` turns
   the scanout-backed pipe on; WindowServer picks it up on its next start (the
   same recipe NVAccel documents for debug.nvaccelfb). This is throwaway probe
   code, not the P4/P5 design. */
static int gVioScanout = 0;
/* Set only after the display pipe has copied a complete frame and the host has
   accepted both the transfer and the flush. The vblank timer reads this atomic
   flag without dereferencing the display pipe, whose lifetime it does not own. */
static int gVioIopOwnsScanout = 0;
static void vioAccelSetScanoutOwned(bool owned)
{
    __atomic_store_n(&gVioIopOwnsScanout, owned ? 1 : 0, __ATOMIC_RELEASE);
}
extern "C" int vioAccelScanoutOwned(void)
{
    return __atomic_load_n(&gVioIopOwnsScanout, __ATOMIC_ACQUIRE);
}
SYSCTL_INT(_debug, OID_AUTO, vioiop_owns, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN,
           &gVioIopOwnsScanout, 0, "1 = the last complete scanout frame was presented by IOP");

static void vioAccelPublishDisplayPipeCaps(int on);   /* defined after VirtIOAccel */

/* --- EXPERIMENT (debug.vioaccltypes): CDDisplay's accelerator lookup ---
   RE (2026-10-18, SkyLight disassembly of WS::Updater::UpdateDisplays /
   CDDisplay::compositor): WindowServer picks CompositorSW over the accelerated
   compositor exactly when CDDisplay::accelerator() yields 0. That value comes
   from CoreDisplay's display-device -> IOAccelerator association.

   Measured on this guest, the ONLY node carrying that association is the other
   framebuffer:  iondrvframebuffer
       IOAccelTypes    = "IOService:/AppleACPIPlatformExpert/PCI0/AppleACPIPCI/
                          S38@7/VirtIOAccel"   (our own accelerator!)
       IOAccelIndex    = 0
       IOAccelRevision = 2
   VirtIOFB -- the node the only real display (display0/AppleDisplay) hangs off
   -- carries none of the three, so its display resolves to no accelerator and
   WindowServer keeps CompositorSW for the desktop. Default 0 = unchanged;
   `sysctl -w debug.vioaccltypes=1` publishes the trio on VirtIOFB, then
   WindowServer picks it up on its next start. Reboot (or writing 0) undoes it. */
static int gVioAccelRef = 0;
static void vioAccelPublishAccelReference(int on);   /* defined after VirtIOAccel */
static int vioaccltypes_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gVioAccelRef;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || req->newptr == 0) return err;
    vioAccelPublishAccelReference(v ? 1 : 0);
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, vioaccltypes, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN,
            NULL, 0, vioaccltypes_sysctl, "I",
            "1 = point VirtIOFB at our accelerator: IOAccelTypes/IOAccelIndex/IOAccelRevision");

extern "C" IOService *vioFramebufferService(void);   /* VirtIOGPU.cpp */

/* THE GATE (measured 2025-xx, baseline boot): WindowServer never asks this port
   for anything display related -- isCurrentModeAcceleratorBacked,
   initScanoutResource and initFramebufferResource are each called 0 times -- and
   its GPUWrangler dump says "fb1:-1" (our display framebuffer has NO display
   pipe) while listing the accelerator.  The reason is that no Apple accelerator
   is believed to drive a display unless it carries the property
   IOAccelDisplayPipeCapabilities:
       { DisplayPipeSupported = true, TransactionsSupported = true }
   Apple's IntelAccelerator declares it in its Info.plist personality, as do
   AMDRadeonX4000/5000/6000 (both found verbatim in SystemKernelExtensions.kc),
   and NVAccel publishes it at runtime under debug.nvaccel_iop.  This port has
   never published it anywhere, so the display is never accelerator backed and
   WindowServer keeps CompositorSW for the desktop.
   Writing 1 here is the throwaway experiment: publish it now, so WindowServer --
   restarted, exactly as NVAccel documents -- should take the accelerated path. */
static int vioscanout_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gVioScanout;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || req->newptr == 0) return err;
    vioAccelPublishDisplayPipeCaps(v ? 1 : 0);
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, vioscanout, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN,
            NULL, 0, vioscanout_sysctl, "I",
            "1 = publish IOAccelDisplayPipeCapabilities and serve the scanout from the display pipe");

/* the scanout, published by VirtIOGPU.cpp (same kext) */
extern "C" IOPhysicalAddress vioGPU_scanoutPhys(void);
extern "C" IOByteCount       vioGPU_scanoutBytes(void);
extern "C" uint32_t          vioGPU_scanoutPitch(void);
/* P4.3 present path: the copy destination and the push to the host */
extern "C" void             *vioGPU_scanoutKva(void);
extern "C" uint32_t          vioGPU_scanoutHeight(void);
extern "C" bool              vioGPU_flushFrame(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* VirtIOGPU.cpp's framebuffer asks this to decide whether to declare its
   display mode kDisplayModeAcceleratorBackedFlag. That flag -- not the
   capability dictionary on its own -- is what WindowServer reads when it picks
   a compositor for the display: without it CDDisplay::render_update runs
   CompositorSW and the display pipe's own methods are never called (measured:
   isCurrentModeAcceleratorBacked / initScanoutResource 0 times). Same probe
   switch as the display-pipe publish, so one sysctl arms the whole story. */
extern "C" int vioAccelScanoutProbe(void) { return gVioScanout; }


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
    static unsigned sWillCount, sDidCount;
    void free() APPLE_KEXT_OVERRIDE;
    bool displayModeWillChange() APPLE_KEXT_OVERRIDE {
        vioAccelSetScanoutOwned(false);
        sWillCount++; ALOG("DM displayModeWillChange"); return true;
    }
    bool displayModeDidChange() APPLE_KEXT_OVERRIDE {
        vioAccelSetScanoutOwned(false);
        sDidCount++; ALOG("DM displayModeDidChange -> true"); return true;
    }
    /* observability only: the family calls this for each IOFramebuffer it finds,
       and this count is what NVAccel logs to show its heads registered. */
    void found_framebuffer(IOFramebuffer *fb) APPLE_KEXT_OVERRIDE {
        IOAccelLegacyDisplayMachine::found_framebuffer(fb);
        ALOG("DM found_framebuffer(%s) -> framebuffer count now %u",
             fb ? fb->getName() : "?", getFramebufferCount());
    }
};
static VIOAccelDisplayMachine *gVioDisplayMachine = nullptr;
unsigned VIOAccelDisplayMachine::sWillCount = 0;
unsigned VIOAccelDisplayMachine::sDidCount = 0;
void VIOAccelDisplayMachine::free()
{
    if (gVioDisplayMachine == this) gVioDisplayMachine = nullptr;
    IOAccelLegacyDisplayMachine::free();
}
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
    /* A real scanout is already backed: allocPhysical() republishes its host
       physical range, which getPhysicalSegment() reports. Other VidMemory
       objects are VA-only, as in AppleParavirt, and expose no physical range. */
    unsigned long long fPhys = 0;
    unsigned long long fBytes = 0;
    bool fScanout = false;

    bool allocPhysical() APPLE_KEXT_OVERRIDE {
        if (fScanout) {
            if (!fPhys || !fBytes) { ALOG("VidMem %p allocPhysical: SCANOUT with no address", this); return false; }
            *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = fPhys;
            ALOG("VidMem %p allocPhysical: SCANOUT phys 0x%llx (%llu bytes) — already backed, nothing allocated",
                 this, fPhys, fBytes);
            return true;
        }
        /* Venus/paravirt VidMemory is a VA-only family object, not a BAR
           allocation.  Match AppleParavirt: wire-down succeeds while
           getPhysicalSegment() below continues to report no physical segments
           when fPhys is zero.  Only the real scanout has a host physical range. */
        return true;
    }
    void deallocPhysical() APPLE_KEXT_OVERRIDE {
        if (!fScanout) return;
        ALOG("VidMem %p deallocPhysical: SCANOUT — not ours to free, only forgetting it", this);
        fPhys = 0; fBytes = 0; fScanout = false;
        *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = 0;
    }
    uint64_t getPhysicalSegment(unsigned long long a0, unsigned long long *a1) APPLE_KEXT_OVERRIDE {
        const unsigned long long total = *(const volatile unsigned long long *)((const unsigned char *)this + 0x40);
        if (!fPhys || a0 >= total) { if (a1) *a1 = 0; return 0; }
        if (a1) *a1 = total - a0;
        return fPhys + a0;
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

/* built after VirtIOAccel is complete (it calls the family's createVidMemory) */
static IOAccelMemory *vioAccelScanoutMemory(unsigned int index, IOAccelResource2 *res, const char *who);
/* Each resource owns the memory returned by initFramebufferResource or
   initScanoutResource and releases it from the corresponding family destructor.
   Do not cache a bare pointer across mode changes. */

/* --- P4.3 present path: the surface and transaction ABI ---------------------
   WindowServer's present path hands the display pipe a transaction whose
   plane 0 names the compositor's IOSurface (getPlaneIOSurface) and/or the
   accelerator resource behind it (getPlaneResource). Both class shapes are the
   reference's (kexts/NVRM/accel/nvrm-accel.cpp), verified against this guest's
   kernel: SystemKernelExtensions.kc defines
   IOAccelDisplayPipeTransaction2::getPlaneIOSurface(2)/getPlaneResource(2) and
   IOSurface::getWidth/getHeight/getBytesPerRow/getPixelFormat/getSurfaceID/
   getMemoryDescriptor, and the family calls them exactly this way. This is the
   macOS counterpart of UniXNU's GpuScanoutPresenter: the display pipe holds the
   scanout, the compositor's surface is copied into it, and the transaction is
   retired -- see tmp/UniXNU/drivers/AdrenoMetal/GpuScanoutPresenter.h. */
class IOSurface {
public:
    IOMemoryDescriptor *getMemoryDescriptor() const;
    unsigned long long  getWidth() const;
    unsigned long long  getHeight() const;
    unsigned long long  getBytesPerRow() const;
    unsigned int        getPixelFormat() const;
    unsigned int        getSurfaceID() const;
};
class IOAccelDisplayPipeTransaction2 {
public:
    IOSurface *getPlaneIOSurface(unsigned int, unsigned int) const;
    OSObject  *getPlaneResource(unsigned int, unsigned int) const;
};

/* one mapped compositor surface, kept across frames (WindowServer reuses the
   same surfaces): the map is dropped when the slot is recycled */
enum { kVioIopSrcSlots = 4 };
struct VIOIopSrc {
    IOMemoryMap *map = nullptr;
    IOMemoryDescriptor *md = nullptr;
    unsigned int id = 0;
};
static SInt64 gVioIopOk = 0, gVioIopFail = 0, gVioIopEmpty = 0;
static SInt64 gVioIopFlushFail = 0;
static unsigned gVioIopLogs = 0;
/* The kernel ring buffer on this guest is flooded by AppleSMCFamily errors, so
   the iop: lines rarely survive long enough to be read. Keep the counters and
   the last note where sysctl can always reach them. */
static char gVioIopLast[192] = "no transaction yet";
#define VIOIOPLOG(fmt, ...) do { \
    snprintf(gVioIopLast, sizeof gVioIopLast, fmt, ##__VA_ARGS__); \
    if (gVioIopLogs < 24) { gVioIopLogs++; ALOG("iop: " fmt, ##__VA_ARGS__); } } while (0)
#define VIOIOP_FAIL(rc, fmt, ...) do { \
    vioAccelSetScanoutOwned(false); \
    OSAddAtomic64(1, &gVioIopFail); \
    VIOIOPLOG(fmt, ##__VA_ARGS__); \
    return (rc); \
} while (0)
SYSCTL_QUAD(_debug, OID_AUTO, vioiop_ok,    CTLFLAG_RD | CTLFLAG_LOCKED, &gVioIopOk,    "display pipe transactions presented to the scanout");
SYSCTL_QUAD(_debug, OID_AUTO, vioiop_fail,  CTLFLAG_RD | CTLFLAG_LOCKED, &gVioIopFail,  "display pipe transactions refused (see debug.vioiop_last)");
SYSCTL_QUAD(_debug, OID_AUTO, vioiop_empty, CTLFLAG_RD | CTLFLAG_LOCKED, &gVioIopEmpty, "display pipe transactions with no plane 0 surface");
SYSCTL_QUAD(_debug, OID_AUTO, vioiop_flush_fail, CTLFLAG_RD | CTLFLAG_LOCKED, &gVioIopFlushFail, "display pipe transfers or flushes refused by the host");
SYSCTL_STRING(_debug, OID_AUTO, vioiop_last, CTLFLAG_RD | CTLFLAG_LOCKED, gVioIopLast, sizeof gVioIopLast, "last display pipe transaction note");

class VIOAccelDisplayPipe;
static VIOAccelDisplayPipe *gVioDisplayPipe = nullptr;
class VIOAccelDisplayPipe : public IOAccelLegacyDisplayPipe {
    OSDeclareDefaultStructors(VIOAccelDisplayPipe)
    VIO_FWD_FWD(VIOAccelDisplayPipe)
public:
    static unsigned sDidCount, sFbInitCount, sFbInitOkCount;
    VIOIopSrc fIopSrc[kVioIopSrcSlots];
    unsigned fIopNext = 0;
    SInt64 fIopCompleted = 0;   /* transactions whose copy is done (retired by sequence) */

    void free() APPLE_KEXT_OVERRIDE {
        vioAccelSetScanoutOwned(false);
        if (gVioDisplayPipe == this) gVioDisplayPipe = nullptr;
        for (unsigned i = 0; i < kVioIopSrcSlots; i++) {
            if (fIopSrc[i].map) { fIopSrc[i].map->release(); fIopSrc[i].map = nullptr; }
            if (fIopSrc[i].md)  { fIopSrc[i].md->complete(); fIopSrc[i].md->release(); fIopSrc[i].md = nullptr; }
            fIopSrc[i].id = 0;
        }
        IOAccelLegacyDisplayPipe::free();
    }
    /* map a surface's memory read-only into the kernel, reusing a slot when the
       same surface comes back next frame (as the reference's iopMapSurface). */
    const void *iopMapSurface(unsigned int id, IOMemoryDescriptor *md, unsigned long long need) {
        for (unsigned i = 0; i < kVioIopSrcSlots; i++)
            if (fIopSrc[i].md == md && fIopSrc[i].id == id && fIopSrc[i].map)
                return (fIopSrc[i].map->getLength() >= need) ? (const void *)fIopSrc[i].map->getVirtualAddress() : nullptr;
        if (md->getLength() < need) return nullptr;
        VIOIopSrc *e = &fIopSrc[fIopNext % kVioIopSrcSlots]; fIopNext++;
        if (e->map) { e->map->release(); e->map = nullptr; }
        if (e->md)  { e->md->complete(); e->md->release(); e->md = nullptr; }
        e->id = 0;
        if (md->prepare() != kIOReturnSuccess) return nullptr;
        IOMemoryMap *m = md->map(kIOMapReadOnly);
        if (!m) { md->complete(); return nullptr; }
        md->retain(); e->md = md; e->map = m; e->id = id;
        VIOIOPLOG("mapped source surface 0x%x (%llu bytes) into slot %u", id, (unsigned long long)m->getLength(), (fIopNext - 1) % kVioIopSrcSlots);
        return (const void *)m->getVirtualAddress();
    }
    IOAccelMemory *initFramebufferResource(unsigned int index, IOAccelResource2 *res) APPLE_KEXT_OVERRIDE {
        sFbInitCount++;
        ALOG("Pipe %p initFramebufferResource(%u, res %p)", this, index, res);
        if (IOAccelMemory *m = vioAccelScanoutMemory(index, res, "initFramebufferResource")) { sFbInitOkCount++; return m; }
        ALOG("Pipe %p initFramebufferResource(%u) -> NULL (no accel scanout)", this, index);
        return nullptr;
    }
    void displayModeDidChange() APPLE_KEXT_OVERRIDE {
        vioAccelSetScanoutOwned(false);
        sDidCount++;
        IOAccelLegacyDisplayPipe::displayModeDidChange();
    }
    /* EXPERIMENT. This is the query that decides whether the accelerator can
       drive the display at all, and the baseline answer is the family's, since
       this port never implemented it. Log the family's answer either way so the
       probe tells us which of the two it was. */
    bool isCurrentModeAcceleratorBacked() APPLE_KEXT_OVERRIDE {
        const bool base = IOAccelLegacyDisplayPipe::isCurrentModeAcceleratorBacked();
        if (gVioScanout) {
            ALOG("Pipe %p isCurrentModeAcceleratorBacked: family said %d -> TRUE (probe)", this, base);
            return true;
        }
        ALOG("Pipe %p isCurrentModeAcceleratorBacked -> %d (family)", this, base);
        return base;
    }
    /* the scanout resource WindowServer composites into. Baseline: the family's
       own setup, which has no real scanout behind it to hand over. */
    /* init_scanout_resource() stores and prepares this returned memory. The
       reconstructed header originally inferred bool; returning TRUE made the
       family write through address 0x1 and panic at object+0xc. */
    IOAccelMemory *initScanoutResource(unsigned int index, IOAccelResource2 *res) APPLE_KEXT_OVERRIDE {
        IOAccelMemory *base = IOAccelLegacyDisplayPipe::initScanoutResource(index, res);
        ALOG("Pipe %p initScanoutResource(%u, res %p): family -> %p", this, index, res, base);
        if (!gVioScanout || base) return base;
        IOAccelMemory *m = vioAccelScanoutMemory(index, res, "initScanoutResource");
        if (m) { ALOG("Pipe %p initScanoutResource: family declined, fresh scanout %p offered", this, m); return m; }
        ALOG("Pipe %p initScanoutResource: nothing to hand over", this);
        return nullptr;
    }
    IOReturn validateTransaction(IOAccelDisplayPipeTransaction2 *txn) APPLE_KEXT_OVERRIDE {
        (void)txn;
        /* nothing to reject: the copy is synchronous, so any transaction the
           family formed is one we can serve. Success lets it call us. */
        return kIOReturnSuccess;
    }
    /* P4.3: the present. Copy the transaction's plane-0 compositor surface into
       this pipe's scanout, then push it to the host (T2D+FLUSH). The copy is the
       CPU row copy the reference does for a non-zero-copy panel; the virtio 2D
       scanout is a linear B8G8R8A8 store, so the rows land directly. */
    IOReturn performTransaction(IOAccelDisplayPipeTransaction2 *txn) APPLE_KEXT_OVERRIDE {
        if (!txn) VIOIOP_FAIL(kIOReturnBadArgument, "performTransaction(NULL)");
        void *dst = vioGPU_scanoutKva();
        const uint32_t dstPitch = vioGPU_scanoutPitch(), dstH = vioGPU_scanoutHeight();
        if (!dst || !dstPitch || !dstH) VIOIOP_FAIL(kIOReturnNotReady, "no scanout to present into");

        IOSurface *s = txn->getPlaneIOSurface(0, 0);
        OSObject  *ro = txn->getPlaneResource(0, 0);
        if (!s && !ro) { OSAddAtomic64(1, &gVioIopEmpty); return kIOReturnSuccess; }   /* nothing to show */

        const uint8_t *src = nullptr;
        unsigned w = 0, h = 0, pitch = 0;
        if (s) {
            const unsigned fmt = s->getPixelFormat();
            if (fmt != 0x42475241u) VIOIOP_FAIL(kIOReturnUnsupported, "surface 0x%x format 0x%08x is not BGRA", s->getSurfaceID(), fmt);
            const unsigned long long W = s->getWidth(), H = s->getHeight(), P = s->getBytesPerRow();
            if (!W || !H || P < 4 * W || (P & 3)) VIOIOP_FAIL(kIOReturnBadArgument, "surface geometry %llux%llu pitch %llu", W, H, P);
            IOMemoryDescriptor *md = s->getMemoryDescriptor();
            if (!md) VIOIOP_FAIL(kIOReturnNoMemory, "surface 0x%x has no memory descriptor", s->getSurfaceID());
            src = (const uint8_t *)iopMapSurface(s->getSurfaceID(), md, H * P);
            if (!src) VIOIOP_FAIL(kIOReturnVMError, "surface 0x%x would not prepare/map %llu bytes", s->getSurfaceID(), H * P);
            w = (unsigned)W; h = (unsigned)H; pitch = (unsigned)P;
        } else {
            VIOIOP_FAIL(kIOReturnUnsupported, "plane resource %p is not an IOSurface we can map", ro);
        }
        if (!w || !h || pitch < 4) VIOIOP_FAIL(kIOReturnBadArgument, "source geometry %ux%u pitch %u", w, h, pitch);
        if (w != dstPitch / 4 || h != dstH) VIOIOP_FAIL(kIOReturnBadArgument, "source %ux%u is not the scanout's %ux%u", w, h, dstPitch / 4, dstH);

        const size_t rowBytes = (size_t)w * 4;
        uint8_t *d = (uint8_t *)dst;
        if (rowBytes == pitch && pitch == dstPitch) {
            bcopy(src, d, rowBytes * h);
        } else {
            for (unsigned y = 0; y < h; y++)
                bcopy(src + (size_t)y * pitch, d + (size_t)y * dstPitch, rowBytes);
        }
        if (!vioGPU_flushFrame(0, 0, w, h)) {
            OSAddAtomic64(1, &gVioIopFlushFail);
            VIOIOP_FAIL(kIOReturnIOError, "transaction surface 0x%x transfer/flush failed", s->getSurfaceID());
        }
        vioAccelSetScanoutOwned(true);

        const SInt64 n = OSAddAtomic64(1, &gVioIopOk);
        OSAddAtomic64(1, &fIopCompleted);
        if (n <= 3 || n == 600 || (n & 0xffff) == 0)
            VIOIOPLOG("transaction #%lld: %ux%u pitch %u surface 0x%x -> scanout, flushed", (long long)n, w, h, pitch, s->getSurfaceID());
        return kIOReturnSuccess;
    }
    /* The base IOAccelDisplayPipeUserClient2 selector 8 endpoint queues the
       transaction; the family workloop later calls this method through the
       pipe vtable. IOAccelLegacyDisplayPipe::performTransaction belongs to the
       older path and is not a substitute: leaving submitTransaction to the
       family makes IOAccelDisplayPipeTransactionEnd return kIOReturnNotReady.
       AppleParavirtDisplayPipe implements this same entry point and returns an
       IOReturn (the old inferred header incorrectly declared it void). */
    IOReturn submitTransaction(IOAccelDisplayPipeTransaction2 *txn) APPLE_KEXT_OVERRIDE {
        return performTransaction(txn);
    }
    /* P4.3: retirement. The copy above is synchronous, so by the time the family
       asks, the transaction is already on the panel -- nothing is in flight. */
    bool isTransactionComplete(IOAccelDisplayPipeTransaction2 *txn) APPLE_KEXT_OVERRIDE {
        (void)txn;
        return true;
    }
};
unsigned VIOAccelDisplayPipe::sDidCount = 0;
unsigned VIOAccelDisplayPipe::sFbInitCount = 0;
unsigned VIOAccelDisplayPipe::sFbInitOkCount = 0;
OSDefineMetaClassAndStructors(VIOAccelDisplayPipe, IOAccelLegacyDisplayPipe)

/* transactionEnd rejects kIOReturnNotReady before transaction_end() when the
   accelerator-ready bit is clear, pipe +0x282 is set, or isActive (+0x298) is
   false. These are family-private state bytes, so sample them on demand rather
   than guessing which of the three gates rejected the present. */
static int vioiop_gate_sysctl SYSCTL_HANDLER_ARGS
{
    char note[448];
    VIOAccelDisplayPipe *pipe = gVioDisplayPipe;
    VirtIOAccel *accel = gVioAccelInst;
    VIOAccelDisplayMachine *dm = gVioDisplayMachine;
    if (!pipe || !accel || !dm)
        snprintf(note, sizeof note, "incomplete display state (pipe %p accel %p dm %p)", pipe, accel, dm);
    else {
        const volatile uint8_t *p = (const volatile uint8_t *)pipe;
        const volatile uint8_t *a = (const volatile uint8_t *)accel;
        const volatile uint8_t *d = (const volatile uint8_t *)dm;
        const volatile uint8_t *res = *(const volatile uint8_t *const volatile *)(p + 0xe0);
        const void *mem = res ? *(const void *const volatile *)(res + 0x88) : nullptr;
        IOFramebuffer *fb = *(IOFramebuffer *const volatile *)(p + 0x98);
        IOService *ours = vioFramebufferService();
        snprintf(note, sizeof note,
                 "pipe %p fb=%s(%p) ours=%p +280=%u +282=%u active(+298)=%u +299=%u; accel flags(+c78)=%#x ready=%u +180=%p; queue %u/%u\n"
                 "dm %p pipes=%u nest=%u callbacks will/did=%u/%u; pipe did=%u fbinit=%u/%u; resource=%p memory=%p",
                 pipe, fb ? fb->getName() : "?", fb, ours, p[0x280], p[0x282], p[0x298], p[0x299],
                 *(const volatile uint32_t *)(a + 0xc78),
                 (*(const volatile uint32_t *)(a + 0xc78) & 2) != 0,
                 *(const void *const volatile *)(a + 0x180),
                 *(const volatile uint32_t *)(p + 0x238),
                 *(const volatile uint32_t *)(p + 0x23c), dm,
                 *(const volatile uint32_t *)(d + 0x108),
                 *(const volatile uint32_t *)(d + 0x10c),
                 VIOAccelDisplayMachine::sWillCount, VIOAccelDisplayMachine::sDidCount,
                 VIOAccelDisplayPipe::sDidCount, VIOAccelDisplayPipe::sFbInitCount,
                 VIOAccelDisplayPipe::sFbInitOkCount, res, mem);
    }
    return sysctl_handle_string(oidp, note, sizeof note, req);
}
SYSCTL_PROC(_debug, OID_AUTO, vioiop_gate, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN,
            NULL, 0, vioiop_gate_sysctl, "A", "live IOAccelDisplayPipeUserClient2 transactionEnd gate state");

/* Publishing the display-pipe capabilities after the family's initial sweep does
   not itself generate an IOFB mode-change notification. Deliver the family's
   normal will/did pair on demand, exactly as the NVAccel bring-up does: the did
   side calls enableAccelerator() and initializes each pipe's framebuffer
   resource, whose successful prepare() is what sets isActive (+0x298). */
static int vioactivate_sysctl SYSCTL_HANDLER_ARGS
{
    int v = 0;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || req->newptr == 0) return err;
    VIOAccelDisplayMachine *dm = gVioDisplayMachine;
    if (!v) return 0;
    if (!dm || !gVioDisplayPipe) return kIOReturnNotReady;
    vioAccelSetScanoutOwned(false);
    if (!vioAccelFindVramDescriptor())
        ALOG("activation: framebuffer still has no usable VRAM descriptor");
    dm->display_mode_will_change(0);
    dm->display_mode_did_change(0);
    vioAccelSetScanoutOwned(false);
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, vioactivate, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_KERN,
            NULL, 0, vioactivate_sysctl, "I", "deliver the family's display mode will/did activation callbacks");

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
        gVioDisplayMachine = d;
        ALOG("newDisplayMachine -> %p", d);
        return d;
    }
    OSObject *newDisplayPipe() APPLE_KEXT_OVERRIDE {
        VIOAccelDisplayPipe *p = OSTypeAlloc(VIOAccelDisplayPipe);
        if (!p) { ALOG("newDisplayPipe: OSTypeAlloc FAILED — the family stores this NULL unchecked"); return NULL; }
        gVioDisplayPipe = p;
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
        /* Custom debug sysctls are not initialized from boot-args by XNU.
           Parse ours before the family creates its display machine and pipes:
           scanout memory creation needs both gVioScanout and gVioAccelInst. */
        uint32_t on = 0;
        if (PE_parse_boot_argn("debug.vioscanout", &on, sizeof on) && on)
            gVioScanout = 1;
        on = 0;
        if (PE_parse_boot_argn("debug.vioaccltypes", &on, sizeof on) && on)
            gVioAccelRef = 1;
        gVioAccelInst = this;
        vioAccelSetScanoutOwned(false);
        if (gVioScanout) vioAccelPublishDisplayPipeCaps(1);
        ALOG("start(%s): calling IOGraphicsAccelerator2::start — the family's own bring-up", provider ? provider->getName() : "?");
        bool r = IOGraphicsAccelerator2::start(provider);
        ALOG("IOGraphicsAccelerator2::start -> %d", r);
        if (r) {
            if (gVioAccelRef) vioAccelPublishAccelReference(1);
            registerService();
            ALOG("registerService() — Metal can now find MetalPluginName=%s", "VMTLDriver");
            sysctl_register_oid(&sysctl__debug_nvaccel_headless);
            sysctl_register_oid(&sysctl__debug_vioscanout);
            sysctl_register_oid(&sysctl__debug_vioaccltypes);
            sysctl_register_oid(&sysctl__debug_vioiop_ok);
            sysctl_register_oid(&sysctl__debug_vioiop_fail);
            sysctl_register_oid(&sysctl__debug_vioiop_empty);
            sysctl_register_oid(&sysctl__debug_vioiop_flush_fail);
            sysctl_register_oid(&sysctl__debug_vioiop_owns);
            sysctl_register_oid(&sysctl__debug_vioiop_last);
            sysctl_register_oid(&sysctl__debug_vioiop_gate);
            sysctl_register_oid(&sysctl__debug_vioactivate);
            ALOG("probe switch: debug.vioscanout = %d (0 = P3 behaviour; 1 = scanout-backed display pipe)",
                 gVioScanout);
        } else if (gVioAccelInst == this) {
            gVioAccelInst = nullptr;
        }
        return r;
    }
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE {
        ALOG("stop");
        vioAccelSetScanoutOwned(false);
        sysctl_unregister_oid(&sysctl__debug_nvaccel_headless);
        sysctl_unregister_oid(&sysctl__debug_vioscanout);
        sysctl_unregister_oid(&sysctl__debug_vioaccltypes);
        sysctl_unregister_oid(&sysctl__debug_vioiop_ok);
        sysctl_unregister_oid(&sysctl__debug_vioiop_fail);
        sysctl_unregister_oid(&sysctl__debug_vioiop_empty);
        sysctl_unregister_oid(&sysctl__debug_vioiop_flush_fail);
        sysctl_unregister_oid(&sysctl__debug_vioiop_owns);
        sysctl_unregister_oid(&sysctl__debug_vioiop_last);
        sysctl_unregister_oid(&sysctl__debug_vioiop_gate);
        sysctl_unregister_oid(&sysctl__debug_vioactivate);
        if (gVioAccelInst == this) gVioAccelInst = nullptr;
        IOGraphicsAccelerator2::stop(provider);
    }
};
OSDefineMetaClassAndStructors(VirtIOAccel, IOGraphicsAccelerator2)

static bool vioAccelFindVramDescriptor()
{
    return gVioAccelInst && gVioAccelInst->find_vram_descriptor();
}

/* EXPERIMENT (debug.vioscanout): the one thing that was missing -- the
   accelerator must advertise a display pipe at all before WindowServer will
   consider the display accelerator backed.  Publishing and removing it at
   runtime keeps "reboot" the only undo (the property is not in Info.plist). */
static void vioAccelPublishDisplayPipeCaps(int on)
{
    if (!on) vioAccelSetScanoutOwned(false);
    VirtIOAccel *a = gVioAccelInst;
    if (!a) { ALOG("probe: no accelerator instance to publish on"); return; }
    if (on) {
        OSDictionary *caps = OSDictionary::withCapacity(2);
        if (!caps) { ALOG("probe: OSDictionary::withCapacity FAILED"); return; }
        caps->setObject("DisplayPipeSupported", kOSBooleanTrue);
        caps->setObject("TransactionsSupported", kOSBooleanTrue);
        a->setProperty("IOAccelDisplayPipeCapabilities", caps);
        caps->release();
        gVioScanout = 1;
        ALOG("probe: debug.vioscanout=1 -- IOAccelDisplayPipeCapabilities %p published on %p, display pipe OPEN",
             a->getProperty("IOAccelDisplayPipeCapabilities"), a);
    } else {
        gVioScanout = 0;
        a->removeProperty("IOAccelDisplayPipeCapabilities");
        ALOG("probe: debug.vioscanout=0 -- capabilities removed on %p, back to P3 behaviour", a);
    }
}

/* EXPERIMENT (debug.vioaccltypes): publish the accelerator association on
   VirtIOFB, the way IONDRVSupport publishes it on IONDRVFramebuffer. The path
   we write is exactly what getPath() produces, which is exactly the format the
   Apple node carries, so the consumer (whatever CoreDisplay uses to resolve
   display device -> IOAccelerator) sees a byte-identical value. */
static void vioAccelPublishAccelReference(int on)
{
    IOService *fb = vioFramebufferService();
    VirtIOAccel *a = gVioAccelInst;
    if (!fb) { ALOG("acclref: no VirtIOFB service (reached start()?)"); return; }
    if (!on) {
        fb->removeProperty("IOAccelTypes");
        fb->removeProperty("IOAccelIndex");
        fb->removeProperty("IOAccelRevision");
        gVioAccelRef = 0;
        ALOG("acclref: IOAccelTypes removed from %s (%p)", fb->getName(), fb);
        return;
    }
    if (!a) { ALOG("acclref: no accelerator instance to point at"); return; }
    char path[512];
    int len = sizeof(path);
    if (!a->getPath(path, &len, gIOServicePlane)) {
        ALOG("acclref: getPath failed (needs %d bytes)", len);
        return;
    }
    fb->setProperty("IOAccelTypes", path);
    fb->setProperty("IOAccelIndex", 0ull, 32);
    fb->setProperty("IOAccelRevision", 2ull, 32);
    gVioAccelRef = 1;
    ALOG("acclref: IOAccelTypes=%s IOAccelIndex=0 IOAccelRevision=2 published on %s (%p)",
         path, fb->getName(), fb);
}

/* EXPERIMENT (debug.vioscanout): wrap the live scanout in a VidMemory — the same
   shape NVAccel's display pipe uses (phys at +0xc8, getPhysicalSegment reporting
   it). NULL unless the probe is on and VirtIOGPU has published a scanout. */
static IOAccelMemory *vioAccelScanoutMemory(unsigned int index, IOAccelResource2 *res, const char *who)
{
    if (!gVioScanout) return nullptr;
    if (!gVioAccelInst) { ALOG("%s(%u): no accelerator instance", who, index); return nullptr; }
    const IOPhysicalAddress phys = vioGPU_scanoutPhys();
    const IOByteCount bytes = vioGPU_scanoutBytes();
    if (!phys || !bytes) { ALOG("%s(%u): the framebuffer has no scanout to give", who, index); return nullptr; }
    OSObject *o = gVioAccelInst->createVidMemory(nullptr, res, bytes, nullptr);
    VIOAccelVidMemory *m = OSDynamicCast(VIOAccelVidMemory, o);
    if (!m) {
        ALOG("%s(%u): createVidMemory gave %p, not a VIOAccelVidMemory", who, index, o);
        if (o) o->release();
        return nullptr;
    }
    m->fScanout = true; m->fPhys = phys; m->fBytes = bytes;
    *(volatile unsigned long long *)((unsigned char *)m + 0xc8) = phys;
    ALOG("%s(%u, res %p) -> vidmem %p over the LIVE SCANOUT phys 0x%llx pitch %u (%llu bytes)",
         who, index, res, m, (unsigned long long)phys, vioGPU_scanoutPitch(), (unsigned long long)bytes);
    return (IOAccelMemory *)m;
}

static void vioAccelWriteStamp(int s, unsigned int v) {
    if (!gVioAccelInst) return;
    gVioAccelInst->storeStamp(s, v);
}

/* naked thunks for the inherited slots Apple stopped exporting (vio_fwd.h) */
VIO_FWD_FWD_DEFS
