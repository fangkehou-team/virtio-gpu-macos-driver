/*
 * VirtIOAGDC — the AppleGraphicsDeviceControl answer for the virtio GPU.
 *
 * Why this exists (measured; tmp/RESEARCH-PLAN.md section G): the desktop is
 * composited by CompositorSW because the display framebuffer is never
 * associated with the accelerator. WindowServer's own log says why, on every
 * boot, in this order:
 *
 *   [CoreDisplay] GetSystemGPUVendors: IONDRVFramebuffer / VirtIOAppleParavirtFB
 *   (IOPresentment) Error: Unable to get AGDC information.  Return code: 0x10000003
 *   (IOPresentment) Error: Unable to locate service dependencies
 *   (IOPresentment) Error: Unable to acquire map
 *   [CoreDisplay] [ERROR] exception: CoreDisplay::Framebuffer::Framebuffer(...)
 *   [CoreDisplay] Creating FB 1 of 2 -> Failed to create FB 1 of 2
 *
 * and its GPU table then prints "fb1:-1" for /VirtIOGPU/VirtIOAppleParavirtFB: no display
 * pipe. That association is built -- inside WindowServer, by IOPresentment --
 * out of the GPU's AGDC (AppleGraphicsDeviceControl):
 *
 *   ioPresentmentFindAGDCMatch -> kAGDCVendorInfo -> kAGDCGPUCapability
 *                              -> DisplayPipeSupported / TransactionsSupported
 *
 * A GPU with no AGDC object gets none of it. The NV reference driver answers
 * exactly these two vendor commands (kexts/NVRMAGDC/nvrm-agdc.cpp) and the NV
 * display chain lists its NVRMAGDC as the "display policy" leg; this kext is
 * the same answer for the virtio GPU: vendor VirtIO/0x1af4, and a
 * kAGDCGPUCapability payload whose framebuffer[] slots name VirtIOAppleParavirtFB in
 * IOFBDependentIndex order -- i.e. the "which framebuffers does this GPU
 * drive" table IOPresentment is missing.
 *
 * Deliberately NOT copied from the reference: the plane-scaler capability
 * (0x711) and the -nvoff boot-arg check. 0x921 kAGDCLinkConfig *is* answered
 * (behind debug.vioagdc_link), from this port's one published timing; see
 * vio-agdc-link.h. Everything that is not one of the commands above stays
 * kIOReturnUnsupported and is counted.
 *
 * The AGDC object is created only when `sysctl debug.vioagdc=1` is written
 * (default 0 = the GPU has no AGDC, exactly as before this kext existed), so a
 * bad bring-up cannot boot-loop the VM and a plain reboot is the certain undo.
 * This is a throwaway measurement build: it is not wired into package/.
 *
 * It compiles into io.vmtl.VirtIOGPU, it is NOT its own kext. A standalone
 * io.vmtl.VirtIOAGDC.kext was built and installed first; the system silently
 * rolled /Library/Extensions AND the aux KC back to the last approved state at
 * boot (KextPolicy has no entry for the new bundle id, and macOS 13.7 only
 * grants that approval interactively), so nothing of it survived to load.
 * Shipping inside the already-approved bundle is the same answer the VirtIOAccel
 * header reaches, and it is why this file has no _start/_stop/KMOD_EXPLICIT_DECL:
 * VirtIOGPU.cpp owns those for the whole bundle. It is also why nothing here is
 * reached through an Info.plist personality: the in-bundle entry point is
 * vioAgdcInstall(), called by VirtIOAppleParavirtFB::start() -- see the comment on it below
 * for the measurement that forced that.
 */

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <sys/sysctl.h>
#include <kern/thread_call.h>
#include <libkern/OSAtomic.h>
#include <sys/proc.h>   /* proc_selfpid/proc_name: which process issues a command */
#include <string.h>

#define VIOLOG(fmt, ...) do { kprintf("VirtIOAGDC: " fmt "\n", ##__VA_ARGS__); IOLog("VirtIOAGDC: " fmt "\n", ##__VA_ARGS__); } while (0)
extern "C" IOService *vioFramebufferService(void);

#include "AppleGraphicsDeviceControl_re.h"
#include "vio-agdc-link.h"   /* AGDCLinkConfig_t (0x921) + this port's one timing */

/* AGDCVendorInfo_t / AGDCGPUCapability_t, layout taken from the NV reference
   (kexts/NVRMAGDC/nvrm-agdc.cpp), where both sizes are pinned by static_assert
   against the real family structs. */
#pragma pack(push, 1)
struct VIOAGDCVendorInfo {
    uint32_t version;
    char     vendor[0x20];
    uint32_t vendorID;
    uint32_t vendorClass;
};
struct VIOAGDCGPUCapability {
    uint64_t portMask;
    uint64_t portFeatureA;
    uint64_t portFeatureB;
    uint64_t portFeatureC;
    uint32_t countA, countB, countC, countD;
    uint32_t maxFramebuffers;
    IOService *pciDevice;
    IOService *framebuffer[20];
};
#pragma pack(pop)
static_assert(sizeof(VIOAGDCVendorInfo) == 0x2c, "AGDCVendorInfo_t is 0x2c");
static_assert(sizeof(VIOAGDCGPUCapability) == 0xdc, "AGDCGPUCapability_t is 0xdc");

