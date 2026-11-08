#!/usr/bin/env python3
"""Exercise the production FairPlay wrappers with host IOKit stubs.

This checks forwarding, CF and iterator ownership, and diagnostic output. It does not
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
typedef uint32_t io_registry_entry_t;
typedef uint32_t IOOptionBits;
typedef char io_string_t[512];
typedef int kern_return_t;
typedef const void *CFDictionaryRef;
typedef const void *CFAllocatorRef;
typedef unsigned long CFTypeID;
typedef long CFIndex;
struct cf_object { CFTypeID type; CFIndex length; const char *text; };
typedef const struct cf_object *CFTypeRef;
typedef CFTypeRef CFStringRef;
typedef CFTypeRef CFErrorRef;
#define kCFStringEncodingUTF8 0x08000100
#define KERN_SUCCESS 0
#define IO_OBJECT_NULL 0
static bool ours;
static uintptr_t own_image_base, own_text_start, own_text_size, return_address;
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
static unsigned path_calls, property_calls, metadata_calls, release_calls, retain_calls;
static unsigned gestalt_calls, entitlement_calls;
static const char *expected_path;
static io_registry_entry_t registry_result;
static CFStringRef expected_key;
static CFTypeRef property_result;
static CFAllocatorRef expected_allocator;
static IOOptionBits expected_options;
static CFErrorRef *expected_error;
static const struct cf_object good_key = {2, 14, "unique-chip-id"};
static const struct cf_object unsafe_key = {2, 42, "/private/var/containers/UUID-PRIVATE/key"};
static const struct cf_object newline_key = {2, 8, "key\nextra"};
static const struct cf_object long_key = {2, 110,
    "long-property-name-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"};
static const struct cf_object values[] = {
    {1, 32, "AUTHENTICATION-PAYLOAD"}, {2, 36, "VALUE-PRIVATE-UUID"},
    {3, 4, "ARRAY-PRIVATE"}, {4, 5, "DICTIONARY-PRIVATE"},
    {5, 8, "NUMBER-PRIVATE"}, {6, 1, "BOOLEAN-PRIVATE"}, {99, 77, "UNKNOWN-PRIVATE"}
};

static io_registry_entry_t IORegistryEntryFromPath(mach_port_t port, const char *path) {
    path_calls++;
    assert(port == 17 && path == expected_path);
    errno = ENODEV;
    return registry_result;
}
static CFTypeRef IORegistryEntryCreateCFProperty(io_registry_entry_t entry, CFStringRef key,
                                                CFAllocatorRef allocator, IOOptionBits options) {
    property_calls++;
    assert(entry == 103 && key == expected_key && allocator == expected_allocator);
    assert(options == expected_options);
    errno = EBUSY;
    return property_result;
}
static CFTypeRef MGCopyAnswer(CFStringRef key) {
    gestalt_calls++; assert(key == expected_key); errno = EBUSY;
    return property_result;
}
static CFTypeRef SecTaskCopyValueForEntitlement(void *task, CFStringRef key, CFErrorRef *error) {
    entitlement_calls++;
    assert(task == (void *)(uintptr_t)111 && key == expected_key && error == expected_error);
    if (error) *error = &values[6];
    errno = EBUSY;
    return property_result;
}
static CFTypeID CFGetTypeID(CFTypeRef value) {
    assert(value && (value == expected_key || value == property_result));
    metadata_calls++; errno = EACCES;
    return value->type;
}
static CFTypeID CFDataGetTypeID(void) { return 1; }
static CFTypeID CFStringGetTypeID(void) { return 2; }
static CFTypeID CFArrayGetTypeID(void) { return 3; }
static CFTypeID CFDictionaryGetTypeID(void) { return 4; }
static CFTypeID CFNumberGetTypeID(void) { return 5; }
static CFTypeID CFBooleanGetTypeID(void) { return 6; }
static CFIndex metadata_length(CFTypeRef value, CFTypeID type) {
    assert(value == property_result && value->type == type);
    metadata_calls++; errno = EACCES;
    return value->length;
}
static CFIndex CFDataGetLength(CFTypeRef value) { return metadata_length(value, 1); }
static CFIndex CFStringGetLength(CFTypeRef value) { return metadata_length(value, 2); }
static CFIndex CFArrayGetCount(CFTypeRef value) { return metadata_length(value, 3); }
static CFIndex CFDictionaryGetCount(CFTypeRef value) { return metadata_length(value, 4); }
static bool CFStringGetCString(CFStringRef value, char *buffer, CFIndex size, unsigned encoding) {
    assert(value == expected_key && value->type == 2 && encoding == kCFStringEncodingUTF8);
    metadata_calls++; errno = EACCES;
    if (strlen(value->text) >= (size_t)size) {
        memcpy(buffer, value->text, (size_t)size - 1); buffer[size - 1] = 0;
        return false;
    }
    strcpy(buffer, value->text);
    return true;
}
static void CFRelease(CFTypeRef value) { assert(value == property_result); release_calls++; }
static CFTypeRef CFRetain(CFTypeRef value) {
    assert(value == property_result); retain_calls++; return value;
}
static void CFDataGetBytes(CFTypeRef value) { (void)value; assert(!"property payload read"); }
#define __builtin_return_address(level) ((void *)return_address)
#define __builtin_extract_return_addr(address) (address)

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
    path_calls = property_calls = metadata_calls = release_calls = retain_calls = 0;
    gestalt_calls = entitlement_calls = 0;
    own_image_base = own_text_start = 0x100000000UL; own_text_size = 0x1000;
    return_address = own_image_base + 0x234;
    driver_result = 0;
}
static void registry_path(bool logging, const char *path, const char *kind, bool succeeds) {
    reset(logging);
    expected_path = path; registry_result = succeeds ? 107 : IO_OBJECT_NULL;
    assert(watched_IORegistryEntryFromPath(17, path) == registry_result);
    assert(path_calls == 1 && errno == ENODEV && metadata_calls == 0);
    assert(logging == (log_size != 0));
    assert(strstr(log_text, "UUID-PRIVATE") == NULL && strstr(log_text, private_path) == NULL);
    if (logging) {
        char expected[96];
        snprintf(expected, sizeof(expected), "path_kind=%s ", kind);
        assert(strstr(log_text, expected));
        snprintf(expected, sizeof(expected), "entry_present=%u errno_after=%d", succeeds, ENODEV);
        assert(strstr(log_text, expected));
        assert(strstr(log_text, "caller_main=1 caller_offset=0x234"));
        if (!strcmp(kind, "other") || !strcmp(kind, "null"))
            assert(strstr(log_text, "path_hash=0000000000000000 path_bytes=0 path_complete=0"));
        else assert(strstr(log_text, "path_hash=0000000000000000") == NULL);
    }
}
static void registry_paths(bool logging) {
    registry_path(logging, "IODeviceTree:/chosen", "IODeviceTree", true);
    registry_path(logging, "IOService:/Root/UUID-PRIVATE", "IOService", false);
    registry_path(logging, private_path, "other", true);
    registry_path(logging, "IODeviceTree:private", "other", true);
    registry_path(logging, "IOService:private", "other", false);
    registry_path(logging, NULL, "null", false);
    char full_path[sizeof(io_string_t)];
    memset(full_path, 'x', sizeof(full_path)); memcpy(full_path, "IOService:/", 11);
    registry_path(logging, full_path, "IOService", true);
    if (logging) assert(strstr(log_text, "path_bytes=512 path_complete=0"));
    assert(caller_offset(own_image_base) == 0);
    assert(caller_offset(own_image_base + own_text_size - 1) == own_text_size - 1);
    assert(caller_offset(own_image_base - 1) == UINTPTR_MAX);
    assert(caller_offset(own_image_base + own_text_size) == UINTPTR_MAX);
    return_address = own_image_base + own_text_size;
    log_size = 0; log_text[0] = 0;
    assert(watched_IORegistryEntryFromPath(17, full_path) == registry_result);
    assert(errno == ENODEV && path_calls == 2);
    if (logging) assert(strstr(log_text, "caller_main=0 caller_offset=0"));
    own_image_base = 0;
    assert(caller_offset(own_text_start + 1) == UINTPTR_MAX);
}
static void registry_property(bool logging, CFTypeRef value, CFStringRef key, const char *key_text) {
    reset(logging);
    expected_key = key; property_result = value;
    expected_allocator = (void *)(uintptr_t)109; expected_options = 0x104;
    assert(watched_IORegistryEntryCreateCFProperty(103, key, expected_allocator, expected_options) == value);
    assert(property_calls == 1 && errno == EBUSY && release_calls == 0 && retain_calls == 0);
    assert(logging == (log_size != 0));
    assert(strstr(log_text, "PRIVATE") == NULL && strstr(log_text, "AUTHENTICATION-PAYLOAD") == NULL);
    if (logging) {
        char expected[192];
        const char *units[] = {"none", "bytes", "utf16", "elements", "entries"};
        const char *types[] = {"null", "data", "string", "array", "dictionary", "number", "boolean"};
        CFTypeID type = value ? value->type : 0;
        bool sized = type >= 1 && type <= 4;
        snprintf(expected, sizeof(expected), "key=%s value_present=%u type=%s type_id=%lu length=%ld units=%s errno_after=%d",
                 key_text, value != NULL, type <= 6 ? types[type] : "other", type,
                 sized ? value->length : -1, units[sized ? type : 0], EBUSY);
        assert(strstr(log_text, expected));
        assert(strstr(log_text, "caller_main=1 caller_offset=0x234"));
        assert(strstr(log_text, "key\nextra") == NULL);
    } else assert(metadata_calls == 0);
}
static void registry_properties(bool logging) {
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++)
        registry_property(logging, &values[i], &good_key, "unique-chip-id");
    registry_property(logging, NULL, &good_key, "unique-chip-id");
    registry_property(logging, &values[0], NULL, "(unreadable)");
    registry_property(logging, &values[0], &unsafe_key, "(unreadable)");
    registry_property(logging, &values[0], &newline_key, "(unreadable)");
    registry_property(logging, &values[0], &long_key, "(unreadable)");
    registry_property(logging, &values[0], &values[0], "(unreadable)");
    expected_key = NULL; expected_allocator = NULL; expected_options = 0;
    return_address = own_image_base - 1;
    log_size = 0; log_text[0] = 0;
    assert(watched_IORegistryEntryCreateCFProperty(103, NULL, NULL, 0) == property_result);
    assert(property_calls == 2 && errno == EBUSY && release_calls == 0 && retain_calls == 0);
    if (logging) assert(strstr(log_text, "caller_main=0 caller_offset=0"));
}
static void other_property_calls(bool logging, bool succeeds) {
    reset(logging);
    expected_key = &good_key; property_result = succeeds ? &values[0] : NULL;
    assert(watched_MGCopyAnswer(expected_key) == property_result);
    assert(gestalt_calls == 1 && errno == EBUSY);
    CFErrorRef error = NULL; expected_error = &error;
    assert(watched_SecTaskCopyValueForEntitlement((void *)(uintptr_t)111, expected_key, &error) == property_result);
    assert(entitlement_calls == 1 && error == &values[6] && errno == EBUSY);
    expected_key = NULL; expected_error = NULL;
    assert(watched_MGCopyAnswer(NULL) == property_result);
    assert(gestalt_calls == 2 && errno == EBUSY);
    assert(watched_SecTaskCopyValueForEntitlement((void *)(uintptr_t)111, NULL, NULL) == property_result);
    assert(entitlement_calls == 2 && errno == EBUSY && release_calls == 0 && retain_calls == 0);
    assert(strstr(log_text, "AUTHENTICATION-PAYLOAD") == NULL);
    assert(logging == (log_size != 0));
    if (!logging) assert(metadata_calls == 0);
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
        registry_paths(logging);
        registry_properties(logging);
        other_property_calls(logging, false);
        other_property_calls(logging, true);
    }
    puts("PASS: FairPlay forwarding, lengths, ownership, call counts, errno, caller offsets and private output");
    return 0;
}
'''


def extract(source, name):
    match = re.search(r'static [^\n]+\n' + re.escape(name) + r'\([^)]*\)\n\{.*?\n\}', source, re.S)
    assert match, name
    return match.group(0) + '\n'


def main():
    source = (ROOT / 'userland/src/inject/inject_fairplay.c').read_text()
    names = ['say', 'caller_offset', 'watched_IORegistryEntryFromPath',
             'watched_IORegistryEntryCreateCFProperty', 'watched_IOServiceGetMatchingServices', 'watched_IOIteratorNext',
             'watched_IOConnectCallStructMethod', 'watched_dlopen', 'watched_MGCopyAnswer',
             'watched_SecTaskCopyValueForEntitlement']
    functions = {name: extract(source, name) for name in names}
    for symbol in ('IOIteratorNext', 'IOConnectCallStructMethod',
                   'IORegistryEntryFromPath', 'IORegistryEntryCreateCFProperty'):
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
        ('extra registry lookup', 'watched_IORegistryEntryFromPath',
         '\treturn entry;', '\tIORegistryEntryFromPath(port, path);\n\treturn entry;'),
        ('changed registry result', 'watched_IORegistryEntryFromPath',
         '\treturn entry;', '\treturn IO_OBJECT_NULL;'),
        ('registry path text exposed', 'watched_IORegistryEntryFromPath',
         '\treturn entry;', '\tif (ours && path) say("%s", path);\n\treturn entry;'),
        ('changed property options', 'watched_IORegistryEntryCreateCFProperty',
         'IORegistryEntryCreateCFProperty(entry, key, allocator, options);',
         'IORegistryEntryCreateCFProperty(entry, key, allocator, options + 1);'),
        ('extra property lookup', 'watched_IORegistryEntryCreateCFProperty',
         '\treturn value;', '\tIORegistryEntryCreateCFProperty(entry, key, allocator, options);\n\treturn value;'),
        ('changed property result', 'watched_IORegistryEntryCreateCFProperty',
         '\treturn value;', '\treturn NULL;'),
        ('property released by probe', 'watched_IORegistryEntryCreateCFProperty',
         '\treturn value;', '\tif (value) CFRelease(value);\n\treturn value;'),
        ('property retained by probe', 'watched_IORegistryEntryCreateCFProperty',
         '\treturn value;', '\tif (value) CFRetain(value);\n\treturn value;'),
        ('property payload read', 'watched_IORegistryEntryCreateCFProperty',
         '\treturn value;', '\tif (value) CFDataGetBytes(value);\n\treturn value;'),
        ('property metadata changes errno', 'watched_IORegistryEntryCreateCFProperty',
         '\terrno = saved_errno;', '\t(void)saved_errno;'),
        ('foreign caller address exposed', 'caller_offset',
         '\treturn UINTPTR_MAX;', '\treturn address;'),
        ('gestalt metadata changes errno', 'watched_MGCopyAnswer',
         '\terrno = saved_errno;', '\t(void)saved_errno;'),
        ('entitlement metadata changes errno', 'watched_SecTaskCopyValueForEntitlement',
         '\terrno = saved_errno;', '\t(void)saved_errno;'),
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
