/*
 * rwsh: run the commands the host sends while iOS runs, and put their output into the console log.
 *
 * The host sends "fastboot oem sh TEXT"; the loader writes TEXT into the running kernel's mailbox
 * (struct loader_live in osfmk/arm64/loader_return.c) and the kernel shows it as kern.redwood.cmd,
 * "SEQ TEXT". This program looks at that every 250 ms and runs each new SEQ once. It is started from
 * rc.jobs ("/redwood/bin/rwsh &") and so runs as root, outside any sandbox.
 *
 * Every line of output goes to stderr, which init leaves on the console:
 *   rwsh SEQ start TEXT
 *   rwsh SEQ| LINE                     at most RWSH_MAX_LINES of them
 *   rwsh SEQ exit STATUS MS ms
 * tools/k90auto/rsh.py on the host sends the command and collects these lines.
 *
 * iOS has no shell, so the words of TEXT are split on blanks with no quoting, and these are built in:
 *   ps                         pid, ppid, uid, user and system CPU seconds, threads, name
 *   threads PID                per thread: id, run state, current/base priority, user and system
 *                              CPU seconds, name
 *   dump PID                   kern.redwood.thread_dump: one kernel log line per thread of PID
 *   kill SIGNAL PID            kill(2)
 *   notify NAME                notify_post(3)
 *   sysctl NAME                the value as a string, or as integers when it is not printable
 *   sysctl NAME=NUMBER         set an integer sysctl
 *   setenv WORD...             kern.redwood.env becomes the words, each PROCESS:NAME=VALUE or
 *                              PROCESS:-NAME; libredwoodmetal applies its process's words when the
 *                              process loads it
 *   addenv WORD...             the words are added to kern.redwood.env (a command is at most 119
 *                              characters, so a long setting is sent in pieces)
 *   restart NAME...            SIGKILL to every process of that name, for launchd to start again
 *   pull PATH                  kern.redwood.inbox, the last file the host pushed ("oem push"),
 *                              written to PATH (mode 755) through a temporary file and a rename
 *   outbox PATH                the file into kern.redwood.outbox: after the run the loader stages it
 *                              ("oem recording") and run-auto.py fetches it as run/outbox.bin
 *   cat PATH [OFFSET]          the file from OFFSET on, 48 bytes a line in base64 ("cat AT DATA"),
 *                              as many lines as fit, then "cat end SIZE next AT" (fetch.py on the host)
 *   ls PATH                    the directory's entries: type, size, name
 *   rm PATH                    unlink(2)
 * Anything else is a program: posix_spawnp with stdout and stderr read back, killed after
 * RWSH_LIMIT_SECONDS.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

/* libproc lives in libSystem; the iPhoneOS SDK ships no header for it (see lsprobe.m). The flavors
 * and layouts are those of bsd/sys/proc_info.h and proc_info_private.h in xnu-12377. */
extern int proc_listpids(uint32_t type, uint32_t typeinfo, void *buffer, int buffersize);
extern int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);
extern int proc_name(int pid, void *buffer, uint32_t buffersize);

#define PROC_ALL_PIDS           1
#define PROC_PIDTASKINFO        4
#define PROC_PIDTHREADID64INFO  15
#define PROC_PIDLISTTHREADIDS   28

struct proc_taskinfo {
	uint64_t pti_virtual_size, pti_resident_size;
	uint64_t pti_total_user, pti_total_system;      /* mach_absolute_time units */
	uint64_t pti_threads_user, pti_threads_system;
	int32_t pti_policy, pti_faults, pti_pageins, pti_cow_faults, pti_messages_sent;
	int32_t pti_messages_received, pti_syscalls_mach, pti_syscalls_unix, pti_csw;
	int32_t pti_threadnum, pti_numrunning, pti_priority;
};

struct proc_threadinfo {
	uint64_t pth_user_time, pth_system_time;        /* nanoseconds */
	int32_t pth_cpu_usage, pth_policy, pth_run_state, pth_flags, pth_sleep_time;
	int32_t pth_curpri, pth_priority, pth_maxpriority;
	char pth_name[64];
};

#define RWSH_POLL_US            250000
#define RWSH_LIMIT_SECONDS      120     /* var_store save FILE packs all of /private/var */
#define RWSH_MAX_PIDS           2048
#define RWSH_MAX_THREADS        1024

extern char **environ;
static unsigned long long current;

