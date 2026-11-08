/* Compare combined NV12 sampling with native Metal measurements. No presentation UI. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurfaceRef.h>
#include <math.h>
#include <signal.h>
#include <unistd.h>
static void deadline(int signal) { (void)signal; _exit(124); }
static const float expected[4][4] = {
    {.419433594f,.166503906f,0,1}, {.444335938f,.179199219f,0,1},
    {.426269531f,.204589844f,.029785156f,1}, {.447753906f,.215820312f,.019042969f,1}
};
static unsigned compare(const float *p, const float *want) {
    unsigned errors=0;
    for(unsigned c=0;c<4;c++) errors += !isfinite(p[c]) || fabsf(p[c]-want[c]) > .002f;
    return errors;
}
static BOOL submit(id<MTLCommandBuffer> command) {
    [command commit]; [command waitUntilCompleted];
    if(command.status==MTLCommandBufferStatusCompleted && !command.error) return YES;
    printf("video_combined: status=%lu error=%s:%ld\n",(unsigned long)command.status,
        command.error.domain.UTF8String ?: "-",(long)command.error.code);
    return NO;
}
int main(int argc,char **argv) { @autoreleasepool { @try {
    setbuf(stdout,NULL); signal(SIGALRM,deadline); alarm(30);
    if(argc!=2) {fprintf(stderr,"usage: video_combined_test LIBRARY.metallib\n");return 2;}
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue=[device newCommandQueue]; NSError *error=nil;
    id<MTLLibrary> library=[device newLibraryWithFile:[NSString stringWithUTF8String:argv[1]] error:&error];
    if(!library) {printf("video_combined: library error=%s:%ld\n",error.domain.UTF8String ?: "-",(long)error.code);return 3;}
    id<MTLFunction> compute=[library newFunctionWithName:@"combined_sample"];
    id<MTLComputePipelineState> cp=[device newComputePipelineStateWithFunction:compute error:&error];
    MTLRenderPipelineDescriptor *rd=[MTLRenderPipelineDescriptor new];
    rd.vertexFunction=[library newFunctionWithName:@"combined_vertex"];
    rd.fragmentFunction=[library newFunctionWithName:@"combined_fragment"];
    rd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA32Float;
    id<MTLRenderPipelineState> rp=[device newRenderPipelineStateWithDescriptor:rd error:&error];
    if(!cp || !rp) {printf("video_combined: pipeline error=%s:%ld\n",error.domain.UTF8String ?: "-",(long)error.code);return 4;}
    MTLTextureDescriptor *gd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:1 height:1 mipmapped:NO];
    gd.usage=MTLTextureUsageShaderRead;gd.storageMode=MTLStorageModeShared;
    id<MTLTexture> guard=[device newTextureWithDescriptor:gd];uint8_t value=128;
    [guard replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:&value bytesPerRow:1];
    unsigned total=0;
    for(unsigned i=0;i<4;i++) {
        CVPixelBufferRef pixel=NULL;unsigned full=i/2;
        if(CVPixelBufferCreate(NULL,32,16,full?kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            (CFDictionaryRef)@{(id)kCVPixelBufferIOSurfacePropertiesKey:@{}},&pixel)) return 5;
        CVBufferSetAttachment(pixel,kCVImageBufferYCbCrMatrixKey,i%2?kCVImageBufferYCbCrMatrix_ITU_R_709_2:kCVImageBufferYCbCrMatrix_ITU_R_601_4,kCVAttachmentMode_ShouldPropagate);
        CVPixelBufferLockBaseAddress(pixel,0);
        for(unsigned p=0;p<2;p++) {
            uint8_t *base=CVPixelBufferGetBaseAddressOfPlane(pixel,p);
            size_t stride=CVPixelBufferGetBytesPerRowOfPlane(pixel,p),w=CVPixelBufferGetWidthOfPlane(pixel,p),h=CVPixelBufferGetHeightOfPlane(pixel,p);
            for(size_t y=0;y<h;y++) for(size_t x=0;x<w*(p+1);x++)base[y*stride+x]=p?(x%2?160:96):64;
        }
        CVPixelBufferUnlockBaseAddress(pixel,0);
        MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)500 width:32 height:16 mipmapped:NO];
        d.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;d.storageMode=MTLStorageModeShared;
        IOSurfaceRef surface=CVPixelBufferGetIOSurface(pixel);
        id<MTLTexture> texture=[device newTextureWithDescriptor:d iosurface:surface plane:0];
        if(!texture || texture.pixelFormat!=500 || texture.iosurface!=surface || texture.iosurfacePlane!=0) return 6;
        id<MTLBuffer> out=[device newBufferWithLength:32 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb=[queue commandBuffer];id<MTLComputeCommandEncoder> enc=[cb computeCommandEncoder];
        [enc setComputePipelineState:cp];[enc setTexture:texture atIndex:0];[enc setTexture:guard atIndex:1];[enc setBuffer:out offset:0 atIndex:0];
        [enc dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];[enc endEncoding];
        MTLTextureDescriptor *targetDesc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:1 height:1 mipmapped:NO];
        targetDesc.usage=MTLTextureUsageRenderTarget;targetDesc.storageMode=MTLStorageModeShared;
        id<MTLTexture> target=[device newTextureWithDescriptor:targetDesc];
        MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture=target;pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        id<MTLRenderCommandEncoder> render=[cb renderCommandEncoderWithDescriptor:pass];
        [render setRenderPipelineState:rp];[render setFragmentTexture:texture atIndex:0];[render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[render endEncoding];
        [texture release];CFRelease(pixel);
        if(!submit(cb))return 7;
        float *v=out.contents,rgba[4],guardExpected[4]={128.f/255.f,0,0,1};
        [target getBytes:rgba bytesPerRow:16 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0];
        unsigned errors=compare(v,expected[i])+compare(v+4,guardExpected)+compare(rgba,expected[i]);total+=errors;
        printf("video_combined: full=%u matrix=%u status=%lu compute=%.9f,%.9f,%.9f,%.9f fragment=%.9f,%.9f,%.9f,%.9f errors=%u\n",
            full,i%2?709:601,(unsigned long)cb.status,v[0],v[1],v[2],v[3],rgba[0],rgba[1],rgba[2],rgba[3],errors);
        [out release];[target release];
    }
    [guard release];[rd.vertexFunction release];[rd.fragmentFunction release];[rd release];[rp release];[cp release];[compute release];[library release];[queue release];[device release];alarm(0);
    return total?1:0;
} @catch(NSException *e) {printf("video_combined: exception=%s\n",e.name.UTF8String);return 8;} } }
