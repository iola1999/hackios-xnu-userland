/*
 * libredwood-fpvolume.dylib, inserted into fileproviderd (tools/k90auto/package.py FPVOLUME): the
 * ramfs on /private/var accepted as a volume for FileProvider's domains.
 *
 * fileproviderd keeps every domain on a volume it has accepted, and it accepts one only in
 * -[FPDVolume initWithStatFS:knownRole:volumeManager:], which asks FPVolumeFPFSSupported
 * (FileProvider.framework): read-write, local, not quarantined, and f_fstypename "apfs". The home
 * volume is the one under /private/var/mobile, and /private/var is the ramfs init mounts, so it is
 * refused. -[FPDServer _setup] then goes on with no volume: fileproviderctl dump lists 5 providers and
 * no domain, Files has no "On My iPhone", Safari cannot move a finished download anywhere, and
 * opening Photos stops the daemon with "FPDDomain must have a volume attached"
 * (kernel-12377-rate10-w2).
 *
 * FileProviderDaemon reaches FPVolumeFPFSSupported and statfs_ext through the shared cache's direct
 * branches, so neither can be interposed. The method that asks is replaced instead, and for the ramfs
 * on /private/var it gets a copy of the statfs whose type reads "apfs"; the other three conditions are
 * still checked on the real flags. LocalStorage is not an FPFS provider (-[FPDProvider _supportsFPFS]
 * is NO for it), so its domain needs nothing else of APFS.
 *
 * Built by tools/k90auto/package.py (FPVOLUME_SOURCES).
 */
#include <objc/runtime.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>

typedef id (*init_with_statfs_t)(id, SEL, struct statfs *, long, id);

static init_with_statfs_t original;

static id
init_with_statfs(id self, SEL cmd, struct statfs *fs, long role, id manager)
{
	struct statfs copy;

	if (fs == NULL || strcmp(fs->f_fstypename, "ramfs") != 0 || strcmp(fs->f_mntonname, "/private/var") != 0) {
		return original(self, cmd, fs, role, manager);
	}
	copy = *fs;
	strlcpy(copy.f_fstypename, "apfs", sizeof(copy.f_fstypename));
	fprintf(stderr, "fpvolume: /private/var (ramfs) offered to FPDVolume as apfs, role %ld\n", role);
	return original(self, cmd, &copy, role, manager);
}

__attribute__((constructor)) static void
fpvolume_start(void)
{
	Class volume = objc_getClass("FPDVolume");
	Method method = volume != Nil ? class_getInstanceMethod(volume, sel_registerName("initWithStatFS:knownRole:volumeManager:")) : NULL;

	if (method == NULL) {
		fprintf(stderr, "fpvolume: -[FPDVolume initWithStatFS:knownRole:volumeManager:] not found, nothing replaced\n");
		return;
	}
	original = (init_with_statfs_t)method_setImplementation(method, (IMP)init_with_statfs);
}
