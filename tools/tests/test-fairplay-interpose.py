#!/usr/bin/env python3
"""Exercise the production FairPlay wrappers with host IOKit stubs.

This checks forwarding, iterator ownership and diagnostic output. It does not
validate dyld interposition or FairPlay behavior on the device.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
typedef uint32_t mach_port_t;
typedef uint32_t io_iterator_t;
typedef uint32_t io_object_t;
typedef int kern_return_t;
typedef const void *CFDictionaryRef;
#define KERN_SUCCESS 0
#define IO_OBJECT_NULL 0
static bool ours;
static char log_text[2048];
static size_t log_size;
static unsigned match_calls, next_calls, struct_calls, dlopen_calls, dlerror_calls;
static int driver_result;
static io_object_t driver_objects[] = {41, 42, IO_OBJECT_NULL};
static size_t requested_capacity, driver_length;
static io_iterator_t *expected_iterator;
static const char authentication_input[] = "AUTHENTICATION-PAYLOAD";
static unsigned char output_buffer[32];
static size_t *expected_output_size;
static const void *expected_input;
static size_t expected_input_length;
static void *expected_output;
static const char private_path[] = "/private/var/containers/UUID-PRIVATE/probe";
static void *library_result;

static ssize_t fake_write(int fd, const void *bytes, size_t length) {
    assert(fd == 2 && log_size + length < sizeof(log_text));
    memcpy(log_text + log_size, bytes, length);
    log_size += length; log_text[log_size] = 0;
    errno = EBADF;
    return -1;
}
static kern_return_t IOServiceGetMatchingServices(mach_port_t port, CFDictionaryRef matching,
                                                 io_iterator_t *existing) {
    match_calls++;
    assert(port == 17 && matching == (const void *)(uintptr_t)23);
    assert(existing == expected_iterator);
    if (driver_result == KERN_SUCCESS && existing) *existing = 29;
    errno = EIO;
    return driver_result;
}
static io_object_t IOIteratorNext(io_iterator_t iterator) {
    assert(iterator == 29 && next_calls < 3);
    errno = ENXIO;
    return driver_objects[next_calls++];
}
static kern_return_t IOConnectCallStructMethod(mach_port_t connection, uint32_t selector,
    const void *input, size_t input_length, void *output, size_t *output_length) {
    struct_calls++;
    assert(connection == 101 && selector == 7);
    assert(input == expected_input && input_length == expected_input_length);
    assert(output == expected_output && output_length == expected_output_size);
    if (output_length) {
        assert(*output_length == requested_capacity);
        *output_length = driver_length;
    }
    if (output) memset(output, 0xa5, sizeof(output_buffer));
    errno = EINVAL;
    return driver_result;
}
static void *fake_dlopen(const char *path, int mode) {
    dlopen_calls++;
    assert(path == private_path && mode == 5);
    errno = ENOENT;
    return library_result;
}
static const char *fake_dlerror(void) { dlerror_calls++; return "PRIVATE-LOADER-ERROR"; }
#define write fake_write
#define dlopen fake_dlopen
#define dlerror fake_dlerror
'''
SCENARIOS = r'''
static void reset(bool logging) {
    ours = logging;
    log_text[0] = 0; log_size = 0;
    match_calls = next_calls = struct_calls = dlopen_calls = dlerror_calls = 0;
    driver_result = 0;
}
static void iterator_calls(bool logging) {
    reset(logging);
    io_iterator_t iterator = 88;
    expected_iterator = &iterator;
    assert(watched_IOServiceGetMatchingServices(17, (void *)(uintptr_t)23, &iterator) == 0);
    assert(match_calls == 1 && iterator == 29 && next_calls == 0 && errno == EIO);
    for (unsigned n = 0; n < 3; n++) {
        assert(watched_IOIteratorNext(iterator) == driver_objects[n]);
        assert(next_calls == n + 1 && errno == ENXIO);
    }
    assert(logging == (log_size != 0));
    reset(logging);
    driver_result = -17; iterator = 88; expected_iterator = &iterator;
    assert(watched_IOServiceGetMatchingServices(17, (void *)(uintptr_t)23, &iterator) == -17);
    assert(match_calls == 1 && iterator == 88 && next_calls == 0 && errno == EIO);
    expected_iterator = NULL;
    assert(watched_IOServiceGetMatchingServices(17, (void *)(uintptr_t)23, NULL) == -17);
    assert(match_calls == 2 && next_calls == 0 && errno == EIO);
}
static void struct_call(bool logging, bool size_pointer, int result, size_t returned_length) {
    reset(logging);
    size_t output_size = sizeof(output_buffer);
    expected_output_size = size_pointer ? &output_size : NULL;
    expected_input = authentication_input; expected_input_length = sizeof(authentication_input);
    expected_output = output_buffer;
    requested_capacity = output_size; driver_length = returned_length; driver_result = result;
    assert(watched_IOConnectCallStructMethod(101, 7, authentication_input,
           sizeof(authentication_input), output_buffer, expected_output_size) == result);
    assert(struct_calls == 1 && errno == EINVAL);
    assert(output_size == (size_pointer ? returned_length : sizeof(output_buffer)));
    for (size_t n = 0; n < sizeof(output_buffer); n++) assert(output_buffer[n] == 0xa5);
    assert(strstr(log_text, authentication_input) == NULL);
    assert(logging == (log_size != 0));
    if (logging) {
        char expected[256];
        snprintf(expected, sizeof(expected), "selector=7 input_bytes=%zu output_capacity=%zu "
                 "output_bytes=%zu output_size_present=%u result=%#x", sizeof(authentication_input),
                 size_pointer ? requested_capacity : 0, size_pointer ? returned_length : 0,
                 size_pointer, result);
        assert(strstr(log_text, expected) != NULL);
    }
}
static void empty_struct_call(bool logging) {
    reset(logging);
    expected_input = expected_output = NULL;
    expected_input_length = 0; expected_output_size = NULL;
    driver_result = -25;
    assert(watched_IOConnectCallStructMethod(101, 7, NULL, 0, NULL, NULL) == -25);
    assert(struct_calls == 1 && errno == EINVAL);
    if (logging) assert(strstr(log_text, "input_bytes=0 output_capacity=0 output_bytes=0 output_size_present=0"));
}
static void loader_calls(bool logging, bool succeeds) {
    reset(logging);
    library_result = succeeds ? (void *)(uintptr_t)71 : NULL;
    assert(watched_dlopen(private_path, 5) == library_result);
    assert(dlopen_calls == 1 && dlerror_calls == 0 && errno == ENOENT);
    assert(strstr(log_text, private_path) == NULL);
    assert(strstr(log_text, "UUID-PRIVATE") == NULL);
    assert(strstr(log_text, "PRIVATE-LOADER-ERROR") == NULL);
    assert(logging == (log_size != 0));
}
int main(void) {
    for (unsigned logging = 0; logging < 2; logging++) {
        iterator_calls(logging);
        struct_call(logging, true, 0, 8);
        struct_call(logging, true, -23, 64);
        struct_call(logging, false, -24, 0);
        empty_struct_call(logging);
        loader_calls(logging, false);
        loader_calls(logging, true);
    }
    puts("PASS: FairPlay forwarding, lengths, iterator calls, errno and private output");
    return 0;
}
'''


def extract(source, name):
    match = re.search(r'static [^\n]+\n' + re.escape(name) + r'\([^)]*\)\n\{.*?\n\}', source, re.S)
    assert match, name
    return match.group(0) + '\n'


def main():
    source = (ROOT / 'userland/src/inject/inject_fairplay.c').read_text()
    names = ['say', 'watched_IOServiceGetMatchingServices', 'watched_IOIteratorNext',
             'watched_IOConnectCallStructMethod', 'watched_dlopen']
    functions = {name: extract(source, name) for name in names}
    for symbol in ('IOIteratorNext', 'IOConnectCallStructMethod'):
        assert '{ (const void *)watched_' + symbol + ', (const void *)' + symbol + ' }' in source
    assert 'IOIteratorReset(' not in source and 'IOObjectRelease(' not in source
    compiler = shlex.split(os.environ.get('CC', 'clang'))
    mutations = [
        ('extra iterator consumption', 'watched_IOServiceGetMatchingServices',
         '\tif (ours) {', '\tif (result == 0 && existing) IOIteratorNext(*existing);\n\tif (ours) {'),
        ('changed iterator result', 'watched_IOIteratorNext', 'return object;', 'return IO_OBJECT_NULL;'),
        ('changed selector', 'watched_IOConnectCallStructMethod',
         'IOConnectCallStructMethod(connection, selector,', 'IOConnectCallStructMethod(connection, selector + 1,'),
        ('changed output length', 'watched_IOConnectCallStructMethod',
         '\treturn result;', '\tif (output_struct_count) *output_struct_count = 0;\n\treturn result;'),
        ('diagnostic changes errno', 'say', 'errno = saved_errno;', '(void)saved_errno;'),
        ('diagnostic consumes loader error', 'watched_dlopen', '\tif (ours) {', '\tif (ours) {\n\t\tdlerror();'),
    ]
    with tempfile.TemporaryDirectory(prefix='fairplay-interpose-') as temporary:
        cfile, binary = Path(temporary) / 'test.c', Path(temporary) / 'test'
        for name, mutation in [('production', None), *((m[0], m[1:]) for m in mutations)]:
            current = functions.copy()
            if mutation:
                function, before, after = mutation
                assert current[function].count(before) == 1, name
                current[function] = current[function].replace(before, after)
            cfile.write_text(PRELUDE + '\n'.join(current.values()) + SCENARIOS)
            subprocess.run(compiler + ['-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                                       str(cfile), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            if mutation:
                assert result.returncode != 0, name
                print('mutation rejected:', name)
            else:
                assert result.returncode == 0, result.stdout + result.stderr
                print(result.stdout, end='')


if __name__ == '__main__':
    main()