enum { kAgdcCmdVendorInfo = 1, kAgdcCmdGpuCapability = 0x980, kAgdcCmdDisplayPipe = 0x2001,
       kAgdcCmdLinkConfig = 0x921, kAgdcCmdFbExtCap = 0x711, kAgdcCmdStream925 = 0x925 };

static const char *kFramebufferClass = "VirtIOAppleParavirtFB";

static SInt64 gAgdcCmds = 0, gAgdcUnsupported = 0;
static int    gAgdcLastRefused = 0;
static int    gAgdcLogged = 0;
static SInt64 gAgdcCapReplies = 0;
static int    gAgdcLastMaxFB = -1;

/* 0x2001 probe (G.7 continuation): WindowServer asks kAGDCGPUCapability's
   sibling command 0x2001 with a 4-byte input, every start, right after the
   vendor-info query, and stops at "Interface creation error: 0x2002" when we
   refuse it. We do not yet know its reply struct, so the probe answers it with
   kIOReturnSuccess and whatever size the caller asked for, zeroed, and logs the
   exact input words + requested out size. debug.vioagdc_yes: 0 off, 1 probe
   0x2001 only, 2 probe every unhandled command. */
static int    gAgdcYes = 0;
static SInt64 gAgdcYesAnswered = 0;
static char   gAgdcLastCmd[256] = "none (no unhandled command seen yet)";

/* 0x921 kAGDCLinkConfig probe (see vio-agdc-link.h): once the presentment
   object exists, WindowServer asks the AGDC to configure the display link.
   debug.vioagdc_link: 0 refuse (baseline), 1 answer with this port's one
   timing. The link config is always logged, answered or not. */
static int    gAgdcLink = 0;
static SInt64 gAgdcLinkAnswered = 0;
static SInt64 gAgdcFbCapAnswered = 0;
static SInt64 gAgdc925Answered = 0;
/* every 0x711 type WindowServer has asked for, 'A' = answered, 'R' = refused */
static char   gAgdcFbCapTypes[256] = "none";

static inline void agdcPutQ(uint8_t *o, unsigned off, uint64_t v) { memcpy(o + off, &v, 8); }
static inline void agdcPutD(uint8_t *o, unsigned off, uint32_t v) { memcpy(o + off, &v, 4); }

/* 0x711 kAGDCFBGetExtendedCapability, transcribed from
   AppleParavirtFramebuffer::doDeviceAttribute (tmp/p43-apv-fbattr.c). The
   caller hands over a 6508-byte {fbIndex, type, ...}; Apple's own paravirt
   framebuffer answers these types at these byte offsets, so we answer the same
   way. Type 0x10 is the detailed timing and stays with vioAgdcFillPlaneScaler.
   Types Apple itself refuses (0x8000 among them) are refused here too. */
static bool vioAgdcFillFbExtCap(void *outp, uint32_t type)
{
    uint8_t *o = (uint8_t *)outp;
    switch (type) {
    case 0x1:
        agdcPutQ(o, 8, 0x500000001ull);
        agdcPutQ(o, 16, 0);
        agdcPutQ(o, 24, 0x800000000ull);
        agdcPutD(o, 32, 0);
        agdcPutQ(o, 36, 0);
        agdcPutD(o, 44, 197376);
        agdcPutQ(o, 48, 0x3F800000BF800000ull);
        return true;
    case 0x2:
        agdcPutD(o, 8, 1); agdcPutD(o, 12, 5);
        agdcPutQ(o, 16, 0);              /* Apple: (fb+544 != 0); no second plane here */
        agdcPutQ(o, 24, 0x8000000000ull);
        agdcPutD(o, 32, 0);
        agdcPutQ(o, 36, 0);
        agdcPutQ(o, 44, 0x3F80000000000000ull);
        agdcPutQ(o, 52, 0);
        return true;
    case 0x4:
        agdcPutD(o, 8, 1);
        for (unsigned off = 12; off <= 52; off += 8) agdcPutQ(o, off, 0);
        return true;
    case 0x8:
        agdcPutD(o, 8, 1);
        memset(o + 12, 0, 812);
        return true;
    case 0x20:                            /* LABEL_66: supported, one qword of flags */
    case 0x80:
    case 0x400:
    case 0x800:
        agdcPutQ(o, 8, 1); agdcPutQ(o, 16, 0);
        return true;
    case 0x100:
        agdcPutD(o, 8, 1);
        agdcPutQ(o, 12, 0); agdcPutQ(o, 20, 0); agdcPutD(o, 28, 0);
        return true;
    case 0x200:                           /* LABEL_60: supported, nothing else */
    case 0x2000:
    case 0x4000:
        agdcPutQ(o, 8, 1);
        return true;
    case 0x1000:
        agdcPutD(o, 8, 1);
        agdcPutQ(o, 12, 0); agdcPutQ(o, 20, 0); agdcPutQ(o, 28, 0);
        agdcPutQ(o, 36, 0); agdcPutQ(o, 44, 0); agdcPutD(o, 52, 0);
        return true;
    default:
        return false;
    }
}

