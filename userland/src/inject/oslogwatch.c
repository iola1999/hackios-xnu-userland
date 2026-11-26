/*
 * libannibale-oslog.dylib: a process's os_log messages, default level and above, into the kernel log.
 *
 * Applications and the extensions they start have their standard error going nowhere, and
 * OS_ACTIVITY_DT_MODE is read once while libSystem starts, before an inserted library runs
 * (inject_posterboard.c). os_log_set_hook, the hook libtrace keeps for Xcode's console, sees every
 * message the process emits; each goes to kern.annibale.say as "oslog TYPE: TEXT". At most
 * OSLOG_LINES are forwarded per process, so a chatty process does not push the rest of the kernel's
 * log out. package.py inserts this library into the programs OSLOG names and sets OSLOG_LINES from
 * its own OSLOG_LINES; with OSLOG_MATCH ("WORD|WORD...") only messages containing one of the words
 * are forwarded, for a chatty process such as SpringBoard.
 */
#include <os/log.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

/* libsystem_trace's private interface (exported, not in the SDK's headers) */
typedef struct os_log_message_s *os_log_message_t;
typedef void (^os_log_hook_t)(os_log_type_t type, os_log_message_t message);
extern os_log_hook_t os_log_set_hook(os_log_type_t level, os_log_hook_t hook);
extern char *os_log_copy_message_string(os_log_message_t message);

#ifndef OSLOG_LINES
#define OSLOG_LINES 4000
#endif

/*
 * libsystem_trace decides per message whether the private payloads of an os_log message
 * serialize or mask as <private>, and the process-level override is one byte in its __data:
 * nonzero, every message this process emits keeps its private arguments. The byte is written
 * once as the library initializes and read on each emission, so setting it in this constructor
 * covers every message after it; the page is writable in each process (the library's own
 * initialization stores into it), the write needs no vm_protect, and the cache page's
 * copy-on-write keeps the change inside this process (harvest-break-and-ioerror-hooks.md
 * §2.2/§2.4). The address is the 23G90 cache's, before the slide: another system build puts
 * the byte elsewhere, and the patch has to move with it. Espresso's handle_ex_plan logs the
 * exception's what() text -- the weight file paths its consumers actually open -- as a %s
 * without a {public} annotation, which is what this unlocks. First-round verification:
 * mediaanalysisd's "exception=" is followed by "Error loading file:", "Error mapping file:"
 * or "MMapped data not available" instead of <private>.
 */
#define CACHE_BASE 0x180000000UL          /* where the shared cache was built to be, before the slide */
#define TRACE_PRIVACY_BYTE 0x1ecca1ce0UL  /* libsystem_trace's tools-privacy byte, 23G90 */

extern const void *_dyld_get_shared_cache_range(size_t *length);

static _Atomic unsigned said;
static os_log_hook_t previous;

static void
say(const char *text)
{
	(void)sysctlbyname("kern.annibale.say", NULL, NULL, (void *)text, strlen(text) + 1);
}

static const char *
type_name(os_log_type_t type)
{
	switch (type) {
	case OS_LOG_TYPE_ERROR: return "error";
	case OS_LOG_TYPE_FAULT: return "fault";
	case OS_LOG_TYPE_INFO: return "info";
	case OS_LOG_TYPE_DEBUG: return "debug";
	default: return "default";
	}
}

static int
wanted(const char *text)
{
#ifdef OSLOG_MATCH
	for (const char *word = OSLOG_MATCH; *word != '\0';) {
		size_t len = strcspn(word, "|");

		for (const char *at = text; (at = strchr(at, word[0])) != NULL; at++) {
			if (len != 0 && strncmp(at, word, len) == 0)
				return 1;
		}
		word += len + (word[len] == '|');
	}
	return 0;
#else
	(void)text;
	return 1;
#endif
}

static void
forward(os_log_type_t type, os_log_message_t message)
{
	char line[256], *text;
	unsigned n;

	text = os_log_copy_message_string(message);
	if (text == NULL)
		return;
	if (!wanted(text)) {
		free(text);
		return;
	}
	n = atomic_fetch_add(&said, 1);
	if (n >= OSLOG_LINES) {
		if (n == OSLOG_LINES)
			say("oslog: line limit reached, later messages are not forwarded");
		free(text);
		return;
	}
	/* one kernel log line per message: say cuts a line at its end of line */
	for (char *c = text; *c != '\0'; c++) {
		if (*c == '\n' || *c == '\r')
			*c = ' ';
	}
	snprintf(line, sizeof(line), "oslog %s: %s", type_name(type), text);
	free(text);
	say(line);
}

static void
unlock_private_log(void)
{
	size_t length = 0;
	const void *cache = _dyld_get_shared_cache_range(&length);

	if (cache == NULL) {
		say("oslog: no shared cache range found; private payloads stay masked");
		return;
	}
	uintptr_t slide = (uintptr_t)cache - CACHE_BASE;
	volatile uint8_t *byte = (volatile uint8_t *)(TRACE_PRIVACY_BYTE + slide);

	if (*byte != 0) {
		char line[128];

		snprintf(line, sizeof(line), "oslog: trace privacy byte is %u, expected 0; left alone",
		    (unsigned)*byte);
		say(line);
		return;
	}
	*byte = 1;
	say("oslog: trace privacy byte set; private payloads serialize");
}

__attribute__((constructor)) static void
oslog_start(void)
{
	unlock_private_log();
	previous = os_log_set_hook(OS_LOG_TYPE_DEFAULT, ^(os_log_type_t type, os_log_message_t message) {
		if (previous != NULL)
			previous(type, message);
		forward(type, message);
	});
	say("oslog: forwarding this process's os_log messages");
}
