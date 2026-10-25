# runit, init and service supervision, http://smarden.org/runit/
# (fetched from Debian, whose orig tarball is the upstream release)

NAME=runit
VERSION=2.3.1
URL=https://deb.debian.org/debian/pool/main/r/runit/runit_$VERSION.orig.tar.gz
SHA256=634f23c8c4d1d440043be0fe928ddf904626289e97bfe7c5826e93aaf2cc6fe9
ARCHIVE_DIR=admin/runit-$VERSION

build()
{
    cd src

    # the system's toolchain ($CC), and what the build finds out by
    # running programs on the build host given instead: poll() for
    # iopause, uint64 as unsigned long long (right whether unsigned long
    # has 32 or 64 bits), the system type; no utmp and no IP sockets
    # (only utmpset and svlogd use them)
    echo "$CC -Os -Wall" > conf-cc
    echo "$CC -s" > conf-ld
    sed -i -e 's|\./choose clr trypoll iopause.h1 iopause.h2|cat iopause.h2|' \
           -e 's|\./choose clr tryulong64 uint64.h1 uint64.h2|cat uint64.h1|' \
           -e 's|sh find-systype.sh > systype|echo aquila-i386 > systype|' \
           -e '/tryuwtmpx.c 2>\/dev\/null/{N;s|.*|\techo "/* sysdep: -utmp */" > uw_tmp.h|}' \
           -e '/^socket.lib:/,/^$/c\socket.lib:\n\t: > socket.lib\n' Makefile
    sed -i -e "s|'ar cr|'$AR cr|" -e "s|'ranlib|'$RANLIB|" print-ar.sh

    # not utmpset and svlogd (see above)
    local progs="chpst runit runit-init runsv runsvchdir runsvdir sv"
    make $progs

    install -d "$DESTDIR/sbin" "$DESTDIR/bin"
    install -m 755 runit runit-init "$DESTDIR/sbin/"
    install -m 755 chpst runsv runsvchdir runsvdir sv "$DESTDIR/bin/"
}
