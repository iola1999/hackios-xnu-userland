/*
 * The bootstrap service namespace as launchd on this platform actually serves it, probed to tell
 * apart why com.apple.PowerManagement.control does not resolve (IOPSCreatePowerSource fails with
 * 0xe00002cd before any MIG message is sent: bootstrap_look_up2 cannot find the service).
 * Three candidate families remain after the static checks (out/goal-1124/batch7/powerd-mig-root-cause.md):
 *
 *   family A   the endpoint was imported at job load and invalidated later (the receive right died
 *              on its way to or inside powerd; control is powerd's only legacy-checkin service,
 *              the other four check in through XPC)
 *   family B   the endpoint was never imported: launchd loaded the job but the MachServices
 *              endpoints never entered the namespace; then all five powerd services are absent
 *   family C   the flags=8 lookup path itself is degraded: plain bootstrap_look_up resolves a
 *              service that bootstrap_look_up2(..., 0, 8) does not
 *
 *   svcprobe              one pass
 *   svcprobe N S          N passes, S seconds apart (default S 120): the first pass runs as early
 *                         as the invoking rc.jobs line executes, the last pass minutes later; the
 *                         timing verdict compares them (an endpoint that dies at runtime supports
 *                         family A, one that is absent from the earliest probe supports family B)
 *
 * One pass looks up every service below with all three lookup methods and prints the raw
 * kern_return of each, registers a service of its own and looks it back up (the register-lookup
 * loop itself can be excluded or incriminated), then destroys the port and looks the name up
 * once more (launchd's death-notification invalidation path, the mechanism family A accuses).
 * Every line is self-contained for unattended grep judgement; the verdict lines are:
 *
 *   verdict channel       the lookup channel itself, from the positive controls
 *   verdict flags8        whether flags=8 lookups fail where plain ones succeed, and for how many services
 *   verdict target        the candidate-family judgement for com.apple.PowerManagement.control
 *   verdict loopback      the register-lookup roundtrip of this process's own service
 *   verdict invalidation  whether a registered endpoint dies with its port (family A's mechanism, sign-inverted)
 *   verdict timing        first pass against last pass (multi-pass runs only)
 */
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* the bootstrap client interface: the SDK ships mach/bootstrap.h empty, the symbols are in libSystem */
extern mach_port_t bootstrap_port;
extern kern_return_t bootstrap_look_up(mach_port_t bp, const char *service_name, mach_port_t *sp);
extern kern_return_t bootstrap_look_up2(mach_port_t bp, const char *service_name, mach_port_t *sp, pid_t target_pid,
    uint64_t flags);
extern kern_return_t bootstrap_register(mach_port_t bp, const char *service_name, mach_port_t sp);

/* the lookup methods, as the design orders them: the legacy call and the look_up2 flags the
   IOKit client of powerd uses (0x8, the privileged-forwarding bit family C is about) */
enum { M_PLAIN, M_F0, M_F8, NMETHODS };
static const char *const method_name[NMETHODS] = { "plain", "flags0", "flags8" };

/* the service list: the target, powerd's four other MachServices (same job), two positive
   controls from daemons unrelated to powerd, and a name nothing registers as the error-code
   baseline (what "unknown service" actually numerically is on this launchd) */
enum { S_TARGET, S_POWERDXPC, S_LOWPOWER, S_SMARTNAP, S_CORENAP, S_NOTIFYD, S_SYSLOGD, S_BASELINE, NSERVICES };
static const struct {
	const char *name;
	const char *role;
	int sibling;
	int positive;
} services[NSERVICES] = {
	[S_TARGET]    = { "com.apple.PowerManagement.control", "target", 0, 0 },
	[S_POWERDXPC] = { "com.apple.iokit.powerdxpc", "sibling", 1, 0 },
	[S_LOWPOWER]  = { "com.apple.powerd.lowpowermode", "sibling", 1, 0 },
	[S_SMARTNAP]  = { "com.apple.powerd.smartpowernap", "sibling", 1, 0 },
	[S_CORENAP]   = { "com.apple.powerd.coresmartpowernap", "sibling", 1, 0 },
	[S_NOTIFYD]   = { "com.apple.system.notification_center", "positive", 0, 1 },
	[S_SYSLOGD]   = { "com.apple.system.logger", "positive", 0, 1 },
	[S_BASELINE]  = { "com.annibale.svcprobe.no-such-service", "baseline", 0, 0 },
};

