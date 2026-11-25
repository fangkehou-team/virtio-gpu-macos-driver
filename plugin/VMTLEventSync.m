/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

@interface VMTLQueueReservation : NSObject { @public __weak VMTLCommandBuffer *buffer; } @end
@implementation VMTLQueueReservation @end
@interface VMTLSubmissionPart : NSObject {
@public
    nvk_cmdbuf commands;
    NSSet *resources, *surfaces;
    NSArray *scratch;
    id waitEvent, signalEvent;
    uint64_t value;
    unsigned encoders;
    NSArray *signals;
    vmtl_inflight *inflight;
    int aheadFailed;
    BOOL waitFolded;
    BOOL aheadEmpty;
}
@end
static _Atomic unsigned gsub_inflight;
static _Atomic unsigned long long gsub_ahead, gsub_peak;
static NSMutableArray *vmtl_quarantined_parts;
static int vmtl_gsub_finish(VMTLSubmissionPart *part) {
    vmtl_inflight *const f = part->inflight;
    part->inflight = NULL;
    const int r = vmtl_vk_submit_finish(f);
    atomic_fetch_sub(&gsub_inflight, 1);
    if (r == 0) vmtl_vk_inflight_free(f);
    return r;
}
@implementation VMTLSubmissionPart
- (void)dealloc {
    if (inflight) {
        nvlog("commit: G-SUB BUG - a part submitted ahead reached dealloc unfinished; finishing it now");
        if (vmtl_gsub_finish(self) != 0) {
            (void)CFBridgingRetain(resources); (void)CFBridgingRetain(surfaces); (void)CFBridgingRetain(scratch);
            memset(&commands, 0, sizeof commands);
            return;
        }
    }
    if (commands.cb) vmtl_vk_cmd_abandon(&commands);
}
@end

static NSMutableArray<VMTLCommandBuffer *> *vmtl_pending_buffers;
static BOOL vmtl_pumping;
static void vmtl_pump_buffers(void);
static BOOL vmtl_async_commit_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("VMTL_ASYNC_COMMIT"); on = !(e && e[0] == '0');
        nvlog("commit: %s (VMTL_ASYNC_COMMIT=%s)", on ? "ASYNC (default) - execution worker, submit ahead, queue capacity enforced" : "synchronous (VMTL_ASYNC_COMMIT=0)", e ? e : "unset"); }
    return on;
}
static _Atomic unsigned long long gSkipEmpty;
static int vmtl_skip_empty_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("VMTL_SKIP_EMPTY"); on = !(e && e[0] == '0');
        nvlog("commit: empty submission parts %s (VMTL_SKIP_EMPTY=%s)", on ? "are NOT submitted" : "are submitted as before b78", e ? e : "unset"); }
    return on;
}

