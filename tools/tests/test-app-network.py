#!/usr/bin/env python3
"""Test production request diagnostics with native Foundation objects and local task methods.

No network requests are sent. Live 23G90 method coverage still needs a device run.
"""
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'userland/src/inject/inject_app_network.inc'
OUT = Path(os.environ.get('APP_NETWORK_TEST_OUT', ROOT / 'out/app-network-test'))

HARNESS = r'''
#import <Foundation/Foundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *who = "network-test";
static char captured[1024];
static size_t captured_length;
static unsigned resumed, finished;
static id expected_task, expected_error;
static SEL expected_selector;
static void (*resume_task)(id, SEL);
static void say(const char *text, size_t length) {
    assert(length < 384 && captured_length + length < sizeof(captured));
    memcpy(captured + captured_length, text, length);
    captured_length += length; captured[captured_length] = 0;
    errno = EACCES;
}

@interface ProbeTask : NSObject
@property(nonatomic, strong) NSURLRequest *originalRequest;
@property(nonatomic, strong) NSURLResponse *response;
- (NSUInteger)taskIdentifier;
- (int64_t)countOfBytesReceived;
- (void)_onqueue_didFinishWithError:(NSError *)error;
@end
@implementation ProbeTask
- (NSUInteger)taskIdentifier { errno = EACCES; return 42; }
- (int64_t)countOfBytesReceived { return 1234; }
- (void)_onqueue_didFinishWithError:(NSError *)error {
    assert(self == expected_task && _cmd == expected_selector && error == expected_error);
    assert(errno == EBUSY); finished++; errno = ENODEV;
}
@end
@interface WrongABI : NSObject
- (id)_onqueue_didFinishWithError:(id)error;
@end
@implementation WrongABI
- (id)_onqueue_didFinishWithError:(id)error { return error; }
@end

/* PRODUCTION */

static void original_resume(id task, SEL selector) {
    assert(task == expected_task && selector == expected_selector && errno == EBUSY);
    resumed++; errno = ENODEV;
}
static void reset_log(void) { captured_length = 0; captured[0] = 0; errno = EBUSY; }
static void no_private_output(void) {
    for (const char **p = (const char *[]) {"PRIVATE", "user:", "example.com", "Authorization",
                                          "token=", "fragment", "userInfo", NULL}; *p; ++p)
        assert(strstr(captured, *p) == NULL);
}
int main(void) {
  @autoreleasepool {
    unsetenv("REDWOOD_NET_TRACE"); watch_network_completion();
    assert(finish_local_task == NULL && finish_background_task == NULL && finish_background_download_task == NULL);
    setenv("REDWOOD_NET_TRACE", "0", 1); watch_network_completion();
    assert(finish_local_task == NULL && finish_background_task == NULL && finish_background_download_task == NULL);
    char bounded[2048]; memset(bounded, 'a', sizeof(bounded));
    assert(network_hash(bounded) != 0 && network_hash(NULL) == 0);

    ProbeTask *task = [ProbeTask new];
    NSURL *url = [NSURL URLWithString:@"https://user:PRIVATE@example.com/PRIVATE/path?token=PRIVATE#fragment"];
    task.originalRequest = [NSURLRequest requestWithURL:url];
    task.response = [[NSHTTPURLResponse alloc] initWithURL:url statusCode:403 HTTPVersion:@"HTTP/1.1"
                                            headerFields:@{@"Authorization": @"PRIVATE"}];
    expected_task = task; expected_selector = sel_registerName("resume"); resume_task = original_resume;
    reset_log(); watched_resume_task(task, expected_selector);
    assert(resumed == 1 && errno == ENODEV);
    assert(strstr(captured, "stage=resume task=42 scheme=https"));
    assert(strstr(captured, "http_status=403 error_present=0 error_domain=none error_code=0 received=1234"));
    no_private_output();

    NSError *error = [NSError errorWithDomain:NSURLErrorDomain code:-1009
                                     userInfo:@{NSLocalizedDescriptionKey: @"PRIVATE userInfo"}];
    expected_error = error; expected_selector = sel_registerName("_onqueue_didFinishWithError:");
    watch_network_finish("ProbeTask", (IMP)watched_finish_local_task, &finish_local_task);
    assert(finish_local_task != NULL);
    IMP original = (IMP)finish_local_task;
    watch_network_finish("ProbeTask", (IMP)watched_finish_local_task, &finish_local_task);
    assert((IMP)finish_local_task == original);
    reset_log(); [task _onqueue_didFinishWithError:error];
    assert(finished == 1 && errno == ENODEV);
    assert(strstr(captured, "stage=local-finish"));
    assert(strstr(captured, "http_status=403 error_present=1 error_domain=NSURLErrorDomain error_code=-1009"));
    no_private_output();

    finish_background_task = (void (*)(id, SEL, id))original;
    reset_log(); watched_finish_background_task(task, expected_selector, error);
    assert(finished == 2 && errno == ENODEV && strstr(captured, "stage=background-finish"));
    no_private_output();
    finish_background_download_task = (void (*)(id, SEL, id))original;
    reset_log(); watched_finish_background_download_task(task, expected_selector, error);
    assert(finished == 3 && errno == ENODEV && strstr(captured, "stage=background-download-finish"));
    no_private_output();
    IMP wrong_before = class_getMethodImplementation([WrongABI class], expected_selector);
    IMP untouched = NULL;
    watch_network_finish("WrongABI", (IMP)watched_finish_local_task, &untouched);
    watch_network_finish("MissingNetworkTestClass", (IMP)watched_finish_local_task, &untouched);
    assert(untouched == NULL && class_getMethodImplementation([WrongABI class], expected_selector) == wrong_before);

    reset_log(); network_task_record(nil, nil, "resume");
    assert(errno == EBUSY && strstr(captured, "task=0 scheme=other host_hash=0000000000000000"));
    NSError *private_error = [NSError errorWithDomain:@"PRIVATE-domain" code:7 userInfo:nil];
    reset_log(); network_task_record(task, private_error, "local-finish");
    assert(errno == EBUSY && strstr(captured, "error_domain=other error_code=7")); no_private_output();

    NSMutableString *long_url = [NSMutableString stringWithString:@"https://example.com/"];
    for (int i = 0; i < 3000; i++) [long_url appendString:@"x"];
    [long_url appendString:@"?token=PRIVATE"];
    task.originalRequest = [NSURLRequest requestWithURL:[NSURL URLWithString:long_url]];
    who = "a-very-long-process-name-that-exceeds-the-sixty-four-byte-logging-limit-PRIVATE";
    reset_log(); network_task_record(task, nil, "resume");
    assert(errno == EBUSY && captured_length < 384); no_private_output();
    method_setImplementation(class_getInstanceMethod([ProbeTask class], expected_selector), original);
    puts("PASS: request metadata, completion forwarding, errno, ABI guards and bounded private output");
  }
}
'''