/* One line per write(2), so a line is never split by what other processes print meanwhile. */
static void
emit(const char *format, ...)
{
	char line[1100];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line) - 1, format, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > (int)sizeof(line) - 2)
		n = (int)sizeof(line) - 2;
	line[n] = '\n';
	write(2, line, (size_t)n + 1);
}

/* The console log is a 1.3 MB ring the host reads whole, so one command's output is capped: a
 * program that prints without end would otherwise push everything else out of it. */
#define RWSH_MAX_LINES          600
static unsigned long lines, dropped;

#define out(format, ...) do { \
	if (++lines <= RWSH_MAX_LINES) \
		emit("rwsh %llu| " format, current, ##__VA_ARGS__); \
	else \
		dropped++; \
} while (0)

static double
mach_seconds(uint64_t ticks)
{
	static mach_timebase_info_data_t tb;

	if (tb.denom == 0)
		mach_timebase_info(&tb);
	return (double)ticks * tb.numer / tb.denom / 1e9;
}

static int
do_ps(void)
{
	static int pids[RWSH_MAX_PIDS];
	int count = proc_listpids(PROC_ALL_PIDS, 0, pids, (int)sizeof(pids)) / (int)sizeof(pids[0]);

	out("%6s %6s %5s %9s %9s %4s %s", "pid", "ppid", "uid", "user_s", "sys_s", "thr", "name");
	for (int i = 0; i < count; i++) {
		struct kinfo_proc kp;
		size_t size = sizeof(kp);
		int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, pids[i] };
		struct proc_taskinfo ti;
		char name[64] = "?";

		if (pids[i] <= 0 || sysctl(mib, 4, &kp, &size, NULL, 0) != 0 || size == 0)
			continue;
		memset(&ti, 0, sizeof(ti));
		proc_pidinfo(pids[i], PROC_PIDTASKINFO, 0, &ti, (int)sizeof(ti));
		proc_name(pids[i], name, sizeof(name));
		out("%6d %6d %5u %9.2f %9.2f %4d %s", pids[i], kp.kp_eproc.e_ppid, kp.kp_eproc.e_ucred.cr_uid,
		    mach_seconds(ti.pti_total_user), mach_seconds(ti.pti_total_system), ti.pti_threadnum, name);
	}
	return 0;
}

static int
do_threads(int pid)
{
	static uint64_t ids[RWSH_MAX_THREADS];
	static const char *const states[] = { "?", "run", "stop", "wait", "uninterruptible", "halted" };
	int bytes = proc_pidinfo(pid, PROC_PIDLISTTHREADIDS, 0, ids, (int)sizeof(ids));

	if (bytes <= 0) {
		out("threads: pid %d: %s", pid, strerror(errno));
		return 1;
	}
	out("%10s %-15s %3s %3s %9s %9s %s", "tid", "state", "cur", "pri", "user_s", "sys_s", "name");
	for (int i = 0; i < bytes / (int)sizeof(ids[0]); i++) {
		struct proc_threadinfo th;

		if (proc_pidinfo(pid, PROC_PIDTHREADID64INFO, ids[i], &th, (int)sizeof(th)) <= 0)
			continue;
		out("%10llu %-15s %3d %3d %9.2f %9.2f %s", (unsigned long long)ids[i],
		    th.pth_run_state >= 1 && th.pth_run_state <= 5 ? states[th.pth_run_state] : states[0],
		    th.pth_curpri, th.pth_priority, th.pth_user_time / 1e9, th.pth_system_time / 1e9, th.pth_name);
	}
	return 0;
}

static int
do_sysctl(char *name)
{
	static unsigned char value[9000];
	size_t size = sizeof(value) - 1;
	size_t printable = 0;
	char *equals = strchr(name, '=');

	if (equals != NULL) {
		int number = atoi(equals + 1);

		*equals = '\0';
		if (sysctlbyname(name, NULL, NULL, &number, sizeof(number)) != 0) {
			out("sysctl %s=%d: %s", name, number, strerror(errno));
			return 1;
		}
		out("%s = %d", name, number);
		return 0;
	}
	if (sysctlbyname(name, value, &size, NULL, 0) != 0) {
		out("sysctl %s: %s", name, strerror(errno));
		return 1;
	}
	while (printable < size && (value[printable] >= 0x20 || value[printable] == '\n' || value[printable] == '\t'))
		printable++;
	if (size > 0 && (printable == size || (printable == size - 1 && value[printable] == 0))) {
		char *line, *rest = NULL;

		value[printable] = 0;
		line = strtok_r((char *)value, "\n", &rest);
		out("%s = %s", name, line ? line : "");
		while ((line = strtok_r(NULL, "\n", &rest)) != NULL)
			out("%s", line);
	} else if (size == sizeof(int32_t)) {
		out("%s = %d", name, *(int32_t *)value);
	} else if (size == sizeof(int64_t)) {
		out("%s = %lld", name, *(long long *)value);
	} else {
		out("%s: %zu bytes", name, size);
	}
	return 0;
}

