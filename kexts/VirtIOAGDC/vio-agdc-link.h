/*
 * VirtIOAGDC — the AGDC display-link ABI (AGDCLinkConfig_t), and the one timing
 * this port has to give it.
 *
 * Why this file exists (measured; tmp/RESEARCH-PLAN.md section G.7): once the
 * presentment object creation succeeds, WindowServer immediately asks the AGDC
 * for the framebuffer's *link configuration* — AGDC command 0x921,
 * kAGDCLinkConfig, a 176-byte (0xb0) AGDCLinkConfig_t in and out. That is the
 * AGDC acting as the authority on the display link: the answer is what tells
 * the window server the display pipe is wired to this GPU. Refusing 0x921 is
 * the last thing standing between "presentment object created" and an
 * accelerator-backed display.
 *
 * The struct layout is Apple's, shared by every AGDC vendor; the definition
 * here mirrors the reconstructed copy in kexts/NVRMAGDC/nvrm-agdc-k5.h (same
 * _Static_assert sizes), because both drivers answer the same base class. Only
 * the *timing* differs per hardware.
 *
 * virtio-gpu has no mode-setting hardware and no EDID: QEMU owns the 2D
 * scanout and the mode is fixed at 1280x800@60. So the timing handed back is
 * exactly the one VirtIOFB::getTimingInfoForDisplayMode() already publishes to
 * CoreDisplay (CEA-style 1280x800@60, 83.5 MHz, 1440x823) — the AGDC's link
 * config and the framebuffer's mode then agree, which is the property
 * WindowServer actually checks.
 */

#ifndef VIO_AGDC_LINK_H
#define VIO_AGDC_LINK_H
#include <stdint.h>

#define VIOK5_CMD_LINKCONFIG 0x921u   /* kAGDCLinkConfig       */
#define VIOK5_CMD_FBCAPEX    0x711u   /* kAGDCFBGetExtendedCapability */
#define VIOK5_CAP_PLANESCALER 0x10u   /* c->type for the plane-scaler entry */

#pragma pack(push, 1)
/* AGDCTiming_t (0x94): the display timing block inside a link config. */
struct VIOAGDCTiming {
    uint32_t hScaledInset, vScaledInset, scalerFlags, hScaled, vScaled, signalConfig, signalLevels;
    uint64_t pixelClock, minPixelClock, maxPixelClock;
    uint32_t hActive, hBlanking, hSyncOffset, hSyncPulseWidth;
    uint32_t vActive, vBlanking, vSyncOffset, vSyncPulseWidth;
    uint32_t hBorderLeft, hBorderRight, vBorderTop, vBorderBottom;
    uint32_t hSyncConfig, hSyncLevel, vSyncConfig, vSyncLevel;
    uint32_t numLinks, vBlankingExtension;
    uint16_t pixelEncoding, bitsPerColorComponent, colorimetry, dynamicRange;
    uint16_t dscBitsPerPixel, dscSliceHeight, dscSliceWidth, field8a;
    uint8_t  tail[8];
};
/* AGDCLinkConfig_t (0xb0). The caller fills in `id` (the framebuffer/pipe
   index); the AGDC fills in everything else. */
struct VIOAGDCLinkConfig {
    int32_t  id;
    int32_t  groupID;
    uint64_t flags;
    struct VIOAGDCTiming timing;
    uint32_t port, stream;
    uint32_t state;
};
/* AGDCFBGetExtendedCapability_t (0x196c=6508): the caller sets fbIndex+type,
   the AGDC fills count+entry[]. type 0x10 is the plane scaler. */
struct VIOAGDCFBExtCap {
    uint32_t fbIndex, type, count;
    uint8_t  entry[8][0x32c];
};
#pragma pack(pop)
static_assert(sizeof(struct VIOAGDCTiming) == 0x94, "AGDC timing is 0x94");
static_assert(sizeof(struct VIOAGDCLinkConfig) == 0xb0, "AGDCLinkConfig_t is 0xb0");
static_assert(__builtin_offsetof(struct VIOAGDCLinkConfig, port) == 0xa4, "stream address at +0xa4");
static_assert(sizeof(struct VIOAGDCFBExtCap) == 0x196c, "AGDCFBGetExtendedCapability_t is 0x196c");

/* The mode VirtIOFB reports: CEA-style 1280x800@60, 83.5 MHz, 1440x823.
   Keep in lockstep with VirtIOFB::getTimingInfoForDisplayMode() — if the two
   ever disagree, WindowServer sees an AGDC link that does not match the
   framebuffer's mode and the display pipe stays unassociated. */