/* the loopback service this process registers; the same name every pass, so a stale endpoint
   surviving the first pass's port death shows up as NAME_IN_USE in the next pass's register */
#define LOOPBACK_NAME "com.annibale.svcprobe.loopback"

/* seconds between destroying the loopback port and re-looking it up: dead-name delivery is
   a matter of milliseconds on an idle system, one second is generous settling time */
#define DESTROY_SETTLE 1

/* seconds since boot, wall-clock adjustments excluded: the passes are placed against boot and
   against one another, and a network time correction must not move them */
static long
uptime(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return -1;
	}
	return ts.tv_sec;
}

/* what the number means, for the lines humans end up reading; the canonical bootstrap server
   codes (they travel as plain integers, outside the mach err_system encoding) */
static const char *
decode(kern_return_t kr)
{
	switch (kr) {
	case KERN_SUCCESS: return "SUCCESS";
	case 1100: return "BOOTSTRAP_NOT_PRIVILEGED";
	case 1101: return "BOOTSTRAP_NAME_IN_USE";
	case 1102: return "BOOTSTRAP_UNKNOWN_SERVICE";
	case 1103: return "BOOTSTRAP_SERVICE_ACTIVE";
	case 1104: return "BOOTSTRAP_BAD_COUNT";
	case 1105: return "BOOTSTRAP_NO_MEMORY";
	case 1106: return "BOOTSTRAP_NO_CHILDREN";
	default: return "unrecognized";
	}
}

/* one lookup with one method; the send right a successful lookup hands back is deallocated
   at once, the probe does not hold service ports */
static kern_return_t
lookup(const char *name, int method)
{
	mach_port_t port = MACH_PORT_NULL;
	kern_return_t kr;

	if (method == M_PLAIN) {
		kr = bootstrap_look_up(bootstrap_port, name, &port);
	} else {
		kr = bootstrap_look_up2(bootstrap_port, name, &port, 0, method == M_F8 ? 8 : 0);
	}
	if (kr == KERN_SUCCESS && port != MACH_PORT_NULL) {
		mach_port_deallocate(mach_task_self(), port);
	}
	return kr;
}

static void
print_lookup(int passno, const char *role, const char *name, int method, kern_return_t kr)
{
	printf("svcprobe: pass %d lookup %s %s %s: %d 0x%x (%s)\n", passno, role, name, method_name[method], kr, kr,
	    decode(kr));
}

static int ok(kern_return_t kr)
{
	return kr == KERN_SUCCESS;
}

/* whether service s's failure with method m looks like "no such service": same code the
   nonexistent baseline name returned with the same method */
static int
absent_like(kern_return_t matrix[NSERVICES][NMETHODS], int s, int m)
{
	return matrix[s][m] != KERN_SUCCESS && matrix[s][m] == matrix[S_BASELINE][m];
}

/* drop every right this process holds in the loopback port; when the receive right goes the
   port dies and launchd's registered send right becomes a dead name, which is what the
   invalidation re-probe relies on (mach_port_destroy would do it in one call but is deprecated) */
static void
kill_port(mach_port_t port, int with_send)
{
	if (with_send) {
		mach_port_deallocate(mach_task_self(), port);
	}
	mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1);
}

/* the register-lookup loopback plus the death-invalidation re-probe; verdicts are printed
   where they are computed so a crashed pass still leaves the evidence before them */
