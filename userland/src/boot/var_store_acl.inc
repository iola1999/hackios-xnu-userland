/*
 * An access control list put back on a file (included by var_store.c, before unpack_one).
 *
 * iOS keeps a file's ACL in the extended attribute com.apple.system.Security, a struct
 * kauth_filesec stored big-endian. setxattr refuses every com.apple.system. name, so the list is
 * turned back into the in-memory structure and given to chmod_extended, which stores it as that
 * attribute again. containermanagerd makes the system containers' directories mode 0, owned by
 * uid -2, and lets their processes in through such a list alone: restored without it, SpringBoard
 * cannot make its caches and aborts (kernel-12377-cmd30-warm3: "Couldn't create directory at
 * /private/var/containers/Data/System/...").
 */
#define SYS_chmod_extended      282
#define ACL_ATTRIBUTE           "com.apple.system.Security"
#define FILESEC_MAGIC           0x012cc16du
#define FILESEC_HEAD            44      /* magic, owner and group GUIDs, entry count, flags */
#define ACE_BYTES               24      /* applicable GUID, flags, rights */
#define ACL_MAX_ENTRIES         128     /* KAUTH_ACL_MAX_ENTRIES */
#define ACL_NONE                0xffffffffu     /* KAUTH_FILESEC_NOACL: a filesec without a list */
#define OWNER_UNCHANGED         (0xffffffffu - 100)     /* KAUTH_UID_NONE and KAUTH_GID_NONE */

static uint32_t
from_big(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int acl_restored;        /* the entry being unpacked had a list in the store */

/* 1 when ATTRIBUTE is the ACL one, whether or not the list could be put back; 0 for any other */
static int
restore_acl(const char *file, const char *attribute, const uint8_t *value, uint32_t vlen)
{
	static uint32_t filesec[(FILESEC_HEAD + ACL_MAX_ENTRIES * ACE_BYTES) / 4];
	uint32_t count, entries;

	if (strcmp(attribute, ACL_ATTRIBUTE) != 0) {
		return 0;
	}
	acl_restored = 1;
	count = vlen >= FILESEC_HEAD ? from_big(value + 36) : 0;
	entries = count == ACL_NONE ? 0 : count;
	if (vlen < FILESEC_HEAD || from_big(value) != FILESEC_MAGIC || entries > ACL_MAX_ENTRIES ||
	    vlen < FILESEC_HEAD + entries * ACE_BYTES) {
		printf("var_store: %s: an access list that does not read as one is left off\n", file);
		return 1;
	}
	/* the GUIDs are byte strings and go as they are; the 32-bit words are turned round */
	memcpy(filesec, value, FILESEC_HEAD + entries * ACE_BYTES);
	filesec[0] = FILESEC_MAGIC;
	filesec[9] = count;
	filesec[10] = from_big(value + 40);
	for (uint32_t i = 0; i < entries; i++) {
		const uint8_t *ace = value + FILESEC_HEAD + i * ACE_BYTES;

		filesec[11 + i * 6 + 4] = from_big(ace + 16);
		filesec[11 + i * 6 + 5] = from_big(ace + 20);
	}
	if (mini_syscall(SYS_chmod_extended, (long)file, OWNER_UNCHANGED, OWNER_UNCHANGED, -1, (long)filesec, 0) < 0) {
		printf("var_store: %s: the access list could not be put back\n", file);
	}
	return 1;
}

/*
 * The list a new file inherits from its directory, taken off again when the store had none for it.
 * iconservicesagent removes the inherited list from its cache directory and throws the whole cache
 * away whenever it finds one there (-[ISMutableIconCache initWithURL:]), so a restore that left the
 * inherited list in place cost every boot the icon cache (kernel-12377-cmd31-warm5).
 */
#define FILESEC_REMOVE_ACL      1       /* _FILESEC_REMOVE_ACL: chmod_extended deletes the list */

static void
drop_inherited_acl(const char *file)
{
	if (file == NULL || acl_restored) {
		return;
	}
	mini_syscall(SYS_chmod_extended, (long)file, OWNER_UNCHANGED, OWNER_UNCHANGED, -1, FILESEC_REMOVE_ACL, 0);
}
