/* The processes this library serves. bundle is the application whose compression preference has to
   be set before QuartzCore picks a surface format, NULL for a daemon that has no bundle of its own.
   baseline is the SB26 library, NULL where it would load and return without doing anything.
   switch_value stands in for REDWOOD_SB_METAL where no launchd job can carry it: MercuryPoster is an
   ExtensionKit service started by xpcproxy, so its inject.conf line is its switch, and its stderr
   goes nowhere, so the outcome is put in the kernel log as well. */
struct metal_target { const char *process; const char *bundle; const char *baseline; const char *switch_value; };
static const struct metal_target metal_targets[] = {
    { "SpringBoard", "com.apple.springboard", "/redwood/lib/libredwood-sb26.dylib", NULL },
    { "backboardd", NULL, NULL, NULL },
    { "iconservicesagent", NULL, NULL, NULL },
    { "MercuryPosterExtension", NULL, NULL, "1" },
};
static const struct metal_target *metal_target(const char *process)
{
    for (unsigned i = 0; process && i < sizeof(metal_targets) / sizeof(metal_targets[0]); i++)
        if (!strcmp(process, metal_targets[i].process)) return &metal_targets[i];
    return NULL;
}
/* The original SB26 constructor runs after optional process-local registration. */
static void run_sequence(const char *process, const char *build, const char *flag)
{
    const struct metal_target *target = metal_target(process);
    if (!target) return;
    if (target->switch_value) flag = target->switch_value;
    if (build && !strcmp(build, "23G90") && flag && !strcmp(flag, "1")) {
        @try { attempt_registration(target->bundle); }
        @catch (NSException *exception) { registration_exception(exception); }
    } else { registration_skipped(build, flag); }
    if (target->switch_value) {
        char line[160];
        snprintf(line, sizeof(line), "sbmetal26: %s[%d] registration device=%d\n", process, getpid(), held_device != nil);
        sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line) + 1);
    }
    if (target->baseline) load_baseline(target->baseline);
}