static void
loopback(int passno)
{
	mach_port_t port = MACH_PORT_NULL;
	kern_return_t kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);

	if (kr != KERN_SUCCESS) {
		printf("svcprobe: pass %d verdict loopback: loop-skipped (mach_port_allocate failed: %d)\n", passno, kr);
		return;
	}
	kr = mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("svcprobe: pass %d verdict loopback: loop-skipped (mach_port_insert_right failed: %d)\n", passno, kr);
		kill_port(port, 0);
		return;
	}

	kr = bootstrap_register(bootstrap_port, LOOPBACK_NAME, port);
	printf("svcprobe: pass %d loopback register %s: %d 0x%x (%s)\n", passno, LOOPBACK_NAME, kr, kr, decode(kr));
	if (kr != KERN_SUCCESS) {
		/* NAME_IN_USE can only come from an earlier registration of this probe's own name (an
		   earlier pass of this process or an earlier svcprobe process this boot): the endpoint
		   still occupies the namespace although its port was destroyed, invalidation-lag evidence
		   in its own right */
		if (kr == 1101) {
			printf("svcprobe: pass %d verdict loopback: loop-register-stale (the name is still in use from an earlier "
			    "registration although its port was destroyed: the endpoint outlives its port, arguing against "
			    "family A)\n", passno);
			printf("svcprobe: pass %d verdict invalidation: endpoint-survives-death (an earlier registration still "
			    "occupies the namespace long after its port died)\n", passno);
		} else {
			printf("svcprobe: pass %d verdict loopback: loop-register-%s (register did not complete, the loop cannot "
			    "be tested; the positive controls carry the channel-health evidence)\n", passno,
			    kr == 1100 ? "denied" : "error");
			printf("svcprobe: pass %d verdict invalidation: invalidation-untestable (nothing was registered)\n", passno);
		}
		kill_port(port, 1);
		return;
	}

	kern_return_t look[NMETHODS];
	int visible = 0;

	for (int m = 0; m < NMETHODS; m++) {
		look[m] = lookup(LOOPBACK_NAME, m);
		printf("svcprobe: pass %d loopback lookup %s: %d 0x%x (%s)\n", passno, method_name[m], look[m], look[m],
		    decode(look[m]));
		visible += ok(look[m]);
	}
	if (visible == NMETHODS) {
		printf("svcprobe: pass %d verdict loopback: loop-healthy (register-lookup roundtrip works with all three methods; "
		    "the lookup channel is not wholesale degraded)\n", passno);
	} else if (visible == 0) {
		printf("svcprobe: pass %d verdict loopback: loop-broken-lookup (register succeeded but no lookup method finds "
		    "the service: what launchd writes is not what lookups read)\n", passno);
	} else {
		/* the failing methods are named because they are not symmetric evidence: a flags8 failure
		   here calibrates what the privileged lookup path does with a legacy-registered service,
		   a plain failure breaks the loop itself */
		printf("svcprobe: pass %d verdict loopback: loop-partial (plain %d, flags0 %d, flags8 %d: "
		    "%d of %d lookup methods find the self-registered service)\n", passno, look[M_PLAIN], look[M_F0], look[M_F8],
		    visible, NMETHODS);
	}

	/* the port now dies on purpose: the receive right goes, launchd's send right becomes a dead
	   name, and a healthy launchd invalidates the endpoint. This is the invalidation mechanism
	   family A accuses of firing when it should not; the re-lookup checks it fires when it should */
	kill_port(port, 1);
	sleep(DESTROY_SETTLE);

	kern_return_t after[NMETHODS];
	int still = 0, gone = 1;

	for (int m = 0; m < NMETHODS; m++) {
		after[m] = lookup(LOOPBACK_NAME, m);
		printf("svcprobe: pass %d loopback post-destroy lookup %s: %d 0x%x (%s)\n", passno, method_name[m], after[m],
		    after[m], decode(after[m]));
		still += ok(after[m]);
		if (ok(after[m])) gone = 0;
	}
	/* "gone" means every method now fails the lookup; whether the failure code matches the
	   unknown-service baseline is part of the printed evidence but not required here */
	if (gone) {
		printf("svcprobe: pass %d verdict invalidation: invalidation-works (the endpoint died with its port within %ds; "
		    "the death-notification path functions, so family A needs a false trigger, not a broken one)\n", passno,
		    DESTROY_SETTLE);
	} else if (still == NMETHODS) {
		printf("svcprobe: pass %d verdict invalidation: endpoint-survives-death (the endpoint still resolves %ds after "
		    "its port was destroyed: invalidation does not even fire when it should, which argues against family A)\n",
		    passno, DESTROY_SETTLE);
	} else {
		printf("svcprobe: pass %d verdict invalidation: invalidation-partial (%d of %d methods still resolve the dead service)\n",
		    passno, still, NMETHODS);
	}
}