static void vioAgdcNoteFbCapType(uint32_t type, bool answered)
{
    char ent[16];
    snprintf(ent, sizeof ent, " %#x:%c", type, answered ? 'A' : 'R');
    const size_t n = strlen(ent);
    for (const char *p = gAgdcFbCapTypes; *p; p++)      /* no strstr() in the kext libc */
        if (strncmp(p, ent, n) == 0) return;
    if (strlen(gAgdcFbCapTypes) > 200) strlcpy(gAgdcFbCapTypes, "...", sizeof gAgdcFbCapTypes);
    if (strcmp(gAgdcFbCapTypes, "none") == 0) gAgdcFbCapTypes[0] = 0;
    strlcat(gAgdcFbCapTypes, ent, sizeof gAgdcFbCapTypes);
}

enum { kAgdcMaxFB = 4 };
static IOService *gAgdcFB[kAgdcMaxFB];
static unsigned  gAgdcNFB = 0;
enum { kAgdcFBMapBytes = 3 + kAgdcMaxFB * 25 + 52 + 1 };
static char   gAgdcFBMap[kAgdcFBMapBytes] = "never scanned";

class VIOAGDCDeviceControl : public AppleGraphicsDeviceControl {
    OSDeclareDefaultStructors(VIOAGDCDeviceControl)
public:
    IOService *fPci; IOService *fFB;
    using AppleGraphicsDeviceControl::vendor_doDeviceAttribute;
    virtual IOReturn vendor_doDeviceAttribute(unsigned int cmd, unsigned long *in, unsigned long inSize,
                                              unsigned long *out, unsigned long *outSize, IOExternalMethodArguments *args) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(VIOAGDCDeviceControl, AppleGraphicsDeviceControl)

IOReturn VIOAGDCDeviceControl::vendor_doDeviceAttribute(unsigned int cmd, unsigned long *in, unsigned long inSize,
                                                        unsigned long *out, unsigned long *outSize, IOExternalMethodArguments *args)
{
    OSAddAtomic64(1, &gAgdcCmds);
    if (cmd == kAgdcCmdVendorInfo) {
        if (!out || !outSize || *outSize != sizeof(VIOAGDCVendorInfo)) return kIOReturnBadArgument;
        VIOAGDCVendorInfo *v = (VIOAGDCVendorInfo *)out; bzero(v, sizeof *v);
        v->version = 0x30000; strlcpy(v->vendor, "VirtIO", sizeof v->vendor); v->vendorID = 0x1af4; v->vendorClass = 2;
        VIOLOG("vendor info asked -> VirtIO 0x1af4 (version %#x)", v->version);
        return kIOReturnSuccess;
    }
    if (cmd == kAgdcCmdGpuCapability) {
        if (!out || !outSize || *outSize != sizeof(VIOAGDCGPUCapability)) return kIOReturnBadArgument;
        VIOAGDCGPUCapability *c = (VIOAGDCGPUCapability *)out; bzero(c, sizeof *c);
        unsigned n = gAgdcNFB;
        if (n == 0 || n > kAgdcMaxFB) {
            c->portMask = 1ull << 1;
            c->countA = c->countB = c->countC = c->countD = 1;
            c->maxFramebuffers = 1; c->pciDevice = fPci; c->framebuffer[0] = fFB;
        } else {
            for (unsigned i = 0; i < n; i++) { c->framebuffer[i] = gAgdcFB[i]; c->portMask |= 1ull << (1 + i); }
            c->countA = c->countB = c->countC = c->countD = n;
            c->maxFramebuffers = n; c->pciDevice = fPci;
        }
        gAgdcLastMaxFB = (int)c->maxFramebuffers;
        OSAddAtomic64(1, &gAgdcCapReplies);
        VIOLOG("gpu capability asked -> pci %p, maxFramebuffers %u, portMask %#llx [%s]",
               fPci, c->maxFramebuffers, (unsigned long long)c->portMask, gAgdcFBMap);
        return kIOReturnSuccess;
    }
    if (cmd == kAgdcCmdLinkConfig) {
        /* AGDCLinkConfig_t, 176 bytes, in and out. Log the caller's `id` (the
           framebuffer/pipe index) whatever we answer: it says which slot
           WindowServer is wiring up. */
        const unsigned long want = sizeof(struct VIOAGDCLinkConfig);
        if (!out || !outSize || *outSize != want || !in || inSize != want) {
            OSAddAtomic64(1, &gAgdcUnsupported); gAgdcLastRefused = (int)cmd;
            snprintf(gAgdcLastCmd, sizeof gAgdcLastCmd,
                     "cmd %#x LINKCONFIG bad sizes in %lu out %lu (want %lu)",
                     cmd, inSize, out ? *outSize : 0, want);
            if (gAgdcLogged < 40) { gAgdcLogged++; VIOLOG("LINKCONFIG bad sizes (in %lu out %lu) -> BadArgument", inSize, out ? *outSize : 0); }
            return kIOReturnBadArgument;
        }
        struct VIOAGDCLinkConfig *l = (struct VIOAGDCLinkConfig *)out;
        const struct VIOAGDCLinkConfig *r = (const struct VIOAGDCLinkConfig *)in;
        const int32_t asked = r->id;
        const int32_t askedGroup = r->groupID;
        const uint64_t askedFlags = r->flags;
        memcpy(out, in, want);
        snprintf(gAgdcLastCmd, sizeof gAgdcLastCmd,
                 "cmd %#x LINKCONFIG id %d group %d flags %#llx -> %s",
                 cmd, asked, askedGroup, (unsigned long long)askedFlags, gAgdcLink ? "answered" : "refused");
        if (gAgdcLink) {
            vioAgdcFillLink(l, 1u + (asked >= 0 ? (uint32_t)asked : 0u));
            OSAddAtomic64(1, &gAgdcLinkAnswered);
            if (gAgdcLinkAnswered <= 8)
                VIOLOG("LINKCONFIG id %d -> answered (h %ux%u %llu Hz, port %u) [%lld]",
                       asked, l->timing.hActive, l->timing.vActive, l->timing.pixelClock, l->port, gAgdcLinkAnswered);
            return kIOReturnSuccess;
        }
        OSAddAtomic64(1, &gAgdcUnsupported); gAgdcLastRefused = (int)cmd;
        if (gAgdcLogged < 40) { gAgdcLogged++; VIOLOG("LINKCONFIG id %d -> Unsupported (debug.vioagdc_link=0) [%d/40]", asked, gAgdcLogged); }
        return kIOReturnUnsupported;
    }
    if (cmd == kAgdcCmdFbExtCap) {
        /* 0x711 kAGDCFBGetExtendedCapability: the caller sets fbIndex+type, we
           fill the entry. The types and field offsets are the ones
           AppleParavirtFramebuffer answers (vioAgdcFillFbExtCap); 0x10 is the
           detailed timing and has its own filler. */
        const unsigned long want = sizeof(struct VIOAGDCFBExtCap);
        if (!out || !outSize || *outSize != want || !in || inSize != want) {
            OSAddAtomic64(1, &gAgdcUnsupported); gAgdcLastRefused = (int)cmd;
            snprintf(gAgdcLastCmd, sizeof gAgdcLastCmd, "cmd %#x FBCAPEX bad sizes in %lu out %lu (want %lu)",
                     cmd, inSize, out ? *outSize : 0, want);
            if (gAgdcLogged < 40) { gAgdcLogged++; VIOLOG("FBCAPEX bad sizes (in %lu out %lu) -> BadArgument", inSize, out ? *outSize : 0); }
            return kIOReturnBadArgument;
        }
        struct VIOAGDCFBExtCap *c = (struct VIOAGDCFBExtCap *)out;
        const struct VIOAGDCFBExtCap *r = (const struct VIOAGDCFBExtCap *)in;
        const uint32_t fbIndex = r->fbIndex, type = r->type;
        memcpy(out, in, want);
        const bool scaler = (type == VIOK5_CAP_PLANESCALER);
        const bool answered = gAgdcLink && (scaler || vioAgdcFillFbExtCap(out, type));
        vioAgdcNoteFbCapType(type, answered);
        snprintf(gAgdcLastCmd, sizeof gAgdcLastCmd, "cmd %#x FBCAPEX fbIndex %u type %#x -> %s",
                 cmd, fbIndex, type, answered ? "answered" : "refused");
        if (answered) {
            if (scaler) vioAgdcFillPlaneScaler(c);
            OSAddAtomic64(1, &gAgdcFbCapAnswered);
            if (gAgdcFbCapAnswered <= 12)
                VIOLOG("FBCAPEX fbIndex %u type %#x -> answered [%lld]", fbIndex, type, gAgdcFbCapAnswered);
            return kIOReturnSuccess;
        }
        OSAddAtomic64(1, &gAgdcUnsupported); gAgdcLastRefused = (int)cmd;
        if (gAgdcLogged < 40) { gAgdcLogged++; VIOLOG("FBCAPEX fbIndex %u type %#x -> Unsupported [%d/40]", fbIndex, type, gAgdcLogged); }
        return kIOReturnUnsupported;
    }
    if (cmd == kAgdcCmdStream925) {
        /* 0x925: 12 bytes in and out, asked right after kAGDCLinkConfig (0x921)
           and before 0x711. Unknown; log the words and (in probe mode) hand the
           caller's own 12 bytes back with success. */
        unsigned long w[3] = { 0, 0, 0 }; unsigned long words = inSize / sizeof(unsigned long);
        if (in) for (unsigned long i = 0; i < 3 && i < words; i++) w[i] = in[i];
        const unsigned long outBytes = (out && outSize) ? *outSize : 0;
        snprintf(gAgdcLastCmd, sizeof gAgdcLastCmd, "cmd %#x (0x925) in %lu out %lu w= %#lx %#lx %#lx -> %s",
                 cmd, inSize, outBytes, w[0], w[1], w[2], gAgdcLink ? "echoed" : "refused");
        if (gAgdcLink) {
            if (out && in && *outSize == inSize) memcpy(out, in, inSize);
            OSAddAtomic64(1, &gAgdc925Answered);
            if (gAgdc925Answered <= 8) VIOLOG("0x925 -> echoed success (in %lu: %#lx %#lx %#lx) [%lld]", inSize, w[0], w[1], w[2], gAgdc925Answered);
            return kIOReturnSuccess;
        }
        OSAddAtomic64(1, &gAgdcUnsupported); gAgdcLastRefused = (int)cmd;
        if (gAgdcLogged < 40) { gAgdcLogged++; VIOLOG("0x925 (in %lu out %lu) w %#lx %#lx %#lx -> Unsupported [%d/40]", inSize, outBytes, w[0], w[1], w[2], gAgdcLogged); }
        return kIOReturnUnsupported;
    }
    OSAddAtomic64(1, &gAgdcUnsupported); gAgdcLastRefused = (int)cmd;
    unsigned long w[4] = { 0, 0, 0, 0 };
    if (in && inSize) { unsigned long n = inSize > 4 ? 4 : inSize; for (unsigned long i = 0; i < n; i++) w[i] = in[i]; }
    const int pid = proc_selfpid();
    char pname[64]; pname[0] = 0; proc_name(pid, pname, (int)sizeof pname);
    const unsigned long outBytes = (out && outSize) ? *outSize : 0;
    snprintf(gAgdcLastCmd, sizeof gAgdcLastCmd,
             "cmd %#x in %lu out %lu from %s[%d] in[]= %#lx %#lx %#lx %#lx",
             cmd, inSize, outBytes, pname, pid, w[0], w[1], w[2], w[3]);
    if (gAgdcLogged < 40) {
        gAgdcLogged++;
        VIOLOG("command %#x (in %lu bytes, out %lu) from %s[%d] w0 %#lx -> Unsupported [%d/40]",
               cmd, inSize, outBytes, pname, pid, w[0], gAgdcLogged);
    }
    if (gAgdcYes == 2 || (gAgdcYes == 1 && cmd == kAgdcCmdDisplayPipe)) {
        if (out && outBytes) bzero(out, outBytes);
        OSAddAtomic64(1, &gAgdcYesAnswered);
        if (gAgdcYesAnswered <= 8) VIOLOG("probe: answered cmd %#x with success (%lu out bytes zeroed)", cmd, outBytes);
        return kIOReturnSuccess;
    }
    return kIOReturnUnsupported;
}

static int gAgdcState = 0; static VIOAGDCDeviceControl *gAgdc = nullptr; static thread_call_t gAgdcCall = nullptr; static int gAgdcWant = 0;
static IOService *gAgdcFBRef = nullptr;

static void vioAgdcDropTable(void)
{
    unsigned n = gAgdcNFB; gAgdcNFB = 0;
    __asm__ __volatile__("" ::: "memory");
    for (unsigned i = 0; i < kAgdcMaxFB; i++) { IOService *f = gAgdcFB[i]; gAgdcFB[i] = nullptr; if (f) f->release(); }
    if (n) snprintf(gAgdcFBMap, sizeof gAgdcFBMap, "released (the last table had %u slot(s))", n);
}
static void vioAgdcDropFB(void) { if (gAgdcFBRef) { gAgdcFBRef->release(); gAgdcFBRef = nullptr; } vioAgdcDropTable(); }

static void vioAgdcTargets(IOService **pci, IOService **fb)
{
    *pci = nullptr; *fb = nullptr;
    OSDictionary *m = IOService::serviceMatching(kFramebufferClass); if (!m) return;
    IOService *f = IOService::copyMatchingService(m); m->release();
    if (!f) {
        /* A boot-arg worker can run from inside the framebuffer's start(),
           before copyMatchingService() exposes the new object. The framebuffer
           publishes this pointer immediately before calling vioAgdcInstall(). */
        f = vioFramebufferService();
        if (f) f->retain();
    }
    if (!f) return;
    IOService *p = f->getProvider(); int hops = 0;
    while (p && !p->metaCast("IOPCIDevice") && hops++ < 8) p = p->getProvider();
    if (!p || !p->metaCast("IOPCIDevice")) { f->release(); return; }
    gAgdcFBRef = f; *fb = f; *pci = p;
}

static int gAgdcScanLogged = 0;
#define SCANLOG(fmt, ...) do { if (gAgdcScanLogged < 8) { gAgdcScanLogged++; VIOLOG("scan: " fmt " [%d/8]", ##__VA_ARGS__, gAgdcScanLogged); } } while (0)

/* Same rule as the reference: slot i holds the framebuffer whose
   IOFBDependentIndex is i, and only framebuffers on OUR PCI device count. */
static void vioAgdcScan(IOService *pci)
{
    vioAgdcDropTable(); gAgdcScanLogged = 0;
    IOService *tab[kAgdcMaxFB] = {}; unsigned seen = 0, noidx = 0, range = 0, dup = 0, other = 0, hole = 0;
    OSDictionary *m = IOService::serviceMatching(kFramebufferClass);
    OSIterator *it = m ? IOService::getMatchingServices(m) : nullptr; OSSafeReleaseNULL(m);
    if (!it) { snprintf(gAgdcFBMap, sizeof gAgdcFBMap, "n=0 (no iterator: the earlier one-head reply)"); VIOLOG("slot table %s", gAgdcFBMap); return; }
    OSObject *o;
    while (seen < 16 && (o = it->getNextObject())) {
        seen++;
        IOService *f = OSDynamicCast(IOService, o); if (!f) continue;
        unsigned long long rid = f->getRegistryEntryID();
        IOService *p = f->getProvider(); int hops = 0;
        while (p && !p->metaCast("IOPCIDevice") && hops++ < 8) p = p->getProvider();
        if (p != pci) { other++; SCANLOG("framebuffer id %#llx is under another PCI device: not ours to report", rid); continue; }
        OSObject *io = f->copyProperty("IOFBDependentIndex"); OSNumber *num = OSDynamicCast(OSNumber, io);
        long long ix = num ? (long long)num->unsigned32BitValue() : -1; OSSafeReleaseNULL(io);
        if (ix < 0) { noidx++; SCANLOG("framebuffer id %#llx has no IOFBDependentIndex number: left out", rid); continue; }
        if (ix >= kAgdcMaxFB) { range++; SCANLOG("framebuffer id %#llx IOFBDependentIndex %lld >= %d: left out", rid, ix, (int)kAgdcMaxFB); continue; }
        if (tab[ix]) { dup++; SCANLOG("IOFBDependentIndex %lld claimed twice: id %#llx kept, id %#llx left out", ix, (unsigned long long)tab[ix]->getRegistryEntryID(), rid); continue; }
        f->retain(); tab[ix] = f;
    }
    it->release();
    unsigned n = 0; while (n < kAgdcMaxFB && tab[n]) n++;
    for (unsigned i = n; i < kAgdcMaxFB; i++) if (tab[i]) {
        hole++; SCANLOG("slot %u (id %#llx) sits beyond a hole at slot %u: left out", i, (unsigned long long)tab[i]->getRegistryEntryID(), n);
        tab[i]->release(); tab[i] = nullptr;
    }
    for (unsigned i = 0; i < n; i++) gAgdcFB[i] = tab[i];
    gAgdcNFB = n;
    int w = snprintf(gAgdcFBMap, sizeof gAgdcFBMap, "n=%u", n);
    for (unsigned i = 0; i < n && w > 0 && (unsigned)w < sizeof gAgdcFBMap; i++)
        w += snprintf(gAgdcFBMap + w, sizeof gAgdcFBMap - (size_t)w, " [%u]id %#llx", i, (unsigned long long)gAgdcFB[i]->getRegistryEntryID());
    if (w > 0 && (unsigned)w < sizeof gAgdcFBMap && (noidx | range | dup | other | hole))
        snprintf(gAgdcFBMap + w, sizeof gAgdcFBMap - (size_t)w, " (left out: noidx %u range %u dup %u gpu %u hole %u)", noidx, range, dup, other, hole);
    VIOLOG("slot table %s (%u %s node(s) seen)", gAgdcFBMap, seen, kFramebufferClass);
}

static SInt32 gAgdcBusy = 0;
static void vioAgdcWorkBody(void);

static void vioAgdcWork(thread_call_param_t, thread_call_param_t)
{
    OSIncrementAtomic(&gAgdcBusy); vioAgdcWorkBody(); OSDecrementAtomic(&gAgdcBusy);
}

static void vioAgdcWorkBody(void)
{
    if (gAgdcWant == 1 && !gAgdc) {
        IOService *pci = nullptr, *fb = nullptr; vioAgdcTargets(&pci, &fb);
        if (!pci || !fb) { gAgdcState = -1; VIOLOG("no PCI device or no %s yet (pci %p fb %p)", kFramebufferClass, pci, fb); return; }
        vioAgdcScan(pci);
        VIOAGDCDeviceControl *d = OSTypeAlloc(VIOAGDCDeviceControl);
        if (!d) { gAgdcState = -2; VIOLOG("alloc failed"); vioAgdcDropFB(); return; }
        if (!d->init(nullptr)) { gAgdcState = -3; VIOLOG("init failed"); d->release(); vioAgdcDropFB(); return; }
        d->fPci = pci; d->fFB = fb;
        if (!d->attach(pci)) { gAgdcState = -4; VIOLOG("attach(%s) failed", pci->getName()); d->release(); vioAgdcDropFB(); return; }
        if (!d->start(pci)) { gAgdcState = -5; VIOLOG("AppleGraphicsDeviceControl::start failed (vendor calls so far %lld)", gAgdcCmds); d->detach(pci); d->release(); vioAgdcDropFB(); return; }
        gAgdc = d; gAgdcState = 1;
        VIOLOG("UP on %s -- %u framebuffer slot(s), vendor calls %lld, refused %lld (last %#x)",
               pci->getName(), gAgdcNFB, gAgdcCmds, gAgdcUnsupported, gAgdcLastRefused);
    } else if (gAgdcWant == 0 && gAgdc) {
        VIOAGDCDeviceControl *d = gAgdc; gAgdc = nullptr;
        bool ok = d->terminate(kIOServiceRequired | kIOServiceSynchronous); d->release(); vioAgdcDropFB(); gAgdcState = 2;
        VIOLOG("terminate -> %d (a reboot is the certain way back; this is the polite one)", ok);
    }
}

static int vioagdc_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gAgdcState; int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v != 0 && v != 1) return EINVAL;
    gAgdcWant = v;
    if (!gAgdcCall) gAgdcCall = thread_call_allocate(vioAgdcWork, nullptr);
    if (!gAgdcCall) return ENOMEM;
    thread_call_enter(gAgdcCall); return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, vioagdc, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0, vioagdc_sysctl, "I",
            "write 1 = create our AppleGraphicsDeviceControl object, 0 = terminate it; reads the state (1 UP, <0 failed step)");
