/*
 * vmtl_compat13.h — macOS 13 guest / SDK 14.2 shims.
 *
 * The NullMoth plugin was written against the macOS 15 SDK. Three newer
 * Metal declarations are missing from the SDK 14.2 headers this VM's
 * Xcode ships:
 *   - MTLResidencySet / MTLResidencySetDescriptor (residency sets are
 *     implicit on our backend — VMTLResidencySet is a pure no-op stub)
 *   - MTLShaderValidation / MTLShaderValidationDisabled
 *   - the full MTLCommandQueueDescriptor interface (the 14.2 Metal.h only
 *     forward-declares @class MTLCommandQueueDescriptor)
 * Shims are compiled only when the SDK predates macOS 15 so a newer build
 * uses the real declarations.
 */
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#if !defined(__MAC_15_0) || MAC_OS_X_VERSION_MAX_ALLOWED < __MAC_15_0

@protocol MTLResidencySet <NSObject>
@end

@protocol MTLAllocation <NSObject>
- (NSUInteger)allocatedSize;
@property (nullable, readonly, copy) NSString *label;
@end

@interface MTLResidencySetDescriptor : NSObject
@property (nullable, nonatomic, copy) NSString *label;
@end

typedef NS_ENUM(NSUInteger, MTLShaderValidation) {
    MTLShaderValidationDefault  = 0,
    MTLShaderValidationDisabled = 1,
    MTLShaderValidationEnabled  = 2,
};

@interface MTLCommandQueueDescriptor : NSObject
@property (readwrite, nonatomic) NSUInteger maxCommandBufferCount;
- (nullable id)logState;   /* optional newer API: the plugin only respondsToSelector: -tests it */
@end

#endif /* SDK older than macOS 15 */