@implementation VMTLCommandBuffer (EventSubmission)
- (void)vmtlReserveQueue {
    if (_reservation) return;
    VMTLQueueReservation *r = [VMTLQueueReservation new]; r->buffer = self;
    _reservation = r;
    if (!_queue->_reservations) _queue->_reservations = [NSMutableArray new];
    [_queue->_reservations addObject:r];
}
- (void)vmtlReleaseReservation {
    if (_reservation) [_queue->_reservations removeObjectIdenticalTo:_reservation];
    _reservation = nil;
}
- (void)vmtlSealPartWithSignal:(id)event value:(uint64_t)value restart:(BOOL)restart {
    if (!_parts) _parts = [NSMutableArray new];
    vmtl_g79_flush(self);
    VMTLSubmissionPart *part = [VMTLSubmissionPart new];
    part->commands = _c;
    memset(&_c, 0, sizeof(_c));
    part->resources = [_resources copy];
    [_resources removeAllObjects]; part->surfaces = [_surfDirty copy];
    part->scratch = [_scratch copy]; part->signalEvent = event; part->value = value;
    part->encoders = _nEnc - _nEncSealed; _nEncSealed = _nEnc;
    if (_g79Sig.count) { part->signals = [_g79Sig copy]; [_g79Sig removeAllObjects]; }
    [_parts addObject:part];
    [_surfDirty removeAllObjects]; [_scratch removeAllObjects];
    if (restart && vmtl_vk_cmd_begin(&_queue->_q, &_c)) {
        _cbError = [NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorOutOfMemory
            userInfo:@{NSLocalizedDescriptionKey:@"Could not allocate recording after event boundary"}];
    }
}
- (void)vmtlRecordWait:(id)event value:(uint64_t)value {
    if (!event || _committed) return;
    if (vmtl_g79_on(VMTL_G79_EV)) {
        BOOL self_ = NO;
        for (NSArray *p in _g79Promised) if (p[0] == event && [p[1] unsignedLongLongValue] >= value) { self_ = YES; break; }
        BOOL held = !self_ && [event respondsToSelector:@selector(signaledValue)]
            && ((uint64_t(*)(id,SEL))objc_msgSend)(event, @selector(signaledValue)) >= value;
        if (self_ || held) {
            if (_g79C2C && vmtl_g79_on(VMTL_G79_WFOLD)) { _g79Owed = YES; vmtl_g79_count(G79C_WAITFOLD); }
            else if (_c.cb) { vmtl_vk_cmd_barrier(&_c); _g79Owed = NO; _g79C2C = NO; }
            vmtl_g79_count(self_ ? G79C_WAITSELF : G79C_WAITHELD);
            return;
        }
        vmtl_g79_count(G79C_WAITSPLIT);
    }
    [self vmtlSealPartWithSignal:nil value:0 restart:YES];
    VMTLSubmissionPart *part = [VMTLSubmissionPart new];
    part->waitEvent = event; part->value = value; [_parts addObject:part];
}
- (void)vmtlRecordSignal:(id)event value:(uint64_t)value {
    if (!event || _committed) return;
    if (vmtl_g79_on(VMTL_G79_EV)) {
        if (!_g79Sig) _g79Sig = [NSMutableArray new];
        if (!_g79Promised) _g79Promised = [NSMutableArray new];
        [_g79Sig addObject:@[event, @(value)]]; [_g79Promised addObject:@[event, @(value)]];
        vmtl_g79_count(G79C_SIGRIDE);
        return;
    }
    [self vmtlSealPartWithSignal:event value:value restart:YES];
}
- (void)vmtlDispatchHandlers:(NSArray *)handlers scheduled:(BOOL)scheduled {
    if (!handlers.count) return;
    dispatch_queue_t order;
    dispatch_queue_t target;
    @synchronized (_queue) {
        if (!_queue->_callbackOrderQueue)
            _queue->_callbackOrderQueue = dispatch_queue_create("com.vmtl.command-callbacks", DISPATCH_QUEUE_SERIAL);
        order = _queue->_callbackOrderQueue;
        target = _queue->_completionQueue ?: dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    }
    for (NSUInteger i = 0; i < handlers.count; ++i) {
        dispatch_group_enter(_callbackGroup);
        if (scheduled) dispatch_group_enter(_scheduledGroup);
    }
    dispatch_async(order, ^{
        dispatch_sync(target, ^{
            for (MTLCommandBufferHandler handler in handlers) {
                handler(self);
                if (scheduled) dispatch_group_leave(self->_scheduledGroup);
                dispatch_group_leave(self->_callbackGroup);
            }
        });
    });
}
- (void)vmtlMarkScheduled {
    [_completionCondition lock];
    if (!_wasScheduled) {
        _wasScheduled = YES; _cbstatus = MTLCommandBufferStatusScheduled;
        NSArray *handlers = [_scheduledHandlers copy]; [_scheduledHandlers removeAllObjects];
        [self vmtlDispatchHandlers:handlers scheduled:YES];
        [_completionCondition broadcast];
    }
    [_completionCondition unlock];
}
- (void)vmtlCollectDeviceTiming:(nvk_cmdbuf *)commands {
    if (!commands->timing.valid) { _gpuTimingInvalid = YES; return; }
    if (!_gpuTimingSeen) { _gpuTiming = commands->timing; _gpuTimingSeen = YES; }
    else {
        _gpuTiming.end_tick = commands->timing.end_tick;
        _gpuTiming.host_after_ns = commands->timing.host_after_ns;
    }
}
- (void)vmtlFinishExecution:(NSError *)error {
    for (NSUInteger i = _nextPart; i < _parts.count; i++) {
        VMTLSubmissionPart *p = _parts[i];
        if (![p isKindOfClass:[VMTLSubmissionPart class]] || !p->inflight) continue;
        int result = vmtl_gsub_finish(p);
        if (!result) [self vmtlCollectDeviceTiming:&p->commands];
        else _gpuTimingInvalid = YES;
        if (result == -2) {
            if (!vmtl_quarantined_parts) vmtl_quarantined_parts = [NSMutableArray new];
            [vmtl_quarantined_parts addObject:p];
        }
    }
    if (!error) {
        for (dispatch_block_t action in _presentations) action();
    }
    [_presentations removeAllObjects]; _present = nil;
    double deviceStart = 0, deviceEnd = 0;
    if (!error && _gpuTimingSeen && !_gpuTimingInvalid)
        (void)vmtl_vk_interval_seconds(&_gpuTiming, &deviceStart, &deviceEnd);
    [_completionCondition lock];
    _gpuStart = deviceStart; _gpuEnd = deviceEnd;
    _legacyKernelEnd = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
    _cbError = error; _cbstatus = error ? MTLCommandBufferStatusError : MTLCommandBufferStatusCompleted;
    _executionDone = YES;
    [_completionCondition unlock];
    [_parts removeAllObjects];
    if (!error) vmtl_res2_cb_done(self);
    [self vmtlReleaseCapacity];
}
- (void)vmtlFinishCallbacks {
    [_completionCondition lock];
    NSArray *handlers = [_handlers copy]; [_handlers removeAllObjects];
    [self vmtlDispatchHandlers:handlers scheduled:NO];
    _completionReady = YES;
    [_completionCondition broadcast]; [_completionCondition unlock];
}
static unsigned vmtl_gsub_cap(void) {
    static int cap = -1;
    if (cap < 0) { const char *e = getenv("VMTL_GSUB_INFLIGHT"); int v = e ? atoi(e) : 16;
        cap = v < 1 ? 1 : v > 32 ? 32 : v;
        nvlog("commit: G-SUB submit-ahead %s, at most %d parts in flight (VMTL_GSUB_INFLIGHT=%s)",
              vmtl_async_commit_on() && vmtl_g79_on(VMTL_G79_SUB) ? "ON" : "off", cap, e ? e : "unset"); }
    return (unsigned)cap;
}
static BOOL vmtl_gsub_ahead_on(void) { return vmtl_async_commit_on() && vmtl_g79_on(VMTL_G79_SUB) && vmtl_gsub_cap(); }
static NSMapTable *vmtl_evfold_submitted;
static _Atomic unsigned long long gEvFold, gEvFoldEmpty;
static BOOL vmtl_evfold_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("VMTL_EVFOLD"); on = !(e && e[0] == '0');
        nvlog("commit: sub1 EVFOLD %s - an event wait whose signal is already in the channel %s (VMTL_EVFOLD=%s)",
              on ? "ON" : "off", on ? "is ordered by the channel, no CPU hold" : "is held until the CPU reads the signal", e ? e : "unset"); }
    return on;
}
static uint64_t vmtl_evfold_promised(id event) {
    return event ? [[vmtl_evfold_submitted objectForKey:event] unsignedLongLongValue] : 0;
}
static void vmtl_evfold_promise(id event, uint64_t value) {
    if (!event || !vmtl_evfold_on()) return;
    if (!vmtl_evfold_submitted) vmtl_evfold_submitted = [NSMapTable weakToStrongObjectsMapTable];
    if (value > vmtl_evfold_promised(event)) [vmtl_evfold_submitted setObject:@(value) forKey:event];
}
static void vmtl_evfold_promise_part(VMTLSubmissionPart *part) {
    if (part->signalEvent) vmtl_evfold_promise(part->signalEvent, part->value);
    for (NSArray *s in part->signals) vmtl_evfold_promise(s[0], [s[1] unsignedLongLongValue]);
}
static void vmtl_evfold_census(_Atomic unsigned long long *c, const char *what) {
    const unsigned long long k = atomic_fetch_add(c, 1) + 1;
    if (!(k & (k - 1))) nvlog("commit: sub1 EVFOLD %llu %s", k, what);
}
static void vmtl_gsub_census(void) {
    const unsigned long long k = atomic_load(&gsub_ahead);
    if (k & (k - 1)) return;
    uint64_t s[9]; vmtl_vk_gsub_nvk_stats(s, 9);
    nvlog("commit: G-SUB %llu parts submitted ahead, peak %llu in flight | NVK %s submits %llu acquires %llu cpuwaits %llu slotwaits %llu fifowaits %llu deferred %llu reaped %llu waits %llu",
          k, (unsigned long long)atomic_load(&gsub_peak), s[0] == ~0ull ? "G-SUB ABSENT" : s[0] ? "async" : "SYNC (NVRM_ASYNC=0)",
          (unsigned long long)s[1], (unsigned long long)s[2], (unsigned long long)s[3], (unsigned long long)s[4],
          (unsigned long long)s[5], (unsigned long long)s[6], (unsigned long long)s[7], (unsigned long long)s[8]);
}
- (BOOL)vmtlSubmitAhead {
    if (_executionDone || _cbError || !_committed) return NO;
    if (_nextSubmit < _nextPart) _nextSubmit = _nextPart;
    while (_nextSubmit < _parts.count) {
        if (atomic_load(&gsub_inflight) >= vmtl_gsub_cap()) return NO;
        VMTLSubmissionPart *part = _parts[_nextSubmit];
        if (![part isKindOfClass:[VMTLSubmissionPart class]]) return NO;
        if (part->waitEvent) {
            if (!vmtl_evfold_on() || vmtl_evfold_promised(part->waitEvent) < part->value) return NO;
            part->waitFolded = YES; _nextSubmit++; vmtl_evfold_census(&gEvFold, "event waits ordered by the channel (no CPU hold)");
            continue;
        }
        if (!part->commands.cb) return NO;
        if (!part->encoders && !part->resources.count && !part->surfaces.count && !part->scratch.count && vmtl_skip_empty_on()) {
            if (!vmtl_evfold_on() || _nextSubmit + 1 >= _parts.count) return NO;
            [self vmtlMarkScheduled];
        if (!_legacyKernelStart) _legacyKernelStart = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
            vmtl_vk_cmd_abandon(&part->commands);
            part->aheadEmpty = YES; _nextSubmit++;
            vmtl_evfold_promise_part(part);
            vmtl_evfold_census(&gEvFoldEmpty, "empty split parts retired by the ahead pass");
            continue;
        }
        {
            static int hold = -1;
            if (hold < 0) { const char *e = getenv("VMTL_GSUB_HOSTPTR_LIFT"); hold = (e && *e && *e != '0') ? 0 : 1; }
            if (hold) {
                for (id resource in part->resources)
                    if ([resource isKindOfClass:[VMTLBuffer class]] && ((VMTLBuffer *)resource)->_hostPtr) return NO;
            }
        }
        vmtl_inflight *f = vmtl_vk_inflight_new();
        if (!f) return NO;
        [self vmtlMarkScheduled];
        if (!_legacyKernelStart) _legacyKernelStart = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
        const int r = vmtl_vk_submit_begin(&part->commands, f);
        _nextSubmit++;
        if (r) {
            vmtl_vk_inflight_free(f);
            part->aheadFailed = r;
            return NO;
        }
        part->inflight = f;
        vmtl_evfold_promise_part(part);
        const unsigned long long n = atomic_fetch_add(&gsub_inflight, 1) + 1;
        if (n > atomic_load(&gsub_peak)) atomic_store(&gsub_peak, n);
        atomic_fetch_add(&gsub_ahead, 1);
        vmtl_gsub_census();
    }
    return YES;
}
static void vmtl_gsub_ahead_pass1(void);
static void vmtl_gsub_ahead_pass(void) {
    for (int round = 0; round < 64; round++) {
        const unsigned long long before = atomic_load(&gEvFold);
        vmtl_gsub_ahead_pass1();
        if (atomic_load(&gEvFold) == before) break;
    }
}
static void vmtl_gsub_ahead_pass1(void) {
    NSMutableSet *queues = [NSMutableSet new];
    for (VMTLCommandBuffer *cb in vmtl_pending_buffers) {
        id q = cb->_queue;
        if (!q || [queues containsObject:q]) continue;
        [queues addObject:q];
        for (VMTLQueueReservation *r in [cb->_queue->_reservations copy]) {
            VMTLCommandBuffer *b = r->buffer;
            if (!b) continue;
            if (![b vmtlSubmitAhead]) break;
        }
    }
}
- (BOOL)vmtlAdvancePart {
    if (_executionDone) return NO;
    if (_cbError) { [self vmtlFinishExecution:_cbError]; return YES; }
    if (_nextPart >= _parts.count) { [self vmtlFinishExecution:nil]; return YES; }
    VMTLSubmissionPart *part = _parts[_nextPart];
    const BOOL ahead = part->inflight || part->aheadFailed;
    if (part->waitEvent && !ahead && !part->waitFolded) {
        if (![part->waitEvent respondsToSelector:@selector(signaledValue)]) {
            [self vmtlFinishExecution:[NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal
                userInfo:@{NSLocalizedDescriptionKey:@"Cannot wait on an event from another backend"}]];
            return YES;
        }
        uint64_t have = ((uint64_t(*)(id,SEL))objc_msgSend)(part->waitEvent,@selector(signaledValue));
        if (have < part->value) return NO;
    }
    if (!ahead && part->commands.cb && !part->encoders && !part->resources.count && !part->surfaces.count && !part->scratch.count && vmtl_skip_empty_on()) {
        [self vmtlMarkScheduled];
        if (!_legacyKernelStart) _legacyKernelStart = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
        vmtl_vk_cmd_abandon(&part->commands);
        const unsigned long long k = atomic_fetch_add(&gSkipEmpty, 1);
        if (!k || ((k + 1) & 0xFFF) == 0) nvlog("commit: %llu empty submission parts NOT submitted (no encoder, no resources) - each WAS a full submit + wait", k + 1);
    }
    if (part->commands.cb || ahead) {
        [self vmtlMarkScheduled];
        if (!_legacyKernelStart) _legacyKernelStart = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
        int result;
        if (part->inflight) result = vmtl_gsub_finish(part);
        else if (part->aheadFailed) result = part->aheadFailed;
        else {
            for (id resource in part->resources)
                if ([resource isKindOfClass:[VMTLBuffer class]]) vmtl_buffer_sync_in(resource);
            result = vmtl_vk_submit_wait(&part->commands);
        }
        if (result == -2) {
            if (!vmtl_quarantined_parts) vmtl_quarantined_parts = [NSMutableArray new];
            [vmtl_quarantined_parts addObject:part];
        }
        if (result == 0) [self vmtlCollectDeviceTiming:&part->commands];
        BOOL published = result == 0;
        if (published) {
            for (VMTLTexture *texture in part->surfaces) if (![texture vmtlSurfaceOut]) published = NO;
            for (id resource in part->resources)
                if ([resource isKindOfClass:[VMTLBuffer class]]) vmtl_buffer_sync_out(resource);
        }
        if (!published) {
            [self vmtlFinishExecution:[NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal
                userInfo:@{NSLocalizedDescriptionKey:result ? @"GPU event segment submission failed" : @"Could not publish event segment writes"}]];
            return YES;
        }
    }
    if (part->signalEvent) {
        if (![part->signalEvent respondsToSelector:@selector(setSignaledValue:)]) {
            [self vmtlFinishExecution:[NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal
                userInfo:@{NSLocalizedDescriptionKey:@"Cannot signal an event from another backend"}]];
            return YES;
        }
        ((void(*)(id,SEL,uint64_t))objc_msgSend)(part->signalEvent,@selector(setSignaledValue:),part->value);
    }
    for (NSArray *s in part->signals) {
        id event = s[0];
        if (![event respondsToSelector:@selector(setSignaledValue:)]) {
            [self vmtlFinishExecution:[NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal
                userInfo:@{NSLocalizedDescriptionKey:@"Cannot signal an event from another backend"}]];
            return YES;
        }
        ((void(*)(id,SEL,uint64_t))objc_msgSend)(event, @selector(setSignaledValue:), [s[1] unsignedLongLongValue]);
    }
    _parts[_nextPart++] = [NSNull null];
    if (_nextPart == _parts.count) [self vmtlFinishExecution:nil];
    return YES;
}
- (void)vmtlSubmitWithEvents {
    @synchronized ([VMTLCommandBuffer class]) {
        if (_committed) return;
        [self vmtlReserveQueue];
        _committed = YES;
        if (!__atomic_load_n(&_rep, __ATOMIC_ACQUIRE)) __atomic_store_n(&_rep, vmtl_ep_enter(), __ATOMIC_RELEASE);
        [_completionCondition lock]; _cbstatus = MTLCommandBufferStatusCommitted; [_completionCondition unlock];
        [self vmtlSealPartWithSignal:nil value:0 restart:NO];
        if (!vmtl_pending_buffers) vmtl_pending_buffers = [NSMutableArray new];
        [vmtl_pending_buffers addObject:self];
        vmtl_pump_buffers();
    }
}
@end

