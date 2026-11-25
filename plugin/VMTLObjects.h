/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#include "vmtl_vk.h"

static inline NSMutableData *vmtl_fc_storage(NSUInteger count) {
    const NSUInteger stride = 2 * sizeof(uint32_t) + sizeof(void *) + 16;
    if (!count || count > NSUIntegerMax / stride) return nil;
    return [NSMutableData dataWithLength:count * stride];
}
@class VMTLHeap;
@class VMTLCommandBuffer;
@interface VMTLBuffer : NSObject <MTLBuffer> { @public nvk_buffer _b; MTLStorageMode _storage; VMTLHeap *_heap; NSUInteger _heapOffset;
    void *_hostPtr; size_t _hostLen; id _hostDealloc;
    BOOL _hostSyncInOnly;
    VMTLHeap *_subHeap; NSUInteger _subCharge; BOOL _aliased;
    nvk_buffer _shadow; int _syncOut; int _zshadow;
    NSUInteger _dirtyLo, _dirtyHi; } @end
#include <sched.h>
static inline void vmtl_buf_settle(VMTLBuffer *b) {
    if (!__atomic_load_n(&b->_zshadow, __ATOMIC_ACQUIRE)) return;
    int owed = 1;
    if (__atomic_compare_exchange_n(&b->_zshadow, &owed, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        if (b->_shadow.map) memset(b->_shadow.map, 0, b->_shadow.size);
        __atomic_store_n(&b->_zshadow, 0, __ATOMIC_RELEASE); return; }
    while (__atomic_load_n(&b->_zshadow, __ATOMIC_ACQUIRE) == 2) sched_yield();
}
static inline void *vmtl_buf_cpu(VMTLBuffer *b) { vmtl_buf_settle(b); return b->_shadow.map ? b->_shadow.map : b->_b.map; }
@interface VMTLTexture : NSObject <MTLTexture> {
  @public nvk_image _i; nvk_queue *_q; IOSurfaceRef _surf; MTLPixelFormat _fmt; NSUInteger _plane;
  VMTLHeap *_heap; NSUInteger _heapOffset;
  VMTLHeap *_subHeap; NSUInteger _subCharge;
  BOOL _aliased;
  VMTLTexture *_parent;
  void *_sampledDepthView;
  void *_ownView;
  uint32_t _mips;
  VMTLTexture *_viewParent;
  uint32_t _viewType, _viewSlices, _relativeLevel, _relativeSlice;
  uint32_t _baseSlice, _baseLevel;
  uint32_t _surfSeed, _surfGen; BOOL _surfSynced, _surfOwned;
  nvk_buffer _vramBuf; BOOL _vramOn;
  BOOL _stex;
  BOOL _tiledCopy, _tiledDirty;
  uint64_t _resid;
  NSUInteger _usage;
  VMTLBuffer *_backBuf; NSUInteger _backOff, _backBPR;
}
- (nvk_image *)nvi;
- (void *)nvview;
- (void)vmtlSurfaceIn;
- (void)vmtlSurfaceInCmd:(VMTLCommandBuffer *)cb;
- (BOOL)vmtlSurfaceOut;
@end
@interface VMTLFunction : NSObject <MTLFunction> { @public NSData *_spirv; NSString *_fname; NSString *_stage; NSString *_air; NSDictionary *_fc; NSArray *_embedded; unsigned char _bcHash[32]; BOOL _bcHashed;
  NSDictionary *_refl; NSDictionary *_airNames; NSArray *_arguments;
  id _lib; id _fcv; NSString *_specName; BOOL _needsLink; } @end
NSDictionary *vmtl_function_reflection(VMTLFunction *function);
NSDictionary *vmtl_air_names(VMTLFunction *function);
uint64_t vmtl_resid_for_texture(id t);
void vmtl_resid_texture_gone(uint64_t token);
uint64_t vmtl_resid_for_sampler(id s);
@interface VMTLType : NSObject { @public MTLDataType _dataType; } @end
@interface VMTLVertexAttribute : NSObject { @public NSString *_name; NSUInteger _index; MTLDataType _dataType; } @end
@interface VMTLTextureReferenceType : VMTLType { @public MTLDataType _texDataType; MTLTextureType _texType; MTLBindingAccess _access; BOOL _depth; } @end
@interface VMTLStructType : VMTLType { @public NSArray *_members; } @end
@interface VMTLPointerType : VMTLType { @public MTLDataType _elem; MTLBindingAccess _access; NSUInteger _align, _size; BOOL _isArgBuf; VMTLStructType *_elemStruct; } @end
@interface VMTLStructMember : NSObject { @public NSString *_name; NSUInteger _offset; MTLDataType _dataType; NSUInteger _argIndex;
  VMTLStructType *_struct; VMTLTextureReferenceType *_texRef; VMTLPointerType *_ptr; } @end
@interface VMTLArgument : NSObject { @public NSString *_name; MTLArgumentType _type; MTLBindingAccess _access; NSUInteger _index; BOOL _active;
  NSUInteger _arrayLength, _bufAlign, _bufSize; MTLDataType _bufDataType; VMTLStructType *_bufStruct; VMTLPointerType *_bufPtr;
  MTLTextureType _texType; MTLDataType _texDataType; BOOL _depth; } @end
@interface VMTLRenderPipelineReflection : NSObject { @public NSArray *_v, *_f, *_o, *_m; } @end
@interface VMTLComputePipelineReflection : NSObject { @public NSArray *_a; } @end
NSArray<VMTLArgument *> *vmtl_arguments_for_function(VMTLFunction *fn);
VMTLArgument *vmtl_argument_for_buffer(VMTLFunction *fn, NSUInteger index);
id vmtl_render_reflection(MTLRenderPipelineDescriptor *d);
id vmtl_compute_reflection(id<MTLFunction> fn);
id<MTLFunction> vmtl_link_kernel(MTLComputePipelineDescriptor *d, NSError **err);
id<MTLFunction> vmtl_link_stage(id<MTLFunction> k, MTLLinkedFunctions *lf, NSArray *dylibs, NSError **err);
void vmtl_render_keep_linked(id ps, id<MTLFunction> vf, id<MTLFunction> ff);
void vmtl_note_compile_dylibs(id lib, NSArray *libraries);
id vmtl_new_dylib(id<MTLLibrary> lib, NSError **err);
id vmtl_new_dylib_url(NSURL *url, NSError **err);
id vmtl_new_binary_archive(MTLBinaryArchiveDescriptor *d, NSError **err);
NSString *vmtl_archive_refusal(NSArray *archives, NSString *key);
NSString *vmtl_archive_key_compute(id<MTLFunction> f);
NSString *vmtl_archive_key_render(MTLRenderPipelineDescriptor *d);
static inline uint32_t vmtl_vk_layers_for_desc(uint32_t t, uint32_t n, uint32_t depth) { return t == 7 ? (depth ? depth : 1u) : vmtl_vk_layers_for_type(t, n); }

@interface VMTLArgumentEncoder : NSObject <MTLArgumentEncoder> {
  @public NSArray<NSDictionary *> *_fields; NSDictionary *_fieldByIndex;
  int64_t *_offAt; NSUInteger _offN;
  VMTLBuffer *_dst; NSUInteger _dstOffset; NSString *_label;
} @end
extern id gVMTLMainDevice;
@interface VMTLLibrary : NSObject <MTLLibrary> { @public NSDictionary<NSString *, NSData *> *_fns; NSMutableDictionary<NSString *, NSData *> *_compiledFns; NSDictionary<NSString *, NSString *> *_stages, *_airs; NSArray<NSString *> *_externs;
  NSData *_raw; id _appleTwin; BOOL _twinTried; NSString *_rawPath; } @end
@interface VMTLRenderPipelineState : NSObject <MTLRenderPipelineState> { @public nvk_pipeline _p;
  NSString *_vname, *_fname, *_blend, *_vdesc; MTLRenderPipelineDescriptor *_descriptor; NSMutableDictionary *_samplerVariants; } @end
@interface VMTLCommandQueue : NSObject <MTLCommandQueue> {
  @public nvk_queue _q; id _dev; id _completionQueue; dispatch_queue_t _callbackOrderQueue; NSMutableArray *_reservations; dispatch_semaphore_t _capacity; NSUInteger _capacityCount;
} @end
@interface VMTLCommandBuffer : NSObject <MTLCommandBuffer> {
  NSError *_cbError;
  NSMutableDictionary *_userDictionary;
  @public nvk_cmdbuf _c; VMTLCommandQueue *_queue; BOOL _committed; BOOL _capacityHeld; MTLCommandBufferStatus _cbstatus;
  VMTLTexture *_present;
  NSMutableArray *_presentations;
  NSMutableArray *_handlers;
  NSMutableArray *_scratch;
  NSMutableSet *_resources;
  NSMutableSet *_tiledSynced;
  NSUInteger _tiledPart;
  NSMutableSet *_surfDirty;
  unsigned _nEnc, _nEnd, _nHandlers, _nStatus, _nEnqueue, _nLabel, _nPresent, _nWait;
  unsigned _nEncSealed;
  CFTimeInterval _gpuStart, _gpuEnd;
  CFTimeInterval _legacyKernelStart, _legacyKernelEnd;
  vmtl_gpu_interval _gpuTiming; BOOL _gpuTimingSeen, _gpuTimingInvalid;
  NSMutableArray *_parts, *_scheduledHandlers;
  NSUInteger _nextPart;
  NSUInteger _nextSubmit;
  id _reservation;
  NSCondition *_completionCondition;
  dispatch_group_t _callbackGroup, _scheduledGroup;
  BOOL _wasScheduled, _executionDone, _completionReady;
  NSMutableArray *_signals;
  NSMutableArray *_waits;
}
- (void)vmtlReserveQueue;
- (void)vmtlReleaseReservation;
- (void)vmtlRecordWait:(id)event value:(uint64_t)value;
- (void)vmtlRecordSignal:(id)event value:(uint64_t)value;
- (void)vmtlSubmitWithEvents;
- (BOOL)vmtlAdvancePart;
- (BOOL)vmtlSubmitAhead;
- (void)vmtlReleaseCapacity;
- (void)vmtlRecordEncodingError:(NSError *)error;
- (void)vmtlFinishCallbacks;
@end
@interface VMTLDepthStencilState : NSObject <MTLDepthStencilState> { @public int _write; int _compare;
  int _stencil; uint32_t _sf[6], _sb[6]; } @end
@interface VMTLSamplerState : NSObject <MTLSamplerState> { @public nvk_sampler _s; NSDictionary *_pixelState; uint64_t _resid; } @end
@interface VMTLFence : NSObject <MTLFence> { @public id<MTLDevice> _dev; NSString *_label; } @end
@interface VMTLResidencySet : NSObject <MTLResidencySet> { @public id<MTLDevice> _dev; NSString *_label; NSMutableArray *_allocs; } @end
@interface VMTLEvent : NSObject <MTLEvent> { @public id<MTLDevice> _dev; NSString *_label; uint64_t _value; NSCondition *_condition; } @end
@interface VMTLSharedEvent : NSObject <MTLSharedEvent> { @public id<MTLDevice> _dev; NSString *_label; uint64_t _value; NSCondition *_condition; NSMutableArray *_notifications; } @end
@interface VMTLHeap : NSObject <MTLHeap> {
  @public id<MTLDevice> _dev; NSUInteger _size; NSUInteger _used;
  MTLStorageMode _storage; MTLCPUCacheMode _cache; NSString *_label;
  nvk_heap _hm; MTLHeapType _type;
}
- (void)vmtlReleaseSubAllocation:(NSUInteger)bytes;
- (void)vmtlGiveRange:(NSUInteger)off length:(NSUInteger)len;
@end
@interface VMTLComputePipelineState : NSObject <MTLComputePipelineState> { @public nvk_pipeline _p; VMTLFunction *_function; NSMutableDictionary *_samplerVariants;
    BOOL _icb;
} @end
@interface VMTLIndirectCommand : NSObject <MTLIndirectRenderCommand, MTLIndirectComputeCommand> {
  @public id _ps; NSMutableArray *_vbufs; NSMutableArray *_fbufs;
  MTLPrimitiveType _type; NSUInteger _start, _count, _instances, _baseInstance;
  id _ibuf; NSUInteger _ioff; MTLIndexType _itype; NSInteger _baseVertex;
  BOOL _indexed, _armed;
  MTLCullMode _cull; BOOL _hasCull;
  MTLWinding _winding; BOOL _hasWinding;
  MTLTriangleFillMode _fill; BOOL _hasFill;
  MTLDepthClipMode _clip; BOOL _hasClip;
  id _ds; float _bias, _slope, _clamp; BOOL _hasBias;
  id _cps; NSMutableArray *_kbufs; MTLSize _grid, _tg; BOOL _isCompute;
} @end
@interface VMTLIndirectCommandBuffer : NSObject <MTLIndirectCommandBuffer> {
  @public NSMutableArray<VMTLIndirectCommand *> *_cmds; id<MTLDevice> _dev; NSString *_label;
} @end
@interface VMTLComputeCommandEncoder : NSObject <MTLComputeCommandEncoder> {
  @public VMTLCommandBuffer *_cb; VMTLComputePipelineState *_ps; NSMutableArray *_scratch; NSMutableDictionary *_samplers;
  NSMutableDictionary *_tgLen;
  NSMutableDictionary *_bufAt;
  NSMutableSet *_resources;
} @end
@interface VMTLBlitCommandEncoder : NSObject <MTLBlitCommandEncoder> { @public VMTLCommandBuffer *_cb; } @end
@interface VMTLAccelerationStructure : VMTLBuffer <MTLAccelerationStructure> { @public nvk_accel _as; nvk_buffer _uid; uint32_t _iftAdd; } @end
@interface VMTLAccelerationStructureCommandEncoder : NSObject <MTLAccelerationStructureCommandEncoder> { @public VMTLCommandBuffer *_cb; } @end
uint32_t vmtl_geoms_from_descriptor(id desc, vmtl_geom *out, uint32_t max, int *instance, uint32_t *instance_count, NSMutableSet *track);
@interface VMTLRenderCommandEncoder : NSObject <MTLRenderCommandEncoder> {
  @public VMTLCommandBuffer *_cb; VMTLTexture *_target; float _clear[4]; BOOL _begun;
  VMTLTexture *_resolve; uint32_t _passSamples;
  int _cull; uint32_t _passFmt; int _passDepth;
  uint32_t _load; BOOL _hasVP, _hasSC; float _vp[6]; uint32_t _sc[4];
  NSMutableDictionary *_vertexSamplers, *_fragmentSamplers;
  NSMutableDictionary *_vbufAt, *_fbufAt;
  VMTLRenderPipelineState *_ps;
  NSMutableArray *_scratch;
  VMTLDepthStencilState *_ds;
  VMTLBuffer *_tfb; NSUInteger _tfbOff, _tfbStride; float _tfScale; BOOL _tfScaleSet;
  VMTLBuffer *_visBuf;
  uint32_t _visSlot; NSUInteger _visOffset;
  BOOL _visActive, _visPending;
  NSMutableArray *_visRecords;
  nvk_pass _pass; nvk_rtsig _passSig; BOOL _hasRef, _refused;
  VMTLBuffer *_snapVB; NSUInteger _snapVBOff[8]; uint32_t _snapVBCount;
  VMTLBuffer *_snapMB; NSUInteger _snapMBOff[16][2]; uint32_t _snapMBCount;
  unsigned char _snapMBExpected[16][272];
  VMTLTexture *_diagPlane0, *_diagPlane1;
} @end
@interface VMTLRenderCommandEncoder ()
- (BOOL)vmtlBuildPass:(MTLRenderPassDescriptor *)d;
- (BOOL)vmtlBeginPassLoad:(uint32_t)load;
- (BOOL)vmtlEnsurePass:(VMTLRenderPipelineState *)p;
- (void)vmtlApplyDepthStencil;
@end

NSArray<NSDictionary *> *vmtl_embedded_bindings(VMTLFunction *function);
void vmtl_bind_embedded(VMTLCommandBuffer *cb, uint32_t set, VMTLFunction *fn, NSDictionary *bufferAtIndex);
uint64_t vmtl_resid_for_texture(id t);
uint64_t vmtl_resid_for_sampler(id s);

@interface VMTLParallelRenderCommandEncoder : NSObject <MTLParallelRenderCommandEncoder> { @public VMTLCommandBuffer *_cb; MTLRenderPassDescriptor *_desc;
  NSMutableArray *_subs; NSLock *_lock; BOOL _ended; NSString *_label; } @end

@interface VMTLCounter : NSObject <MTLCounter> @end
@interface VMTLCounterSet : NSObject <MTLCounterSet> { @public NSArray *_counters; } @end
@interface VMTLCounterSampleBuffer : NSObject <MTLCounterSampleBuffer> {
  @public void *_pool; NSUInteger _count; MTLStorageMode _storage; NSString *_label; id<MTLDevice> _dev; } @end
id<MTLCounterSet> vmtl_timestamp_counter_set(void);

@interface VMTLBuffer () { @public NSString *_label; MTLResourceOptions _ropt;
  int _resRefs, _rstate, _lruReg, _wantOn; uint64_t _lastUse;
  uint64_t _useEp;
  BOOL _texSeen;
} @end
@interface VMTLTexture () { @public NSString *_label; MTLResourceOptions _ropt; BOOL _shareable; BOOL _fbo;
  MTLTextureSwizzleChannels _swz; BOOL _hasSwz;
  void *_tbView, *_tbAlias; BOOL _tbStorage;
  int _resRefs;
  int _nviews;
  int _rstate;
  uint64_t _lastUse;
  int _lruReg, _wantOn;
  uint64_t _useEp;
} @end
#include <os/lock.h>
@interface VMTLResSet : NSMutableSet { @public NSMutableSet *_s; os_unfair_lock _lk; } @end
@interface VMTLFunction () { @public NSString *_label; } @end
@interface VMTLLibrary () { @public NSString *_label; } @end
@interface VMTLCommandQueue () { @public NSString *_label; } @end
@interface VMTLCommandBuffer () { @public NSString *_label; uint64_t _rep; } @end
@interface VMTLDepthStencilState () { @public NSString *_label; } @end
@interface VMTLSamplerState () { @public NSString *_label; } @end
@interface VMTLComputePipelineState () { @public NSString *_label; } @end
@interface VMTLComputePipelineState () { @public uint64_t _resid; NSUInteger _attrT, _askT; } @end
@interface VMTLRenderPipelineState () { @public uint64_t _resid; } @end
@interface VMTLIndirectCommandBuffer () { @public uint64_t _resid; } @end
@interface VMTLIndirectCommand () - (void)vmtlCopyFrom:(VMTLIndirectCommand *)o; @end
NSUInteger vmtl_air_max_tg(VMTLFunction *fn);
@interface VMTLRenderCommandEncoder () { @public NSString *_label; } @end
@interface VMTLRenderCommandEncoder () { @public uint32_t _vstride[VMTL_NVIN]; uint32_t _vstrideSet; } @end
@interface VMTLComputeCommandEncoder () { @public NSString *_label; } @end
@interface VMTLBlitCommandEncoder () { @public NSString *_label; } @end
@interface VMTLAccelerationStructureCommandEncoder () { @public NSString *_label; } @end
@interface VMTLHeap () { @public MTLHazardTrackingMode _hazard; } @end
@interface VMTLIndirectCommandBuffer () { @public MTLResourceOptions _opts; } @end
@interface VMTLBuffer () { @public uint32_t _purge; } @end
@interface VMTLTexture () { @public uint32_t _purge; } @end
@interface VMTLHeap () { @public uint32_t _purge; } @end
#include <os/lock.h>
@interface VMTLHeap () { @public NSUInteger *_fr; uint32_t _nfr, _cfr; os_unfair_lock _fl; BOOL _backed; } @end
@interface VMTLBuffer () { @public BOOL _subPlaced; } @end
typedef struct { nvk_image img; uint32_t key[12]; NSUInteger off, len; } vmtl_park_t;
@interface VMTLTexture () { @public BOOL _subPlaced, _parkable; uint32_t _pkey[12]; NSUInteger _pkLen; } @end
@interface VMTLHeap () { @public vmtl_park_t *_pk; uint32_t _npk; } @end
@interface VMTLBuffer () { @public vmtl_park_t *_bpk; uint32_t _nbpk; os_unfair_lock _bpl; } @end
@interface VMTLTexture () { @public BOOL _bparkable; } @end
static void vmtl_heap_park(VMTLHeap *h, nvk_image *img, const uint32_t *key, NSUInteger off, NSUInteger len);
void vmtl_heap_back(VMTLHeap *h, NSUInteger size, int shared);
@interface VMTLIndirectCommandBuffer () { @public uint32_t _purge; } @end
static inline MTLPurgeableState vmtl_purge(uint32_t *st, MTLPurgeableState s) {
    if (s < MTLPurgeableStateKeepCurrent || s > MTLPurgeableStateEmpty) {
        static int said; if (!said++) nvlog("setPurgeableState: %lu is no MTLPurgeableState - answered as KeepCurrent", (unsigned long)s);
        s = MTLPurgeableStateKeepCurrent;
    }
    uint32_t prev = s == MTLPurgeableStateKeepCurrent ? __atomic_load_n(st, __ATOMIC_ACQUIRE) : __atomic_exchange_n(st, (uint32_t)s, __ATOMIC_ACQ_REL);
    return prev ? (MTLPurgeableState)prev : MTLPurgeableStateNonVolatile;
}
static inline kern_return_t vmtl_owner_check(task_id_token_t tok) {
    mach_port_t p = MACH_PORT_NULL;
    if (task_identity_token_get_task_port(tok, TASK_FLAVOR_NAME, &p) != KERN_SUCCESS) return kIOReturnBadArgument;
    mach_port_deallocate(mach_task_self(), p);
    return KERN_SUCCESS;
}
static inline MTLCPUCacheMode vmtl_res_cache(VMTLHeap *hp, MTLResourceOptions o) { return hp ? hp->_cache : (MTLCPUCacheMode)(o & 0xF); }
static inline MTLHazardTrackingMode vmtl_res_hazard(VMTLHeap *hp, MTLResourceOptions o) {
    if (hp) return hp->_hazard == MTLHazardTrackingModeTracked ? MTLHazardTrackingModeTracked : MTLHazardTrackingModeUntracked;
    return ((o >> MTLResourceHazardTrackingModeShift) & 0x3) == MTLHazardTrackingModeUntracked ? MTLHazardTrackingModeUntracked : MTLHazardTrackingModeTracked;
}
extern unsigned vmtl_apps1_resolve_miss, vmtl_apps1_resolve_extra, vmtl_apps1_ds_resolve_miss;
@interface MTLSharedTextureHandle (VMTLSPI)
- (instancetype)initWithIOSurface:(IOSurfaceRef)surface label:(NSString *)label;
- (IOSurfaceRef)ioSurface;
@end
@interface MTLIOAccelDevice (VMTLApps1)
- (id<MTLTexture>)newSharedTextureWithHandle:(MTLSharedTextureHandle *)handle;
@end

NSDictionary *vmtl_index_argument_fields(NSArray *fields);