/* A program's stdout and stderr, line by line, until it exits or its time is up. */
static int
do_spawn(char **argv)
{
	posix_spawn_file_actions_t actions;
	int pipefd[2], status = 0;
	char buffer[4096];
	size_t held = 0;
	pid_t pid;
	uint64_t deadline = mach_absolute_time();
	mach_timebase_info_data_t tb;

	mach_timebase_info(&tb);
	deadline += (uint64_t)RWSH_LIMIT_SECONDS * 1000000000ULL * tb.denom / tb.numer;
	if (pipe(pipefd) != 0) {
		out("pipe: %s", strerror(errno));
		return 1;
	}
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, pipefd[1], 1);
	posix_spawn_file_actions_adddup2(&actions, pipefd[1], 2);
	posix_spawn_file_actions_addclose(&actions, pipefd[0]);
	posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
	int error = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&actions);
	close(pipefd[1]);
	if (error != 0) {
		out("%s: %s", argv[0], strerror(error));
		close(pipefd[0]);
		return 127;
	}
	for (;;) {
		struct pollfd p = { pipefd[0], POLLIN, 0 };
		ssize_t n;

		if (mach_absolute_time() > deadline) {
			emit("rwsh %llu| %s: still running after %d s, killed", current, argv[0], RWSH_LIMIT_SECONDS);
			kill(pid, SIGKILL);
			break;
		}
		if (poll(&p, 1, 200) <= 0)
			continue;
		n = read(pipefd[0], buffer + held, sizeof(buffer) - 1 - held);
		if (n <= 0)
			break;
		held += (size_t)n;
		char *line = buffer, *end;

		while ((end = memchr(line, '\n', held - (size_t)(line - buffer))) != NULL) {
			*end = 0;
			out("%s", line);
			line = end + 1;
		}
		held -= (size_t)(line - buffer);
		memmove(buffer, line, held);
		/* a line longer than the buffer goes out in pieces */
		if (held == sizeof(buffer) - 1) {
			buffer[held] = 0;
			out("%s", buffer);
			held = 0;
		}
	}
	if (held > 0) {
		buffer[held] = 0;
		out("%s", buffer);
	}
	close(pipefd[0]);
	waitpid(pid, &status, 0);
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static int
do_env(int argc, char **argv, int append)
{
	char value[1024] = "";
	size_t size = sizeof(value) - 1, at;

	if (append && sysctlbyname("kern.redwood.env", value, &size, NULL, 0) != 0) {
		out("%s: kern.redwood.env: %s", argv[0], strerror(errno));
		return 1;
	}
	value[append ? size : 0] = 0;
	at = strlen(value);
	for (int i = 1; i < argc; i++) {
		int n = snprintf(value + at, sizeof(value) - at, "%s%s", at ? " " : "", argv[i]);

		if (n < 0 || (size_t)n >= sizeof(value) - at) {
			out("%s: kern.redwood.env holds %zu characters", argv[0], sizeof(value) - 1);
			return 1;
		}
		at += (size_t)n;
	}
	if (sysctlbyname("kern.redwood.env", NULL, NULL, value, at + 1) != 0) {
		out("%s: kern.redwood.env: %s", argv[0], strerror(errno));
		return 1;
	}
	out("kern.redwood.env = %s", value);
	return 0;
}

static int
do_restart(int argc, char **argv)
{
	static int pids[RWSH_MAX_PIDS];
	int count = proc_listpids(PROC_ALL_PIDS, 0, pids, (int)sizeof(pids)) / (int)sizeof(pids[0]);
	int killed = 0;

	for (int i = 0; i < count; i++) {
		char name[64] = "";

		if (pids[i] <= 1 || proc_name(pids[i], name, sizeof(name)) <= 0)
			continue;
		for (int k = 1; k < argc; k++) {
			if (strcmp(name, argv[k]) != 0)
				continue;
			if (kill(pids[i], SIGKILL) == 0) {
				out("restart: killed %s %d", name, pids[i]);
				killed++;
			} else {
				out("restart: %s %d: %s", name, pids[i], strerror(errno));
			}
		}
	}
	return killed > 0 ? 0 : 1;
}

