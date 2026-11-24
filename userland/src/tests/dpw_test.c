/*
 * dpw_test: reproduce locationd's archive append and say what write(2) answers.
 *
 * locationd aborts on -[NSConcreteFileHandle writeData:] while it concatenates the
 * locScoreInfo secondary archives into a combined file: the destination is opened
 * open_dprotected_np(path, O_WRONLY|O_CREAT|O_APPEND, class 2) and the first write
 * to it fails. This does the same opens and writes in the same directory and prints
 * the errno each step returns.
 *
 * usage: dpw_test
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* open_dprotected_np(path, flags, class, dpflags, mode) per sys/fcntl.h */

static const char * DIR_ = "/var/root/Library/Caches/locationd/locScoreInfo";

static void
step(const char * what, long ret)
{
	printf("%-52s %4ld  errno=%d (%s)\n", what, ret, ret < 0 ? errno : 0, ret < 0 ? strerror(errno) : "-");
}

int
main(void)
{
	char path[256];
	char buf[256];
	int fd, src;
	long n;
	memset(buf, 0x41, sizeof(buf));

	snprintf(path, sizeof(path), "%s/dpw_probe", DIR_);
	unlink(path);
	step("open_dprotected_np(dst, W|CREAT|APPEND, class2)", open_dprotected_np(path, O_WRONLY | O_CREAT | O_APPEND, 2, 0, 0644));
	fd = open_dprotected_np(path, O_WRONLY | O_CREAT | O_APPEND, 2, 0, 0644);
	if (fd >= 0) {
		step("write(dp-created file)", write(fd, buf, sizeof(buf)));
		step("fsync(dp-created file)", fsync(fd));
		close(fd);
		step("reopen O_APPEND plain open", open(path, O_WRONLY | O_APPEND));
		fd = open(path, O_WRONLY | O_APPEND);
		if (fd >= 0) {
			step("write(dp file, plain fd)", write(fd, buf, sizeof(buf)));
			close(fd);
		}
	}

	snprintf(path, sizeof(path), "%s/dpw_probe_plain", DIR_);
	unlink(path);
	fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
	step("open(dst, W|CREAT|APPEND)", fd);
	if (fd >= 0) {
		step("write(plain file)", write(fd, buf, sizeof(buf)));
		close(fd);
	}

	snprintf(path, sizeof(path), "%s/secondary-817146251.189629-1", DIR_);
	src = open(path, O_RDONLY);
	step("open(secondary-1, O_RDONLY)", src);
	if (src >= 0) {
		n = read(src, buf, sizeof(buf));
		step("read(secondary-1)", n);
		close(src);
	}
	return 0;
}
