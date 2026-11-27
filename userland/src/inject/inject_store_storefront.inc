/* Storefront hooks, request tracing, reporting, and constructor, included into inject_store.c. */

/* ---- the storefront, answered ----------------------------------------------------------------- */

static id (*old_ams_storefront)(id, SEL);
static id (*old_cached_storefront)(id, SEL);
static id (*old_bag_storefront)(id, SEL, id);
static id (*old_bag_storefront_active)(id, SEL, id);
static id (*old_copy_storefront)(id, SEL, id);
static id (*old_bag_account)(id, SEL, id, bool);
static id (*old_perform_fetch)(id, SEL, long, id, id);
static id (*old_query_items)(id, SEL, id, id, id, id, id, id);
static id (*old_perform_request)(id, SEL, id, id);
static id (*old_perform_request_block)(id, SEL, id, id);
static void (*old_resume_base)(id, SEL);
static void (*old_resume_local)(id, SEL);
static id (*old_create_request)(id, SEL, id);
static id (*old_session_promise)(id, SEL);
static id (*old_data_task_promise)(id, SEL, id);
static id (*old_data_task)(id, SEL, id);

static id
new_ams_storefront(id self, SEL _cmd)
{
	entered[COUNT_ams_storefront]++;
	id was = old_ams_storefront != NULL ? old_ams_storefront(self, _cmd) : NULL;
	static bool said;

	if (!said) {
		char buffer[128];

		said = true;
		say("annibale: store: ams_storefront was %s, now " STOREFRONT "\n", text_of(was, buffer, sizeof(buffer)));
	}
	returned[COUNT_ams_storefront]++;
	return was != NULL ? was : (id)storefront;
}

static id
new_cached_storefront(id self, SEL _cmd)
{
	entered[COUNT_cachedStorefront]++;
	id was = old_cached_storefront != NULL ? old_cached_storefront(self, _cmd) : NULL;

	say("annibale: store: cachedStorefront was %s\n", was != NULL ? "set" : "(nil), now " STOREFRONT);
	returned[COUNT_cachedStorefront]++;
	return was != NULL ? was : (id)storefront;
}

static id
new_bag_storefront(id self, SEL _cmd, id media_type)
{
	entered[COUNT_bagStorefront_fixed]++;
	id was = old_bag_storefront != NULL ? old_bag_storefront(self, _cmd, media_type) : NULL;
	char buffer[128];

	say("annibale: store: bagStorefrontForAccountMediaType:%s was %s\n", text_of(media_type, buffer, sizeof(buffer)),
	    was != NULL ? "set" : "(nil), now " STOREFRONT);
	returned[COUNT_bagStorefront_fixed]++;
	return was != NULL ? was : (id)storefront;
}

static id
new_bag_storefront_active(id self, SEL _cmd, id media_type)
{
	entered[COUNT_bagStorefront_active]++;
	id was = old_bag_storefront_active != NULL ? old_bag_storefront_active(self, _cmd, media_type) : NULL;
	char buffer[128];

	say("annibale: store: active bagStorefrontForAccountMediaType:%s was %s\n",
	    text_of(media_type, buffer, sizeof(buffer)), was != NULL ? "set" : "(nil), now " STOREFRONT);
	returned[COUNT_bagStorefront_active]++;
	return was != NULL ? was : (id)storefront;
}

static id
new_copy_storefront(id self, SEL _cmd, id account)
{
	entered[COUNT_copyStorefront]++;
	id was = old_copy_storefront != NULL ? old_copy_storefront(self, _cmd, account) : NULL;

	say("annibale: store: ams_copyStorefrontFromAccount: was %s\n",
	    was != NULL ? "set" : "(nil), now " STOREFRONT);
	returned[COUNT_copyStorefront]++;
	return was != NULL ? was : (id)storefront;
}

/* not replaced, watched: this is the fallback line, and which way it goes is the question */
static id
new_bag_account(id self, SEL _cmd, id media_type, bool must_contain_storefront)
{
	entered[COUNT_bagAccount]++;
	id account = old_bag_account(self, _cmd, media_type, must_contain_storefront);
	returned[COUNT_bagAccount]++;
	char one[128], two[192];

	say("annibale: store: _bagAccountForAccountMediaType:%s mustContainStorefront:%d -> %s\n",
	    text_of(media_type, one, sizeof(one)), must_contain_storefront ? 1 : 0, text_of(account, two, sizeof(two)));
	return account;
}

