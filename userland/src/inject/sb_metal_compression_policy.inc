#include <ptrauth.h>
#include "sb_metal_compression_preference.inc"
static BOOL prepare_uncompressed_policy(const char *want)
{
    const char *bundle = [NSBundle mainBundle].bundleIdentifier.UTF8String;
    fprintf(stderr, "sbmetal26: compression application=%s expected=%s\n", bundle ?: "nil", want ?: "none");
    /* A daemon has no bundle to match. The write goes to kCFPreferencesCurrentApplication, and
       apply_uncompressed_preference then asks QuartzCore's own getter, so a domain the two do not
       share fails there instead of leaving compression quietly enabled. */
    if (want && (!bundle || strcmp(bundle, want))) return fail("compression application mismatch");
    void *anchor = dlsym(RTLD_DEFAULT, "CACurrentMediaTime");
    Dl_info image = { 0 };
    if (!anchor || !dladdr(anchor, &image) || !image.dli_fbase || !image.dli_fname ||
        strcmp(image.dli_fname, "/System/Library/Frameworks/QuartzCore.framework/QuartzCore"))
        return fail("QuartzCore image unavailable or mismatch");
    /* 23G90 arm64e cache: local CADeviceUseCompression at image + 0x2b7140.
       These bytes include its dispatch_once test and final policy combination. */
    static const unsigned char expected[] = {
        0x48,0x60,0x34,0xf0,0x08,0x29,0x41,0xf9,0x1f,0x05,0x00,0xb1,0x01,0x01,0x00,0x54,
        0x08,0xda,0x32,0xf0,0x08,0x45,0x53,0x39,0x49,0x60,0x34,0xd0,0x29,0x61,0x65,0x39,
        0x28,0x01,0x28,0x0a,0x00,0x01,0x00,0x12,0xc0,0x03,0x5f,0xd6,0x7f,0x23,0x03,0xd5,
        0xfd,0x7b,0xbf,0xa9,0xfd,0x03,0x00,0x91,0x40,0x60,0x34,0xf0,0x00,0x40,0x09,0x91
    };
    void *entry = (char *)image.dli_fbase + 0x2b7140;
    if (memcmp(entry, expected, sizeof(expected))) return fail("QuartzCore compression getter bytes mismatch");
    bool (*native_use)(void) = (bool (*)(void))ptrauth_sign_unauthenticated(entry, ptrauth_key_function_pointer, 0);
    fprintf(stderr, "sbmetal26: compression getter_image=%s offset=0x2b7140 bytes_match=1\n", image.dli_fname);
    return apply_uncompressed_preference(kCFPreferencesCurrentApplication, native_use);
}
