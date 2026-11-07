/*
 * Iris VideoToolbox integration is intentionally unavailable until the native
 * HFI gen2 session packets and VideoToolbox callback ABI are implemented.
 * This file remains an explicit load-time gate and is not in MEDIA_SOURCES.
 */
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

static void
iris_plugin_log(const char *format, ...)
{
	char line[256];
	va_list ap;
	va_start(ap, format);
	int n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > 0) {
		write(2, line, (size_t)n);
	}
}

void
register_iris_videotoolbox_decoders(void)
{
	static bool reported;
	if (!reported) {
		reported = true;
		iris_plugin_log("iris_vt: decoder registration unavailable; native HFI and VideoToolbox callback ABI are incomplete\n");
	}
}

__attribute__((constructor))
static void
iris_plugin_init(void)
{
	register_iris_videotoolbox_decoders();
}
