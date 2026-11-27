/*
 * libannibale-store.dylib: code of ours inside appstored (see inject.c for how it gets there).
 *
 * What it is for. The App Store opens and shows LOADING for ever. The front page is a "bag" --
 * AppleMediaServices' configuration -- and AMS will not load one until it knows the storefront, the
 * number that says which country's store this device belongs to. Everything outside the process has
 * been measured and is well:
 *   - the bag itself is fetched from this phone by hand, 56 KB in half a second, and the server
 *     hands over a storefront in the bargain: "x-set-apple-store-front: 143469-2,8" from both
 *     bag.itunes.apple.com and init.itunes.apple.com (rc.jobs.store, src/net_test.m);
 *   - appstored opens no socket at all while the store spins, so nothing is failing on the network;
 *   - the storefront cannot be put on the device from another process: AMS's "local" account is not
 *     a row in accountsd at all -- a different identifier every boot -- so there is nothing to write
 *     to and nothing that would keep (src/store_test.m).
 *
 * So the storefront is not written anywhere here either. The account is made to ANSWER with one:
 * -[ACAccount ams_storefront] is the door every consumer in AMS goes through, and its neighbours are
 * taken in the same pass because different callers knock at different ones. No persistence, no
 * accountsd, nothing that outlives the process.
 *
 * And the question that is worth more than the storefront: is a bag request ever built? From outside
 * only the socket log could answer, and only negatively, over a console that drops lines. From in
 * here it can be answered positively. -[AMSBagNetworkTask _performFetchWithAttemptedCount:account:
 * storefront:] and its _queryItemsForClientInfo:... are where a request is assembled: they are not
 * replaced, only watched, and what they were called with is written down. If neither is entered,
 * nothing about the network matters and the gate is above them.
 *
 * The storefront is 143469-2,8 because that is what Apple's own bag server returned for THIS device
 * when asked with none -- a region, picked from the address the request came from. No Apple ID is
 * signed in, none can be, and the store must still refuse to install: there is nothing to buy with.
 *
 * AppleMediaServices and Accounts are not loaded yet when this library is: the methods are taken
 * when their images arrive (_dyld_register_func_for_add_image).
 *
 * WHAT IT ANSWERED. The storefront was the gate, and it is not the only one.
 *   entered    3 -[ACAccount ams_storefront]                     (answered 143469-2,8; was (nil))
 *   entered    3 -[AMSBagActiveAccountProvider _bagAccountForAccountMediaType:mustContainStorefront:]
 *                 mustContainStorefront:0 -> the local account, mustContainStorefront:1 -> (nil)
 *   entered    1 -[AMSBagNetworkTask _performFetchWithAttemptedCount:account:storefront:]
 *                 "BAG FETCH attempt 0, storefront 143469-2,8, account local (...)" -> a promise
 *   entered    1 -[AMSBagNetworkTask _createRequestWithQueryItems:] -> <AMSMutablePromise: ...>
 *   entered    0 -[AMSBagNetworkTask URLSessionPromise]
 *   entered    0 -[AMSURLSession dataTaskPromiseWithRequest:]
 *   entered    0 -[AMSURLSession dataTaskWithRequest:]
 *   entered    0 -[NSURLSessionTask resume]
 * So with a storefront to answer with, AMS DOES build a bag request -- that question is settled, and
 * positively, from inside. And the request never becomes a transfer: it is assembled into an
 * AMSMutablePromise that nothing ever resolves, so no session is asked for, no task is made and
 * nothing is ever resumed. Not one task is resumed in the whole daemon in a six-minute session,
 * which is why the socket log never had appstored in it. The store still shows LOADING, as it must:
 * the bag never loads.
 * AND THEN, with entry and exit counted separately -- a method that hangs and a method that is
 * never called leave the same silence below them, so every wrapper counts both:
 *   entered    1 returned    1 -[AMSBagNetworkTask _createRequestWithQueryItems:]
 *   entered    0 returned    0  the four steps that finish a request (cookies, storefront header,
 *                               anisette headers, -[AMSURLSession _prepareRequest:properties:error:])
 *   entered   12 returned   12 -[AMSMutablePromise continueWithBlock:]
 *   entered    5 returned    5 -[AMSMutablePromise thenWithBlock:]
 *   entered    1 returned    1 -[AMSMutablePromise _finishWithResult:error:logDuplicateFinishes:]
 * Nothing hangs. _createRequestWithQueryItems: returns at once, with a promise -- and that promise is
 * never finished. Seventeen continuations are attached to promises in that session and exactly ONE
 * promise is ever finished, and by address it is not the request's:
 *   PROMISE 0x101709240 waited on (continue)
 *   STEP _createRequestWithQueryItems: -> <AMSMutablePromise: 0x10170b930>
 *   PROMISE 0x10170b930 waited on (then) / (continue)
 *   PROMISE 0x101414370 finished          <- the only one, and a different promise
 * So the bag request is never produced at all: the step that would produce it is waiting on
 * something that never arrives, which is why no header is ever added, no session asked for, no task
 * made and nothing resumed. The anisette guess is ruled out with the other three: none is entered.
 *
 * AND THE THREADS SAY WHY, which is a bigger thing than the bag. All eleven threads of the daemon
 * are waiting (run state 3; one idle libdispatch worker in __workq_kernreturn, so the queues are not
 * starved of threads), and four of them are in the same place:
 *   -[AMSBagValue valueWithError:] -> -[AMSPromise resultWithError:] -> resultBeforeDate:error:
 *   -> -[NSConditionLock lockWhenCondition:beforeDate:] -> -[NSCondition waitUntilDate:]
 * -- a thread blocked until the bag loads. Walking every thread's frames and giving them to
 * "ipsw dyld a2s" names the circle:
 *   -[AMSBagNetworkTask performFetch]_block_invoke
 *     -> -[AMSBagActiveAccountProvider bagStorefrontForAccountMediaType:]
 *       -> _bagAccountForAccountMediaType:mustContainStorefront:
 *         -> -[ACAccountStore _ams_localiTunesAccountAfterUpdatingStorefront:]
 *           -> -[AMSPromise resultWithError:], for ever
 * while the promise it waits on is finished by the continuation of
 * _fetchLocaliTunesAccountForAccountType:shouldUpdateStorefront: running
 * _updateStorefrontInLocalAccount:, which is blocked in resultWithError: on another thread. The
 * thread that would fetch the bag waits on an account update that waits on the bag.
 * (The same walk explains an earlier puzzle: the cookies are added by the FOUR-argument
 * ams_addCookiesForAccount:clientInfo:bag:cleanupGlobalCookies:, inside
 * -[AMSURLRequestEncoder requestByEncodingRequest:parameters:], which is why a hook on the
 * three-argument one was never entered.)
 * Answering _ams_localiTunesAccountAfterUpdatingStorefront: at once (above) does cut that wait --
 * _bagAccountForAccountMediaType: goes from blocked to 3 entered 3 returned -- and the bag still
 * does not load: four threads are still blocked in resultBeforeDate:error: elsewhere. So the stall
 * is not one method; AMS's promises in this daemon are not being completed at all.
 *
 * THE ROOT, found by the promise graph rather than by recognising another circle. Every promise is
 * remembered here by address, with where it was first seen, how many continuations were attached,
 * which threads block in it and whether it was ever finished; the table is printed at 150 s beside
 * the thread dump, and a promise with waiters and no finish is marked. Sixteen promises in a
 * session, seven of them so marked, the bag's request and the bag's fetch among them. Following the
 * threads of the marked ones down (ipsw dyld a2s again) ends outside AMS altogether:
 *   thread 4: mach_msg2_trap <- _AMSFairPlayGetDeviceType <- _CjHbHx <- (three FairPlay functions)
 * -- an AMS thread blocked in a Mach call into FairPlay, waiting for a reply. And in the same
 * session's launchd log:
 *   com.apple.fairplayd.H2: 34 spawns, every one "Service only ran for 0 seconds",
 *   "exited due to exit(0)", "service throttled by 10 seconds"
 * fairplayd does not crash: it starts, decides there is nothing it can do here, and leaves at once,
 * over and over. So the reply AMS is waiting for never comes, the promise behind it is never
 * fulfilled, and seventeen waits queue behind that one. The storefront deadlock sits on top of this,
 * not beside it.
 * The next piece of work is therefore not in AMS at all: find why fairplayd.H2 exits 0 immediately
 * and keep it up. Everything in this file above that line is a consequence of it.
 *
 * Two things this library learned about itself, for whoever writes the next one:
 *   - class_getInstanceMethod walks up the chain, so replacing what a class only INHERITS replaces it
 *     for everything else that inherits it too. Doing that to an NSObject category restarted
 *     appstored 31 times in one run. image_added now refuses anything the superclass answers to.
 *   - an ObjC exception ends in abort(), not in SIGSEGV, so a fault reporter that watches only the
 *     memory signals says nothing at all about the usual way a replacement of ours kills a daemon.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <fcntl.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL                /* where the cache was built to be, before the slide */
