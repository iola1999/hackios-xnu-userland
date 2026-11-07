/*
 * framewatch: catch the instant the compositor stops submitting frames, while the processes that
 * should be submitting are still alive.
 *
 * The question it is for: one round put 17 frames on the panel and another put 3550, with no death
 * recorded in either and no restart of backboardd or SpringBoard in the first. A thread dump taken
 * afterwards shows where the threads sit at that moment; it cannot show where they sat when the
 * frames stopped, because the condition that stopped them may have passed. This program waits for
 * the stop and asks for the dump during it.
 *
 * usage: framewatch STILL_SECONDS MAX_DUMPS [NAME...]
 *   poll kern.redwood.frames once a second; when it has not grown for STILL_SECONDS, ask for a
 *   thread dump of every NAME (default backboardd and SpringBoard), then keep counting. After
 *   MAX_DUMPS dumps it keeps watching and prints one line a second, which is what tells a lasting
 *   stop from an intermittent one. NAME matches p_comm exactly first, then as a prefix.
 *
 * usage: framewatch --every SECONDS --count N [NAME...]
 *   ask for a dump of every NAME every SECONDS seconds, N times, without waiting for a stop. The
 *   stop-triggered mode answers "where did the threads sit when the frames stopped"; this one
 *   answers "what does the same dump look like while the system is healthy", which is the only way
 *   to tell a stuck thread from a thread that always parks there. Both modes print in the same
 *   format so a log can be read without knowing which produced it.
 *
 * Two readings, both from counters that do not throttle:
 *   - "stopped" is frames that did not grow (kern.redwood.frames counts every swap);
 *   - "for how long" is mach_absolute_time, the clock the tl: lines carry, so a framewatch line and
 *     a frame log line can be placed on one axis.
 * The touch samples (tl: p) are printed on powers of two, so a log line count there is not an event
 * count; nothing here reads them.
 *
 * The resumed line is not decoration: a stop that ends by itself is a wait that expired, and a stop
 * that never ends is a deadlock. A dump alone cannot tell those apart.
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

static const char *names[MAX_NAMES] = { "backboardd", "SpringBoard" };
static int name_count = 2;
static volatile sig_atomic_t finish;
static int stalls;
static unsigned longest;
static uint64_t longest_at;

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

/* Frames put on the panel since boot. The count comes back through a pointer so that an absent
 * sysctl (an older kernel) reads differently from a panel that has drawn nothing yet. */
static int
frames_shown(uint32_t *frames)
{
	size_t size = sizeof(*frames);

	*frames = 0;
	return sysctlbyname("kern.redwood.frames", frames, &size, NULL, 0) == 0;
}

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
		if (strcmp(comm, name) == 0 || strncmp(comm, name, strlen(name)) == 0) {
			return pids[i];
		}
	}
	return 0;
}

/* One dump of one process. A dump that was not written and a dump that found nothing stuck read
 * the same in a log that stays quiet, so the refusal is printed with its errno. */
static void
dump_one(const char *name, int stalled)
{
	pid_t pid = find_named(name);
	int value;
	int error;

	if (pid <= 0) {
		say("framewatch: no process named %s at %llu\n", name, (unsigned long long)now_us());
		return;
	}
	value = pid;
	if (sysctlbyname("kern.redwood.thread_dump", NULL, NULL, &value, sizeof(value)) != 0) {
		error = errno;
		say("framewatch: dump refused errno %d (%s) for %s pid %d\n", error, strerror(error),
		    name, pid);
		return;
	}
	say("framewatch: no frames for %d s at %llu, dump asked for %s pid %d\n", stalled,
	    (unsigned long long)now_us(), name, pid);
}

static void
dump_all(int stalled)
{
	for (int i = 0; i < name_count; i++) {
		dump_one(names[i], stalled);
	}
}

static void
on_signal(int which)
{
	(void)which;
	finish = 1;
}

static void
on_exit(void)
{
	say("framewatch: done, %d stalls, longest %u s at %llu\n", stalls, longest,
	    (unsigned long long)longest_at);
}

