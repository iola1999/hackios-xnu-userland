/*
 * Run the kernel's APFS reading code on the build machine.
 *
 * bsd/miscfs/apfs/apfs_lib.c is built into this program unchanged, so what it says about
 * an image here is what the driver will see.  `census` prints a line per directory entry
 * that tools/apfs/apfs.py can print too, and run-tests.sh diffs the two: the Python
 * reader has already been checked against 7-Zip over the whole volume, so agreeing with
 * it byte for byte is the C code agreeing with the image.
 *
 * Built by tools/apfs/build-tool.sh.
 *
 * usage: apfs-tool info   IMAGE
 *        apfs-tool ls     IMAGE [PATH]
 *        apfs-tool stat   IMAGE PATH
 *        apfs-tool cat    IMAGE PATH
 *        apfs-tool census IMAGE [PATH] [--meta]   (--meta: do not read contents)
 *        apfs-tool lookups IMAGE    (every name looked up by name; see cmd_lookups)
 *        apfs-tool writable IMAGE   (may the volume be written in place, and why not)
 *        apfs-tool write  IMAGE PATH OFFSET FILE|- [--fail-after N | --corrupt-after N]
 *        apfs-tool store  info IMAGE | save IMAGE FILE|- | load IMAGE FILE|-
 *                                   (the store of fixed-length slot files: bsd/miscfs/apfs/apfs_store.c)
 *                                   (the kernel's apfs_write.c; --fail-after is a power failure,
 *                                    --corrupt-after a device that lies about the write)
 */

#include "apfs_tool.h"

int
main(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		if ((strcmp(argv[i], "--fail-after") == 0 ||
		    strcmp(argv[i], "--corrupt-after") == 0) && i + 1 < argc) {
			write_fail_after = strtoull(argv[i + 1], NULL, 0);
			if (argv[i][2] == 'f') {
				write_fails = true;
			} else {
				write_corrupts = true;
			}
			memmove(&argv[i], &argv[i + 2], (size_t)(argc - i - 2) * sizeof(*argv));
			argc -= 2;
			i--;
		}
	}
	if (argc < 3) {
		fprintf(stderr,
		    "usage: apfs-tool info   IMAGE\n"
		    "       apfs-tool ls     IMAGE [PATH]\n"
		    "       apfs-tool stat   IMAGE PATH\n"
		    "       apfs-tool cat    IMAGE PATH\n"
		    "       apfs-tool census IMAGE [PATH] [--meta]\n"
		    "       apfs-tool write  IMAGE PATH OFFSET FILE|- [--fail-after N | --corrupt-after N]\n"
		    "       apfs-tool writable IMAGE\n"
		    "       apfs-tool store  info IMAGE | save IMAGE FILE|- [NOTE] | load IMAGE FILE|-\n"
		    "       apfs-tool lookups IMAGE\n"
		    "       apfs-tool cfrace IMAGE [THREADS [ROUNDS]]\n"
		    "       apfs-tool namehash IMAGE [OUT]\n"
		    "       apfs-tool lookupnames IMAGE LIST\n");
		return 2;
	}
	if (strcmp(argv[1], "info") == 0) {
		return cmd_info(argv[2]);
	}
	if (strcmp(argv[1], "ls") == 0) {
		return cmd_ls(argv[2], argc > 3 ? argv[3] : "/");
	}
	if (strcmp(argv[1], "stat") == 0 && argc > 3) {
		return cmd_stat(argv[2], argv[3]);
	}
	if (strcmp(argv[1], "cat") == 0 && argc > 3) {
		return cmd_cat(argv[2], argv[3]);
	}
	if (strcmp(argv[1], "census") == 0) {
		return cmd_census(argv[2], (argc > 3 && argv[3][0] == '/') ? argv[3] : NULL,
		    (argc > 3 && strcmp(argv[argc - 1], "--meta") == 0));
	}
	if (strcmp(argv[1], "write") == 0 && argc > 5) {
		return cmd_write(argv[2], argv[3], argv[4], argv[5]);
	}
	if (strcmp(argv[1], "writable") == 0) {
		return cmd_writable(argv[2]);
	}
	if (strcmp(argv[1], "store") == 0 && argc > 3) {
		if (strcmp(argv[2], "info") == 0) {
			return cmd_store_info(argv[3]);
		}
		if (strcmp(argv[2], "save") == 0 && argc > 4) {
			return cmd_store_save(argv[3], argv[4],
			    argc > 5 ? strtoull(argv[5], NULL, 0) : 0);
		}
		if (strcmp(argv[2], "load") == 0 && argc > 4) {
			return cmd_store_load(argv[3], argv[4]);
		}
		fprintf(stderr, "apfs-tool: store: info, save or load\n");
		return 2;
	}
	if (strcmp(argv[1], "lookups") == 0) {
		return cmd_lookups(argv[2]);
	}
	if (strcmp(argv[1], "namehash") == 0) {
		return cmd_namehash(argv[2], argc > 3 ? argv[3] : NULL);
	}
	if (strcmp(argv[1], "lookupnames") == 0 && argc > 3) {
		return cmd_lookupnames(argv[2], argv[3]);
	}
	if (strcmp(argv[1], "cfrace") == 0) {
		return cmd_cfrace(argv[2], argc > 3 ? atoi(argv[3]) : 4, argc > 4 ? atoi(argv[4]) : 3);
	}
	fprintf(stderr, "apfs-tool: unknown command %s\n", argv[1]);
	return 2;
}