#define STOREFRONT "143469-2,8"                 /* what bag.itunes.apple.com returned for this device */

static uintptr_t slide;
static CFStringRef storefront;

/* the table itself is at the end, where its functions are; the fetch hook reports what is missing */
struct replacement {
	const char *class_name;
	const char *selector;
	bool class_method;
	IMP replacement;
	IMP *original;
	bool done;
};
static struct replacement replacements[];
static size_t replacement_count(void);
static void image_added(const struct mach_header *header, intptr_t image_slide);

/*
 * How many times each hook was entered: a count that survives a console that drops lines. The names
 * below are in the order the hooks count themselves in, which is NOT the order of the table at the
 * end -- the first version of this printed one against the other and every count came out against
 * the wrong name, which had to be undone by hand afterwards. Keep the two lists apart on purpose.
 */
enum {
	COUNT_ams_storefront, COUNT_cachedStorefront, COUNT_bagStorefront_fixed, COUNT_bagStorefront_active,
	COUNT_copyStorefront, COUNT_bagAccount, COUNT_performFetch, COUNT_queryItems, COUNT_performRequest,
	COUNT_performRequestBlock, COUNT_resume, COUNT_resumeLocal, COUNT_createRequest, COUNT_sessionPromise,
	COUNT_dataTaskPromise, COUNT_dataTask, COUNT_addCookies, COUNT_addStorefrontHeader, COUNT_addAnisette,
	COUNT_prepareRequest, COUNT_finishWithResult, COUNT_finishWithError, COUNT_finishFunnel,
	COUNT_finishResultError, COUNT_thenWithBlock, COUNT_continueWithBlock, COUNT_bagValue,
	COUNT_resultBefore, COUNT_afterUpdating, COUNT_addCookies4, COUNT_fpDeviceType, COUNT_brotli,
	COUNT_processStream, COUNT_updateStorefront, COUNT_addCookiesBlocking, COUNT_dataTaskCreated,
	COUNT_last
};
static const char *const counted[COUNT_last] = {
	"-[ACAccount ams_storefront]", "-[AMSBagNetworkDataSource cachedStorefront]",
	"-[AMSBagFixedAccountProvider bagStorefrontForAccountMediaType:]",
	"-[AMSBagActiveAccountProvider bagStorefrontForAccountMediaType:]",
	"-[ACAccount ams_copyStorefrontFromAccount:]",
	"-[AMSBagActiveAccountProvider _bagAccountForAccountMediaType:mustContainStorefront:]",
	"-[AMSBagNetworkTask _performFetchWithAttemptedCount:account:storefront:]",
	"-[AMSBagNetworkTask _queryItemsForClientInfo:...]", "-[AMSURLSession performRequest:completion:]",
	"-[AMSURLSession performRequest:completionBlock:]", "-[NSURLSessionTask resume]",
	"-[__NSCFLocalSessionTask resume]", "-[AMSBagNetworkTask _createRequestWithQueryItems:]",
	"-[AMSBagNetworkTask URLSessionPromise]", "-[AMSURLSession dataTaskPromiseWithRequest:]",
	"-[AMSURLSession dataTaskWithRequest:]", "ams_addCookiesForAccount:clientInfo:bag:",
	"ams_addStoreFrontHeaderForAccount:client:withBag:", "ams_addAnisetteHeadersForAccount:type:bag:",
	"-[AMSURLSession _prepareRequest:properties:error:]", "-[AMSMutablePromise finishWithResult:]",
	"-[AMSMutablePromise finishWithError:]", "-[AMSMutablePromise _finishWithResult:error:logDuplicateFinishes:]",
	"-[AMSMutablePromise finishWithResult:error:]", "-[AMSMutablePromise thenWithBlock:]",
	"-[AMSMutablePromise continueWithBlock:]", "-[AMSBagValue valueWithError:]",
	"-[AMSPromise resultBeforeDate:error:]",
	"-[AMSBagValue valueWithError:]", "-[AMSPromise resultBeforeDate:error:]",
	"-[ACAccountStore _ams_localiTunesAccountAfterUpdatingStorefront:]",
	"ams_addCookiesForAccount:clientInfo:bag:cleanupGlobalCookies:",
	"-[AMSUserAgent _sharedComponentFairPlayDeviceType]",
	"-[JEBrotliDecoder decompressStreamWithAvailableIn:nextIn:availableOut:nextOut:]",
	"-[JPReader processStream:signatureVerifier:options:error:]",
	"-[ACAccountStore _updateStorefrontInLocalAccount:]",
	"ams_addCookiesForAccount:clientInfo:bag:cleanupGlobalCookies: (answered empty)",
	"-[NSURLSession dataTaskWithRequest:...] (created, not resumed)",
};
static unsigned long entered[COUNT_last];
/*
 * And how many times each came BACK. A method that hangs and a method that is never called look the
 * same in one counter -- both leave everything below them at zero -- so every wrapper counts its
 * entry and its return separately. The one with entered > returned is where the thread is.
 */
