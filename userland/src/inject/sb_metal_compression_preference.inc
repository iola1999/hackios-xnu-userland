/* Use the same application domain queried by the native QuartzCore initializer. */
static BOOL apply_uncompressed_preference(CFStringRef application, bool (*native_use)(void))
{
    CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"), kCFBooleanFalse, application);
    Boolean valid = false;
    Boolean value = CFPreferencesGetAppBooleanValue(CFSTR("CAEnableIOSurfaceCompression"), application, &valid);
    fprintf(stderr, "sbmetal26: compression preference valid=%d value=%d\n", valid, value);
    if (!valid || value) return fail("compression preference readback mismatch");
    /* The native dispatch_once may have run before this constructor. Verify its actual result. */
    bool enabled = native_use();
    fprintf(stderr, "sbmetal26: compression native_use=%d\n", enabled);
    if (enabled) return fail("native compression policy remains enabled");
    return YES;
}