static int
do_outbox(const char *path)
{
	static char piece[16 + (1 << 20)];
	uint64_t head[2] = { 0, 0 };
	struct stat st;
	int fd = open(path, O_RDONLY);

	if (fd < 0 || fstat(fd, &st) != 0 || st.st_size == 0) {
		out("outbox %s: %s", path, fd < 0 ? strerror(errno) : "empty or unreadable");
		if (fd >= 0)
			close(fd);
		return 1;
	}
	head[0] = (uint64_t)st.st_size;
	while (head[1] < head[0]) {
		ssize_t n = read(fd, piece + sizeof(head), sizeof(piece) - sizeof(head));

		if (n <= 0)
			break;
		memcpy(piece, head, sizeof(head));
		if (sysctlbyname("kern.redwood.outbox", NULL, NULL, piece, sizeof(head) + (size_t)n) != 0) {
			out("outbox %s: at %llu: %s", path, (unsigned long long)head[1], strerror(errno));
			break;
		}
		head[1] += (uint64_t)n;
	}
	close(fd);
	out("outbox %s: %llu of %llu bytes", path, (unsigned long long)head[1], (unsigned long long)head[0]);
	return head[1] == head[0] ? 0 : 1;
}

static int
do_pull(const char *path)
{
	size_t size = 0;
	char *data, temporary[1024];
	uint64_t seq;
	int fd, ok;

	if (sysctlbyname("kern.redwood.inbox", NULL, &size, NULL, 0) != 0 || size < sizeof(seq) ||
	    (data = malloc(size)) == NULL || sysctlbyname("kern.redwood.inbox", data, &size, NULL, 0) != 0) {
		out("pull: kern.redwood.inbox: %s", strerror(errno));
		return 1;
	}
	memcpy(&seq, data, sizeof(seq));
	if (seq == 0 || size == sizeof(seq)) {
		out("pull: nothing has been pushed");
		free(data);
		return 1;
	}
	snprintf(temporary, sizeof(temporary), "%s.pull", path);
	fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	ok = fd >= 0 && write(fd, data + sizeof(seq), size - sizeof(seq)) == (ssize_t)(size - sizeof(seq));
	ok = (fd < 0 || close(fd) == 0) && ok && rename(temporary, path) == 0;
	if (!ok) {
		out("pull: %s: %s", path, strerror(errno));
		free(data);
		return 1;
	}
	out("pull: push %llu, %zu bytes to %s", (unsigned long long)seq, size - sizeof(seq), path);
	free(data);
	return 0;
}

static int
do_cat(const char *path, long long from)
{
	static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	unsigned char chunk[48];
	char text[65];
	struct stat st;
	long long at = from;
	int fd = open(path, O_RDONLY);

	if (fd < 0 || fstat(fd, &st) != 0 || lseek(fd, from, SEEK_SET) < 0) {
		out("cat %s: %s", path, strerror(errno));
		if (fd >= 0)
			close(fd);
		return 1;
	}
	while (lines + 2 < RWSH_MAX_LINES) {
		ssize_t n = read(fd, chunk, sizeof(chunk));
		size_t o = 0;

		if (n <= 0)
			break;
		for (ssize_t i = 0; i < n; i += 3) {
			unsigned v = chunk[i] << 16 | (i + 1 < n ? chunk[i + 1] << 8 : 0) | (i + 2 < n ? chunk[i + 2] : 0);

			text[o++] = digits[v >> 18 & 63];
			text[o++] = digits[v >> 12 & 63];
			text[o++] = i + 1 < n ? digits[v >> 6 & 63] : '=';
			text[o++] = i + 2 < n ? digits[v & 63] : '=';
		}
		text[o] = 0;
		out("cat %lld %s", at, text);
		at += n;
	}
	out("cat end %lld next %lld", (long long)st.st_size, at);
	close(fd);
	return 0;
}