/* ---- is a request ever built? ------------------------------------------------------------------ */

static id
new_perform_fetch(id self, SEL _cmd, long attempt, id account, id front)
{
	entered[COUNT_performFetch]++;
	char one[160], two[64];

	id promise;

	say("annibale: store: BAG FETCH attempt %ld, storefront %s, account %s\n", attempt,
	    text_of(front, two, sizeof(two)), text_of(account, one, sizeof(one)));
	for (size_t i = 0; i < replacement_count(); i++) {
		if (!replacements[i].done) {
			say("annibale: store: NOT TAKEN %c[%s %s]\n", replacements[i].class_method ? '+' : '-',
			    replacements[i].class_name, replacements[i].selector);
		}
	}
	promise = old_perform_fetch(self, _cmd, attempt, account, front);
	returned[COUNT_performFetch]++;
	note_label(promise, "THE BAG FETCH", NULL);
	say("annibale: store: BAG FETCH returned %s\n", promise != NULL ? "a promise" : "(nil)");
	return promise;
}

static id
new_query_items(id self, SEL _cmd, id client, id profile, id version, id cookies, id account, id front)
{
	entered[COUNT_queryItems]++;
	id items = old_query_items(self, _cmd, client, profile, version, cookies, account, front);
	char one[64], two[160];

	say("annibale: store: BAG QUERY profile %s, storefront %s -> %s\n", text_of(profile, one, sizeof(one)),
	    text_of(front, two, sizeof(two)), items != NULL ? "items" : "(nil)");
	return items;
}

/* the session layer: a request that is built but never sent stops between these and the wire */
static id
new_perform_request(id self, SEL _cmd, id request, id completion)
{
	entered[COUNT_performRequest]++;
	char buffer[320];

	say("annibale: store: AMSURLSession performRequest:completion: %s\n", text_of(request, buffer, sizeof(buffer)));
	return old_perform_request(self, _cmd, request, completion);
}

static id
new_perform_request_block(id self, SEL _cmd, id request, id completion)
{
	entered[COUNT_performRequestBlock]++;
	char buffer[320];

	say("annibale: store: AMSURLSession performRequest:completionBlock: %s\n", text_of(request, buffer, sizeof(buffer)));
	return old_perform_request_block(self, _cmd, request, completion);
}

/*
 * And the narrowest gate of all: every transfer, foreground or background, in process or handed to
 * nsurlsessiond, becomes a task that somebody resumes. If the bag request is built and no task is
 * ever resumed, it never became a transfer at all; if one is, the loss is below CFNetwork.
 */
static void
say_resume(id task, const char *where)
{
	char buffer[320];
	id request = ((id (*)(id, SEL))objc_msgSend)(task, sel_registerName("originalRequest"));

	say("annibale: store: TASK RESUME (%s) %s\n", where, text_of(request, buffer, sizeof(buffer)));
}

static void
new_resume_base(id self, SEL _cmd)
{
	entered[COUNT_resume]++;
	say_resume(self, "NSURLSessionTask");
	remember_task(self);
	old_resume_base(self, _cmd);
	returned[COUNT_resume]++;
}

static void
new_resume_local(id self, SEL _cmd)
{
	entered[COUNT_resumeLocal]++;
	say_resume(self, "__NSCFLocalSessionTask");
	old_resume_local(self, _cmd);
}

/*
 * What a class really answers to. The bag request is built and no task is ever resumed, so the stop
 * is between the two, in AMS's own session layer -- whose selectors are not the ones guessed from
 * the cache's strings (AMSURLSession has no performRequest:completion:). Rather than guess again,
 * ask the runtime, late enough that the console is no longer dropping lines.
 */
static void
dump_selectors(const char *class_name, const char *needle)
{
	Class cls = objc_getClass(class_name);
	unsigned count = 0;
	Method *methods = cls != NULL ? class_copyMethodList(cls, &count) : NULL;

	if (methods == NULL) {
		say("annibale: store: %s: no such class\n", class_name);
		return;
	}
	for (unsigned i = 0; i < count; i++) {
		const char *name = sel_getName(method_getName(methods[i]));

		if (needle == NULL || strstr(name, needle) != NULL) {
			say("annibale: store: %s has -%s\n", class_name, name);
		}
	}
	free(methods);
}