static void vmtl_pump_buffers_sync(void) {
    @synchronized ([VMTLCommandBuffer class]) {
        if (vmtl_pumping) return;
        vmtl_pumping = YES;
        BOOL progress;
        do {
            progress = NO;
            for (VMTLCommandBuffer *cb in [vmtl_pending_buffers copy]) {
                NSMutableArray *reservations = cb->_queue->_reservations;
                while (reservations.count && !((VMTLQueueReservation *)reservations.firstObject)->buffer)
                    [reservations removeObjectAtIndex:0];
                if (reservations.firstObject != cb->_reservation) continue;
                if ([cb vmtlAdvancePart]) progress = YES;
                if (cb->_executionDone) {
                    [cb vmtlReleaseReservation];
                    [vmtl_pending_buffers removeObjectIdenticalTo:cb];
                    [cb vmtlFinishCallbacks];
                }
            }
        } while (progress);
        vmtl_pumping = NO;
    }
}

static uint64_t vmtl_pump_generation;
static void vmtl_pump_buffers(void) {
    static dispatch_once_t once; static dispatch_queue_t worker;
    const BOOL async = vmtl_async_commit_on();
    if (async) dispatch_once(&once, ^{ worker = dispatch_queue_create("vmtl.execution", DISPATCH_QUEUE_SERIAL); });
    if (!async) { vmtl_vk_clock_refresh(); vmtl_pump_buffers_sync(); return; }
    @synchronized ([VMTLCommandBuffer class]) {
        if (!vmtl_pending_buffers.count) return;
        ++vmtl_pump_generation;
        if (vmtl_pumping) return;
        vmtl_pumping = YES;
        dispatch_async(worker, ^{
            for (;;) { @autoreleasepool {
                vmtl_vk_clock_refresh();
                NSArray *batch; uint64_t generation;
                @synchronized ([VMTLCommandBuffer class]) {
                    if (vmtl_gsub_ahead_on()) vmtl_gsub_ahead_pass();
                    batch = [vmtl_pending_buffers copy]; generation = vmtl_pump_generation;
                }
                BOOL progress = NO;
                for (VMTLCommandBuffer *cb in batch) {
                    BOOL eligible;
                    @synchronized ([VMTLCommandBuffer class]) {
                        NSMutableArray *reservations = cb->_queue->_reservations;
                        while (reservations.count && !((VMTLQueueReservation *)reservations.firstObject)->buffer)
                            [reservations removeObjectAtIndex:0];
                        eligible = reservations.firstObject == cb->_reservation;
                    }
                    if (!eligible) continue;
                    if ([cb vmtlAdvancePart]) progress = YES;
                    @synchronized ([VMTLCommandBuffer class]) {
                        if (cb->_executionDone) {
                            [cb vmtlReleaseReservation];
                            [vmtl_pending_buffers removeObjectIdenticalTo:cb];
                            [cb vmtlFinishCallbacks];
                        }
                    }
                }
                @synchronized ([VMTLCommandBuffer class]) {
                    if (!progress && generation == vmtl_pump_generation) {
                        vmtl_pumping = NO; return;
                    }
                }
            } }
        });
    }
}

