/*
 * Request diagnostics preserve URLSession behavior and object ownership. Logs contain
 * host/path hashes, HTTP status and error domain/code; URL text and userInfo stay private.
 * REDWOOD_NET_TRACE=1 observes the local, background and background-download completion methods
 * present in 23G90 CFNetwork.
 */
static void (*finish_local_task)(id, SEL, id), (*finish_background_task)(id, SEL, id);
static void (*finish_background_download_task)(id, SEL, id);

static id
network_object(id object, const char *selector)
{
	SEL cmd = sel_registerName(selector);
	if (object == NULL || !class_respondsToSelector(object_getClass(object), cmd)) {
		return NULL;
	}
	return ((id (*)(id, SEL))objc_msgSend)(object, cmd);
}

static long long
network_number(id object, const char *selector)
{
	SEL cmd = sel_registerName(selector);
	if (object == NULL || !class_respondsToSelector(object_getClass(object), cmd)) {
		return 0;
	}
	return ((long long (*)(id, SEL))objc_msgSend)(object, cmd);
}

static const char *
network_text(id object)
{
	return object == NULL ? NULL : ((const char *(*)(id, SEL))objc_msgSend)(object, sel_registerName("UTF8String"));
}

/* A bounded hash permits request correlation without outputting URL components. */
static uint64_t
network_hash(const char *text)
{
	uint64_t hash = UINT64_C(14695981039346656037);
	if (text == NULL) {
		return 0;
	}
	for (size_t i = 0; i < 2048 && text[i] != 0; i++) {
		hash = (hash ^ (unsigned char)text[i]) * UINT64_C(1099511628211);
	}
	return hash;
}

static const char *
network_error_domain(id error)
{
	const char *domain = network_text(network_object(error, "domain"));
	static const char *const known[] = { "NSURLErrorDomain", "NSPOSIXErrorDomain", "NSOSStatusErrorDomain",
		"NSCocoaErrorDomain", "kCFErrorDomainCFNetwork", "kCFErrorDomainSSL" };
	if (error == NULL) {
		return "none";
	}
	for (size_t i = 0; domain != NULL && i < sizeof(known) / sizeof(known[0]); i++) {
		if (strcmp(domain, known[i]) == 0) {
			return known[i];
		}
	}
	return "other";
}

static void
network_task_record(id task, id error, const char *stage)
{
	int saved_errno = errno;
	id request = network_object(task, "originalRequest");
	id url = network_object(request, "URL");
	id response = network_object(task, "response");
	const char *scheme = network_text(network_object(url, "scheme"));
	const char *kind = scheme != NULL && strcmp(scheme, "https") == 0 ? "https" :
	    scheme != NULL && strcmp(scheme, "http") == 0 ? "http" : "other";
	uint64_t host = network_hash(network_text(network_object(url, "host")));
	uint64_t path = network_hash(network_text(network_object(url, "path")));
	char line[384];
	int n = snprintf(line, sizeof(line),
	    "redwood: %.64s: network stage=%s task=%lld scheme=%s host_hash=%016llx path_hash=%016llx "
	    "http_status=%lld error_present=%u error_domain=%s error_code=%lld received=%lld\n",
	    who, stage, network_number(task, "taskIdentifier"), kind, (unsigned long long)host,
	    (unsigned long long)path, network_number(response, "statusCode"), error != NULL,
	    network_error_domain(error), network_number(error, "code"), network_number(task, "countOfBytesReceived"));
	if (n > 0) {
		say(line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
	}
	errno = saved_errno;
}

static void
watched_resume_task(id self, SEL cmd)
{
	network_task_record(self, NULL, "resume");
	resume_task(self, cmd);
}

static void
watched_finish_local_task(id self, SEL cmd, id error)
{
	network_task_record(self, error, "local-finish");
	finish_local_task(self, cmd, error);
}

static void
watched_finish_background_task(id self, SEL cmd, id error)
{
	network_task_record(self, error, "background-finish");
	finish_background_task(self, cmd, error);
}

static void
watched_finish_background_download_task(id self, SEL cmd, id error)
{
	network_task_record(self, error, "background-download-finish");
	finish_background_download_task(self, cmd, error);
}

static void
watch_network_finish(const char *name, IMP replacement, void *original)
{
	Class cls = objc_getClass(name);
	SEL selector = sel_registerName("_onqueue_didFinishWithError:");
	Method method = cls != NULL ? class_getInstanceMethod(cls, selector) : NULL;
	/* The static 23G90 method type is v24@0:8@16. Check the live ABI before changing it. */
	if (method != NULL && method_getNumberOfArguments(method) == 3 &&
	    strcmp(method_getTypeEncoding(method), "v24@0:8@16") == 0 &&
	    method_getImplementation(method) != replacement) {
		*(IMP *)original = method_setImplementation(method, replacement);
	}
}

static void
watch_network_completion(void)
{
	const char *enabled = getenv("REDWOOD_NET_TRACE");
	if (enabled == NULL || strcmp(enabled, "1") != 0) {
		return;
	}
	watch_network_finish("__NSCFLocalSessionTask", (IMP)watched_finish_local_task, &finish_local_task);
	watch_network_finish("__NSCFBackgroundSessionTask", (IMP)watched_finish_background_task, &finish_background_task);
	watch_network_finish("__NSCFBackgroundDownloadTask", (IMP)watched_finish_background_download_task,
	    &finish_background_download_task);
	char line[192];
	int install_errno = errno;
	int n = snprintf(line, sizeof(line),
	    "redwood: %.64s: network completion hooks local=%u background=%u background_download=%u\n",
	    who, finish_local_task != NULL, finish_background_task != NULL, finish_background_download_task != NULL);
	if (n > 0) {
		say(line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
	}
	errno = install_errno;
}