static void
one_report(unsigned seconds)
{
	bool first = seconds <= 15;

	say("annibale: store(%s): report at %u s\n", who, seconds);
	image_added(NULL, 0);           /* and try again for anything whose class has turned up since */
	/*
	 * These two on their own, before the long report: last time the brotli counter was one line of
	 * sixty and it went missing, and a missing line is what this branch has been wrong about five
	 * times. A number that matters does not travel in a crowd.
	 */
	say("annibale: store: KEY brotli entered %lu returned %lu, processStream entered %lu returned %lu\n",
	    entered[COUNT_brotli], returned[COUNT_brotli], entered[COUNT_processStream], returned[COUNT_processStream]);

	for (size_t i = 0; i < replacement_count(); i++) {
		if (!replacements[i].done) {
			say("annibale: store: NOT TAKEN %c[%s %s]\n", replacements[i].class_method ? '+' : '-',
			    replacements[i].class_name, replacements[i].selector);
		}
	}
	for (int i = 0; i < COUNT_last; i++) {
		/*
		 * A hook that never fires says so loudly, once. Silence from one is not evidence about the
		 * code: ams_addCookiesForAccount:clientInfo:bag: was hooked and never entered all night
		 * because the cookies go through the FOUR-argument
		 * ams_addCookiesForAccount:clientInfo:bag:cleanupGlobalCookies: instead. Hooked and never
		 * entered can mean the hook is wrong, not the code.
		 */
		say("annibale: store: entered %4lu returned %4lu %s%s\n", entered[i], returned[i], counted[i],
		    entered[i] > returned[i] ? "   <-- STILL IN IT" :
		    entered[i] == 0 ? "   <-- NEVER ENTERED: suspect the hook, not the code" : "");
	}
	dump_promises();
	dump_tasks();
	dump_bagkeys();
	dump_errors();
	dump_threads();
	/* the long dumps once; the numbers in every report, because only a late one survives the log */
	if (first) {
		dump_selectors("AMSURLSession", "equest");
		dump_selectors("JEBrotliDecoder", NULL);
		dump_selectors("_TtGC9JetEngine7PromiseT__", NULL);
		dump_selectors("JetEngine", NULL);
		dump_classes("Jet");
		dump_classes("JE");
	}
	say("annibale: store: %lu notifications posted\n", notifications);
}

/*
 * Every minute rather than once. appstored starts with the system and the App Store starts when
 * somebody taps it, a minute and a half later, so a single report at 150 s falls past the end of
 * the run for one of them -- and the app's side of the story went missing in exactly that way.
 */
static void *
late_report(__unused void *ignored)
{
	/*
	 * Every fifteen seconds, to the end of the run. The loader keeps the first 256 KB and the last
	 * 1 MB of a run's log and drops everything between, and with iOS and REST=1 the middle is most
	 * of it: a report once a minute still fell inside the hole, twice in a row, which is why these
	 * measurements kept looking missing when nothing was wrong with them. Often enough that one
	 * lands in the last megabyte is the only thing that works, so the long dumps happen once and the
	 * numbers repeat.
	 */
	for (unsigned n = 1; n <= 40; n++) {
		sleep(15);
		one_report(n * 15);
	}
	return NULL;
}

/*
 * The four steps between a bag request and a transfer, as the runtime says AMS really spells them:
 * AMSBagNetworkTask builds the request (_createRequestWithQueryItems:), asks for its session
 * (URLSessionPromise -- made lazily, and AMS has a line about being deallocated "while lazily
 * instantiating its AMSURLSession"), and the session makes a task (dataTaskPromiseWithRequest: or
 * dataTaskWithRequest:) which somebody resumes. resume is never reached, so the stop is one of
 * these; each says what it was given and what it gave back.
 */
static id
new_create_request(id self, SEL _cmd, id items)
{
	id request;
	char buffer[320];

	entered[COUNT_createRequest]++;
	request = old_create_request(self, _cmd, items);
	returned[COUNT_createRequest]++;
	say("annibale: store: STEP _createRequestWithQueryItems: -> %s\n", text_of(request, buffer, sizeof(buffer)));
	note_label(request, "THE BAG REQUEST", NULL);
	return request;
}

static id
new_session_promise(id self, SEL _cmd)
{
	id promise;
	char buffer[200];

	entered[COUNT_sessionPromise]++;
	promise = old_session_promise(self, _cmd);
	returned[COUNT_sessionPromise]++;
	say("annibale: store: STEP URLSessionPromise -> %s\n", text_of(promise, buffer, sizeof(buffer)));
	return promise;
}