static unsigned long returned[COUNT_last];
static unsigned serial;


/* Each line of ours alone on the console: it drops them under the start's flood (see rc.jobs.store). */
/*
 * To the console itself, not to standard error. A launchd job of ours has /dev/console on fd 2
 * (mkjobs.py sets StandardErrorPath), but an application started by SpringBoard has nothing there,
 * so the first run with this library inside AppStore.app looked exactly like a run where it had not
 * been inserted at all -- another silence that meant "wrong channel", not "did not happen".
 */
static const char *who = "store";

static void
say(const char *format, ...)
{
	static int console = -2;
	char line[512];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	if (console == -2) {
		console = open("/dev/console", O_WRONLY | O_APPEND);
	}
	/*
	 * And say which process. Two of them carry this library and both write to the one console, so a
	 * report read by position belongs to whichever process happened to be writing -- the lines are
	 * interleaved, not grouped. Every line names its own program instead.
	 */
	if (strncmp(line, "annibale: store: ", 16) == 0) {
		char named[sizeof(line) + 32];

		n = snprintf(named, sizeof(named), "annibale: %s: %s", who, line + 16);
		if (n > (int)sizeof(named) - 1) {
			n = sizeof(named) - 1;
		}
		write(console >= 0 ? console : 2, named, (size_t)n);
		return;
	}
	write(console >= 0 ? console : 2, line, (size_t)n);
}

/* an object as a short string, for the log: a description, cut to fit */
static const char *
text_of(id object, char *buffer, size_t size)
{
	CFStringRef description;

	buffer[0] = 0;
	if (object == NULL) {
		return "(nil)";
	}
	description = (CFStringRef)((id (*)(id, SEL))objc_msgSend)(object, sel_registerName("description"));
	if (description == NULL || !CFStringGetCString(description, buffer, (CFIndex)size, kCFStringEncodingUTF8)) {
		return "(?)";
	}
	return buffer;
}

static unsigned
step_in(int which)
{
	unsigned n = __sync_add_and_fetch(&serial, 1);

	entered[which]++;
	say("annibale: store: -> #%u %s\n", n, counted[which]);
	return n;
}

static void
step_out(int which, unsigned n, bool got_something)
{
	returned[which]++;
	say("annibale: store: <- #%u %s%s\n", n, counted[which], got_something ? "" : " (nil)");
}

/*
 * The fault reporter, as in inject.c: iOS answers a crash in a stock daemon with
 * "Failed to send exception EXC_CORPSE_NOTIFY" and nothing else, so a fault in here would be a
 * silent exit. This turns one into an address, unslid, which "ipsw dyld a2s" turns into a name.
 */
/*
 * Up the frame pointers: on arm64 each frame holds the previous one at [fp] and its return address
 * at [fp + 8]. Unslid, so that "ipsw dyld a2s" turns each into a name.
 */
static void
frames_from(uintptr_t fp, char *text, size_t size)
{
	size_t used = 0;

	text[0] = 0;
	for (unsigned i = 0; i < 10 && fp != 0 && (fp & 7) == 0 && used + 20 < size; i++) {
		uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];

		if (ret == 0) {
			break;
		}
		used += (size_t)snprintf(text + used, size - used, " %#lx", ret - slide);
		fp = next > fp ? next : 0;      /* the chain only ever goes up the stack */
	}
}

/*
 * WHAT CAME BACK, not that a task was started. "It fetches its content and the screen still says
 * LOADING" has four shapes behind it -- the request never completes, it completes with an HTTP
 * error, it completes with a body the renderer rejects, or it completes and nothing is drawn -- and
 * they are the same line without the response. So every task that is resumed is kept (retained, or
 * it would be gone by the time the report runs) and asked at 150 s what became of it: its state,
 * the status, the bytes received, the type, and the error if there is one.
 */
#define TASKS 64
static id tasks[TASKS];
static unsigned task_count;
static pthread_mutex_t task_lock = PTHREAD_MUTEX_INITIALIZER;

static void
remember_task(id task)
{
	pthread_mutex_lock(&task_lock);
	if (task_count < TASKS) {
		tasks[task_count++] = (id)CFRetain((CFTypeRef)task);
	}
	pthread_mutex_unlock(&task_lock);
}

static void
dump_tasks(void)
{
	static const char *const states[] = { "running", "suspended", "cancelling", "COMPLETED" };

	say("annibale: store: %u tasks resumed\n", task_count);
	for (unsigned i = 0; i < task_count; i++) {
		id task = tasks[i];
		long state = ((long (*)(id, SEL))objc_msgSend)(task, sel_registerName("state"));
		id response = ((id (*)(id, SEL))objc_msgSend)(task, sel_registerName("response"));
		id error = ((id (*)(id, SEL))objc_msgSend)(task, sel_registerName("error"));
		id request = ((id (*)(id, SEL))objc_msgSend)(task, sel_registerName("originalRequest"));
		id url = request != NULL ? ((id (*)(id, SEL))objc_msgSend)(request, sel_registerName("URL")) : NULL;
		long long got = ((long long (*)(id, SEL))objc_msgSend)(task, sel_registerName("countOfBytesReceived"));
		long status = response != NULL ? ((long (*)(id, SEL))objc_msgSend)(response, sel_registerName("statusCode")) : 0;
		id type = response != NULL ? ((id (*)(id, SEL))objc_msgSend)(response, sel_registerName("MIMEType")) : NULL;
		char one[96], two[64], three[200];

		say("annibale: store: task %u %s status %ld, %lld bytes, %s%s%s, %s\n", i,
		    state >= 0 && state < 4 ? states[state] : "?", status, got,
		    type != NULL ? text_of(type, one, sizeof(one)) : "(no type)",
		    error != NULL ? ", error " : "", error != NULL ? text_of(error, two, sizeof(two)) : "",
		    url != NULL ? text_of(url, three, sizeof(three)) : "(no url)");
	}
}

