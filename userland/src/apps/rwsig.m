/*
 * rwsig: stop and continue named processes, so that one round can carry both halves of an
 * experiment without rebuilding anything.
 *
 * The question it is for: the compositor stops submitting frames for 59 seconds at a time, once
 * a minute, and the same minutes carry a burst of widget intent fetches out of chronod. Whether
 * those fetches hold the compositor is answered by running the frames with chronod frozen and
 * again with it running, in the same boot, on the same desktop.
 *
 * usage: rwsig SECONDS-FROZEN SECONDS-RUNNING ROUNDS NAME...
 *   freeze every NAME for SECONDS-FROZEN, continue them for SECONDS-RUNNING, ROUNDS times.
 *   A name matches p_comm exactly first, then as a prefix, so "chronod" and "extensionk" work
 *   (p_comm is 16 bytes and "extensionkitservice" arrives as "extensionkitse").
 *
 * SIGSTOP rather than SIGKILL on purpose: these jobs have KeepAlive, so a killed daemon comes
 * back at once and the experiment measures the restart instead of the daemon. A stopped process
 * keeps its memory and its sockets; its clients block, which is part of what is being measured.
 * That is why a frozen phase with FEWER frames is also a result: the compositor may be waiting
 * on the frozen daemon's XPC. Counting only "frozen made it better" would read that backwards.
 *
 * Two guards, because a stopped daemon left behind outlives the run:
 *   - every pid this program stopped is continued on the way out, including from a signal
 *     handler, and the whole process table is scanned once more for the names it was given;
 *   - if a frozen phase sees no new frame for RWSIG_ABORT_SECONDS (90 by default), it continues
 *     everything and exits: a freeze that stops the panel says nothing more by lasting longer.
 *
 * Each phase prints one line in microseconds of mach_absolute_time, the clock the frame log
 * (tl: v) carries, so the frames can be cut at the same instants:
 *   rwsig: phase stop|resume round N at <us>
 *
 * Five seconds into every frozen phase it also asks the kernel for a thread dump of backboardd
 * (kern.redwood.thread_dump, osfmk/kern/redwood_threads.c). The library's own watcher only dumps
 * when a commit has not moved for 2.5 s, which cannot happen while backboardd is not committing
 * at all: the stall it is meant to catch leaves nothing to watch. One dump per frozen phase, and
 * another if backboardd's pid changes within it (a restarted compositor is a different stack).
 */
#include <errno.h>
#include <mach/mach_time.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* libproc lives in libSystem; the iPhoneOS SDK ships no header for it (see lsprobe.m). */
extern int proc_listpids(unsigned type, unsigned typeinfo, void *buffer, int buffersize);
extern int proc_name(int pid, char *buffer, unsigned buffersize);

#define PROC_ALL_PIDS   1
#define MAX_PIDS        2048
#define MAX_NAMES       8

static const char *names[MAX_NAMES];
static int name_count;
static pid_t stopped[MAX_PIDS];
static int stopped_count;
static volatile sig_atomic_t finish;
static int abort_after = 90;

static void
say(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vprintf(format, ap);
	va_end(ap);
	fflush(stdout);
}

static uint64_t
now_us(void)
{
	static mach_timebase_info_data_t tb;

	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return mach_absolute_time() * tb.numer / tb.denom / 1000;
}

/* Frames put on the panel since boot, from the framebuffer's own counter through
 * kern.redwood.frames. Zero when the sysctl is absent, which is a kernel without it: the guard
 * then cannot fire and every line says so. */
static uint32_t
frames_shown(void)
{
	uint32_t frames = 0;
	size_t size = sizeof(frames);

	if (sysctlbyname("kern.redwood.frames", &frames, &size, NULL, 0) != 0) {
		return 0;
	}
	return frames;
}

static int
matches(const char *comm, const char *name)
{
	return strcmp(comm, name) == 0 || strncmp(comm, name, strlen(name)) == 0;
}

/* The first process whose p_comm matches, 0 when there is none. */
static pid_t
find_named(const char *name)
{
	int pids[MAX_PIDS];
	int bytes = proc_listpids(PROC_ALL_PIDS, 0, pids, (int)sizeof(pids));
	int count = bytes / (int)sizeof(pids[0]);

	for (int i = 0; i < count; i++) {
		char comm[32] = { 0 };

		if (pids[i] <= 0 || proc_name(pids[i], comm, sizeof(comm)) <= 0) {
			continue;
		}
		if (matches(comm, name)) {
			return pids[i];
		}
	}
	return 0;
}

/* One thread dump of the compositor, into the loader's log. Says when it cannot: a dump that
 * was not written and a dump that found nothing stuck read the same in a log that stays quiet. */
static void
dump_threads(void)
{
	pid_t pid = find_named("backboardd");
	int value;
	int error;

	if (pid <= 0) {
		say("rwsig: thread dump: no backboardd at %llu\n", (unsigned long long)now_us());
		return;
	}
	value = pid;
	if (sysctlbyname("kern.redwood.thread_dump", NULL, NULL, &value, sizeof(value)) != 0) {
		error = errno;
		say("rwsig: thread dump refused errno %d (%s) for pid %d at %llu\n", error,
		    strerror(error), pid, (unsigned long long)now_us());
		return;
	}
	say("rwsig: thread dump asked for pid %d at %llu\n", pid, (unsigned long long)now_us());
}