static id
new_data_task_promise(id self, SEL _cmd, id request)
{
	id promise;
	char buffer[200];

	entered[COUNT_dataTaskPromise]++;
	promise = old_data_task_promise(self, _cmd, request);
	returned[COUNT_dataTaskPromise]++;
	say("annibale: store: STEP dataTaskPromiseWithRequest: -> %s\n", text_of(promise, buffer, sizeof(buffer)));
	return promise;
}

static id
new_data_task(id self, SEL _cmd, id request)
{
	id task;
	char buffer[200];

	entered[COUNT_dataTask]++;
	task = old_data_task(self, _cmd, request);
	returned[COUNT_dataTask]++;
	say("annibale: store: STEP dataTaskWithRequest: -> %s\n", text_of(task, buffer, sizeof(buffer)));
	return task;
}

/*
 * What the request promise waits on. _createRequestWithQueryItems: hands back an AMSMutablePromise
 * and nothing below it is ever reached, so the thread is sitting in one of the steps that finish the
 * request: the cookies (which come from amsaccountsd), the storefront header, the anisette headers
 * (which come from AuthKit through akd, and this device has never been provisioned), or the
 * session's own preparation. Each is entered and left under its own number; the one that is entered
 * and not left is the answer. Arguments are forwarded untouched and never looked at: an anisette
 * "type" is a scalar and _prepareRequest: takes an NSError **, and a description of either would be
 * a crash, not a measurement.
 */
