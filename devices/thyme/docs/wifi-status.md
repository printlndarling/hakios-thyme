# Wi-Fi on HakiOS: where the link stands

Measured on the phone (RAM boots; Android) through 2026-11-15. The three failures that cost
runs are at the end, so the next person does not pay for them again.

## Working

- The radio comes up through the ath11k port (`lx`), joins a WPA2 network by name and
  passphrase (`net_test joinfile`), takes its address through configd's DHCP, resolves names
  and carries traffic: Safari pages, apt against the Procursus repository, and Sileo's own
  downloads (a dependency install passed with seven packages, `logs/070701-run.log`).
- A deauthentication while the system runs is recovered: at 242.65 s an access point
  deauthenticated us (reason 3) and an ASSOCIATE five seconds later brought the link back
  (association accepted 248.20, link up 248.35, traffic after; `logs/090945-run.log`). The
  process that asked is not in the log -- the driver keeps its per-request lines to 200 and
  the name was cut -- but wifid was polling the interface throughout, and the station layer's
  own path is the next section.

## The sleep case, which is the user's "it forgets the network"

wifid logs the session ending `due to SystemSleep` (e.g. 180 s after a join,
`logs/070700-run.log`), and in that run the link never came back: en0 down at 1155 s with its
counters frozen, Sileo's downloads answering "the Internet connection appears to be offline".
The same logs show why a scene cannot simply fix it: the system sleeps about a minute after
boot when nothing holds the display, and a sleeping system suspends every userland process --
scene commands, injected touches and launchd jobs alike; only an application in front that
sets `idleTimerDisabled` keeps SpringBoard from locking (Sileo does; `sleep_hold`'s two PM
timers and injected touches do not).

## The two recovery mechanisms

- **Kernel, on a radio power cycle.** A userland ASSOCIATE now goes through
  `hakios_sta_ask_join_standing`, so the network is the station layer's "standing" one and
  is joined again whenever the radio is powered on (`hakios.thyme_sta.c`, the power-on
  branch). Verified with `net_test power 0` then `power 1` and no joinfile between them: the
  station re-joined by itself (association accepted as station 15, `station driver join
  returned 0`; `logs/101852-run.log`).
- **Userland, on a link that stays away.** `com.hakios.wifi-keepalive` runs
  `net_test keepalive /redwood/fw/network`: it reads the kernel data-link events, waits five
  seconds to tell a real loss from a join's own bounce, and joins again. Verified:
  `net_test leave` at 301.96 s, the daemon's join at 307.05 s, link up at 307.75 s, with the
  address and a name back (`logs/103116-run.log`).

## The testing tools

`net_test` has `joinfile [FILE]`, `leave`, `power 0|1` and `keepalive [FILE] [SECONDS]`;
`sleep_hold SECONDS` holds `kPMMinutesToSleep` and `kPMMinutesToDim` (and refuses when the
old value cannot be read). The scenes `rc.jobs.wifipower` and `rc.jobs.wifidrop` open Sileo
first -- the display must be held by an application or the job is suspended --
`devices/thyme/wifi-verify.sh` runs both, and both need the jb system image for Sileo.

## Open

- The system's own sleep-to-wake path: whether a touch-wake takes the radio through the same
  power transition our power test exercises. It needs the owner's hand on the screen; a scene
  cannot wake a sleeping phone.
- Wi-Fi while the system is asleep (background networking, wake-on-wireless) is not attempted.
- The AP deauthentication's cause was not established; wifid's recovery is what the log
  shows, not ours.