enum {
    VIO_TIMING_H_ACTIVE = 1280,
    VIO_TIMING_H_BLANKING = 160,   /* hTotal 1440 */
    VIO_TIMING_H_SYNC_OFFSET = 48,
    VIO_TIMING_H_SYNC_PULSE = 32,
    VIO_TIMING_V_ACTIVE = 800,
    VIO_TIMING_V_BLANKING = 23,    /* vTotal 823 */
    VIO_TIMING_V_SYNC_OFFSET = 3,
    VIO_TIMING_V_SYNC_PULSE = 6,
    VIO_TIMING_REFRESH_HZ = 60,
};
#define VIO_TIMING_PIXEL_CLOCK 83500000ull

static inline void vioAgdcFillLink(struct VIOAGDCLinkConfig *l, uint32_t port)
{
    const int32_t id = l->id;
    uint8_t *b = (uint8_t *)l;
    for (unsigned long i = 0; i < sizeof *l; i++) b[i] = 0;   /* keep only id */
    l->id = id;
    l->groupID = -1;
    l->flags = 0x8 | 0x1 | 0x2;                 /* the reference's link flags */
    struct VIOAGDCTiming *g = &l->timing;
    g->pixelClock = g->minPixelClock = g->maxPixelClock = VIO_TIMING_PIXEL_CLOCK;
    g->hActive = VIO_TIMING_H_ACTIVE;  g->hBlanking = VIO_TIMING_H_BLANKING;
    g->hSyncOffset = VIO_TIMING_H_SYNC_OFFSET; g->hSyncPulseWidth = VIO_TIMING_H_SYNC_PULSE;
    g->vActive = VIO_TIMING_V_ACTIVE;  g->vBlanking = VIO_TIMING_V_BLANKING;
    g->vSyncOffset = VIO_TIMING_V_SYNC_OFFSET; g->vSyncPulseWidth = VIO_TIMING_V_SYNC_PULSE;
    g->hSyncConfig = 1; g->vSyncConfig = 1;
    g->numLinks = 1;
    g->pixelEncoding = 0x0001;                  /* RGB */
    g->bitsPerColorComponent = 0x0002;          /* 8bpc, reference encoding */
    g->colorimetry = 0x0001; g->dynamicRange = 0x0001;
    l->port = port; l->stream = 0;
    l->state = 1;                               /* configured */
}

/* One DWORD into a byte-addressed extended-capability entry. */
static inline void vioAgdcPut32(uint8_t *e, unsigned off, uint32_t v)
{
    e[off] = (uint8_t)v; e[off + 1] = (uint8_t)(v >> 8);
    e[off + 2] = (uint8_t)(v >> 16); e[off + 3] = (uint8_t)(v >> 24);
}

/* Plane-scaler capability, same layout the reference fills (nvk5_fill_planescaler):
   one entry describing a scaler that can produce the display's active area. */
static inline void vioAgdcFillPlaneScaler(struct VIOAGDCFBExtCap *c)
{
    const uint32_t idx = c->fbIndex, type = c->type;
    uint8_t *b = (uint8_t *)c;
    for (unsigned long i = 0; i < sizeof *c; i++) b[i] = 0;   /* keep fbIndex+type */
    c->fbIndex = idx; c->type = type;
    c->count = 1;
    uint8_t *e = c->entry[0];
    const uint32_t w = VIO_TIMING_H_ACTIVE, h = VIO_TIMING_V_ACTIVE;
    vioAgdcPut32(e, 0x000, 0x80004);
    vioAgdcPut32(e, 0x004, 0);
    vioAgdcPut32(e, 0x01c, 1);
    vioAgdcPut32(e, 0x024, h); vioAgdcPut32(e, 0x028, w); vioAgdcPut32(e, 0x02c, h); vioAgdcPut32(e, 0x030, w);
    vioAgdcPut32(e, 0x034, 0x70007);
    vioAgdcPut32(e, 0x038, 0);
    vioAgdcPut32(e, 0x1a4, 1);
    vioAgdcPut32(e, 0x1ac, h); vioAgdcPut32(e, 0x1b0, w); vioAgdcPut32(e, 0x1b4, h); vioAgdcPut32(e, 0x1b8, w);
    vioAgdcPut32(e, 0x1bc, 0x70007);
    vioAgdcPut32(e, 0x1c0, 0);
}
#endif
