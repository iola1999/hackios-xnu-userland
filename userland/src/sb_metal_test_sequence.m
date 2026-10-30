#import <Foundation/Foundation.h>
#include <assert.h>
#include <string.h>
static char trace[32]; static unsigned n; static int throws; static const char *bundle; static const char *loaded;
static void attempt_registration(const char *b) { trace[n++]='R'; bundle=b; if(throws)[NSException raise:@"Test" format:@"failure"]; }
static void registration_exception(NSException *e) { (void)e;trace[n++]='E'; }
static void registration_skipped(const char *b,const char *f) { (void)b;(void)f;trace[n++]='S'; }
static void load_baseline(const char *p) { trace[n++]='B'; loaded=p; }
#include "sb_metal_registration_sequence.inc"
static void check(const char *p,const char *b,const char *f,int t,const char *expected,const char *want)
{
    n=0;memset(trace,0,sizeof(trace));throws=t;bundle=loaded="<unset>";run_sequence(p,b,f);
    assert(!strcmp(trace,expected));
    if (expected[0]=='R') {
        assert((bundle==NULL)==(want==NULL));
        assert(!want || !strcmp(bundle,want));
    }
    if (strchr(expected,'B')) assert(!strcmp(loaded,"/redwood/lib/libredwood-sb26.dylib"));
}
int main(void) { @autoreleasepool {
 check("SpringBoard","23G90","1",0,"RB","com.apple.springboard");
 check("SpringBoard","23G90","1",1,"REB","com.apple.springboard");
 check("SpringBoard","23G90","0",0,"SB",NULL);check("SpringBoard","23G90",NULL,0,"SB",NULL);
 check("SpringBoard","23G90","01",0,"SB",NULL);check("SpringBoard","20F66","1",0,"SB",NULL);
 check("SpringBoard",NULL,"1",0,"SB",NULL);
 /* (a daemon: no bundle to match, and no SB26 baseline to load) */
 check("backboardd","23G90","1",0,"R",NULL);check("backboardd","23G90","1",1,"RE",NULL);
 check("backboardd","23G90","0",0,"S",NULL);
 check("notifyd","23G90","1",0,"",NULL);check(NULL,"23G90","1",0,"",NULL);
 puts("SEQUENCE_PASS cases=12 baseline_after_registration=1 off_has_no_registration=1 daemon_has_no_bundle_or_baseline=1");
 } return 0; }