/* the candidate-family judgement for one pass, from the lookup matrix */
static void
verdicts(int passno, kern_return_t matrix[NSERVICES][NMETHODS])
{
	int positive_visible = 0, positive_error = 0;

	for (int s = 0; s < NSERVICES; s++) {
		if (!services[s].positive) continue;
		if (ok(matrix[s][M_PLAIN])) positive_visible++;
		else if (!absent_like(matrix, s, M_PLAIN)) positive_error++;
	}
	int baseline_polluted = ok(matrix[S_BASELINE][M_PLAIN]);

	if (positive_visible == 2 && !baseline_polluted) {
		printf("svcprobe: pass %d verdict channel: channel-healthy (both positive controls resolve via plain lookup)\n",
		    passno);
	} else if (positive_visible > 0) {
		printf("svcprobe: pass %d verdict channel: channel-partial (only %d of 2 positive controls resolve)\n", passno,
		    positive_visible);
	} else {
		printf("svcprobe: pass %d verdict channel: channel-degraded (no positive control resolves: %s; "
		    "the target results below cannot be read as powerd-specific)\n", passno,
		    positive_error ? "with non-absence errors" : "absent-like, the namespace itself is degraded");
	}
	if (baseline_polluted) {
		printf("svcprobe: pass %d verdict channel: baseline-polluted (the nonexistent baseline name RESOLVES: "
		    "absence judgements in this pass are worthless)\n", passno);
	}

	/* flags=8 behaviour across the whole matrix: services that plain and flags0 resolve but
	   flags8 does not */
	int f8_target = 0, f8_others = 0, any_plain = 0;

	for (int s = 0; s < NSERVICES; s++) {
		if (s == S_BASELINE) continue;
		if (!ok(matrix[s][M_PLAIN])) continue;
		any_plain++;
		if (ok(matrix[s][M_F0]) && !ok(matrix[s][M_F8])) {
			if (s == S_TARGET) f8_target = 1;
			else f8_others++;
		}
	}
	if (any_plain == 0) {
		printf("svcprobe: pass %d verdict flags8: flags8-untestable (nothing resolves via plain lookup at all)\n", passno);
	} else if (f8_target && f8_others == 0) {
		printf("svcprobe: pass %d verdict flags8: flags8-fails-for-target-only (only the control service is invisible "
		    "to flags=8 while plain resolves it)\n", passno);
	} else if (f8_target || f8_others) {
		printf("svcprobe: pass %d verdict flags8: flags8-fails-widely (flags=8 loses %d services beyond the target "
		    "that plain resolves: the privileged lookup path itself is degraded)\n", passno, f8_others);
	} else {
		printf("svcprobe: pass %d verdict flags8: flags8-consistent (flags=8 agrees with plain everywhere it is testable)\n",
		    passno);
	}

	/* the target judgement */
	kern_return_t *t = matrix[S_TARGET];
	int tp = ok(t[M_PLAIN]), t0 = ok(t[M_F0]), t8 = ok(t[M_F8]);
	int sib_visible = 0, sib_absent = 0, sib_weird = 0;

	for (int s = 0; s < NSERVICES; s++) {
		if (!services[s].sibling) continue;
		if (ok(matrix[s][M_PLAIN])) sib_visible++;
		else if (absent_like(matrix, s, M_PLAIN)) sib_absent++;
		else sib_weird++;
	}

	if (positive_visible == 0) {
		printf("svcprobe: pass %d verdict target: inconclusive-channel-degraded (the lookup channel itself is broken, "
		    "the target matrix means nothing)\n", passno);
	} else if (tp && t0 && t8) {
		printf("svcprobe: pass %d verdict target: control-visible (the service resolves with all three methods in this "
		    "pass: no reproduction; siblings visible %d of 4)\n", passno, sib_visible);
	} else if (tp && t0 && !t8) {
		printf("svcprobe: pass %d verdict target: family-c-flags8-degraded (plain and flags0 resolve the control service, "
		    "flags=8 does not, failing with %d (%s): the endpoint exists and the privileged lookup path loses it; "
		    "siblings visible %d of 4)\n", passno, t[M_F8], decode(t[M_F8]), sib_visible);
	} else if (tp != t0) {
		printf("svcprobe: pass %d verdict target: inconsistent-entry-points (plain %s but flags0 %s: the two lookup "
		    "entry points disagree, read the matrix)\n", passno, tp ? "resolves" : "fails",
		    t0 ? "resolves" : "fails");
	} else if (t8) {
		printf("svcprobe: pass %d verdict target: inverse-flags8-anomaly (flags=8 resolves what plain does not: "
		    "no candidate family predicts this, read the matrix)\n", passno);
	} else if (!absent_like(matrix, S_TARGET, M_PLAIN) || !absent_like(matrix, S_TARGET, M_F0)) {
		printf("svcprobe: pass %d verdict target: control-error-nonabsence (the control lookup fails with a code "
		    "different from the unknown-service baseline: not a missing endpoint, read the raw codes)\n", passno);
	} else {
		/* control is absent-like on every method: the family A/B discrimination is the siblings' */
		if (sib_absent == 4) {
			printf("svcprobe: pass %d verdict target: family-b-import-missing (control and all four sibling services "
			    "are absent-like: the powerd job's endpoints never entered the namespace)\n", passno);
		} else if (sib_visible == 4) {
			printf("svcprobe: pass %d verdict target: family-a-endpoint-invalidated (control alone is absent while "
			    "all four siblings resolve: the job imported fine and the control endpoint died, consistent with "
			    "its unique legacy-checkin position)\n", passno);
		} else {
			printf("svcprobe: pass %d verdict target: mixed-sibling-pattern (siblings visible %d, absent %d, "
			    "other-error %d of 4: no candidate family predicts a partial pattern, read the matrix)\n", passno,
			    sib_visible, sib_absent, sib_weird);
		}
	}
}

