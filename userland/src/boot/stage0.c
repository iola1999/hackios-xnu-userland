/*
 * The first program run from the boot volume or a RAM system image.
 * The auxiliary APFS image on /dev/md1 contains init and its configuration.
 * Mount it on /redwood, then run init to prepare the system and start launchd.
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