/*
 * WHAT HAPPENS TO THE PAGE AFTER IT ARRIVES. The front page comes down whole and nothing draws it,
 * and the report that showed that also showed something nobody was looking for: THE SAME 177 786
 * BYTES WERE FETCHED TWICE. A document that parsed would not be fetched again, so the likeliest
 * story is loaded, rejected, tried again.
 * JetEngine says how it would reject one. It carries jetpackSignatureVerificationDidFailNotification
 * -- a signature is verified and there is a named way of announcing that it was not liked -- and a
 * JEBrotliDecoder, so the body is compressed before any of that; JetPackSigningPolicy,
 * JetpackSignatureVerifierP, JetPackageFormatParser and JetpackErrorDomain are all in the same
 * library. Every one of those rejects AFTER the bytes are on disk, which is the shape measured.
 * Rather than guess the notification's string (a name is one of the ways an instrument lies -- see
 * below), every notification posted in the process is watched and the ones whose name speaks of
 * jetpacks, signatures, verification or brotli are printed.
 * WHAT IT SAYS. No signature-failure notification is ever posted: 169 to 197 notifications in a
 * session of the app and not one about a jetpack, a signature, verification or brotli -- the only
 * JetEngine notification seen at all is JetEngine.Unstable.Accounts.didChangeNotification, so the
 * engine is alive and posting.
 * AND THE READER IS PLAINLY HAPPY. With the counters printed on their own line, early, instead of as
 * one line of a sixty-line report:
 *   annibale: AppStore: JETPACK processStream: in
 *   annibale: AppStore: JETPACK processStream: out, a result, error (none)
 *   annibale: AppStore: KEY brotli entered 0 returned 0, processStream entered 2 returned 2
 *   annibale: appstored: KEY brotli entered 0 returned 0, processStream entered 0 returned 0
 * -[JPReader processStream:signatureVerifier:options:error:] is entered twice -- once per fetch --
 * and both times it RETURNS A RESULT WITH NO ERROR. So the page is fetched whole and parsed without
 * complaint: it is not the signature, not the package format, and not Brotli (which never runs at
 * all). That retires all three at once, and with them the experiment of swapping
 * +[JPSignatureVerifier defaultSignatureVerifier] for +unsafeIgnoreSignatureVerifier -- there is
 * nothing for it to prove, since the verification already passes. An image that does not verify
 * signatures is a worse thing than an experiment that does not, and this one is not needed.
 * The only C++ exception thrown anywhere in either process is std::future_error (every throw is
 * watched through __cxa_throw, whose type name is the second word of its std::type_info), and it is
 * thrown in appstored as well, where everything now works -- so it looked like a lead and not an
 * answer. COUNTED, it looked less innocent: 95 of them in appstored and 68 in the App Store in one
 * session, with only 4 and 2 NSExceptions beside them, and "seventeen waits and one finish" is the
 * shape a flood of broken promises would make. It was tempting, and it is WRONG.
 * The frames and the times say so, which is why both are printed:
 *   C++ THROW #1 at 0.11 s NSt3__112future_errorE from 0x18ff0bd14 0x18fd1c8b8 0x18fd197f0 ...
 *   0x18ff0bd14: std::make_exception_ptr<std::future_error>
 *   0x18fd1c8b8: std::promise<std::tuple<PromiseResult>>::~promise
 *   0x18fd197f0: std::__shared_ptr_emplace<AMSCore::Future<PromiseResult>::FutureImp>::__on_zero_shared
 *   0x1900b57dc: std::shared_ptr<AMSCore::Future<PromiseResult>::FutureImp>::~shared_ptr
 *   0x18fd1a9c4: -[AMSPromise .cxx_destruct]
 * Every one of the first throws is the same path, and every one is inside the first quarter-second
 * of the process -- 0.11 to 0.26 s, long before anything is fetched. It is an AMSPromise being
 * DEALLOCATED: ~promise stores a broken_promise into the shared state when it is destroyed without
 * a value, which is what the C++ standard says it must do, and make_exception_ptr is how it is
 * stored rather than how it is reported. 163 of them a session is AMS's style of letting go of
 * promises nobody needs, not a flood of failures. The number means nothing on its own, the
 * connection to "nothing asks for content" is disproved, and this paragraph is here so that the
 * next reader does not find the number again and make the same handsome mistake.
 * WHAT IS LEFT is the drawing itself: 177 786 bytes arrive, JPReader parses them into a result, and
 * nothing appears. Whoever owns it next is on the stack when it is handed back, so the walk is done
 * at the exit too, and "ipsw dyld a2s" names the image if not the function:
 *   JETPACK processStream: out, a result, error (none), to 0x19d96245c 0x19d963f88 0x19d9634bc ...
 *   0x19d96245c: func_19d962020 + 1084
 *     dylib=/System/Library/PrivateFrameworks/JetEngine.framework/JetEngine
 * -- straight back into JetEngine, into Swift functions the cache carries no names for. That is
 * where the next session starts, and it is static work: the project's Ghidra path (see
 * src/inject_fairplay.c for the tools and their rules).
 * AND THE APP'S OWN WAITS ARE GONE, which is what justifies going there. The graph had shown the app
 * NOT all finished: 192 promises, two threads each parked on two that nobody fulfils -- one in
 * -[ACAccountStore _updateStorefrontInLocalAccount:], one in
 * ams_addCookiesForAccount:clientInfo:bag:cleanupGlobalCookies: inside
 * -[AMSURLRequestEncoder requestByEncodingRequest:parameters:] -- the same family as the deadlock
 * cut in appstored, left over because the hook that cut that one only answers
 * _ams_localiTunesAccountAfterUpdatingStorefront:, and -[AMSBagValue valueWithError:] standing at 92
 * entered and 91 returned, one outstanding at the moment the report printed. Both are answered now
 * (above), and the next run says:
 *   entered   89 returned   89 -[AMSBagValue valueWithError:]
 *   entered  104 returned  104 -[AMSPromise resultBeforeDate:error:]
 *   and not one "STILL IN IT" anywhere in the app
 * Every wait completes, the page is still fetched and still parses without error -- and the screen
 * still says LOADING. So the drawing is not waiting on AppleMediaServices for anything: the last
 * suspects outside JetEngine are cleared, and the static read of its Swift is justified rather than
 * assumed. That is the one thing left and it is the expensive one; it belongs to a fresh session.
 * And SwiftUI itself is not the suspect: the Weather and Calendar home-screen widgets are WidgetKit,
 * which is SwiftUI, and they draw their own content on this phone. Whatever stops _JetEngine_SwiftUI
 * is particular to this path.
 *
 * THE THREE SHAPES OF AN INSTRUMENT THAT LIES, all of them met on this branch, all of them making a
 * thing that happened look like a thing that did not:
 *   the wrong channel -- the library was inside AppStore.app and said nothing, because an app
 *     started by SpringBoard has no console on standard error (say() writes to /dev/console now);
 *   the wrong name -- a hook on ams_addCookiesForAccount:clientInfo:bag: never fired for a whole
 *     evening because the cookies go through the four-argument ...:cleanupGlobalCookies: instead;
 *   the wrong moment -- one report at 150 s, while the App Store starts when it is tapped at 95 s
 *     and its report fell past the end of the run (every minute now);
 *   the wrong order -- the hook on JEBrotliDecoder was NOT TAKEN although the class is plainly in
 *     the process (the report's own selector dump lists its methods): the table is tried when dyld
 *     adds an image, and a Swift framework's classes are registered after that callback runs, so
 *     the one moment it was looked for was the one moment it was not there. The table is tried
 *     again every minute now, which costs nothing and cannot miss a late arrival.
 * The fifth shape belongs in this list when it is found.
 */