SYSCTL_QUAD(_debug, OID_AUTO, vioagdc_cmds,    CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcCmds,        "AGDC vendor calls received");
SYSCTL_QUAD(_debug, OID_AUTO, vioagdc_cap_replies, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcCapReplies, "kAGDCGPUCapability replies handed out");
SYSCTL_QUAD(_debug, OID_AUTO, vioagdc_refused, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcUnsupported, "AGDC vendor calls answered Unsupported");
SYSCTL_INT (_debug, OID_AUTO, vioagdc_last_refused, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcLastRefused, 0, "last AGDC command refused");
SYSCTL_STRING(_debug, OID_AUTO, vioagdc_fbmap, CTLFLAG_RD | CTLFLAG_LOCKED, gAgdcFBMap, sizeof gAgdcFBMap, "the slot table our 0x980 reports: n, then [i]id per IOFBDependentIndex");
SYSCTL_INT (_debug, OID_AUTO, vioagdc_maxfb,   CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcLastMaxFB, 0, "maxFramebuffers in our last 0x980 reply (-1 = never asked)");
SYSCTL_INT (_debug, OID_AUTO, vioagdc_yes, CTLFLAG_RW | CTLFLAG_LOCKED, &gAgdcYes, 0, "probe: 0 off, 1 answer cmd 0x2001 kIOReturnSuccess, 2 answer every unhandled command success");
SYSCTL_QUAD(_debug, OID_AUTO, vioagdc_yes_answered, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcYesAnswered, "commands the vioagdc_yes probe answered with success");
SYSCTL_STRING(_debug, OID_AUTO, vioagdc_last_cmd, CTLFLAG_RD | CTLFLAG_LOCKED, gAgdcLastCmd, sizeof gAgdcLastCmd, "detail (cmd/in/out/caller/input words) of the last unhandled AGDC command");
SYSCTL_INT (_debug, OID_AUTO, vioagdc_link, CTLFLAG_RW | CTLFLAG_LOCKED, &gAgdcLink, 0, "probe: 0 refuse kAGDCLinkConfig 0x921, 1 answer it from this port's timing");
SYSCTL_QUAD(_debug, OID_AUTO, vioagdc_link_answered, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcLinkAnswered, "kAGDCLinkConfig 0x921 requests answered");
SYSCTL_QUAD(_debug, OID_AUTO, vioagdc_fbcap_answered, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcFbCapAnswered, "kAGDCFBGetExtendedCapability 0x711 replies handed out");
SYSCTL_STRING(_debug, OID_AUTO, vioagdc_fbcap_types, CTLFLAG_RD | CTLFLAG_LOCKED, gAgdcFbCapTypes, sizeof gAgdcFbCapTypes, "0x711 types asked for: A answered, R refused");
SYSCTL_QUAD(_debug, OID_AUTO, vioagdc_925_answered, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdc925Answered, "0x925 requests echoed with success under vioagdc_link");