/* one pass: the lookup matrix, the loopback roundtrip, the verdicts; the matrix is copied out
   for the timing comparison */
static void
run_pass(int passno, int passes, kern_return_t matrix[NSERVICES][NMETHODS])
{
	printf("svcprobe: pass %d/%d: uptime %lds, uid %d, pid %d\n", passno, passes, uptime(), getuid(), getpid());
	fflush(stdout);
	for (int s = 0; s < NSERVICES; s++) {
		for (int m = 0; m < NMETHODS; m++) {
			matrix[s][m] = lookup(services[s].name, m);
			print_lookup(passno, services[s].role, services[s].name, m, matrix[s][m]);
		}
	}
	fflush(stdout);
	loopback(passno);
	verdicts(passno, matrix);
	fflush(stdout);
}

int
main(int argc, char **argv)
{
	int passes = argc > 1 ? atoi(argv[1]) : 1;
	int seconds = argc > 2 ? atoi(argv[2]) : 120;
	static kern_return_t first[NSERVICES][NMETHODS], last[NSERVICES][NMETHODS];
	long first_uptime = 0, last_uptime = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (passes < 1) passes = 1;
	for (int p = 1; p <= passes; p++) {
		if (p > 1) sleep(seconds);
		kern_return_t (*matrix)[NMETHODS] = p == 1 ? first : last;

		if (p == 1) first_uptime = uptime();
		last_uptime = uptime();
		run_pass(p, passes, matrix);
	}
	if (passes > 1) {
		int was = ok(first[S_TARGET][M_PLAIN]), now = ok(last[S_TARGET][M_PLAIN]);
		int sib_was = 0, sib_now = 0;

		for (int s = 0; s < NSERVICES; s++) {
			if (!services[s].sibling) continue;
			sib_was += ok(first[s][M_PLAIN]);
			sib_now += ok(last[s][M_PLAIN]);
		}
		if (!was && !now) {
			printf("svcprobe: verdict timing: control-absent-throughout (absent at uptime %lds and at %lds: "
			    "supports family B, never imported, or imported and already dead before the earliest probe; "
			    "siblings visible %d then %d of 4)\n", first_uptime, last_uptime, sib_was, sib_now);
		} else if (was && !now) {
			printf("svcprobe: verdict timing: control-lost-visibility (resolvable at uptime %lds, gone at %lds: "
			    "the endpoint died while the system ran, supports family A; siblings visible %d then %d of 4)\n",
			    first_uptime, last_uptime, sib_was, sib_now);
		} else if (was && now) {
			printf("svcprobe: verdict timing: control-stable-visible (resolvable at uptime %lds and at %lds: "
			    "no reproduction in this round; siblings visible %d then %d of 4)\n", first_uptime, last_uptime,
			    sib_was, sib_now);
		} else {
			printf("svcprobe: verdict timing: control-appeared-late (absent at uptime %lds, resolvable at %lds: "
			    "no candidate family predicts a late appearance, read the matrices)\n", first_uptime, last_uptime);
		}
	}
	fflush(stdout);
	return 0;
}