/*
 * And the decompression, since no signature-failure notification is ever posted (169 to 197
 * notifications in a session of the app, not one of them about a jetpack, a signature, verification
 * or brotli). JEBrotliDecoder is in the App Store's process and not in appstored, and it answers to
 * -decoder and -decompressStreamWithAvailableIn:nextIn:availableOut:nextOut:. The result is an
 * integer status, so it is printed as one: a decode that never runs and a decode that fails are the
 * same silence otherwise.
 */
static long (*old_brotli)(id, SEL, void *, void *, void *, void *);
static long
new_brotli(id self, SEL cmd, void *a, void *b, void *c, void *d)
{
	long result;

	entered[COUNT_brotli]++;
	result = old_brotli(self, cmd, a, b, c, d);
	returned[COUNT_brotli]++;
	if (returned[COUNT_brotli] <= 4 || result != 1) {
		say("annibale: store: BROTLI decompressStream -> %ld (call %lu)\n", result, returned[COUNT_brotli]);
	}
	return result;
}

/*
 * AND THE SHARPER FACT, which was in the task report all along and wanted reading for the thing
 * nobody was looking for: THE APP MAKES EXACTLY FOUR REQUESTS AND NONE OF THEM IS FOR CONTENT.
 * Three sessions, four tasks each, every time the same four: the bag twice, and
 * apps.mzstatic.com/.../ios-appstore.jetpack twice. The jetpack is the store's user interface, not
 * its content; the Today page's items come from a separate feed (amp-api), and no such request is
 * ever made. So "the page is parsed and not drawn" is better said as: nothing ever asks for anything
 * to draw. That moves the question from "why does JetEngine not draw this document" to "what should
 * have asked for the content once the document was loaded, and why does it not".
 * The next thing to measure is accordingly one step earlier: a task that is created and never
 * resumed is a different story from a task that is never created, and they look the same in a report
 * that only counts resumes. Both are counted now -- and the creation hooks fire for nothing, while
 * four resumes are counted, so the four tasks are made somewhere these two do not see. The fact that
 * matters survives either way: four requests, no feed.
 *
 * WHAT IS REACHABLE IN JetEngine FROM A HOOK, since that decides whether the next step is a run or a
 * week of reading. Of 71 042 Objective-C classes in the App Store's process, 40 are named like Jet
 * and 27 like JE, and they are real: JEBrotliDecoder/Encoder, JETreatment, JETreatmentProfile,
 * JETreatmentContext, JETreatmentAction and the JEDeres* actions, JEMediaActivityTracker,
 * JEMetricsDataPredicate, JEUnfairLock, JEWrappedMetricsPipeline, JetEngine itself, JetUI, and --
 * the one worth taking first -- _TtGC9JetEngine7PromiseT__, which is JetEngine's own Promise.
 * The App Store's page is built on promises of its own, and a promise graph is the instrument that
 * found the root in appstored in one slot. The same graph on JetEngine.Promise, with the entry and
 * exit counted apart, is where the next session should start: it says whether anything is waiting
 * for the content that is never asked for, and it costs a run rather than a read.
 */
static id (*old_dataTaskLocal)(id, SEL, void *, void *);
static id
new_dataTaskLocal(id self, SEL cmd, void *request, void *handler)
{
	id task;
	char buffer[260];

	entered[COUNT_dataTaskCreated]++;
	task = ((id (*)(id, SEL, void *, void *))old_dataTaskLocal)(self, cmd, request, handler);
	returned[COUNT_dataTaskCreated]++;
	say("annibale: store: TASK CREATED %s\n", text_of((id)request, buffer, sizeof(buffer)));
	return task;
}

/*
 * One original per row, with no two rows sharing a slot -- the mistake that cost four runs here and
 * is documented beside image_added: the table stores the first original it finds and the other rows
 * then call it with a receiver of another class. Three rows pointed at old_dataTaskLocal and two at
 * old_dataTaskURL, and both daemons aborted from the build that introduced them.
 */
static id (*old_dataTaskAMS)(id, SEL, void *, void *);
static id
new_dataTaskAMS(id self, SEL cmd, void *request, void *handler)
{
	id task;
	char buffer[260];

	entered[COUNT_dataTaskCreated]++;
	task = ((id (*)(id, SEL, void *, void *))old_dataTaskAMS)(self, cmd, request, handler);
	returned[COUNT_dataTaskCreated]++;
	say("annibale: store: TASK CREATED (ams session) %s\n", text_of((id)request, buffer, sizeof(buffer)));
	return task;
}

static id (*old_dataTaskURL)(id, SEL, void *, void *);
static id
new_dataTaskURL(id self, SEL cmd, void *url, void *handler)
{
	id task;
	char buffer[260];

	entered[COUNT_dataTaskCreated]++;
	task = ((id (*)(id, SEL, void *, void *))old_dataTaskURL)(self, cmd, url, handler);
	returned[COUNT_dataTaskCreated]++;
	say("annibale: store: TASK CREATED (url) %s\n", text_of((id)url, buffer, sizeof(buffer)));
	return task;
}

