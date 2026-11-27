/*
 * fhw_test: reproduce locationd's locScoreInfo archive append path.
 *
 * locationd's CLHRequestArchive appends each locScoreInfo/secondary-* file into
 * locScoreInfo/allSecondaryCombined via:
 *   1) open_dprotected_np(dst, O_WRONLY|O_CREAT|O_APPEND, class 2, 0, 0640)
 *   2) open(src, O_RDONLY) + fstat() + mmap(NULL, st_size, PROT_READ, MAP_PRIVATE, src, 0)
 *      wrapped as NSData (no prior userspace page touch before write)
 *   3) -[NSConcreteFileHandle writeData:] -> write(dst, mapped, len)
 *
 * This exercises both the cold mmap NSData path and heap initWithContentsOfFile:,
 * raw write(2) on the mapped pages, unaligned O_APPEND extension (apfs_wresize),
 * cross-page payloads (> 16 KiB), and F_GETPROTECTIONCLASS / F_SETPROTECTIONCLASS.
 *
 * usage: fhw_test [srcPath [dstPath]]
 */
#import <Foundation/Foundation.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef F_GETPROTECTIONCLASS
#define F_GETPROTECTIONCLASS 63
#endif
#ifndef F_SETPROTECTIONCLASS
#define F_SETPROTECTIONCLASS 64
#endif

static const char * DIR_ = "/var/root/Library/Caches/locationd/locScoreInfo";

static void
probe_pclass(int fd, const char * label)
{
	errno = 0;
	int cur = fcntl(fd, F_GETPROTECTIONCLASS);
	printf("%-46s ret=%d errno=%d (%s)\n", label, cur,
	    cur < 0 ? errno : 0, cur < 0 ? strerror(errno) : "-");
}

static bool
find_source_in_dir(char * out, size_t cap)
{
	snprintf(out, cap, "%s/secondary-817146251.189629-1", DIR_);
	if (access(out, R_OK) == 0) {
		return true;
	}
	DIR * d = opendir(DIR_);
	if (d == NULL) {
		return false;
	}
	bool found = false;
	struct dirent * ent;
	while ((ent = readdir(d)) != NULL) {
		if (strncmp(ent->d_name, "secondary-", 10) == 0) {
			snprintf(out, cap, "%s/%s", DIR_, ent->d_name);
			if (access(out, R_OK) == 0) {
				found = true;
				break;
			}
		}
	}
	closedir(d);
	return found;
}

static bool
create_temp_source(const char * path, size_t size)
{
	int fd = open_dprotected_np(path, O_WRONLY | O_CREAT | O_TRUNC, 2, 0, 0640);
	if (fd < 0) {
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0640);
	}
	if (fd < 0) {
		return false;
	}
	uint8_t * buf = malloc(size);
	if (buf == NULL) {
		close(fd);
		return false;
	}
	for (size_t i = 0; i < size; i++) {
		buf[i] = (uint8_t)((i * 29u + (i >> 8)) & 0xffu);
	}
	size_t off = 0;
	bool ok = true;
	while (off < size) {
		ssize_t n = write(fd, buf + off, size - off);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			ok = false;
			break;
		}
		off += (size_t)n;
	}
	free(buf);
	close(fd);
	return ok;
}

static int
run_mmap_fh(const char * tag, const char * dstPath, const char * srcPath, bool useHeap)
{
	int dst = -1, src = -1;
	int rc = 0;

	@autoreleasepool {
		errno = 0;
		dst = open_dprotected_np(dstPath, O_WRONLY | O_CREAT | O_APPEND, 2, 0, 0640);
		printf("%-46s fd=%d errno=%d (%s)\n", "open_dprotected_np(dst)", dst,
		    dst < 0 ? errno : 0, dst < 0 ? strerror(errno) : "-");
		if (dst >= 0) {
			probe_pclass(dst, "fcntl(dst, F_GETPROTECTIONCLASS)");
			errno = 0;
			int sr = fcntl(dst, F_SETPROTECTIONCLASS, 2);
			printf("%-46s ret=%d errno=%d (%s)\n", "fcntl(dst, F_SETPROTECTIONCLASS, 2)", sr,
			    sr < 0 ? errno : 0, sr < 0 ? strerror(errno) : "-");
		}
		errno = 0;
		src = open(srcPath, O_RDONLY);
		printf("%-46s fd=%d errno=%d (%s)\n", "open(src, O_RDONLY)", src,
		    src < 0 ? errno : 0, src < 0 ? strerror(errno) : "-");
		if (dst < 0 || src < 0) {
			rc = 1;
			goto out;
		}

		NSData * data = nil;
		if (useHeap) {
			data = [[NSData alloc] initWithContentsOfFile:@(srcPath)];
			printf("%-46s len=%lu\n", "NSData initWithContentsOfFile(src)",
			    (unsigned long)[data length]);
		} else {
			struct stat st;
			if (fstat(src, &st) != 0) {
				printf("%-46s errno=%d (%s)\n", "fstat(src) failed", errno, strerror(errno));
				rc = 1;
				goto out;
			}
			if (st.st_size > 0) {
				void * map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, src, 0);
				if (map == MAP_FAILED) {
					printf("%-46s errno=%d (%s)\n", "mmap(src, PROT_READ) failed",
					    errno, strerror(errno));
					rc = 1;
					goto out;
				}
				/* Cold mapping: do not pre-touch pages before writeData: enters kernel copyin. */
				data = [[NSData alloc] initWithBytesNoCopy:map
				                                    length:(NSUInteger)st.st_size
				                               deallocator:^(void * p, NSUInteger l) {
					                               munmap(p, l);
				                               }];
			} else {
				data = [NSData data];
			}
			printf("%-46s len=%lu\n", "NSData mmap(PROT_READ, MAP_PRIVATE)",
			    (unsigned long)[data length]);
		}

		NSFileHandle * fh = [[NSFileHandle alloc] initWithFileDescriptor:dst closeOnDealloc:YES];
		dst = -1;
		@try {
			[fh writeData:data];
			printf("%-46s ok\n", "writeData: threw nothing");
		} @catch (NSException * e) {
			printf("%-46s %s: %s\n", "writeData: threw",
			    [[e name] UTF8String], [[e reason] UTF8String]);
			rc = 1;
		}
		[fh closeFile];
	}
