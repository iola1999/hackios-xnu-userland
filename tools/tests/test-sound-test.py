#!/usr/bin/env python3
"""Exercise sound_test's queue lifecycle and deadline with host AudioQueue stubs."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(os.environ.get('SOUND_TEST_SOURCE', ROOT / 'userland/src/tests/sound_test.c'))
HARNESS = r'''
#include <AudioToolbox/AudioToolbox.h>
#include <mach/mach_time.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *mode;
static AudioQueueOutputCallback callback;
static void *context;
static AudioQueueBuffer buffers[3];
static int16_t samples[3][8192];
static AudioQueueBufferRef pending[128];
static unsigned head, tail, allocations, enqueues, stops, disposals, queries, starts;
static uint64_t accepted, ticks = 1000000000;
static bool live, stop_requested;
static AudioQueueRef const queue = (AudioQueueRef)(uintptr_t)1;
static bool is(const char *s) { return !strcmp(mode, s); }
static uint64_t fake_time(void) { return ticks; }
static kern_return_t fake_timebase(mach_timebase_info_t tb) { tb->numer=1;tb->denom=1;return 0; }
static OSStatus fake_new(const AudioStreamBasicDescription *fmt, AudioQueueOutputCallback cb,
    void *ctx, CFRunLoopRef loop, CFStringRef loopmode, UInt32 flags, AudioQueueRef *out) {
    assert(fmt->mSampleRate==48000 && fmt->mBytesPerFrame==4 && fmt->mChannelsPerFrame==2);
    assert(!loop && !loopmode && !flags);
    callback=cb;context=ctx;head=tail=allocations=enqueues=queries=starts=stops=0;
    accepted=0;stop_requested=false;
    if(is("create-error")) return -10;
    live=true;*out=queue;return 0;
}
static OSStatus fake_allocate(AudioQueueRef q, UInt32 size, AudioQueueBufferRef *out) {
    assert(q==queue && size==16384 && allocations<3);
    if(is("allocate-error") && allocations==1) return -11;
    AudioQueueBuffer buffer={.mAudioData=samples[allocations],.mAudioDataBytesCapacity=size};
    memcpy(&buffers[allocations],&buffer,sizeof(buffer));
    *out=&buffers[allocations++];return 0;
}
static OSStatus fake_enqueue(AudioQueueRef q, AudioQueueBufferRef b, UInt32 n,
    const AudioStreamPacketDescription *desc) {
    assert(q==queue && live && !stop_requested && !n && !desc);
    assert(b->mAudioDataByteSize>0 && b->mAudioDataByteSize<=16384 && b->mAudioDataByteSize%4==0);
    const int16_t *pcm=b->mAudioData;
    for(unsigned i=0;i<b->mAudioDataByteSize/2;i++) assert(pcm[i]==0);
    enqueues++;
    if((is("enqueue-error") && enqueues==4) || (is("prime-error") && enqueues==1)) return -12;
    accepted+=b->mAudioDataByteSize/4;pending[head++%128]=b;return 0;
}
static OSStatus fake_start(AudioQueueRef q, const AudioTimeStamp *stamp) {
    assert(q==queue && !stamp && !stop_requested);starts++;
    return is("start-error") ? -13 : 0;
}
static OSStatus fake_stop(AudioQueueRef q, Boolean immediate) {
    assert(q==queue && !immediate && starts==1);stops++;stop_requested=true;
    return is("stop-error") ? -14 : 0;
}
static OSStatus fake_property(AudioQueueRef q, AudioQueuePropertyID prop, void *out, UInt32 *size) {
    assert(q==queue && prop==kAudioQueueProperty_IsRunning && *size==sizeof(UInt32));
    assert(stop_requested);queries++;
    if(is("property-error")) return -15;
    if(is("property-size")) *size=1;
    *(UInt32 *)out = (head!=tail || queries<4 || is("never-stops"));return 0;
}
static OSStatus fake_dispose(AudioQueueRef q, Boolean immediate) {
    assert(q==queue && live && immediate);disposals++;
    /* Disposal may return outstanding buffers. They must not be submitted again. */
    unsigned before=enqueues;
    while(tail<head) callback(context,q,pending[tail++%128]);
    assert(enqueues==before);live=false;return is("dispose-error") ? -16 : 0;
}
static int fake_sleep(useconds_t us) {
    if(!live) return usleep(us);
    ticks+=(uint64_t)us*1000;
    if(!is("no-callback") && tail<head) callback(context,queue,pending[tail++%128]);
    return 0;
}
#define AudioQueueNewOutput fake_new
#define AudioQueueAllocateBuffer fake_allocate
#define AudioQueueEnqueueBuffer fake_enqueue
#define AudioQueueStart fake_start
#define AudioQueueStop fake_stop
#define AudioQueueGetProperty fake_property
#define AudioQueueDispose fake_dispose
#define mach_absolute_time fake_time
#define mach_timebase_info fake_timebase
#define usleep fake_sleep
#define main sound_main
SOURCE_INCLUDE
#undef main

int main(int argc,char **argv) {
    assert(argc==2);mode=argv[1];
    if(is("deadline")) {
        char *args[]={"sound_test","--deadline","0.02","wait","1",NULL};
        return sound_main(5,args);
    }
    if(is("bad-deadline")) {
        const char *bad[]={"0","-1","nan","3601","1oops","0.0000001"};
        for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
            char *args[]={"sound_test","--deadline",(char *)bad[i],"route",NULL};
            assert(sound_main(4,args)==2);
        }
        return 0;
    }
    char *args[]={"sound_test","level","0","tone",is("short") ? "0.01" : "3",NULL};
    int result=sound_main(5,args);
    bool success=is("success") || is("short");
    assert(result==(success ? 0 : 1));
    assert(!live && disposals==(is("create-error") ? 0u : 1u));
    if(success) {
        assert(accepted==(is("short") ? 480u : 144000u));
        assert(stops==1 && queries>=4 && tail==head);
    }
    puts("PASS: queue lifecycle, actual accepted frames and zero PCM");
    return 0;
}
'''


def main():
    cases = ['success', 'short', 'create-error', 'allocate-error', 'prime-error',
             'enqueue-error', 'start-error', 'stop-error', 'property-error',
             'property-size', 'dispose-error', 'no-callback', 'never-stops',
             'deadline', 'bad-deadline']
    with tempfile.TemporaryDirectory(prefix='sound-test-') as temp:
        folder = Path(temp)
        source = folder / 'harness.c'
        source.write_text(HARNESS.replace('SOURCE_INCLUDE', f'#include "{SOURCE}"'))
        binary = folder / 'harness'
        subprocess.run(['xcrun', '--sdk', 'macosx', 'clang', '-std=gnu11', '-O1',
                        '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', str(source),
                        '-framework', 'AudioToolbox', '-framework', 'CoreFoundation',
                        '-framework', 'AVFoundation', '-lobjc', '-o', str(binary)], check=True)
        for case in cases:
            result = subprocess.run([binary, case], capture_output=True, text=True, timeout=5)
            expected = 124 if case == 'deadline' else 1 if case == 'dispose-error' else 0
            if result.returncode != expected:
                raise AssertionError(f'{case}: exit {result.returncode}\n{result.stdout}\n{result.stderr}')
            if case == 'deadline':
                assert 'phase=wait event=begin' in result.stdout
                assert 'phase=deadline result=fail exit=124' in result.stderr
            if case == 'success':
                assert 'accepted=144000 wanted=144000 stopped=1 status=0 dispose=0' in result.stdout
            print(f'PASS {case}')
    print(json.dumps({'cases': len(cases), 'source': str(SOURCE.relative_to(ROOT))
                     if SOURCE.is_relative_to(ROOT) else SOURCE.name}))


if __name__ == '__main__':
    main()