/* AMS makes its own, with a signpost: -[AMSURLSession dataTaskWithRequest:signpostID:completionHandler:] */
static id (*old_dataTaskSignpost)(id, SEL, void *, void *, void *);
static id
new_dataTaskSignpost(id self, SEL cmd, void *request, void *signpost, void *handler)
{
	id task;
	char buffer[260];

	entered[COUNT_dataTaskCreated]++;
	task = ((id (*)(id, SEL, void *, void *, void *))old_dataTaskSignpost)(self, cmd, request, signpost, handler);
	returned[COUNT_dataTaskCreated]++;
	say("annibale: store: TASK CREATED (ams) %s\n", text_of((id)request, buffer, sizeof(buffer)));
	return task;
}

static id (*old_dataTaskPlain)(id, SEL, void *);
static id
new_dataTaskPlain(id self, SEL cmd, void *request)
{
	id task;
	char buffer[260];

	entered[COUNT_dataTaskCreated]++;
	task = ((id (*)(id, SEL, void *))old_dataTaskPlain)(self, cmd, request);
	returned[COUNT_dataTaskCreated]++;
	say("annibale: store: TASK CREATED (no handler) %s\n", text_of((id)request, buffer, sizeof(buffer)));
	return task;
}

/* and what of JetEngine's own surface is reachable from here at all */
static void
dump_classes(const char *needle)
{
	unsigned count = 0;
	Class *classes = objc_copyClassList(&count);
	unsigned shown = 0;

	if (classes == NULL) {
		return;
	}
	for (unsigned i = 0; i < count && shown < 40; i++) {
		const char *name = class_getName(classes[i]);

		if (name != NULL && strstr(name, needle) != NULL) {
			shown++;
			say("annibale: store: class %s\n", name);
		}
	}
	free(classes);
	say("annibale: store: %u classes in all, %u named like \"%s\"\n", count, shown, needle);
}

/*
 * THE TWO WAITS LEFT IN THE APP, answered the way the device type was. The app's own promise graph
 * shows two threads parked on promises nobody fulfils: one in
 * -[ACAccountStore _updateStorefrontInLocalAccount:] and one in
 * ams_addCookiesForAccount:clientInfo:bag:cleanupGlobalCookies: inside
 * -[AMSURLRequestEncoder requestByEncodingRequest:parameters:]. Both are the same family as the
 * deadlock cut in appstored. Answering them says in one run whether they are on the drawing path,
 * which is far cheaper than reading unnamed Swift in JetEngine without knowing the answer is there.
 * The storefront update answers with the local account at once, as its sibling already does. The
 * cookies answer with nothing at all -- an empty set is a legitimate answer for a device with no
 * account, and the app is asking amsaccountsd for cookies and not being answered. Both return NULL
 * rather than a made-up value: nil is safe to message in Objective-C whatever the real return type
 * is, where a plausible-looking 1 would be a pointer. And both say on the line that they were
 * injected, so that a page drawn with no cookies is never read as a page drawn normally.
 */
static void *(*old_updateStorefront)(id, SEL, void *);
static void *
new_updateStorefront(id self, SEL cmd, void *account)
{
	(void)cmd;
	(void)account;
	entered[COUNT_updateStorefront]++;
	returned[COUNT_updateStorefront]++;
	say("annibale: store: INJECTED _updateStorefrontInLocalAccount: answered at once, no update\n");
	return (void *)((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("ams_localiTunesAccount"));
}

static void *(*old_addCookiesBlocking)(id, SEL, void *, void *, void *, void *);
static void *
new_addCookiesBlocking(id self, SEL cmd, void *account, void *client, void *bag, void *cleanup)
{
	(void)self;
	(void)cmd;
	(void)account;
	(void)client;
	(void)bag;
	(void)cleanup;
	entered[COUNT_addCookiesBlocking]++;
	returned[COUNT_addCookiesBlocking]++;
	say("annibale: store: INJECTED ams_addCookiesForAccount:...:cleanupGlobalCookies: answered with "
	    "NO COOKIES (not measured; the real one waits for ever on amsaccountsd)\n");
	return NULL;
}

/*
 * THE IMAGE THAT OPENS A JETPACK IS JetPack, NOT JetEngine. -[JPReader
 * processStream:signatureVerifier:options:error:] is the entry point, and being an Objective-C
 * method with an error: out-parameter it tells the three remaining stories apart on its own: never
 * called, called and returning an error (and which), or called and succeeding while something later
 * drops the result.
 * And the parser throws C++ exceptions -- JetPack::InvalidSegmentTagException takes the offending
 * segment tag as an int -- which is the best explanation of the silence measured so far: a throw
 * that somebody catches makes no notification, no log line and no decompression. A constructor in
 * another image cannot be replaced from here, but every throw goes through __cxa_throw, which is
 * bound through the stubs and can be interposed; the type's mangled name is the second word of its
 * std::type_info (the Itanium ABI puts __type_name there), so every C++ exception thrown anywhere
 * in the process says what it was.
 */
static id (*old_processStream)(id, SEL, void *, void *, void *, id *);
static id
new_processStream(id self, SEL cmd, void *stream, void *verifier, void *options, id *error)
{
	id result;
	char one[200];

	entered[COUNT_processStream]++;
	say("annibale: store: JETPACK processStream: in\n");
	result = old_processStream(self, cmd, stream, verifier, options, error);
	returned[COUNT_processStream]++;
	{
		/* and who owns the result next: whoever is on the stack when it is handed back */
		char frames[10 * 20 + 1] = "";

		frames_from((uintptr_t)__builtin_frame_address(0), frames, sizeof(frames));
		say("annibale: store: JETPACK processStream: out, %s, error %s, to%s\n",
		    result != NULL ? "a result" : "(nil)",
		    error != NULL && *error != NULL ? text_of(*error, one, sizeof(one)) : "(none)", frames);
	}
	return result;
}

/*
 * THE SEVENTH SHAPE OF AN INSTRUMENT THAT LIES, and the worst of them: ONE THAT CHANGES WHAT IT
 * MEASURES. There was an interpose on __cxa_throw here, to see every C++ exception by name. It
 * answered its question -- std::future_error is AMSPromise being deallocated, see above -- and then,
 * when it was trimmed to print only the first few and count the rest, both daemons began to die:
 *   annibale: appstored: signal 6 ... called from _abort, demangling_terminate_handler,
 *   _objc_terminate, std::__terminate
 * which is an exception reaching std::terminate instead of its handler. appstored went from one
 * start a session to eight, each ending in SIGABRT, and the App Store with it. The runs before that
 * build printed 500 to 600 throws and never aborted once, so it is the hook and not the system: an
 * exception thrown from a C frame of ours, with no unwind information, does not always find its way
 * home. The interpose is gone. What it was for is written down above, and if it is ever wanted
 * again it must be written so that the throw leaves through the original frame, not ours.
 * (It was caught at all only because the fault reporter watches SIGABRT, which it does because of an
 * earlier lesson on this branch. One instrument caught another.)
 */

static unsigned long notifications;

static bool
interesting_name(const char *name)
{
	static const char *const words[] = { "jet", "Jet", "ignature", "erif", "rotli", "JE", "Pack" };

	for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
		if (strstr(name, words[i]) != NULL) {
			return true;
		}
	}
	return false;
}