static void vioAgdcRemoveSysctl(void)
{
    sysctl_unregister_oid(&sysctl__debug_vioagdc);
    sysctl_unregister_oid(&sysctl__debug_vioagdc_cmds);
    sysctl_unregister_oid(&sysctl__debug_vioagdc_cap_replies);
    sysctl_unregister_oid(&sysctl__debug_vioagdc_refused);
    sysctl_unregister_oid(&sysctl__debug_vioagdc_last_refused);
    sysctl_unregister_oid(&sysctl__debug_vioagdc_fbmap);
    sysctl_unregister_oid(&sysctl__debug_vioagdc_maxfb);
    sysctl_unregister_oid(&sysctl__debug_vioagdc_yes);
    sysctl_unregister_oid(&sysctl__debug_vioagdc_yes_answered);
    sysctl_unregister_oid(&sysctl__debug_vioagdc_last_cmd);
    sysctl_unregister_oid(&sysctl__debug_vioagdc_link);
    sysctl_unregister_oid(&sysctl__debug_vioagdc_link_answered);
    sysctl_unregister_oid(&sysctl__debug_vioagdc_fbcap_answered);
    sysctl_unregister_oid(&sysctl__debug_vioagdc_fbcap_types);
    sysctl_unregister_oid(&sysctl__debug_vioagdc_925_answered);
}
static void vioAgdcInstallSysctl(void)
{
    sysctl_register_oid(&sysctl__debug_vioagdc);
    sysctl_register_oid(&sysctl__debug_vioagdc_cmds);
    sysctl_register_oid(&sysctl__debug_vioagdc_cap_replies);
    sysctl_register_oid(&sysctl__debug_vioagdc_refused);
    sysctl_register_oid(&sysctl__debug_vioagdc_last_refused);
    sysctl_register_oid(&sysctl__debug_vioagdc_fbmap);
    sysctl_register_oid(&sysctl__debug_vioagdc_maxfb);
    sysctl_register_oid(&sysctl__debug_vioagdc_yes);
    sysctl_register_oid(&sysctl__debug_vioagdc_yes_answered);
    sysctl_register_oid(&sysctl__debug_vioagdc_last_cmd);
    sysctl_register_oid(&sysctl__debug_vioagdc_link);
    sysctl_register_oid(&sysctl__debug_vioagdc_link_answered);
    sysctl_register_oid(&sysctl__debug_vioagdc_fbcap_answered);
    sysctl_register_oid(&sysctl__debug_vioagdc_fbcap_types);
    sysctl_register_oid(&sysctl__debug_vioagdc_925_answered);
}

