/*
 * VIOAccelProbe — ABI gate for the VirtIOAccel personality on this OS build.
 *
 * The reconstructed IOGraphicsAccelerator2/IOAccelerator headers
 * (kexts/NVRM/accel/{re,iofam}) were measured on macOS 15. This class reads
 * the LIVE metaclass instance sizes on whatever OS it loads into; VirtIOAccel
 * is only safe if they match the header constants (IOAccelerator 136,
 * IOGraphicsAccelerator2 0xdd8/3544).
 *
 * It ships inside VirtIOGPU.kext (already user-approved) as a second
 * personality with its own IOMatchCategory and a score BELOW VirtIOGPU's,
 * logs from probe(), and returns NULL so it never claims anything.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include "IOAccelerator.h"
#include "IOGraphicsAccelerator2.h"

#define VPROBE_LOG(fmt, ...) do { kprintf("vioaccel-probe: " fmt "\n", ##__VA_ARGS__); IOLog("vioaccel-probe: " fmt "\n", ##__VA_ARGS__); } while (0)

class VIOAccelProbe : public IOService {
    OSDeclareDefaultStructors(VIOAccelProbe)
public:
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(VIOAccelProbe, IOService)

IOService *VIOAccelProbe::probe(IOService *provider, SInt32 *score)
{
    (void)score;
    if (!OSDynamicCast(IOPCIDevice, provider)) return NULL;
    const unsigned acc  = (unsigned)IOAccelerator::metaClass->getClassSize();
    const unsigned gfx2 = (unsigned)IOGraphicsAccelerator2::metaClass->getClassSize();
    VPROBE_LOG("live IOAccelerator instance %u B (header expects %u)",
               acc, (unsigned)kIOAcceleratorSize);
    VPROBE_LOG("live IOGraphicsAccelerator2 instance %u B / 0x%x (header expects 0x%x)",
               gfx2, gfx2, (unsigned)kIOGraphicsAccelerator2Size);
    VPROBE_LOG("%s",
               (acc == kIOAcceleratorSize && gfx2 == kIOGraphicsAccelerator2Size)
                   ? "MATCH — the reconstructed layout holds on this OS"
                   : "MISMATCH — the VirtIOAccel personality must NOT start on this OS build");
    return NULL;
}