static uint64_t vmtl_event_read(NSCondition *condition, uint64_t *storage) {
    [condition lock]; uint64_t value = *storage; [condition unlock]; return value;
}
static void vmtl_event_signal(id event, NSCondition *condition, uint64_t *storage,
                              NSMutableArray *notifications, uint64_t value) {
    NSMutableArray *ready = [NSMutableArray new];
    [condition lock];
    if (value > *storage) *storage = value;
    value = *storage;
    for (NSArray *notification in notifications)
        if ([notification[1] unsignedLongLongValue] <= value) [ready addObject:notification];
    [notifications removeObjectsInArray:ready];
    [condition broadcast]; [condition unlock];
    for (NSArray *notification in ready) {
        MTLSharedEventListener *listener = notification[0];
        MTLSharedEventNotificationBlock block = notification[2];
        dispatch_async(listener.dispatchQueue, ^{ block(event, value); });
    }
    vmtl_pump_buffers();
}
static void vmtl_event_notify(id event, NSCondition *condition, uint64_t *storage,
                              NSMutableArray *notifications, MTLSharedEventListener *listener,
                              uint64_t wanted, MTLSharedEventNotificationBlock block) {
    if (!listener || !block) return;
    [condition lock]; uint64_t value = *storage;
    if (value < wanted) [notifications addObject:@[listener, @(wanted), [block copy]]];
    [condition unlock];
    if (value >= wanted) dispatch_async(listener.dispatchQueue, ^{ block(event, value); });
}
static BOOL vmtl_event_wait(NSCondition *condition, uint64_t *storage, uint64_t wanted, uint64_t milliseconds) {
    NSDate *deadline = milliseconds == UINT64_MAX ? nil : [NSDate dateWithTimeIntervalSinceNow:(double)milliseconds / 1000.0];
    [condition lock];
    while (*storage < wanted) {
        if (!milliseconds) break;
        if (deadline) { if (![condition waitUntilDate:deadline]) break; }
        else [condition wait];
    }
    BOOL done = *storage >= wanted; [condition unlock]; return done;
}