out:
	if (src >= 0) {
		close(src);
	}
	if (dst >= 0) {
		close(dst);
	}
	printf("-- %s done (rc=%d) --\n", tag, rc);
	return rc;
}

static int
run_raw_mmap_write(const char * tag, const char * dstPath, const char * srcPath)
{
	int dst = -1, src = -1;
	void * map = MAP_FAILED;
	size_t len = 0;
	int rc = 0;

	dst = open_dprotected_np(dstPath, O_WRONLY | O_CREAT | O_APPEND, 2, 0, 0640);
	src = open(srcPath, O_RDONLY);
	struct stat st;
	if (dst < 0 || src < 0 || fstat(src, &st) != 0) {
		printf("%-46s errno=%d (%s)\n", "raw_mmap_write setup failed", errno, strerror(errno));
		rc = 1;
		goto out;
	}
	len = (size_t)st.st_size;
	if (len > 0) {
		map = mmap(NULL, len, PROT_READ, MAP_PRIVATE, src, 0);
		if (map == MAP_FAILED) {
			printf("%-46s errno=%d (%s)\n", "raw mmap(src) failed", errno, strerror(errno));
			rc = 1;
			goto out;
		}
		size_t off = 0;
		while (off < len) {
			errno = 0;
			ssize_t n = write(dst, (const uint8_t *)map + off, len - off);
			if (n < 0) {
				if (errno == EINTR) {
					continue;
				}
				printf("%-46s off=%zu errno=%d (%s)\n", "raw write(dst, mmap_src) failed",
				    off, errno, strerror(errno));
				rc = 1;
				goto out;
			}
			off += (size_t)n;
		}
	}
	printf("%-46s wrote=%zu\n", "raw write(dst, mmap_src) ok", len);
out:
	if (map != MAP_FAILED) {
		munmap(map, len);
	}
	if (src >= 0) {
		close(src);
	}
	if (dst >= 0) {
		close(dst);
	}
	printf("-- %s done (rc=%d) --\n", tag, rc);
	return rc;
}

int
main(int argc, char ** argv)
{
	char dst[512], src[512];
	bool tempFiles = false;

	if (argc >= 2) {
		snprintf(src, sizeof(src), "%s", argv[1]);
		if (argc >= 3) {
			snprintf(dst, sizeof(dst), "%s", argv[2]);
		} else {
			snprintf(dst, sizeof(dst), "%s/fhw_probe_dst", DIR_);
		}
	} else if (find_source_in_dir(src, sizeof(src))) {
		snprintf(dst, sizeof(dst), "%s/allSecondaryCombined", DIR_);
	} else {
		tempFiles = true;
		snprintf(src, sizeof(src), "/private/var/tmp/fhw_src_%d.bin", getpid());
		snprintf(dst, sizeof(dst), "/private/var/tmp/fhw_dst_%d.bin", getpid());
		if (!create_temp_source(src, 20001)) {
			fprintf(stderr, "fhw_test: create_temp_source(%s) failed: %s\n", src, strerror(errno));
			return 1;
		}
	}

	if (tempFiles) {
		unlink(dst);
	}

	int fails = 0;
	fails += run_mmap_fh("mmap fresh create+append", dst, src, false);
	fails += run_mmap_fh("mmap reopen unaligned append", dst, src, false);
	fails += run_raw_mmap_write("raw write(dst, mmap_src) append", dst, src);
	fails += run_mmap_fh("heap initWithContentsOfFile append", dst, src, true);

	if (tempFiles) {
		unlink(src);
		unlink(dst);
	}
	return fails ? 1 : 0;
}
