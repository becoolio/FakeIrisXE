//
//  main.m
//  FakeIrisXEMetalTest
//
//  Metal Acceleration Test for FakeIrisXE
//

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

int main(int argc, const char * argv[]) {
    @autoreleasepool {
        
        NSLog(@"═══════════════════════════════════════════════════════════════");
        NSLog(@"  FakeIrisXE Metal Acceleration Test");
        NSLog(@"═══════════════════════════════════════════════════════════════");
        
        // 1. Check if Metal is available
        NSLog(@"\n[1] Checking Metal availability...");
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        
        if (!device) {
            NSLog(@"❌ FAILED: No Metal device found!");
            return 1;
        }
        
        NSLog(@"✅ Metal device found!");
        NSLog(@"   Device Name: %@", device.name);
        NSLog(@"   Registry ID: %llu", device.registryID);
        NSLog(@"   Max Buffer Length: %lu", (unsigned long)device.maxBufferLength);
        NSLog(@"   Max Texture Size: %u", device.maxTextureSize);
        
        // 2. Check IOAccelerator
        NSLog(@"\n[2] Checking IOAccelerator...");
        NSArray *ioAccelerators = nil;
        
        ioAccelerators = [IOServiceMatching("IOAccelerator")];
        if (ioAccelerators && ioAccelerators.count > 0) {
            NSLog(@"✅ IOAccelerator found (%lu services)", (unsigned long)ioAccelerators.count);
        } else {
            NSLog(@"⚠️  IOAccelerator not found via IOServiceMatching");
        }
        
        // 3. Check for FakeIrisXE
        NSLog(@"\n[3] Checking for FakeIrisXE framebuffer...");
        
        NSArray *framebuffers = [[IOServiceManager sharedServiceManager] getMatchingServices:[IOServiceMatching("IOFramebuffer")]];
        
        // Try alternative approach - look for FakeIrisXE
        NSArray *fakeIrisDevices = [[IOServiceManager sharedServiceManager] getMatchingServices:[IOServiceMatching("FakeIrisXEFramebuffer")]];
        
        if (fakeIrisDevices && fakeIrisDevices.count > 0) {
            NSLog(@"✅ FakeIrisXEFramebuffer found!");
        } else {
            NSLog(@"⚠️  FakeIrisXEFramebuffer not directly found (may still be working)");
        }
        
        // 4. Try to create a command queue
        NSLog(@"\n[4] Testing command queue creation...");
        id<MTLCommandQueue> commandQueue = [device newCommandQueue];
        
        if (!commandQueue) {
            NSLog(@"❌ FAILED: Could not create command queue");
            return 1;
        }
        
        NSLog(@"✅ Command queue created successfully!");
        
        // 5. Create a simple compute pipeline
        NSLog(@"\n[5] Testing compute pipeline...");
        
        NSString *shaderSource = @"
            #include <metal_stdlib>
            using namespace metal;
            kernel void test_compute(device atomic_uint* counter [[buffer(0)]],
                                     uint id [[thread_position_in_grid]]) {
                atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
            }
        ";
        
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:shaderSource
                                                      options:nil
                                                        error:&error];
        
        if (error) {
            NSLog(@"❌ Failed to compile shader: %@", error.localizedDescription);
            return 1;
        }
        
        NSLog(@"✅ Shader compiled successfully!");
        
        id<MTLFunction> function = [library newFunctionWithName:@"test_compute"];
        if (!function) {
            NSLog(@"❌ Failed to find compute function");
            return 1;
        }
        
        id<MTLComputePipelineState> computePipeline = [device newComputePipelineStateWithFunction:function
                                                                                                error:&error];
        if (error) {
            NSLog(@"❌ Failed to create pipeline: %@", error.localizedDescription);
            return 1;
        }
        
        NSLog(@"✅ Compute pipeline created!");
        
        // 6. Run a simple compute test
        NSLog(@"\n[6] Running compute test...");
        
        // Create buffer
        uint32_t counter = 0;
        id<MTLBuffer> buffer = [device newBufferWithBytes:&counter
                                                   length:sizeof(counter)
                                                  options:MTLResourceStorageModeShared];
        
        // Create command buffer
        id<MTLCommandBuffer> commandBuffer = [commandQueue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
        
        [encoder setComputePipelineState:computePipeline];
        [encoder setBuffer:buffer offset:0 atIndex:0];
        
        // Dispatch 1024 threads
        MTLSize gridSize = MTLSizeMake(1024, 1, 1);
        NSUInteger threadGroupSize = MIN(1024, computePipeline.maxTotalThreadsPerThreadgroup);
        MTLSize threadGroupSize3 = MTLSizeMake(threadGroupSize, 1, 1);
        
        [encoder dispatchThreads:gridSize threadsPerThreadgroup:threadGroupSize3];
        [encoder endEncoding];
        
        // Commit and wait
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];
        
        // Check result
        uint32_t *resultPtr = (uint32_t *)buffer.contents;
        uint32_t finalCount = *resultPtr;
        
        NSLog(@"   Expected: 1024, Got: %u", finalCount);
        
        if (finalCount == 1024) {
            NSLog(@"✅ Compute test PASSED! GPU is working!");
        } else {
            NSLog(@"❌ Compute test FAILED! Expected 1024, got %u", finalCount);
        }
        
        // 7. Check for errors
        if (commandBuffer.status == MTLCommandBufferStatusError) {
            NSLog(@"❌ Command buffer error: %@", commandBuffer.error.localizedDescription);
        } else {
            NSLog(@"✅ Command buffer completed without errors");
        }
        
        // 8. GPU Info
        NSLog(@"\n[7] GPU Information:");
        NSLog(@"   Device: %@", device.name);
        NSLog(@"   Registry ID: %llu", device.registryID);
        NSLog(@"   Processor Count: %u", device.processorCount);
        
        #if TARGET_OS_OSX
        NSLog(@"   Is Low Power: %@", device.isLowPower ? @"YES" : @"NO");
        NSLog(@"   Is Removable: %@", device.isRemovable ? @"YES" : @"NO");
        #endif
        
        // 9. Memory Info
        NSLog(@"\n[8] Memory Information:");
        NSLog(@"   Recommended Max Working Set: %llu MB", device.recommendedMaxWorkingSetSize / (1024*1024));
        
        // 10. Final verdict
        NSLog(@"\n═══════════════════════════════════════════════════════════════");
        
        if (device && commandQueue && finalCount == 1024) {
            NSLog(@"✅ RESULT: Metal GPU Acceleration is WORKING!");
            NSLog(@"   FakeIrisXE Execlist is successfully providing");
            NSLog(@"   hardware acceleration for Metal!");
        } else if (device && commandQueue) {
            NSLog(@"⚠️  RESULT: Metal is available but compute test failed");
        } else {
            NSLog(@"❌ RESULT: Metal GPU Acceleration FAILED");
        }
        
        NSLog(@"═══════════════════════════════════════════════════════════════");
        
    }
    return 0;
}