/* The way in. Called from VirtIOAppleParavirtFB::start() (kexts/VirtIOGPU/VirtIOGPU.cpp)
   once the frame buffer has published, which is also the earliest moment an
   AGDC object could answer usefully.

   The first version of this shipped as a VIOAGDCLoader personality matching
   IOResources (the shape NVRMAGDC uses). That made the whole bundle fail the
   aux-KC approval check at boot -- kernelmanagerd logged

     Validate approval for /Library/Extensions/VirtIOGPU.kext in auxKC: not approved
     auxiliary kext collection did not load: Kexts in auxKC not all approved

   and then removed the aux KC and rebuilt it without us, so the GPU lost its
   driver entirely (accel 0x0, display fell back to IONDRVFramebuffer). Nothing
   about the binary was wrong: the same build with the personality deleted and
   this hook called from VirtIOAppleParavirtFB::start() is approved and loads. The approval
   gate keys on something a plain PCI-driver bundle has and an IOResources
   personality does not. */
extern "C" void vioAgdcInstall(void);
static int gAgdcInstalled = 0;
extern "C" void vioAgdcInstall(void)
{
    if (gAgdcInstalled) return;
    gAgdcInstalled = 1;
    vioAgdcInstallSysctl();

    /* As with the accelerator's display-pipe switches, XNU leaves these custom
       debug sysctls at zero even when OpenCore supplies them in boot-args.
       Read them here, before WindowServer starts asking AGDC for capabilities. */
    uint32_t on = 0;
    if (PE_parse_boot_argn("debug.vioagdc_link", &on, sizeof on) && on)
        gAgdcLink = 1;
    on = 0;
    if (PE_parse_boot_argn("debug.vioagdc", &on, sizeof on) && on) {
        gAgdcWant = 1;
        if (!gAgdcCall) gAgdcCall = thread_call_allocate(vioAgdcWork, nullptr);
        if (gAgdcCall) thread_call_enter(gAgdcCall);
        else VIOLOG("boot-arg startup: thread_call_allocate failed");
    }
    VIOLOG("hook installed by VirtIOAppleParavirtFB::start; want=%d link=%d", gAgdcWant, gAgdcLink);
}

/* Kept only as the reference shape for the separate-bundle deployment: no
   personality names it any more, so it is dead code in this build. */
class VIOAGDCLoader : public IOService {
    OSDeclareDefaultStructors(VIOAGDCLoader)
public:
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(VIOAGDCLoader, IOService)

bool VIOAGDCLoader::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    vioAgdcInstallSysctl();
    VIOLOG("loaded; idle until `sysctl -w debug.vioagdc=1`");
    return true;
}

void VIOAGDCLoader::stop(IOService *provider)
{
    vioAgdcRemoveSysctl();
    if (gAgdcCall) {
        thread_call_cancel(gAgdcCall); int spins = 0;
        while (gAgdcBusy && spins++ < 200) IOSleep(10);
        if (gAgdcBusy) VIOLOG("stop: the worker is STILL running after 2 s - leaving its thread call allocated rather than freeing it under it");
        else { thread_call_free(gAgdcCall); gAgdcCall = nullptr; }
    }
    if (gAgdc && !gAgdcBusy) { gAgdcWant = 0; vioAgdcWorkBody(); }
    IOService::stop(provider);
}