#define WRAP4(name) \
static void *(*old_##name)(id, SEL, void *, void *, void *, void *); \
static void * \
new_##name(id self, SEL cmd, void *a, void *b, void *c, void *d) \
{ \
	unsigned n = step_in(COUNT_##name); \
	void *result = old_##name(self, cmd, a, b, c, d); \
	step_out(COUNT_##name, n, result != NULL); \
	return result; \
}

#define WRAP3(name) \
static void *(*old_##name)(id, SEL, void *, void *, void *); \
static void * \
new_##name(id self, SEL cmd, void *a, void *b, void *c) \
{ \
	unsigned n = step_in(COUNT_##name); \
	void *result = old_##name(self, cmd, a, b, c); \
	step_out(COUNT_##name, n, result != NULL); \
	return result; \
}

#define WRAP1(name) \
static void *(*old_##name)(id, SEL, void *); \
static void * \
new_##name(id self, SEL cmd, void *a) \
{ \
	unsigned n = step_in(COUNT_##name); \
	void *result = old_##name(self, cmd, a); \
	say("annibale: store: #%u is for promise %p\n", n, (void *)self); \
	step_out(COUNT_##name, n, result != NULL); \
	return result; \
}

/* counters only, for the ones that would flood the console: the number is the measurement */
#define WRAPQ1(name) \
static void *(*old_##name)(id, SEL, void *); \
static void * \
new_##name(id self, SEL cmd, void *a) \
{ \
	entered[COUNT_##name]++; \
	void *result = old_##name(self, cmd, a); \
	returned[COUNT_##name]++; \
	return result; \
}
#define WRAPQ2(name) \
static void *(*old_##name)(id, SEL, void *, void *); \
static void * \
new_##name(id self, SEL cmd, void *a, void *b) \
{ \
	entered[COUNT_##name]++; \
	void *result = old_##name(self, cmd, a, b); \
	returned[COUNT_##name]++; \
	return result; \
}
#define WRAPQ3(name) \
static void *(*old_##name)(id, SEL, void *, void *, void *); \
static void * \
new_##name(id self, SEL cmd, void *a, void *b, void *c) \
{ \
	entered[COUNT_##name]++; \
	void *result = old_##name(self, cmd, a, b, c); \
	returned[COUNT_##name]++; \
	return result; \
}

/* the funnel every finished promise goes through, and the two ways of waiting on one */
/*
 * IS FAIRPLAY LOAD-BEARING FOR BROWSING AT ALL? The root of the jam is outside this daemon --
 * fairplayd.H2 starts, leaves at once and is restarted every ten seconds, and an AMS thread waits
 * for a reply that never comes (src/inject_fairplay.c). Before reading an obfuscated daemon to find
 * out why it quits, it is worth knowing whether the store needs its answer in the first place. The
 * top of that stack is an Objective-C method -- -[AMSUserAgent _sharedComponentFairPlayDeviceType],
 * a dispatch_once-memoised lookup that goes down to AMS::FairPlayProvider::getDeviceType() and
 * _AMSFairPlayGetHardwareInfo -- so it costs a method_setImplementation, the same move as the
 * storefront getters.
 * The value is deliberately NOT plausible. A zero or an empty string would travel into the user
 * agent and the Mescal path and read like data; this reads like what it is, and the log says on the
 * same line that it was injected, so that nobody later mistakes it for a measurement.
 * Why the wait looks infinite rather than failed, which is worth keeping in mind: launchd holds a
 * job's MachService port whether or not the job is running, so a client's message is QUEUED, not
 * refused. A daemon that is absent fails fast; a daemon launchd keeps restarting makes its clients
 * wait for ever. That is the README's "look at what launchd restarts" bullet seen from the client.
 */
#define FAIRPLAY_DEVICE_TYPE "ANNIBALE-INJECTED-NOT-A-FAIRPLAY-DEVICE"

static CFStringRef fairplay_device_type;

static id (*old_fpDeviceType)(id, SEL);
static id
new_fpDeviceType(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	entered[COUNT_fpDeviceType]++;
	returned[COUNT_fpDeviceType]++;
	say("annibale: store: FairPlay device type answered INJECTED \"" FAIRPLAY_DEVICE_TYPE "\" "
	    "(not measured; the real one blocks for ever, see src/inject_fairplay.c)\n");
	return (id)fairplay_device_type;
}

/*
 * THE WAIT THAT MATTERS, and the one thing this library changes besides answering a storefront.
 * Resolved from the threads' own frames:
 *   -[AMSBagNetworkTask performFetch]_block_invoke
 *     -> -[AMSBagActiveAccountProvider bagStorefrontForAccountMediaType:]
 *       -> -[AMSBagActiveAccountProvider _bagAccountForAccountMediaType:mustContainStorefront:]
 *         -> -[ACAccountStore(AppleMediaServices_Project) _ams_localiTunesAccountAfterUpdatingStorefront:]
 *           -> -[AMSPromise resultWithError:] -> NSConditionLock -> waits for ever
 * and the promise it waits for would be finished by the continuation of
 * _fetchLocaliTunesAccountForAccountType:shouldUpdateStorefront:, which runs
 * _updateStorefrontInLocalAccount: -- itself blocked in resultWithError: on another thread. The
 * local account has no storefront to update and nothing can give it one without the bag, so the
 * thread that would fetch the bag waits for an account update that waits for the bag.
 * So this answers it at once, with the local account as it is: no update, no promise, no wait. The
 * storefront that account answers with is ours, from the getter above.
 */
static id (*old_afterUpdating)(id, SEL, void *);
static id
new_afterUpdating(id self, SEL cmd, void *storefront)
{
	id account;

	(void)storefront;
	entered[COUNT_afterUpdating]++;
	account = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("ams_localiTunesAccount"));
	returned[COUNT_afterUpdating]++;
	say("annibale: store: _ams_localiTunesAccountAfterUpdatingStorefront: answered at once, %s\n",
	    account != NULL ? "with the local account" : "with nothing");
	return account;
}

/*
 * The synchronous wait itself. Four of the daemon's eleven threads sit in
 * -[AMSBagValue valueWithError:] -> -[AMSPromise resultWithError:] -> resultBeforeDate:error: ->
 * NSConditionLock, which is a thread blocked until the bag loads. Who does that, and whether one of
 * them is on the bag's own load path, is the difference between "the store is waiting" and "the bag
 * load is waiting on the bag". These are entered and not left on purpose, so entered > returned here
 * is the measurement, not a fault.
 */
static void *(*old_bagValue)(id, SEL, void *);
static void *
new_bagValue(id self, SEL cmd, void *error)
{
	char frames[10 * 20 + 1] = "";
	void *result;

	char which[120];

	/*
	 * WHICH KEY, and whether it came back. The store asks the bag for its own configuration, and a
	 * key the bag has not got is answered "Bag Value Missing" -- appstored already logs two of
	 * those. If what tells the app where to fetch its content is such a key, then nothing asking
	 * for content is exactly what one would see.
	 */
	entered[COUNT_bagValue]++;
	frames_from((uintptr_t)__builtin_frame_address(0), frames, sizeof(frames));
	result = old_bagValue(self, cmd, error);
	returned[COUNT_bagValue]++;
	/*
	 * The key, and NOT by asking the AMSBagValue to describe itself. That is what the first version
	 * did, and it aborted both daemons: appstored went from one start a session to five, each
	 * ending in SIGABRT through std::terminate, from the run that introduced it (075028) onwards.
	 * -description on this object at this moment raises, nothing catches it, and the process dies.
	 * A plain -key is an NSString and safe to print; if there is none, the key is simply unknown.
	 */
	{
		SEL key_sel = sel_registerName("key");
		id key = class_getInstanceMethod(object_getClass(self), key_sel) != NULL ?
		    ((id (*)(id, SEL))objc_msgSend)(self, key_sel) : NULL;

		note_bagkey(key != NULL ? text_of(key, which, sizeof(which)) : "(no key)", result != NULL);
	}
	return result;
}

static void *(*old_resultBefore)(id, SEL, void *, void *);
static void *
new_resultBefore(id self, SEL cmd, void *date, void *error)
{
	char frames[10 * 20 + 1] = "";
	void *result;

	entered[COUNT_resultBefore]++;
	frames_from((uintptr_t)__builtin_frame_address(0), frames, sizeof(frames));
	note_waiter((void *)self, frames, true);
	result = old_resultBefore(self, cmd, date, error);
	returned[COUNT_resultBefore]++;
	return result;
}

/* this one says which promise, because one promise in seventeen is finished and we want its name */
static void *(*old_finishFunnel)(id, SEL, void *, void *, void *);
static void *
new_finishFunnel(id self, SEL cmd, void *a, void *b, void *c)
{
	void *result;

	entered[COUNT_finishFunnel]++;
	note_finished((void *)self);
	result = old_finishFunnel(self, cmd, a, b, c);
	returned[COUNT_finishFunnel]++;
	return result;
}
WRAPQ2(finishResultError)
static void *(*old_thenWithBlock)(id, SEL, void *);
static void *
new_thenWithBlock(id self, SEL cmd, void *a)
{
	void *result;

	char frames[10 * 20 + 1] = "";

	entered[COUNT_thenWithBlock]++;
	frames_from((uintptr_t)__builtin_frame_address(0), frames, sizeof(frames));
	note_waiter((void *)self, frames, false);
	result = old_thenWithBlock(self, cmd, a);
	returned[COUNT_thenWithBlock]++;
	return result;
}
static void *(*old_continueWithBlock)(id, SEL, void *);
static void *
new_continueWithBlock(id self, SEL cmd, void *a)
{
	void *result;

	char frames[10 * 20 + 1] = "";

	entered[COUNT_continueWithBlock]++;
	frames_from((uintptr_t)__builtin_frame_address(0), frames, sizeof(frames));
	note_waiter((void *)self, frames, false);
	result = old_continueWithBlock(self, cmd, a);
	returned[COUNT_continueWithBlock]++;
	return result;
}

WRAP1(finishWithResult)
WRAP1(finishWithError)
WRAP3(addCookies)
WRAP3(addStorefrontHeader)
WRAP3(addAnisette)
WRAP3(prepareRequest)
WRAP4(addCookies4)

/* ---- installing them --------------------------------------------------------------------------- */

static struct replacement replacements[] = {
	/* the door every consumer of a storefront goes through */
	{ "ACAccount", "ams_storefront", false, (IMP)new_ams_storefront, (IMP *)&old_ams_storefront, false },
	{ "ACAccount", "ams_copyStorefrontFromAccount:", false, (IMP)new_copy_storefront, (IMP *)&old_copy_storefront, false },
	/* and the ones particular callers knock at instead */
	{ "AMSBagNetworkDataSource", "cachedStorefront", false, (IMP)new_cached_storefront, (IMP *)&old_cached_storefront, false },
	{ "AMSBagFixedAccountProvider", "bagStorefrontForAccountMediaType:", false, (IMP)new_bag_storefront,
	  (IMP *)&old_bag_storefront, false },
	{ "AMSBagActiveAccountProvider", "bagStorefrontForAccountMediaType:", false, (IMP)new_bag_storefront_active,
	  (IMP *)&old_bag_storefront_active, false },
	/* watched, not replaced */
	{ "AMSBagActiveAccountProvider", "_bagAccountForAccountMediaType:mustContainStorefront:", false,
	  (IMP)new_bag_account, (IMP *)&old_bag_account, false },
	{ "AMSBagNetworkTask", "_performFetchWithAttemptedCount:account:storefront:", false, (IMP)new_perform_fetch,
	  (IMP *)&old_perform_fetch, false },
	{ "AMSBagNetworkTask", "_queryItemsForClientInfo:profile:profileVersion:cookieNames:account:storefront:", false,
	  (IMP)new_query_items, (IMP *)&old_query_items, false },
	{ "AMSURLSession", "performRequest:completion:", false, (IMP)new_perform_request, (IMP *)&old_perform_request, false },
	{ "AMSURLSession", "performRequest:completionBlock:", false, (IMP)new_perform_request_block,
	  (IMP *)&old_perform_request_block, false },
	{ "AMSBagNetworkTask", "_createRequestWithQueryItems:", false, (IMP)new_create_request, (IMP *)&old_create_request,
	  false },
	{ "AMSBagNetworkTask", "URLSessionPromise", false, (IMP)new_session_promise, (IMP *)&old_session_promise, false },
	{ "AMSURLSession", "dataTaskPromiseWithRequest:", false, (IMP)new_data_task_promise, (IMP *)&old_data_task_promise,
	  false },
	{ "AMSURLSession", "dataTaskWithRequest:", false, (IMP)new_data_task, (IMP *)&old_data_task, false },
	{ "NSMutableURLRequest", "ams_addCookiesForAccount:clientInfo:bag:", false, (IMP)new_addCookies,
	  (IMP *)&old_addCookies, false },
	{ "NSMutableURLRequest", "ams_addStoreFrontHeaderForAccount:client:withBag:", false, (IMP)new_addStorefrontHeader,
	  (IMP *)&old_addStorefrontHeader, false },
	{ "NSMutableURLRequest", "ams_addAnisetteHeadersForAccount:type:bag:", false, (IMP)new_addAnisette,
	  (IMP *)&old_addAnisette, false },
	{ "AMSURLSession", "_prepareRequest:properties:error:", false, (IMP)new_prepareRequest,
	  (IMP *)&old_prepareRequest, false },
	{ "AMSMutablePromise", "finishWithResult:", false, (IMP)new_finishWithResult, (IMP *)&old_finishWithResult, false },
	{ "AMSMutablePromise", "finishWithError:", false, (IMP)new_finishWithError, (IMP *)&old_finishWithError, false },
	{ "AMSMutablePromise", "_finishWithResult:error:logDuplicateFinishes:", false, (IMP)new_finishFunnel,
	  (IMP *)&old_finishFunnel, false },
	{ "AMSMutablePromise", "finishWithResult:error:", false, (IMP)new_finishResultError,
	  (IMP *)&old_finishResultError, false },
	{ "AMSMutablePromise", "thenWithBlock:", false, (IMP)new_thenWithBlock, (IMP *)&old_thenWithBlock, false },
	{ "AMSMutablePromise", "continueWithBlock:", false, (IMP)new_continueWithBlock, (IMP *)&old_continueWithBlock,
	  false },
	{ "ACAccountStore", "_ams_localiTunesAccountAfterUpdatingStorefront:", false, (IMP)new_afterUpdating,
	  (IMP *)&old_afterUpdating, false },
	{ "NSMutableURLRequest", "ams_addCookiesForAccount:clientInfo:bag:cleanupGlobalCookies:", false,
	  (IMP)new_addCookiesBlocking, (IMP *)&old_addCookiesBlocking, false },
	{ "ACAccountStore", "_updateStorefrontInLocalAccount:", false, (IMP)new_updateStorefront,
	  (IMP *)&old_updateStorefront, false },
	{ "JPReader", "processStream:signatureVerifier:options:error:", false, (IMP)new_processStream,
	  (IMP *)&old_processStream, false },
	{ "JEBrotliDecoder", "decompressStreamWithAvailableIn:nextIn:availableOut:nextOut:", false, (IMP)new_brotli,
	  (IMP *)&old_brotli, false },
	{ "NSNotificationCenter", "postNotificationName:object:userInfo:", false, (IMP)new_postName,
	  (IMP *)&old_postName, false },
	{ "NSNotificationCenter", "postNotification:", false, (IMP)new_postNotification,
	  (IMP *)&old_postNotification, false },
	{ "AMSUserAgent", "_sharedComponentFairPlayDeviceType", false, (IMP)new_fpDeviceType,
	  (IMP *)&old_fpDeviceType, false },
	{ "AMSBagValue", "valueWithError:", false, (IMP)new_bagValue, (IMP *)&old_bagValue, false },
	{ "AMSPromise", "resultBeforeDate:error:", false, (IMP)new_resultBefore, (IMP *)&old_resultBefore, false },
	{ "__NSURLSessionLocal", "dataTaskWithRequest:completionHandler:", false, (IMP)new_dataTaskLocal,
	  (IMP *)&old_dataTaskLocal, false },
	{ "__NSURLSessionLocal", "dataTaskWithRequest:", false, (IMP)new_dataTaskPlain,
	  (IMP *)&old_dataTaskPlain, false },
	{ "__NSURLSessionLocal", "dataTaskWithURL:completionHandler:", false, (IMP)new_dataTaskURL,
	  (IMP *)&old_dataTaskURL, false },
	{ "AMSURLSession", "dataTaskWithRequest:signpostID:completionHandler:", false, (IMP)new_dataTaskSignpost,
	  (IMP *)&old_dataTaskSignpost, false },
	{ "AMSURLSession", "dataTaskWithRequest:completionHandler:", false, (IMP)new_dataTaskAMS,
	  (IMP *)&old_dataTaskAMS, false },
	{ "NSError", "errorWithDomain:code:userInfo:", true, (IMP)new_errorWith, (IMP *)&old_errorWith, false },
	{ "NSError", "initWithDomain:code:userInfo:", false, (IMP)new_errorInit, (IMP *)&old_errorInit, false },
	{ "NSURLSessionTask", "resume", false, (IMP)new_resume_base, (IMP *)&old_resume_base, false },
	{ "__NSCFLocalSessionTask", "resume", false, (IMP)new_resume_local, (IMP *)&old_resume_local, false },
};

static size_t
replacement_count(void)
{
	return sizeof(replacements) / sizeof(replacements[0]);
}

static void
image_added(__unused const struct mach_header *header, __unused intptr_t image_slide)
{
	for (size_t i = 0; i < sizeof(replacements) / sizeof(replacements[0]); i++) {
		struct replacement *r = &replacements[i];
		Class cls;
		Method method;
		IMP original;

		if (r->done || (cls = objc_getClass(r->class_name)) == NULL) {
			continue;
		}
		SEL selector = sel_registerName(r->selector);
		Class super = class_getSuperclass(cls);

		method = r->class_method ? class_getClassMethod(cls, selector) : class_getInstanceMethod(cls, selector);
		if (method == NULL) {
			continue;               /* the class is there, the selector is not */
		}
		/*
		 * class_getInstanceMethod walks up: a method this class only INHERITS is the superclass's,
		 * and method_setImplementation on it would replace it for every class that inherits it as
		 * well -- for NSObject, that is every object in the daemon. appstored restarted 31 times in
		 * a run before this check was here, and said nothing on its way out (an ObjC exception ends
		 * in abort(), which the fault reporter did not watch for either; now it does).
		 */
		if (super != NULL && method == (r->class_method ? class_getClassMethod(super, selector) :
		    class_getInstanceMethod(super, selector))) {
			r->done = true;
			say("annibale: store: %c[%s %s] is inherited, not this class's: left alone\n",
			    r->class_method ? '+' : '-', r->class_name, r->selector);
			continue;
		}
		r->done = true;
		original = method_getImplementation(method);
		if (r->original != NULL && *r->original == NULL) {
			*r->original = original;
		}
		method_setImplementation(method, r->replacement);
		say("annibale: store: %c[%s %s] taken (was %#lx unslid)\n", r->class_method ? '+' : '-', r->class_name,
		    r->selector, (uintptr_t)original - slide);
	}
}

__attribute__((constructor))
static void
annibale_store_init(void)
{
	const char *name = getprogname();
	size_t length = 0;

	/*
	 * Both of them: appstored holds the bag, but AppStore.app has an AppleMediaServices of its own
	 * and blocks in the same FairPlay lookup -- with the daemon unjammed and the app not, the
	 * daemon fetches its bag and the screen still says LOADING, which is exactly what the first run
	 * of the FairPlay answer showed.
	 */
	if (name == NULL || (strcmp(name, "appstored") != 0 && strcmp(name, "AppStore") != 0)) {
		return;
	}
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	watch_for_faults();
	storefront = CFStringCreateWithCString(NULL, STOREFRONT, kCFStringEncodingUTF8);
	fairplay_device_type = CFStringCreateWithCString(NULL, FAIRPLAY_DEVICE_TYPE, kCFStringEncodingUTF8);
	who = name;
	say("annibale: store(%s): ours is in, storefront to answer with is " STOREFRONT "\n", name);
	{
		pthread_t thread;

		if (pthread_create(&thread, NULL, late_report, NULL) == 0) {
			pthread_detach(thread);
		}
	}
	_dyld_register_func_for_add_image(image_added);         /* also called for every image already there */
}
