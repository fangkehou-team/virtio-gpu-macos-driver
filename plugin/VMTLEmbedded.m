/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#import <objc/runtime.h>

#import <os/lock.h>

#define VMTL_RESID_SAMPLER (1ull << 63)
#define VMTL_RESID_HEAP    (1ull << 62)

_Static_assert(sizeof(uintptr_t) == sizeof(uint64_t), "resource ID keys require 64-bit pointers");
static NSMapTable *vmtl_resid_map;
static os_unfair_lock vmtl_resid_lock = OS_UNFAIR_LOCK_INIT;
static uint64_t vmtl_resid_seq;

static uint64_t vmtl_resid_assign(id resource, uint64_t tag, uint32_t heapSlot)
{
    if (!resource) return 0;
    os_unfair_lock_lock(&vmtl_resid_lock);
    if (!vmtl_resid_map) vmtl_resid_map = [[NSMapTable alloc]
        initWithKeyOptions:NSPointerFunctionsOpaqueMemory | NSPointerFunctionsIntegerPersonality
        valueOptions:NSPointerFunctionsWeakMemory capacity:0];
    uint64_t token = heapSlot ? (VMTL_RESID_HEAP | heapSlot) : ((++vmtl_resid_seq) | tag);
    [vmtl_resid_map setObject:resource forKey:(__bridge id)(void *)(uintptr_t)token];
    uint64_t censusIssued = 0; NSUInteger censusLive = 0; BOOL doCensus = ((vmtl_resid_seq & 0xFFF) == 0);
    if (doCensus) { censusIssued = vmtl_resid_seq & ~VMTL_RESID_SAMPLER; censusLive = vmtl_resid_map.count; }
    os_unfair_lock_unlock(&vmtl_resid_lock);
    if (doCensus)
        nvlog("resid census: %llu handles issued, %lu still live in the table",
              (unsigned long long)censusIssued, (unsigned long)censusLive);
    return token;
}

static id vmtl_resid_lookup(uint64_t token)
{
    if (!token) return nil;
    os_unfair_lock_lock(&vmtl_resid_lock);
    id resource = [vmtl_resid_map objectForKey:(__bridge id)(void *)(uintptr_t)token];
    os_unfair_lock_unlock(&vmtl_resid_lock);
    return resource;
}

uint64_t vmtl_resid_for_texture(id t) { return vmtl_resid_assign(t, 0, vmtl_vk_bindless_put([(VMTLTexture *)t nvview])); }
uint64_t vmtl_resid_for_sampler(id s) { return vmtl_resid_assign(s, VMTL_RESID_SAMPLER, 0); }
void vmtl_resid_texture_gone(uint64_t token) { if (token & VMTL_RESID_HEAP) vmtl_vk_bindless_drop((uint32_t)(token & 0xffffffffu)); }