def run(source, name):
    path = OUT / (name + '.m')
    binary = OUT / name
    path.write_text(HARNESS.replace('/* PRODUCTION */', source))
    args = ['xcrun', '--sdk', 'macosx', 'clang', '-fobjc-arc', '-O1', '-Wall', '-Wextra', '-Werror',
            '-framework', 'Foundation', '-fsanitize=address,undefined', str(path), '-o', str(binary)]
    subprocess.run(args, check=True, capture_output=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10,
                            env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
    (OUT / (name + '.log')).write_text(result.stdout + result.stderr)
    return result


OUT.mkdir(parents=True, exist_ok=True)
source = SOURCE.read_text()
result = run(source, 'production')
assert result.returncode == 0, result.stdout + result.stderr
print(result.stdout.strip())
mutations = [
    ('lost-completion', 'finish_local_task(self, cmd, error);', '(void)self; (void)cmd; (void)error;'),
    ('changed-completion-error', 'finish_local_task(self, cmd, error);', 'finish_local_task(self, cmd, NULL);'),
    ('lost-background-download-completion', 'finish_background_download_task(self, cmd, error);',
     '(void)self; (void)cmd; (void)error;'),
    ('lost-errno', 'errno = saved_errno;', '(void)saved_errno;'),
    ('private-error-domain', 'return "other";', 'return domain != NULL ? domain : "other";'),
    ('unbounded-hash', 'i < 2048 && text[i] != 0', 'text[i] != 0'),
    ('ignored-live-abi', 'strcmp(method_getTypeEncoding(method), "v24@0:8@16") == 0', '1'),
]
for name, old, new in mutations:
    assert source.count(old) == 1, name
    result = run(source.replace(old, new), name)
    assert result.returncode != 0, name
    print('mutation rejected:', name)
