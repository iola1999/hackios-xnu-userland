/* Compare combined NV12/P010 samples against native NV12 results and YCbCr matrices. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurfaceRef.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <unistd.h>
static void deadline(int signal) { (void)signal; _exit(124); }
static const float expected[2][6][4] = {
    {
        {.419433594f,.166503906f,0,1}, {.444335938f,.179199219f,0,1},
        {.429835225f,.161063796f,0,1},
        {.426269531f,.204589844f,.029785156f,1}, {.447753906f,.215820312f,.019042969f,1},
        {.436028235f,.199930980f,.014883137f,1}
    },
    /* P010 codes Y=256, Cb=384, Cr=640; limited ranges 64..940/64..960,
     * full range 0..1023 with chroma centered at 512 (ITU-R BT.601/709/2020). */
    {
        {.419463796f,.166320939f,0,1}, {.444149511f,.179063796f,0,1},
        {.429835225f,.161063796f,0,1},
        {.425665689f,.203949169f,.028527859f,1}, {.447286804f,.215110068f,.018067644f,1},
        {.434749560f,.199344673f,.014839492f,1}
    }
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
    BOOL native=NO,p010=NO;int libraryArg=1;
    while(libraryArg<argc-1) {
        if(!strcmp(argv[libraryArg],"--native"))native=YES;
        else if(!strcmp(argv[libraryArg],"--p010"))p010=YES;
        else break;
        libraryArg++;
    }
    if(libraryArg!=argc-1 || argv[libraryArg][0]=='-') {fprintf(stderr,"usage: video_combined_test [--native] [--p010] LIBRARY.metallib\n");return 2;}
    id<MTLDevice> device=nil;
    if(native) device=MTLCreateSystemDefaultDevice();
    else {
        int console=open("/dev/console",O_WRONLY|O_NOCTTY);
        if(console>=0) {dup2(console,STDOUT_FILENO);dup2(console,STDERR_FILENO);if(console>STDERR_FILENO)close(console);}
        unsetenv("RWA_SOFTWARE");
        if(!dlopen("/annibale/lib/libannibalemetal.dylib",RTLD_NOW|RTLD_LOCAL)) {printf("video_combined: driver load failed\n");return 2;}
        Class cls=objc_getClass("AnnibaleMetalDevice");SEL init=sel_registerName("initWithAcceleratorPort:");
        if(!cls || ![cls instancesRespondToSelector:init])return 2;
        device=((id(*)(id,SEL,uint32_t))objc_msgSend)([cls alloc],init,0);
        void *air=dlopen("/annibale/lib/libannibaleair.dylib",RTLD_NOW|RTLD_NOLOAD);
        const char *(*device_name)(void *)=air?dlsym(air,"rwa_device_name"):NULL;
        SEL mesa=sel_registerName("mesaDevice");
        if(!device_name || ![device respondsToSelector:mesa])return 2;
        const char *name=device_name(((void *(*)(id,SEL))objc_msgSend)(device,mesa));
        if(!name || strstr(name,"softpipe"))return 2;
        printf("video_combined: backend=%s\n",name);
    }
    id<MTLCommandQueue> queue=[device newCommandQueue]; NSError *error=nil;
    if(!device || !queue)return 2;
    NSData *data=[NSData dataWithContentsOfFile:[NSString stringWithUTF8String:argv[libraryArg]]];
    if(!data.length)return 3;
    dispatch_data_t wrapped=dispatch_data_create(data.bytes,data.length,NULL,DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    id<MTLLibrary> library=[device newLibraryWithData:wrapped error:&error];
    dispatch_release(wrapped);
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
    CFStringRef matrices[]={kCVImageBufferYCbCrMatrix_ITU_R_601_4,kCVImageBufferYCbCrMatrix_ITU_R_709_2,kCVImageBufferYCbCrMatrix_ITU_R_2020};
    unsigned matrixNames[]={601,709,2020};MTLPixelFormat format=(MTLPixelFormat)(p010?505:500);
    for(unsigned i=0;i<6;i++) {
        CVPixelBufferRef pixel=NULL;unsigned full=i/3;
        OSType pixelFormat=p010?(full?kCVPixelFormatType_420YpCbCr10BiPlanarFullRange:kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange)
            :(full?kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
        if(CVPixelBufferCreate(NULL,32,16,pixelFormat,
            (CFDictionaryRef)@{(id)kCVPixelBufferIOSurfacePropertiesKey:@{}},&pixel)) return 5;
        CVBufferSetAttachment(pixel,kCVImageBufferYCbCrMatrixKey,matrices[i%3],kCVAttachmentMode_ShouldPropagate);
        CVPixelBufferLockBaseAddress(pixel,0);
        for(unsigned p=0;p<2;p++) {
            uint8_t *base=CVPixelBufferGetBaseAddressOfPlane(pixel,p);
            size_t stride=CVPixelBufferGetBytesPerRowOfPlane(pixel,p),w=CVPixelBufferGetWidthOfPlane(pixel,p),h=CVPixelBufferGetHeightOfPlane(pixel,p);
            for(size_t y=0;y<h;y++) for(size_t x=0;x<w*(p+1);x++) {
                uint16_t code=p?(x%2?160:96):64;
                if(p010)((uint16_t *)(base+y*stride))[x]=code<<8;
                else base[y*stride+x]=(uint8_t)code;
            }
        }
        CVPixelBufferUnlockBaseAddress(pixel,0);
        MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:32 height:16 mipmapped:NO];
        d.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;d.storageMode=MTLStorageModeShared;
        IOSurfaceRef surface=CVPixelBufferGetIOSurface(pixel);
        id<MTLTexture> texture=[device newTextureWithDescriptor:d iosurface:surface plane:0];
        if(!texture || texture.pixelFormat!=format || texture.iosurface!=surface || texture.iosurfacePlane!=0) return 6;
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
        unsigned errors=compare(v,expected[p010][i])+compare(v+4,guardExpected)+compare(rgba,expected[p010][i]);total+=errors;
        printf("video_combined: depth=%u full=%u matrix=%u status=%lu compute=%.9f,%.9f,%.9f,%.9f fragment=%.9f,%.9f,%.9f,%.9f errors=%u\n",
            p010?10:8,full,matrixNames[i%3],(unsigned long)cb.status,v[0],v[1],v[2],v[3],rgba[0],rgba[1],rgba[2],rgba[3],errors);
        [out release];[target release];
    }
    [guard release];[rd.vertexFunction release];[rd.fragmentFunction release];[rd release];[rp release];[cp release];[compute release];[library release];[queue release];[device release];alarm(0);
    return total?1:0;
} @catch(NSException *e) {printf("video_combined: exception=%s\n",e.name.UTF8String);return 8;} } }