/* The periodic mode: same dump, asked for on a clock rather than on a stop. */
static int
every(int every_seconds, int count)
{
	for (int i = 1; i <= count && !finish; i++) {
		for (int waited = 0; waited < every_seconds && !finish; waited++) {
			sleep(1);
		}
		if (finish) {
			break;
		}
		for (int k = 0; k < name_count; k++) {
			pid_t pid = find_named(names[k]);

			if (pid <= 0) {
				say("framewatch: no process named %s at %llu\n", names[k],
				    (unsigned long long)now_us());
				continue;
			}
			say("framewatch: periodic dump %d of %d, asked for %s pid %d\n", i, count,
			    names[k], pid);
			{
				int value = pid;

				if (sysctlbyname("kern.redwood.thread_dump", NULL, NULL, &value,
				                 sizeof(value)) != 0) {
					say("framewatch: dump refused errno %d (%s) for %s pid %d\n",
					    errno, strerror(errno), names[k], pid);
				}
			}
		}
	}
	return 0;
}

int
main(int argc, char **argv)
{
	int still_seconds, max_dumps, dumps = 0, stalled = 0;
	uint32_t frames, last;
	int periodic = 0, every_seconds = 0, count = 0, arg_base = 0;

	if (argc >= 5 && strcmp(argv[1], "--every") == 0 && strcmp(argv[3], "--count") == 0) {
		every_seconds = atoi(argv[2]);
		count = atoi(argv[4]);
		if (every_seconds <= 0 || count <= 0) {
			fprintf(stderr, "framewatch: --every and --count must be positive\n");
			return 2;
		}
		periodic = 1;
		arg_base = 5;
	} else if (argc < 3) {
		fprintf(stderr, "usage: framewatch STILL_SECONDS MAX_DUMPS [NAME...]\n"
		                "       framewatch --every SECONDS --count N [NAME...]\n");
		return 2;
	}
	still_seconds = periodic ? 1 : atoi(argv[1]);
	max_dumps = periodic ? 0 : atoi(argv[2]);
	if (!periodic && (still_seconds <= 0 || max_dumps < 0)) {
		fprintf(stderr, "framewatch: STILL_SECONDS must be positive and MAX_DUMPS not negative\n");
		return 2;
	}
	if (argc > arg_base) {
		name_count = 0;
		for (int i = arg_base; i < argc && name_count < MAX_NAMES; i++) {
			names[name_count++] = argv[i];
		}
	}
	if (periodic) {
		signal(SIGINT, on_signal);
		signal(SIGTERM, on_signal);
		say("framewatch: periodic dumps every %d s, %d of them, %d name%s\n", every_seconds,
		    count, name_count, name_count == 1 ? "" : "s");
		return every(every_seconds, count);
	}
	if (!frames_shown(&last)) {
		fprintf(stderr, "framewatch: kern.redwood.frames is not there: nothing to watch\n");
		return 1;
	}
	atexit(on_exit);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);
	signal(SIGQUIT, on_signal);
	say("framewatch: watching kern.redwood.frames from %u, still after %d s, at most %d dumps\n",
	    last, still_seconds, max_dumps);

	while (!finish) {
		sleep(1);
		if (finish) {
			break;
		}
		if (!frames_shown(&frames)) {
			say("framewatch: kern.redwood.frames went away at %llu\n",
			    (unsigned long long)now_us());
			break;
		}
		if (frames > last) {
			if (stalled >= still_seconds) {
				say("framewatch: resumed at %llu after %d s\n",
				    (unsigned long long)now_us(), stalled);
				stalls++;
			}
			stalled = 0;
			last = frames;
			continue;
		}
		stalled++;
		if (stalled > (int)longest) {
			longest = (unsigned)stalled;
			longest_at = now_us();
		}
		if (stalled == still_seconds && dumps < max_dumps) {
			dump_all(stalled);
			dumps++;
			continue;
		}
		if (stalled > still_seconds && dumps >= max_dumps) {
			say("framewatch: still no frames, %d s at %llu\n", stalled,
			    (unsigned long long)now_us());
		}
	}
	if (stalled >= still_seconds) {
		stalls++;
	}
	return 0;
}