static int
do_ls(const char *path)
{
	DIR *dir = opendir(path);
	struct dirent *e;

	if (dir == NULL) {
		out("ls %s: %s", path, strerror(errno));
		return 1;
	}
	while ((e = readdir(dir)) != NULL) {
		char full[1024];
		struct stat st;

		snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
		if (lstat(full, &st) != 0)
			out("? %s", e->d_name);
		else
			out("%c %10lld %s", S_ISDIR(st.st_mode) ? 'd' : S_ISLNK(st.st_mode) ? 'l' : 'f',
			    (long long)st.st_size, e->d_name);
	}
	closedir(dir);
	return 0;
}

static int
run(char *text)
{
	char *argv[32];
	int argc = 0;

	for (char *word = strtok(text, " \t"); word != NULL && argc < 31; word = strtok(NULL, " \t"))
		argv[argc++] = word;
	argv[argc] = NULL;
	if (argc == 0)
		return 0;
	if (strcmp(argv[0], "ps") == 0)
		return do_ps();
	if (strcmp(argv[0], "threads") == 0 && argc == 2)
		return do_threads(atoi(argv[1]));
	if (strcmp(argv[0], "dump") == 0 && argc == 2) {
		int pid = atoi(argv[1]);

		if (sysctlbyname("kern.redwood.thread_dump", NULL, NULL, &pid, sizeof(pid)) != 0) {
			out("dump %d: %s", pid, strerror(errno));
			return 1;
		}
		out("dump %d: the threads are in the kernel log", pid);
		return 0;
	}
	if (strcmp(argv[0], "kill") == 0 && argc == 3) {
		if (kill(atoi(argv[2]), atoi(argv[1])) != 0) {
			out("kill: %s", strerror(errno));
			return 1;
		}
		return 0;
	}
	if (strcmp(argv[0], "notify") == 0 && argc == 2)
		return notify_post(argv[1]) == NOTIFY_STATUS_OK ? 0 : 1;
	if (strcmp(argv[0], "sysctl") == 0 && argc == 2)
		return do_sysctl(argv[1]);
	if (strcmp(argv[0], "setenv") == 0 || strcmp(argv[0], "addenv") == 0)
		return do_env(argc, argv, argv[0][0] == 'a');
	if (strcmp(argv[0], "restart") == 0 && argc >= 2)
		return do_restart(argc, argv);
	if (strcmp(argv[0], "pull") == 0 && argc == 2)
		return do_pull(argv[1]);
	if (strcmp(argv[0], "outbox") == 0 && argc == 2)
		return do_outbox(argv[1]);
	if (strcmp(argv[0], "cat") == 0 && (argc == 2 || argc == 3))
		return do_cat(argv[1], argc == 3 ? atoll(argv[2]) : 0);
	if (strcmp(argv[0], "ls") == 0 && argc == 2)
		return do_ls(argv[1]);
	if (strcmp(argv[0], "rm") == 0 && argc == 2) {
		if (unlink(argv[1]) != 0) {
			out("rm %s: %s", argv[1], strerror(errno));
			return 1;
		}
		return 0;
	}
	return do_spawn(argv);
}

int
main(void)
{
	char value[192];
	unsigned long long done = 0;
	size_t size = sizeof(value);

	/* The number already there when this starts was meant for an earlier run of the program. */
	if (sysctlbyname("kern.redwood.cmd", value, &size, NULL, 0) != 0) {
		emit("rwsh: no kern.redwood.cmd (%s): this kernel has no command mailbox", strerror(errno));
		return 1;
	}
	done = strtoull(value, NULL, 10);
	emit("rwsh: ready, last command %llu", done);
	for (;;) {
		char *text;
		unsigned long long seq;
		uint64_t began;
		int status;

		usleep(RWSH_POLL_US);
		size = sizeof(value) - 1;
		if (sysctlbyname("kern.redwood.cmd", value, &size, NULL, 0) != 0)
			continue;
		value[size] = 0;
		seq = strtoull(value, &text, 10);
		if (seq == done)
			continue;
		done = current = seq;
		if (*text == ' ')
			text++;
		lines = dropped = 0;
		began = mach_absolute_time();
		emit("rwsh %llu start %s", seq, text);
		status = run(text);
		if (dropped > 0)
			emit("rwsh %llu dropped %lu lines after the first %d", seq, dropped, RWSH_MAX_LINES);
		emit("rwsh %llu exit %d %.0f ms", seq, status, mach_seconds(mach_absolute_time() - began) * 1e3);
	}
}
