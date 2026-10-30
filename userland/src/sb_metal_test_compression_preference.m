#import <Foundation/Foundation.h>
#include <assert.h>
#include <stdio.h>
static int calls;
static bool native_value;
static CFStringRef domain;
static BOOL fail(const char *s) { fprintf(stderr,"expected failure: %s\n",s); return NO; }
static bool native_use(void) {
    Boolean valid=false;
    assert(!CFPreferencesGetAppBooleanValue(CFSTR("CAEnableIOSurfaceCompression"),domain,&valid) && valid);
    ++calls;return native_value;
}
#include "sb_metal_compression_preference.inc"
int main(void) { @autoreleasepool {
    NSString *name=[@"org.hackios.host-compression-test." stringByAppendingString:NSUUID.UUID.UUIDString];
    domain=(CFStringRef)name;
    CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"),kCFBooleanTrue,domain);
    assert(apply_uncompressed_preference(domain,native_use));assert(calls==1);
    native_value=true;
    assert(!apply_uncompressed_preference(domain,native_use));assert(calls==2);
    CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"),NULL,domain);
    puts("COMPRESSION_PREFERENCE_PASS cases=2 false_before_native_query=1 stale_native_true_rejected=1");
} return 0; }