static id (*old_postName)(id, SEL, id, id, id);
static id
new_postName(id self, SEL cmd, id name, id object, id info)
{
	char buffer[160];
	const char *text = text_of(name, buffer, sizeof(buffer));

	notifications++;
	if (interesting_name(text)) {
		say("annibale: store: NOTIFICATION %s\n", text);
	}
	return old_postName(self, cmd, name, object, info);
}

static id (*old_postNotification)(id, SEL, id);
static id
new_postNotification(id self, SEL cmd, id note)
{
	char buffer[160];
	id name = ((id (*)(id, SEL))objc_msgSend)(note, sel_registerName("name"));
	const char *text = text_of(name, buffer, sizeof(buffer));

	notifications++;
	if (interesting_name(text)) {
		say("annibale: store: NOTIFICATION (object) %s\n", text);
	}
	return old_postNotification(self, cmd, note);
}

/*
 * EVERY ERROR THE STORE MAKES, by domain and code, in a table that repeats. After onboarding the
 * App Store draws its own tab bar and then says "Cannot Connect to App Store" with a Retry button
 * (docs/appstore-cannot-connect.png) -- and the task table still shows the same five requests, so
 * whatever it could not connect to was never asked for. The app knows something we do not, and it
 * knows it as an NSError: this remembers the domain and code of every one made in the process, with
 * a count, and prints them in each report. Nothing of the userInfo is printed -- that is where
 * tokens and account details live, and this library is going to run with an Apple ID signed in one
 * day.
 */
#define ERRORS 64
static struct error_note {
	char domain[48];
	long code;
	unsigned count;
} errors[ERRORS];
static unsigned error_count;
static pthread_mutex_t error_lock = PTHREAD_MUTEX_INITIALIZER;

static void
note_error(id domain, long code)
{
	char text[48];

	if (domain == NULL) {
		return;
	}
	text_of(domain, text, sizeof(text));
	pthread_mutex_lock(&error_lock);
	for (unsigned i = 0; i < error_count; i++) {
		if (errors[i].code == code && strncmp(errors[i].domain, text, sizeof(errors[0].domain) - 1) == 0) {
			errors[i].count++;
			pthread_mutex_unlock(&error_lock);
			return;
		}
	}
	if (error_count < ERRORS) {
		strlcpy(errors[error_count].domain, text, sizeof(errors[0].domain));
		errors[error_count].code = code;
		errors[error_count].count = 1;
		error_count++;
	}
	pthread_mutex_unlock(&error_lock);
}

static void
dump_errors(void)
{
	say("annibale: store: %u kinds of error made\n", error_count);
	for (unsigned i = 0; i < error_count; i++) {
		say("annibale: store: error %-40s code %ld, %u time(s)\n", errors[i].domain, errors[i].code,
		    errors[i].count);
	}
}

static id (*old_errorWith)(id, SEL, void *, long, void *);
static id
new_errorWith(id self, SEL cmd, void *domain, long code, void *info)
{
	note_error((id)domain, code);
	return old_errorWith(self, cmd, domain, code, info);
}

static id (*old_errorInit)(id, SEL, void *, long, void *);
static id
new_errorInit(id self, SEL cmd, void *domain, long code, void *info)
{
	note_error((id)domain, code);
	return old_errorInit(self, cmd, domain, code, info);
}

/*
 * THE SIXTH SHAPE OF AN INSTRUMENT THAT LIES: THE WRONG SLICE. The loader's log keeps the first
 * 256 KB and the last 1 MB and throws away the middle when a run overruns it, saying so in one line
 * -- "the log between the first 256 KB and the last 1 MB was dropped". Every run of this library has
 * hit it: 1 466 624 bytes, every time, with the middle gone. Anything printed once, in the middle of
 * a run, was never going to be read, and several measurements here were "missing" for that reason
 * alone and for no reason in the system at all.
 * So nothing that matters is printed once. Keys are remembered and printed again in every report,
 * the way promises and tasks already are, because the reports repeat and a later one lands in the
 * part of the log that is kept. (The other five shapes are listed beside the hooks below.)
 */
#define BAGKEYS 64
static struct bagkey_note {
	char key[72];
	unsigned asked;
	unsigned answered;
} bagkeys[BAGKEYS];
static unsigned bagkey_count;
static pthread_mutex_t bagkey_lock = PTHREAD_MUTEX_INITIALIZER;

static void
note_bagkey(const char *key, bool answered)
{
	pthread_mutex_lock(&bagkey_lock);
	for (unsigned i = 0; i < bagkey_count; i++) {
		if (strncmp(bagkeys[i].key, key, sizeof(bagkeys[0].key) - 1) == 0) {
			bagkeys[i].asked++;
			bagkeys[i].answered += answered ? 1 : 0;
			pthread_mutex_unlock(&bagkey_lock);
			return;
		}
	}
	if (bagkey_count < BAGKEYS) {
		strlcpy(bagkeys[bagkey_count].key, key, sizeof(bagkeys[0].key));
		bagkeys[bagkey_count].asked = 1;
		bagkeys[bagkey_count].answered = answered ? 1 : 0;
		bagkey_count++;
	}
	pthread_mutex_unlock(&bagkey_lock);
}

static void
dump_bagkeys(void)
{
	say("annibale: store: %u bag keys asked for\n", bagkey_count);
	for (unsigned i = 0; i < bagkey_count; i++) {
		say("annibale: store: bag key %-44s asked %u, answered %u%s\n", bagkeys[i].key, bagkeys[i].asked,
		    bagkeys[i].answered, bagkeys[i].answered == 0 ? "   <-- THE BAG HAS NOT GOT IT" : "");
	}
}

/*
 * A GRAPH OF PROMISES, not a list of calls. One promise in seventeen is ever finished here, and
 * naming the next deadlock by hand would take a slot each: what is wanted is the root -- a promise
 * with waiters, no finish, and a producer that is itself waiting on nothing anybody is fulfilling.
 * So every promise this library sees is remembered by address, with where it was first seen (two or
 * three frames of the walk, which "ipsw dyld a2s" names), how many continuations were attached to
 * it, how many threads are blocked in it synchronously and which, and whether it was ever finished.
 * The table is printed at 150 s beside the thread dump, so a blocked thread id in one is the same
 * thread in the other, and a cycle shows up as a cycle instead of as something to recognise.
 */
