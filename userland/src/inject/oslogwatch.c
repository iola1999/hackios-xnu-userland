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

__attribute__((constructor)) static void
oslog_start(void)
{
	previous = os_log_set_hook(OS_LOG_TYPE_DEFAULT, ^(os_log_type_t type, os_log_message_t message) {
		if (previous != NULL)
			previous(type, message);
		forward(type, message);
	});
	say("oslog: forwarding this process's os_log messages");
}
