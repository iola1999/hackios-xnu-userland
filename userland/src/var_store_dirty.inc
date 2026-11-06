/*
 * Transient path filtering for var_store seed and archive packing.
 */

static const char *
str_find(const char *haystack, const char *needle)
{
	size_t nlen;
	if (!haystack || !needle) {
		return NULL;
	}
	if (!*needle) {
		return haystack;
	}
	nlen = strlen(needle);
	for (; *haystack; haystack++) {
		if (*haystack == *needle && strncmp(haystack, needle, nlen) == 0) {
			return haystack;
		}
	}
	return NULL;
}

static int
path_has_component(const char *p_path, const char *comp)
{
	size_t clen = strlen(comp);
	const char *p = p_path;

	while ((p = str_find(p, comp)) != NULL) {
		if ((p == p_path || *(p - 1) == '/') &&
		    (*(p + clen) == '\0' || *(p + clen) == '/')) {
			return 1;
		}
		p += clen;
	}
	return 0;
}

static int
is_transient_path(const char *relative)
{
	/* strictly preserve LaunchServices and icon caches */
	if (str_find(relative, "LaunchServices") != NULL ||
	    str_find(relative, "iconservices") != NULL ||
	    str_find(relative, "iconcache") != NULL) {
		return 0;
	}
	/* filter WebKit disk caches, URLCache fsCachedData, CrashReporter, Biome, Powerlog, CoreDuet */
	if (str_find(relative, "WebKit") != NULL ||
	    str_find(relative, "fsCachedData") != NULL ||
	    str_find(relative, "CrashReporter") != NULL ||
	    str_find(relative, "Biome") != NULL ||
	    str_find(relative, "Powerlog") != NULL ||
	    str_find(relative, "CoreDuet") != NULL ||
	    str_find(relative, "Safari/Favicon") != NULL ||
	    str_find(relative, "Safari/Touch Icons") != NULL) {
		return 1;
	}
	/* filter transient components across path levels */
	if (path_has_component(relative, "tmp") ||
	    path_has_component(relative, "run") ||
	    path_has_component(relative, "vm") ||
	    path_has_component(relative, "log") ||
	    path_has_component(relative, "logs") ||
	    path_has_component(relative, "diagnostics") ||
	    path_has_component(relative, "uuidtext") ||
	    path_has_component(relative, "jb")) {
		return 1;
	}
	return 0;
}