#define PROMISES 192
struct promise_note {
	void *promise;
	const char *label;
	unsigned waiters;
	unsigned sync_waiters;
	unsigned finished;
	uint64_t tid;                   /* the last thread seen blocked in it */
	char frames[160];
};
static struct promise_note promises[PROMISES];
static unsigned promise_count;
static pthread_mutex_t promise_lock = PTHREAD_MUTEX_INITIALIZER;

/* (the lock is never held across a call into AMS, so this cannot be part of the jam it measures) */
static struct promise_note *
note_for(void *promise, const char *frames)
{
	for (unsigned i = 0; i < promise_count; i++) {
		if (promises[i].promise == promise) {
			return &promises[i];
		}
	}
	if (promise_count >= PROMISES) {
		return NULL;
	}
	promises[promise_count].promise = promise;
	if (frames != NULL) {
		strlcpy(promises[promise_count].frames, frames, sizeof(promises[0].frames));
	}
	return &promises[promise_count++];
}

static void
note_waiter(void *promise, const char *frames, bool synchronous)
{
	struct promise_note *note;
	uint64_t tid = 0;

	pthread_threadid_np(NULL, &tid);
	pthread_mutex_lock(&promise_lock);
	note = note_for(promise, frames);
	if (note != NULL) {
		note->waiters++;
		if (synchronous) {
			note->sync_waiters++;
			note->tid = tid;
		}
	}
	pthread_mutex_unlock(&promise_lock);
}

static void
note_finished(void *promise)
{
	struct promise_note *note;

	pthread_mutex_lock(&promise_lock);
	note = note_for(promise, NULL);
	if (note != NULL) {
		note->finished++;
	}
	pthread_mutex_unlock(&promise_lock);
}

static void
note_label(void *promise, const char *label, const char *frames)
{
	struct promise_note *note;

	pthread_mutex_lock(&promise_lock);
	note = note_for(promise, frames);
	if (note != NULL) {
		note->label = label;
	}
	pthread_mutex_unlock(&promise_lock);
}

static void
dump_promises(void)
{
	say("annibale: store: %u promises seen\n", promise_count);
	for (unsigned i = 0; i < promise_count; i++) {
		struct promise_note *note = &promises[i];

		say("annibale: store: promise %p %-16s waiters %2u sync %u finished %u%s tid %llu seen from%s\n",
		    note->promise, note->label != NULL ? note->label : "", note->waiters, note->sync_waiters,
		    note->finished,
		    note->finished == 0 && note->sync_waiters > 0 ? "  <-- NOBODY FULFILS IT" : "",
		    (unsigned long long)note->tid, note->frames);
	}
}

/*
 * Every thread of the daemon, where it is and how it got there. Seventeen continuations are attached
 * to promises in a session and exactly one promise is ever finished, which is not a fact about the
 * bag: a promise completes when something runs its continuation, and continuations run on dispatch
 * queues, so a queue that is never serviced stops everything behind it. This says what each thread is
 * doing -- waiting in mach_msg for a reply that never comes, parked in __ulock_wait, or not there at
 * all -- and the run state and suspend count tell "blocked" from "never started", which are
 * different problems with different fixes. Nothing is suspended to read it: a snapshot taken by
 * stopping threads could stop one inside the write() this very line needs.
 */
static void
dump_threads(void)
{
	thread_act_array_t threads = NULL;
	mach_msg_type_number_t count = 0;
	mach_port_t me = mach_task_self();

	if (task_threads(me, &threads, &count) != KERN_SUCCESS) {
		say("annibale: store: task_threads() refused\n");
		return;
	}
	say("annibale: store: %u threads\n", count);
	for (unsigned i = 0; i < count; i++) {
		arm_thread_state64_t state;
		mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;
		struct thread_basic_info basic;
		mach_msg_type_number_t basic_count = THREAD_BASIC_INFO_COUNT;
		char text[10 * 20 + 1] = "";
		int run_state = -1, suspend = -1;

		if (thread_info(threads[i], THREAD_BASIC_INFO, (thread_info_t)&basic, &basic_count) == KERN_SUCCESS) {
			run_state = basic.run_state;
			suspend = basic.suspend_count;
		}
		if (thread_get_state(threads[i], ARM_THREAD_STATE64, (thread_state_t)&state, &state_count) != KERN_SUCCESS) {
			say("annibale: store: thread %u: no state (run %d)\n", i, run_state);
			continue;
		}
		frames_from((uintptr_t)arm_thread_state64_get_fp(state), text, sizeof(text));
		say("annibale: store: thread %u: run %d suspend %d pc %#lx lr %#lx from%s\n", i, run_state, suspend,
		    (uintptr_t)arm_thread_state64_get_pc(state) - slide,
		    (uintptr_t)arm_thread_state64_get_lr(state) - slide, text);
	}
	for (unsigned i = 0; i < count; i++) {
		mach_port_deallocate(me, threads[i]);
	}
	vm_deallocate(me, (vm_address_t)threads, count * sizeof(threads[0]));
}

static void
say_the_fault(int signal_number, siginfo_t *info, void *context_in)
{
	ucontext_t *context = context_in;
	uintptr_t pc = 0, lr = 0, fp = 0;
	char frames[10 * 20 + 1] = "";

	if (context != NULL && context->uc_mcontext != NULL) {
		pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(context->uc_mcontext->__ss);
		lr = (uintptr_t)__darwin_arm_thread_state64_get_lr(context->uc_mcontext->__ss);
		fp = (uintptr_t)__darwin_arm_thread_state64_get_fp(context->uc_mcontext->__ss);
	}
	frames_from(fp, frames, sizeof(frames));
	say("annibale: store: signal %d at %p, pc %#lx, lr %#lx, called from%s (unslid)\n", signal_number,
	    info != NULL ? info->si_addr : NULL, pc - slide, lr - slide, frames);
	signal(signal_number, SIG_DFL);
	raise(signal_number);
}

static void
watch_for_faults(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = say_the_fault;
	action.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGBUS, &action, NULL);
	/* an ObjC exception ends in abort(), and a bad replacement of ours is likelier to do that than
	   to touch bad memory: without these a daemon that dies of us leaves nothing at all */
	sigaction(SIGABRT, &action, NULL);
	sigaction(SIGILL, &action, NULL);
	sigaction(SIGTRAP, &action, NULL);
}

#include "inject_store_storefront.inc"
