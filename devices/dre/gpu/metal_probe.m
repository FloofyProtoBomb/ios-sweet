// SPDX-License-Identifier: MIT
// End-to-end acceptance test, not a Metal implementation or GPU emulator.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <dispatch/dispatch.h>
#include <sys/sysctl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static BOOL counter(const char *name, void *value, size_t bytes)
{
    size_t actual = bytes;
    return sysctlbyname(name, value, &actual, NULL, 0) == 0 && actual == bytes;
}
static BOOL completed(id<MTLCommandBuffer> commands, NSError **error)
{
    if (!commands) return NO;
    dispatch_semaphore_t fence = dispatch_semaphore_create(0);
    [commands addCompletedHandler:^(id<MTLCommandBuffer> unused) {
        (void)unused;
        dispatch_semaphore_signal(fence);
    }];
    [commands commit];
    if (dispatch_semaphore_wait(fence, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)))
        return NO;
    if (commands.status != MTLCommandBufferStatusCompleted) {
        if (error) *error = commands.error;
        return NO;
    }
    return YES;
}
static BOOL compute(id<MTLDevice> device, id<MTLCommandQueue> queue,
    id<MTLLibrary> library, NSError **error)
{
    const NSUInteger count = 257, bytes = count * sizeof(uint32_t);
    id<MTLFunction> function = [library newFunctionWithName:@"dre_transform"];
    if (!function) return NO;
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:error];
    if (!pipeline || pipeline.maxTotalThreadsPerThreadgroup < 64) return NO;
    id<MTLBuffer> upload = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> input = [device newBufferWithLength:bytes + 256 options:MTLResourceStorageModePrivate];
    id<MTLBuffer> output = [device newBufferWithLength:bytes + 512 options:MTLResourceStorageModePrivate];
    id<MTLBuffer> readback = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    if (!upload || !input || !output || !readback || !upload.contents || !readback.contents) return NO;
    uint32_t *source = upload.contents, *values = readback.contents;
    for (NSUInteger i = 0; i < count; i++) { source[i] = (0x9e3779b9u * (uint32_t)(i + 1)) ^ 0xa51eu; values[i] = 0; }
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    if (!blit) return NO;
    [blit copyFromBuffer:upload sourceOffset:0 toBuffer:input destinationOffset:256 size:bytes];
    [blit endEncoding];
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    if (!encoder) return NO;
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:input offset:256 atIndex:0];
    [encoder setBuffer:output offset:512 atIndex:1];
    [encoder dispatchThreadgroups:MTLSizeMake((count + 63) / 64, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [encoder endEncoding];
    blit = [commands blitCommandEncoder];
    if (!blit) return NO;
    [blit copyFromBuffer:output sourceOffset:512 toBuffer:readback destinationOffset:0 size:bytes];
    [blit endEncoding];
    if (!completed(commands, error)) return NO;
    for (NSUInteger i = 0; i < count; i++) {
        uint32_t shift = (uint32_t)i & 31, v = source[i];
        uint32_t rotated = shift ? (v << shift) | (v >> (32 - shift)) : v;
        if (values[i] != (rotated ^ 0xa619d22au) + (uint32_t)i * 0x10203u) return NO;
    }
    return YES;
}
static BOOL encodeRender(id<MTLDevice> device, id<MTLLibrary> library,
    id<MTLTexture> target, id<MTLCommandBuffer> commands, NSError **error)
{
    MTLRenderPipelineDescriptor *pipelineDesc = [MTLRenderPipelineDescriptor new];
    pipelineDesc.vertexFunction = [library newFunctionWithName:@"dre_vertex"];
    pipelineDesc.fragmentFunction = [library newFunctionWithName:@"dre_fragment"];
    pipelineDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    if (!pipelineDesc.vertexFunction || !pipelineDesc.fragmentFunction) return NO;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pipelineDesc error:error];
    if (!pipeline) return NO;
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    if (!encoder) return NO;
    [encoder setRenderPipelineState:pipeline];
    [encoder setViewport:(MTLViewport){0, 0, 64, 64, 0, 1}];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    return YES;
}
static BOOL pixels(const void *memory, size_t rowBytes)
{
    if (!memory || rowBytes < 256) return NO;
    const uint8_t *data = memory;
    for (unsigned y = 0; y < 64; y++) for (unsigned x = 0; x < 64; x++) {
        const uint8_t *p = data + y * rowBytes + 4 * x;
        if (p[0] != ((x * 3 + y * 5) & 255) || p[1] != (x ^ y) || p[2] != 165 || p[3] != 255)
            return NO;
    }
    return YES;
}
static BOOL render(id<MTLDevice> device, id<MTLCommandQueue> queue,
    id<MTLLibrary> library, BOOL sharedSurface, NSError **error)
{
    MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
        width:64 height:64 mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget;
    desc.storageMode = sharedSurface ? MTLStorageModeShared : MTLStorageModePrivate;
    IOSurfaceRef surface = NULL;
    id<MTLTexture> texture;
    if (sharedSurface) {
        NSDictionary *properties = @{
            (__bridge NSString *)kIOSurfaceWidth:@64, (__bridge NSString *)kIOSurfaceHeight:@64,
            (__bridge NSString *)kIOSurfaceBytesPerElement:@4, (__bridge NSString *)kIOSurfaceBytesPerRow:@256,
            (__bridge NSString *)kIOSurfaceAllocSize:@16384, (__bridge NSString *)kIOSurfacePixelFormat:@(0x42475241u)};
        surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
        if (!surface) return NO;
        texture = [device newTextureWithDescriptor:desc iosurface:surface plane:0];
    } else texture = [device newTextureWithDescriptor:desc];
    if (!texture) { if (surface) CFRelease(surface); return NO; }
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    if (!encodeRender(device, library, texture, commands, error)) { if (surface) CFRelease(surface); return NO; }
    id<MTLBuffer> readback = nil;
    if (!surface) {
        readback = [device newBufferWithLength:16384 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        if (!readback || !blit) return NO;
        [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
            sourceSize:MTLSizeMake(64, 64, 1) toBuffer:readback destinationOffset:0
            destinationBytesPerRow:256 destinationBytesPerImage:16384];
        [blit endEncoding];
    }
    BOOL ok = completed(commands, error);
    if (ok && surface) {
        ok = IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL) == 0;
        if (ok) {
            ok = pixels(IOSurfaceGetBaseAddress(surface), IOSurfaceGetBytesPerRow(surface));
            IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
        }
    } else if (ok) ok = pixels(readback.contents, 256);
    if (surface) CFRelease(surface);
    return ok;
}
static int writeReport(NSMutableDictionary *report, NSString *path, int result)
{
    report[@"exit_status"] = @(result);
    NSData *data = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:NULL];
    if (path && ![data writeToFile:path atomically:YES]) {
        fprintf(stderr, "Could not write report\n"); return 2;
    }
    fwrite(data.bytes, 1, data.length, stdout); fputc('\n', stdout);
    return result;
}
int main(int argc, const char **argv)
{
    @autoreleasepool {
        if (argc < 3 || argc > 4) { fprintf(stderr, "usage: dre-metal-probe LIBRARY.metallib SOURCE.metal [REPORT.json]\n"); return 2; }
        NSString *path = argc == 4 ? [NSString stringWithUTF8String:argv[3]] : nil;
        NSMutableDictionary *report = [@{@"metal_workload_pass":@NO, @"native_counter_activity":@NO,
            @"metal_acceleration_pass":@NO, @"stage":@"device", @"target":@"DE2118 A619-Holi"} mutableCopy];
        NSError *error = nil;
        @try {
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            if (!device) return writeReport(report, path, 1);
            report[@"device_name"] = device.name ?: @"(unnamed)";
            report[@"stage"] = @"native_backend";
            uint32_t chip = 0;
            uint64_t submitted = 0, retired = 0;
            if (!counter("kern.dre_gpu_chip_id", &chip, sizeof(chip)) || chip != 0x06010900 ||
                !counter("kern.dre_gpu_submitted", &submitted, sizeof(submitted)) ||
                !counter("kern.dre_gpu_completed", &retired, sizeof(retired)))
                return writeReport(report, path, 1);
            report[@"stage"] = @"offline_library";
            id<MTLLibrary> offline = [device newLibraryWithFile:[NSString stringWithUTF8String:argv[1]] error:&error];
            if (!offline) { report[@"error"] = error.localizedDescription ?: @"library unavailable"; return writeReport(report,path,1); }
            id<MTLCommandQueue> queue = [device newCommandQueue];
            report[@"stage"] = @"offline_compute_private_blit";
            if (!queue || !compute(device,queue,offline,&error)) return writeReport(report,path,1);
            report[@"offline_compute_private_blit"] = @YES;
            report[@"stage"] = @"runtime_msl_compile";
            NSString *source = [NSString stringWithContentsOfFile:[NSString stringWithUTF8String:argv[2]]
                encoding:NSUTF8StringEncoding error:&error];
            id<MTLLibrary> runtime = source ? [device newLibraryWithSource:source options:nil error:&error] : nil;
            if (!runtime) { report[@"error"] = error.localizedDescription ?: @"compiler unavailable"; return writeReport(report,path,1); }
            report[@"stage"] = @"runtime_compute_private_blit";
            if (!compute(device,queue,runtime,&error)) return writeReport(report,path,1);
            report[@"runtime_compute_private_blit"] = @YES;
            report[@"stage"] = @"offscreen_render";
            if (!render(device,queue,offline,NO,&error)) return writeReport(report,path,1);
            report[@"offscreen_render"] = @YES;
            report[@"stage"] = @"iosurface_render";
            if (!render(device,queue,offline,YES,&error)) return writeReport(report,path,1);
            report[@"iosurface_render"] = @YES;
            report[@"metal_workload_pass"] = @YES;
            report[@"stage"] = @"native_completion_counters";
            uint64_t afterSubmitted = 0, afterRetired = 0;
            if (!counter("kern.dre_gpu_submitted", &afterSubmitted, sizeof(afterSubmitted)) ||
                !counter("kern.dre_gpu_completed", &afterRetired, sizeof(afterRetired)) ||
                afterSubmitted < submitted || afterRetired < retired ||
                afterSubmitted - submitted < 4 || afterRetired - retired < 4)
                return writeReport(report,path,1);
            report[@"native_submissions_delta"] = @(afterSubmitted - submitted);
            report[@"native_completions_delta"] = @(afterRetired - retired);
            report[@"native_counter_activity"] = @YES;
            report[@"metal_acceleration_pass"] = @YES;
            report[@"stage"] = @"PASS";
            return writeReport(report,path,0);
        } @catch (NSException *exception) {
            report[@"exception"] = exception.name;
            return writeReport(report,path,1);
        }
    }
}
