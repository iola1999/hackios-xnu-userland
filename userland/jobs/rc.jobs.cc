# Commands run by the launchd job com.annibale.demo (see com.annibale.demo.plist).
# Control Center pulled down from the top right corner and closed (home button), three times, three seconds open
# and three closed: for a screen recording (rec_secs=N rec_start=S) and glframes=1, to see in which frame each of
# its parts appears.
/annibale/bin/sound_test springboard
/annibale/bin/anntouch wait 15
/annibale/bin/anntouch swipe 0.92 0.003 0.92 0.5 300 wait 3
/annibale/bin/anntouch button home wait 3
/annibale/bin/anntouch swipe 0.92 0.003 0.92 0.5 300 wait 3
/annibale/bin/anntouch button home wait 3
/annibale/bin/anntouch swipe 0.92 0.003 0.92 0.5 300 wait 3
/annibale/bin/anntouch button home wait 3