static NSUInteger vmtl_ss_enum(NSDictionary *st, NSString *key, NSDictionary<NSString *, NSNumber *> *map, NSUInteger fallback, BOOL *bad)
{
    id v = st[key];
    if (![v isKindOfClass:[NSString class]]) return fallback;
    NSNumber *n = map[v];
    if (!n) { *bad = YES; nvlog("static sampler: %s = %s has no Metal equivalent here", key.UTF8String, [v UTF8String]); return fallback; }
    return n.unsignedIntegerValue;
}
static id<MTLSamplerState> vmtl_static_sampler_from(NSDictionary *st)
{
    NSDictionary *filt = @{ @"Nearest": @(MTLSamplerMinMagFilterNearest), @"Linear": @(MTLSamplerMinMagFilterLinear) };
    NSDictionary *mip = @{ @"None": @(MTLSamplerMipFilterNotMipmapped), @"Nearest": @(MTLSamplerMipFilterNearest),
                           @"Linear": @(MTLSamplerMipFilterLinear) };
    NSDictionary *addr = @{ @"ClampToEdge": @(MTLSamplerAddressModeClampToEdge), @"Repeat": @(MTLSamplerAddressModeRepeat),
                            @"MirroredRepeat": @(MTLSamplerAddressModeMirrorRepeat), @"MirrorRepeat": @(MTLSamplerAddressModeMirrorRepeat), @"ClampToZero": @(MTLSamplerAddressModeClampToZero),
                            @"ClampToBorder": @(MTLSamplerAddressModeClampToBorderColor),
                            @"MirrorClampToEdge": @(MTLSamplerAddressModeMirrorClampToEdge) };
    NSDictionary *cmp = @{ @"Never": @(MTLCompareFunctionNever), @"Less": @(MTLCompareFunctionLess), @"Equal": @(MTLCompareFunctionEqual),
                           @"LessEqual": @(MTLCompareFunctionLessEqual), @"Greater": @(MTLCompareFunctionGreater),
                           @"NotEqual": @(MTLCompareFunctionNotEqual), @"GreaterEqual": @(MTLCompareFunctionGreaterEqual),
                           @"Always": @(MTLCompareFunctionAlways), @"None": @(MTLCompareFunctionNever) };
    NSDictionary *border = @{ @"TransparentBlack": @(MTLSamplerBorderColorTransparentBlack), @"OpaqueBlack": @(MTLSamplerBorderColorOpaqueBlack),
                              @"OpaqueWhite": @(MTLSamplerBorderColorOpaqueWhite) };
    BOOL bad = NO;
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.minFilter = (MTLSamplerMinMagFilter)vmtl_ss_enum(st, @"min_filter", filt, MTLSamplerMinMagFilterNearest, &bad);
    d.magFilter = (MTLSamplerMinMagFilter)vmtl_ss_enum(st, @"mag_filter", filt, MTLSamplerMinMagFilterNearest, &bad);
    d.mipFilter = (MTLSamplerMipFilter)vmtl_ss_enum(st, @"mip_filter", mip, MTLSamplerMipFilterNotMipmapped, &bad);
    d.sAddressMode = (MTLSamplerAddressMode)vmtl_ss_enum(st, @"address_mode_s", addr, MTLSamplerAddressModeClampToEdge, &bad);
    d.tAddressMode = (MTLSamplerAddressMode)vmtl_ss_enum(st, @"address_mode_t", addr, MTLSamplerAddressModeClampToEdge, &bad);
    d.rAddressMode = (MTLSamplerAddressMode)vmtl_ss_enum(st, @"address_mode_r", addr, MTLSamplerAddressModeClampToEdge, &bad);
    d.compareFunction = (MTLCompareFunction)vmtl_ss_enum(st, @"compare_function", cmp, MTLCompareFunctionNever, &bad);
    d.borderColor = (MTLSamplerBorderColor)vmtl_ss_enum(st, @"border_color", border, MTLSamplerBorderColorTransparentBlack, &bad);
    d.normalizedCoordinates = ![st[@"coordinates"] isEqual:@"Pixel"];
    NSNumber *an = st[@"max_anisotropy"], *lo = st[@"lod_min_clamp"], *hi = st[@"lod_max_clamp"];
    if ([an isKindOfClass:[NSNumber class]]) d.maxAnisotropy = MAX(1u, an.unsignedIntegerValue);
    if ([lo isKindOfClass:[NSNumber class]]) d.lodMinClamp = lo.floatValue;
    if ([hi isKindOfClass:[NSNumber class]]) d.lodMaxClamp = hi.floatValue;
    if (bad) return nil;
    return [MTLCreateSystemDefaultDevice() newSamplerStateWithDescriptor:d];
}
static const void *VMTL_SSAMP_KEY = &VMTL_SSAMP_KEY;
static NSArray<NSArray *> *vmtl_static_samplers(VMTLFunction *fn)
{
    NSArray *cached = objc_getAssociatedObject(fn, VMTL_SSAMP_KEY);
    if (cached) return cached;
    NSMutableArray *out = [NSMutableArray new];
    @synchronized (fn) {
        cached = objc_getAssociatedObject(fn, VMTL_SSAMP_KEY);
        if (cached) return cached;
        char e[512] = { 0 };
        NSData *d = (fn->_air && fn->_stage.length && vmtl_xlate_ready() && xlate_refl)
                  ? vmtl_reflect_air_cached(fn, fn->_air, fn->_stage, e, sizeof e) : nil;
        NSDictionary *refl = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:NULL] : nil;
        NSArray *all = [refl isKindOfClass:[NSDictionary class]] ? refl[@"bindings"] : nil;
        for (NSDictionary *b in [all isKindOfClass:[NSArray class]] ? all : @[]) {
            if (![b isKindOfClass:[NSDictionary class]] || ![b[@"kind"] isEqual:@"StaticSampler"]) continue;
            NSNumber *slot = b[@"metal_index"]; NSDictionary *st = b[@"static_sampler"];
            if (![slot isKindOfClass:[NSNumber class]] || ![st isKindOfClass:[NSDictionary class]]) continue;
            id<MTLSamplerState> s = vmtl_static_sampler_from(st);
            if (s) [out addObject:@[slot, s]];
            else nvlog("static sampler: %s slot %@ NOT created - it keeps the dummy sampler", fn->_fname.UTF8String, slot);
        }
        if (out.count) nvlog("static sampler: %s binds %lu constexpr sampler(s) with their own state", fn->_fname.UTF8String, (unsigned long)out.count);
        objc_setAssociatedObject(fn, VMTL_SSAMP_KEY, out, OBJC_ASSOCIATION_RETAIN);
    }
    return out;
}
static int vmtl_static_samplers_on(void)
{
    static int on = -1;
    if (on < 0) { on = !getenv("VMTL_NO_STATIC_SAMPLERS");
        nvlog("static sampler: constexpr samplers %s (VMTL_NO_STATIC_SAMPLERS=%s)", on ? "bind their own state" : "keep the dummy sampler (OLD)",
              getenv("VMTL_NO_STATIC_SAMPLERS") ?: "unset"); }
    return on;
}

