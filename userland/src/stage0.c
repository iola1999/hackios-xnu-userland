/*
 * /sbin/launchd of the big system image: the first program the kernel runs.
 *
 * The system image (the iOS files and the dyld shared cache) takes a minute
 * and a half to upload to the phone, so nothing that changes often lives in
 * it. All of that - the real init, its tests, the list of launchd jobs - is
 * in the small image that travels with the kernel (the second RAM disk,
 * /dev/md1), an APFS container (tools/apfs/mkapfs.py). This program
 * mounts it on /redwood and hands over.
 *
 * iOS 26's launchd trusts a job (event streams, _Conclave) only when the plist's
 * resolved path is in /System/Library. Its system image therefore has an empty
 * /System/Library/LaunchDaemons, and the small image's jobs are bound onto it;
 * iOS 16.5's image has a symbolic link to /redwood/LaunchDaemons there instead.
 * Outside the restore environment it reads the jobs from the cache in /System/Library/xpc
 * (mkcache.py), and the small image's xpc directory is bound there too.
 */
#include "mini.h"

int
main(int argc, char **argv, char **envp)
{
	char *init_argv[] = { "/redwood/sbin/init", NULL };
	int fd = open("/dev/console", O_RDWR, 0);

	(void)argc, (void)argv;
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);
	if (fd > 2) {
		close(fd);
	}

	if (mount("apfs", "/redwood", MNT_RDONLY, (void *)"/dev/md1") != 0) {
		printf("stage0: cannot mount /dev/md1 on /redwood: %s\n", strerror(errno));
	} else {
		struct stat st;

		if (lstat("/System/Library/LaunchDaemons", &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR &&
		    mount("bindfs", "/System/Library/LaunchDaemons", MNT_RDONLY, (void *)"/redwood/LaunchDaemons") != 0) {
			printf("stage0: cannot bind /redwood/LaunchDaemons: %s\n", strerror(errno));
		}
		if (lstat("/redwood/xpc/launchd.plist", &st) == 0 &&
		    mount("bindfs", "/System/Library/xpc", MNT_RDONLY, (void *)"/redwood/xpc") != 0) {
			printf("stage0: cannot bind /redwood/xpc: %s\n", strerror(errno));
		}
		execve(init_argv[0], init_argv, envp);
		printf("stage0: cannot run %s: %s\n", init_argv[0], strerror(errno));
	}
	reboot(0x08 /* RB_HALT */);
	return 1;
}
