static NSUInteger diagnostic_profile(id self, SEL selector)
{
    (void)self; (void)selector;
    return 1;
}
static Class diagnostic_device_class(Class base)
{
    if (!base || !method_matches(base, NO, "featureProfile", "Q16@0:8")) {
        fail("diagnostic featureProfile encoding mismatch"); return Nil;
    }
    const char *name = "RedwoodMetalDeviceSBMPSDiagnostic";
    if (objc_getClass(name)) { fail("diagnostic class already registered"); return Nil; }
    Class diagnostic = objc_allocateClassPair(base, name, 0);
    if (!diagnostic) { fail("diagnostic class allocation failed"); return Nil; }
    if (!class_addMethod(diagnostic, sel_registerName("featureProfile"),
        (IMP)diagnostic_profile, "Q16@0:8")) {
        objc_disposeClassPair(diagnostic);
        fail("diagnostic profile method installation failed"); return Nil;
    }
    objc_registerClassPair(diagnostic);
    fprintf(stderr, "sbmetal26: diagnostic_profile=1 scope=SpringBoard_process apple2_conformance=unverified\n");
    return diagnostic;
}
