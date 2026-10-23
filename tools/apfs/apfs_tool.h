/*
 * Declarations shared between apfs-tool.c (the usage and the command dispatch) and the
 * pieces the commands live in: img.c defines the device, the power failure the device
 * stands in for, and the lock the kernel holds round apfs_lib.  Built by build-tool.sh.
 */

#ifndef APFS_TOOL_H
#define APFS_TOOL_H

/* pread(2) and clock_gettime(2) come with -D_DEFAULT_SOURCE, set by build-tool.sh */
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "apfs_lib.h"
#include "apfs_write.h"
#include "apfs_store.h"

/* The same hash the Python side computes, so contents can be compared without the bytes. */
#define FNV_INIT 0xcbf29ce484222325ULL
uint64_t fnv1a(const uint8_t *p, size_t n, uint64_t h);

/* the image opened by every command, read-only unless want_write says otherwise (img.c) */
extern bool want_lock;                 /* the library under a lock, as in the kernel (walkers, cfrace) */
extern bool want_write;
extern bool write_fails;               /* --fail-after was given; 0 of them is a failure too */
extern bool write_corrupts;            /* --corrupt-after: a device that lies about a write */
extern uint64_t write_fail_after;
int open_image(const char *path, struct apfs_nx *nx, struct apfs_vol *vol);

/* several threads on the library at once, as in the kernel (img.c) */
extern pthread_mutex_t host_lock;
int host_parallel(void *ctx, uint32_t items, uint32_t workers,
    int (*fn)(void *arg, uint32_t item, uint32_t worker), void *arg);
void op_begin(void);
void op_end(void);

/* the commands, one file each: cmd_basic.c, cmd_census.c, cmd_cfrace.c, cmd_names.c, cmd_write.c */
int cmd_info(const char *path);
int cmd_ls(const char *path, const char *where);
int cmd_stat(const char *path, const char *where);
int cmd_cat(const char *path, const char *where);
int cmd_census(const char *path, const char *where, bool meta_only);
int cmd_cfrace(const char *path, int n, int rounds);
int cmd_namehash(const char *path, const char *out);
int cmd_lookupnames(const char *path, const char *list);
int cmd_lookups(const char *path);
int cmd_write(const char *path, const char *in_path, const char *at_str, const char *from);
int cmd_writable(const char *path);
int cmd_store_info(const char *path);
int cmd_store_save(const char *path, const char *from, uint64_t note);
int cmd_store_load(const char *path, const char *to);

#endif /* APFS_TOOL_H */