/* One signal to every process whose p_comm matches one of the names. Returns how many got it. */
static int
signal_named(int which, const char *phase)
{
	int pids[MAX_PIDS];
	int bytes = proc_listpids(PROC_ALL_PIDS, 0, pids, (int)sizeof(pids));
	int count = bytes / (int)sizeof(pids[0]);
	int sent = 0;

	if (count <= 0) {
		say("rwsig: no processes listed (%d)\n", bytes);
		return 0;
	}
	for (int i = 0; i < count; i++) {
		char comm[32] = { 0 };

		if (pids[i] <= 0 || pids[i] == (int)getpid()) {
			continue;
		}
		if (proc_name(pids[i], comm, sizeof(comm)) <= 0) {
			continue;
		}
		int wanted = 0;

		for (int n = 0; n < name_count; n++) {
			if (matches(comm, names[n])) {
				wanted = 1;
				break;
			}
		}
		if (!wanted || kill(pids[i], 0) != 0) {
			continue;
		}
		kill(pids[i], which);
		if (which == SIGSTOP && stopped_count < MAX_PIDS) {
			stopped[stopped_count++] = pids[i];
		}
		say("rwsig: %s %s pid %d\n", phase, comm, pids[i]);
		sent++;
	}
	if (sent == 0) {
		say("rwsig: %s matches nothing\n", phase);
	}
	return sent;
}

/* Everything stopped goes back, then the table is scanned once more for the names: the second
 * pass is the one that catches a process that was forked or renamed while it was stopped. */
static void
continue_all(void)
{
	for (int i = 0; i < stopped_count; i++) {
		if (kill(stopped[i], SIGCONT) == 0) {
			say("rwsig: resume pid %d on the way out\n", stopped[i]);
		}
	}
	stopped_count = 0;
	signal_named(SIGCONT, "resume-all");
}

static void
on_signal(int signal_number)
{
	(void)signal_number;
	finish = 1;
}

static void
on_exit(void)
{
	continue_all();
	say("rwsig: done at %llu\n", (unsigned long long)now_us());
}

/* Sleeps in short steps so that a signal ends it, and while frozen watches the frame counter:
 * no new frame for abort_after seconds means this freeze is stopping the panel, and carrying on
 * would only reach the watchdog. */
static int
sleep_watching(unsigned seconds, int frozen, unsigned round)
{
	uint64_t began = now_us(), last_frame_us = began, end = began + (uint64_t)seconds * 1000000;
	uint64_t dump_at = began + 5000000;
	uint32_t last_frames = frames_shown();
	pid_t dumped = 0;

	while (now_us() < end) {
		usleep(200000);
		if (finish) {
			return 0;
		}
		if (!frozen) {
			continue;
		}
		if (dump_at != 0 && now_us() >= dump_at) {
			pid_t pid = find_named("backboardd");
			int changed = pid > 0 && pid != dumped;

			if (changed || dumped == 0) {
				dump_threads();
				dumped = pid;
			}
			dump_at = now_us() + 5000000;   /* (a restarted backboardd is a new stack) */
		}
		uint32_t frames = frames_shown();

		if (frames != last_frames) {
			last_frames = frames;
			last_frame_us = now_us();
			continue;
		}
		if (now_us() - last_frame_us >= (uint64_t)abort_after * 1000000) {
			say("rwsig: abort, no frames for %d s in the frozen phase of round %u at %llu\n",
			    abort_after, round, (unsigned long long)now_us());
			return -1;
		}
	}
	return 0;
}

int
main(int argc, char **argv)
{
	int frozen_seconds, running_seconds, rounds;
	const char *abort_env;

	if (argc < 5) {
		fprintf(stderr, "usage: rwsig SECONDS-FROZEN SECONDS-RUNNING ROUNDS NAME...\n");
		return 2;
	}
	frozen_seconds = atoi(argv[1]);
	running_seconds = atoi(argv[2]);
	rounds = atoi(argv[3]);
	if (frozen_seconds <= 0 || running_seconds <= 0 || rounds <= 0) {
		fprintf(stderr, "rwsig: seconds and rounds must be positive\n");
		return 2;
	}
	abort_env = getenv("RWSIG_ABORT_SECONDS");
	if (abort_env != NULL && atoi(abort_env) > 0) {
		abort_after = atoi(abort_env);
	}
	for (int i = 4; i < argc && name_count < MAX_NAMES; i++) {
		names[name_count++] = argv[i];
	}
	atexit(on_exit);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);
	signal(SIGQUIT, on_signal);

	if (frames_shown() == 0) {
		say("rwsig: kern.redwood.frames is not there: the 90 s guard cannot fire this run\n");
	}
	if (frozen_seconds >= abort_after) {
		say("rwsig: the frozen phase is %d s, at or beyond the guard's %d s: it will fire\n",
		    frozen_seconds, abort_after);
	}
	for (int round = 1; round <= rounds && !finish; round++) {
		signal_named(SIGSTOP, "stop");
		say("rwsig: phase stop round %d at %llu\n", round, (unsigned long long)now_us());
		if (sleep_watching((unsigned)frozen_seconds, 1, (unsigned)round) != 0) {
			continue_all();
			return 3;
		}
		if (finish) {
			continue_all();
			return 0;
		}
		signal_named(SIGCONT, "resume");
		say("rwsig: phase resume round %d at %llu\n", round, (unsigned long long)now_us());
		if (sleep_watching((unsigned)running_seconds, 0, (unsigned)round) != 0) {
			continue_all();
			return 3;
		}
	}
	continue_all();
	return 0;
}