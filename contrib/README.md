# contrib

Third-party software that is part of the AquilaOS base system, as in the
BSDs' source trees. Unlike the BSDs it isn't kept here: each directory is
a recipe that fetches a pinned upstream release (see
[ports/](../ports/README.md#recipes) for the format), and `make` builds
it into `build/<arch>/contrib/root` (`build/contrib/root` for i386),
which every initrd includes. Optional software is in
[ports/](../ports/).

| recipe | version  |                                                    |
|--------|----------|----------------------------------------------------|
| dash   | 0.5.13.5 | `/bin/dash`, also `/bin/sh`                        |
| netcat | 1.239    | `/bin/nc`, OpenBSD's netcat (from Debian, its port to Linux and without TLS): TCP and UDP, client and server, port scans |
| runit  | 2.3.1    | `/sbin/runit` (init, `/init` links to `/sbin/runit-init`), `runsv`, `runsvdir`, `sv`, `chpst` and `runsvchdir` in `/bin`; without `svlogd` (needs IP sockets) and `utmpset` (no utmp) |
| sbase  | 0.1 (git) | `/bin/sbase-box`, linked as `ls`, `cp`, `sed`, `grep`, ...; without `chroot`, `cron`, `logger`, `nice`, `renice` and `tftp`, which need what the kernel doesn't have yet |
| sdhcp  | 0.1 (git) | `/sbin/sdhcp`, a DHCP client (the `dhcp` service, not enabled); without timerfd, which AquilaOS doesn't have: its timers are `poll()`'s timeout |

A recipe is rebuilt when its files, `mk/recipes.sh` or libc change.
`make contrib` builds only these; `mk/recipes.sh contrib dash` builds
a recipe even when it is up to date.
