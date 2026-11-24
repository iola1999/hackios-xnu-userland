/*
 * fhw_test: reproduce locationd's crash frame on the real archive path.
 *
 * locationd aborts in -[NSConcreteFileHandle writeData:] while appending each
 * locScoreInfo/secondary-* file into locScoreInfo/allSecondaryCombined: the
 * destination is opened with open_dprotected_np(path, O_WRONLY|O_CREAT|O_APPEND,
 * class 2, 0, 0640). POSIX writes in the same directory succeed (dpw_test), so
 * this runs the identical NSFileHandle+writeData: sequence on the real file and
 * prints the exception name/reason and errno if Foundation throws.
 *
 * usage: fhw_test
 */
#import <Foundation/Foundation.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char * DIR_ = "/var/root/Library/Caches/locationd/locScoreInfo";

static void
run_once(const char * tag, const char * dstPath, const char * srcPath)
{
	int dst = -1, src = -1;
	@autoreleasepool {
		dst = open_dprotected_np(dstPath, O_WRONLY | O_CREAT | O_APPEND, 2, 0, 0640);
		printf("%-46s fd=%d errno=%d (%s)\n", "open_dprotected_np(dst)", dst,
		    dst < 0 ? errno : 0, dst < 0 ? strerror(errno) : "-");
		src = open(srcPath, O_RDONLY);
		printf("%-46s fd=%d errno=%d (%s)\n", "open(src, O_RDONLY)", src,
		    src < 0 ? errno : 0, src < 0 ? strerror(errno) : "-");
		if (dst < 0 || src < 0) {
			goto out;
		}
		NSData * data = [[NSData alloc] initWithContentsOfFile:@(srcPath)];
		printf("%-46s len=%lu\n", "NSData initWithContentsOfFile(src)", (unsigned long)[data length]);
		NSFileHandle * fh = [[NSFileHandle alloc] initWithFileDescriptor:dst closeOnDealloc:YES];
		dst = -1; /* the handle owns it now */
		@try {
			[fh writeData:data];
			printf("%-46s ok\n", "writeData: threw nothing");
		} @catch (NSException * e) {
			printf("%-46s %s: %s\n", "writeData: threw",
			    [[e name] UTF8String], [[e reason] UTF8String]);
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
	printf("-- %s done --\n", tag);
}

int
main(void)
{
	char dst[256], src[256];
	snprintf(dst, sizeof(dst), "%s/allSecondaryCombined", DIR_);
	snprintf(src, sizeof(src), "%s/secondary-817146251.189629-1", DIR_);

	run_once("fresh create+append", dst, src);   /* file absent: O_CREAT path */
	run_once("reopen append", dst, src);         /* file exists: O_APPEND path */
	return 0;
}