@interface VMTLEmbeddedPlan : NSObject {
@public
    BOOL wantSampler;
    uint32_t slot;
    NSUInteger ownerIndex, fieldOffset;
    uint32_t dset, dbinding;
    NSNumber *ownerKey;
    NSString *bindingText;
}
@end
@implementation VMTLEmbeddedPlan
@end
static char vmtl_embedded_plan_key;
static BOOL vmtl_spirv_declared(NSData *spv, uint64_t decl[2][8])
{
    memset(decl, 0, 2 * 8 * sizeof(uint64_t));
    const uint32_t *w = (const uint32_t *)spv.bytes; size_t n = spv.length / 4;
    if (!w || n < 5 || w[0] != 0x07230203u) return NO;
    uint32_t bound = w[3]; if (!bound || bound > (1u << 22)) return NO;
    int32_t *set = (int32_t *)malloc(bound * sizeof *set), *bind = (int32_t *)malloc(bound * sizeof *bind);
    if (!set || !bind) { free(set); free(bind); return NO; }
    for (uint32_t k = 0; k < bound; k++) { set[k] = -1; bind[k] = -1; }
    for (size_t i = 5; i < n; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu;
        if (!wc || i + wc > n) { free(set); free(bind); return NO; }
        if (op == 71 && wc >= 4 && w[i + 1] < bound) {
            if (w[i + 2] == 34) set[w[i + 1]] = (int32_t)w[i + 3]; else if (w[i + 2] == 33) bind[w[i + 1]] = (int32_t)w[i + 3];
        }
        i += wc;
    }
    for (uint32_t k = 0; k < bound; k++)
        if ((set[k] == 0 || set[k] == 1) && bind[k] >= 0 && bind[k] < 512) decl[set[k]][bind[k] >> 6] |= 1ull << (bind[k] & 63);
    free(set); free(bind); return YES;
}
static NSArray<VMTLEmbeddedPlan *> *vmtl_embedded_plan(VMTLFunction *fn) {
    NSArray *cached = objc_getAssociatedObject(fn, &vmtl_embedded_plan_key);
    if (cached) return cached;
    @synchronized(fn) {
        cached = objc_getAssociatedObject(fn, &vmtl_embedded_plan_key);
        if (cached) return cached;
        NSMutableArray *plans = [NSMutableArray new];
        uint64_t decl[2][8]; const BOOL haveDecl = vmtl_spirv_declared(fn->_spirv, decl); unsigned heapRouted = 0;
        for (NSDictionary *entry in vmtl_embedded_bindings(fn)) {
            NSDictionary *src = entry[@"embedded_source"];
            NSNumber *slotNum = entry[@"metal_index"];
            if (![src isKindOfClass:[NSDictionary class]] || !slotNum) continue;
            VMTLEmbeddedPlan *plan = [VMTLEmbeddedPlan new];
            plan->wantSampler = [entry[@"kind"] isEqualToString:@"EmbeddedArgBufferSampler"];
            plan->slot = (uint32_t)slotNum.unsignedLongValue;
            plan->ownerIndex = [src[@"buffer_index"] unsignedLongValue];
            plan->fieldOffset = [src[@"field_offset"] unsignedLongValue];
            plan->ownerKey = @(plan->ownerIndex);
            id binding = entry[@"descriptor"][@"binding"];
            plan->bindingText = binding ? [[binding description] copy] : @"(absent)";
            { id ds = entry[@"descriptor"][@"set"];
              plan->dset = [ds respondsToSelector:@selector(unsignedIntValue)] ? [ds unsignedIntValue] : 0;
              plan->dbinding = [binding respondsToSelector:@selector(unsignedIntValue)] ? [binding unsignedIntValue] : UINT32_MAX; }
            if (haveDecl && !plan->wantSampler && plan->dset < 2 && plan->dbinding < 512 &&
                !((decl[plan->dset][plan->dbinding >> 6] >> (plan->dbinding & 63)) & 1)) { heapRouted++; continue; }
            [plans addObject:plan];
        }
        { static _Atomic unsigned said; if (heapRouted && atomic_fetch_add(&said, 1) < 8)
            nvlog("tier2: %s reads %u argument-buffer texture(s) through the bindless heap; %lu field(s) still bound per draw",
                  fn->_fname.UTF8String, heapRouted, (unsigned long)plans.count); }
        cached = [plans copy];
        objc_setAssociatedObject(fn, &vmtl_embedded_plan_key, cached, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        return cached;
    }
}

static BOOL vmtl_embedded_info_due(_Atomic uint64_t *counter) {
    uint64_t n = atomic_fetch_add_explicit(counter, 1, memory_order_relaxed) + 1;
    return n <= 8 || (n && !(n & (n - 1)));
}
static _Atomic uint64_t vmtl_embedded_empty_count, vmtl_embedded_bound_count;
static void vmtl_clear_embedded_slot(VMTLCommandBuffer *cb, uint32_t set, uint32_t slot, BOOL sampler) {
    if (sampler) vmtl_vk_bind_parked_sampler(&cb->_c, set, slot, NULL);
    else {
        vmtl_vk_bind_texture_view(&cb->_c, set, slot, NULL);
        vmtl_vk_bind_storage_view(&cb->_c, set, slot, NULL);
    }
}
void vmtl_bind_embedded(VMTLCommandBuffer *cb, uint32_t set, VMTLFunction *fn, NSDictionary *bufferAtIndex)
{
    if (!cb || !fn) return;
    NSArray<NSArray *> *statics = vmtl_static_samplers_on() ? vmtl_static_samplers(fn) : nil;
    NSArray<VMTLEmbeddedPlan *> *embedded = vmtl_embedded_plan(fn);
    // Give the app back any sampler slot an earlier shader parked its own sampler in and this one leaves alone.
    uint32_t keep = 0;
    for (NSArray *pair in statics) { uint32_t i = [pair[0] unsignedIntValue]; if (i < 32) keep |= 1u << i; }
    for (VMTLEmbeddedPlan *plan in embedded) if (plan->wantSampler && plan->slot < 32) keep |= 1u << plan->slot;
    vmtl_vk_parked_samplers_keep(&cb->_c, set, keep);
    for (NSArray *pair in statics) {
        VMTLSamplerState *s = pair[1];
        vmtl_retain_resource(cb, s);
        if (vmtl_vk_bind_parked_sampler(&cb->_c, set, [pair[0] unsignedIntValue], &s->_s))
            nvlog("static sampler slot %@: bind FAILED", pair[0]);
    }
    if (!embedded.count) return;
    extern int vmtl_vk_emb_same(nvk_cmdbuf *, uint32_t, const void *, const uint64_t *, uint32_t);
    extern void vmtl_vk_emb_note(nvk_cmdbuf *, uint32_t, const void *, const uint64_t *, uint32_t);
    uint64_t embKey[480]; uint32_t embN = 0; BOOL embKeyed = embedded.count * 3 <= 480, embSurf = NO;
    if (embKeyed) {
        NSNumber *lastKey = nil; NSArray *lastBound = nil;
        for (VMTLEmbeddedPlan *plan in embedded) {
            NSArray *bound = (plan->ownerKey == lastKey) ? lastBound : bufferAtIndex[plan->ownerKey];
            lastKey = plan->ownerKey; lastBound = bound;
            if (!bound) { embKeyed = NO; break; }
            VMTLBuffer *owner = bound[0]; NSUInteger base = [bound[1] unsignedLongValue]; size_t msize = owner->_b.size;
            if (!owner->_b.map || base > msize || plan->fieldOffset > msize - base || sizeof(uint64_t) > msize - base - plan->fieldOffset) { embKeyed = NO; break; }
            uint64_t h = 0; memcpy(&h, (const uint8_t *)owner->_b.map + base + plan->fieldOffset, sizeof h);
            embKey[embN++] = (uint64_t)(uintptr_t)(__bridge void *)owner; embKey[embN++] = base; embKey[embN++] = h;
        }
        if (embKeyed && vmtl_vk_emb_same(&cb->_c, set, (__bridge void *)fn, embKey, embN)) return;
    }

    for (VMTLEmbeddedPlan *plan in embedded) {
        BOOL wantSampler = plan->wantSampler;
        const char *what = wantSampler ? "sampler" : "texture";
        uint32_t slot = plan->slot;
        NSUInteger ownerIndex = plan->ownerIndex;
        NSUInteger fieldOffset = plan->fieldOffset;

        NSArray *bound = bufferAtIndex[plan->ownerKey];
        if (!bound) {
            nvlog("embedded %s slot %u: buffer(%lu) is not bound on this encoder — NOT BOUND",
                  what, slot, (unsigned long)ownerIndex);
            vmtl_clear_embedded_slot(cb, set, slot, wantSampler); continue;
        }
        VMTLBuffer *owner = bound[0];
        NSUInteger base = [bound[1] unsignedLongValue];
        size_t msize = owner->_b.size;
        if (!owner->_b.map || base > msize || fieldOffset > msize - base ||
            sizeof(uint64_t) > msize - base - fieldOffset) {
            nvlog("embedded %s slot %u: buffer(%lu) offset %lu+%lu does not fit its %zu-byte mapping — REFUSED",
                  what, slot, (unsigned long)ownerIndex, (unsigned long)base,
                  (unsigned long)fieldOffset, owner->_b.size);
            vmtl_clear_embedded_slot(cb, set, slot, wantSampler); continue;
        }
        uint64_t handle = 0;
        memcpy(&handle, (const uint8_t *)owner->_b.map + base + fieldOffset, sizeof handle);
        if (!handle) {
            if (vmtl_embedded_info_due(&vmtl_embedded_empty_count))
                nvlog("embedded %s slot %u: buffer(%lu)+%lu holds a zero handle — binding cleared",
                      what, slot, (unsigned long)ownerIndex, (unsigned long)fieldOffset);
            vmtl_clear_embedded_slot(cb, set, slot, wantSampler); continue;
        }
        id resource = vmtl_resid_lookup(handle);
        if (!resource) {
            nvlog("embedded %s slot %u: handle 0x%llx resolves to nothing — released, or never ours; REFUSED",
                  what, slot, (unsigned long long)handle);
            vmtl_clear_embedded_slot(cb, set, slot, wantSampler); continue;
        }
        if (wantSampler != ((handle & VMTL_RESID_SAMPLER) != 0)) {
            nvlog("embedded slot %u: the field wants a %s but handle 0x%llx is the other kind — REFUSED",
                  slot, what, (unsigned long long)handle);
            vmtl_clear_embedded_slot(cb, set, slot, wantSampler); continue;
        }
        if (vmtl_embedded_info_due(&vmtl_embedded_bound_count)) {
            const char *bindStr = plan->bindingText.UTF8String;
            nvlog("embedded %s slot %u -> module binding %s (buffer(%lu)+%lu, handle 0x%llx) BOUND",
                  what, slot, bindStr,
                  (unsigned long)ownerIndex, (unsigned long)fieldOffset, (unsigned long long)handle);
        }
        if (wantSampler) {
            VMTLSamplerState *s = resource;
            vmtl_retain_resource(cb, s);
            if (vmtl_vk_bind_parked_sampler(&cb->_c, set, slot, &s->_s))
                nvlog("embedded sampler slot %u: bind FAILED", slot);
        } else {
            VMTLTexture *t = resource;
            vmtl_retain_resource(cb, t);
            [t vmtlSurfaceIn];
            { VMTLTexture *sr = t->_parent ? t->_parent : t; if (sr->_surf) embSurf = YES; }
            if (vmtl_vk_bind_texture_view(&cb->_c, set, slot, [t nvview]))
                nvlog("embedded texture slot %u: bind FAILED", slot);
            vmtl_vk_bind_storage_view(&cb->_c, set, slot, t->_stex ? [t nvview] : NULL);
        }
    }
    vmtl_vk_emb_note(&cb->_c, set, (embKeyed && !embSurf) ? (__bridge void *)fn : NULL, embKey, embN);
}
