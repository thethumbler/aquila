# TinyX, the small X servers of Tiny Core Linux (kdrive, from X.Org's
# xserver 1.2), https://github.com/tinycorelinux/tinyx
# (from git: there are no releases; its changes to X.Org's code are GPLv3)
#
# Xfbdev draws on /dev/fb0 and takes the keyboard and mouse of a virtual
# terminal, with the console ioctls of Linux (KD*, VT_*), which the kernel
# has. aquila.patch builds its Linux code for AquilaOS, and fixes the
# PS/2 mouse's wheel setup (its replies were read as a right click).

NAME=tinyx
VERSION=1.2.0
GIT=https://github.com/tinycorelinux/tinyx.git
COMMIT=feab72ca891bc04b18763763e15ee4e532369cdf
DEPENDS="xorgproto xtrans libxau libxdmcp libfontenc libxfont"

build()
{
    # Linux's headers of the console and framebuffer are <sys/...> here,
    # with Linux's integer types
    mkdir -p aquila/linux
    cat > aquila/linux/types.h <<'EOF'
#include <stdint.h>
typedef uint8_t __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef int16_t __s16;
typedef int32_t __s32;
EOF
    for h in fb kd vt keyboard; do
        printf '#include <linux/types.h>\n#include <sys/%s.h>\n' $h > "aquila/linux/$h.h"
    done

    # xtrans.m4 is xtrans's (in $STAGING)
    autoreconf -i -I "$STAGING/usr/share/aclocal"
    aquila_config_sub

    # no Xvesa (it runs the video BIOS in vm86 mode), no XDMCP, no DPMS
    # (the framebuffer has no power modes), no BigFont (fonts through shared
    # memory, there is none); clients connect through a Unix
    # socket (TCP is built, it doesn't build without, but the kernel has no
    # TCP/IP yet)
    # C17 and -fpermissive: the code is old (old-style definitions, which
    # C23 doesn't have, pointer types that GCC 14 doesn't take anymore)
    # (configure turns on link-time optimization: archives of LTO objects
    # need gcc-ar)
    AR=$TARGET-gcc-ar RANLIB=$TARGET-gcc-ranlib \
    CFLAGS="-O2 -std=gnu17 -fpermissive" CPPFLAGS="-I$PWD/aquila $CPPFLAGS" ./configure --host=$TARGET --prefix=/usr \
        --disable-xvesa --enable-xfbdev --disable-xdmcp --disable-xdm-auth-1 --disable-dpms \
        --disable-xf86bigfont \
        --disable-ipv6 --disable-local-transport \
        --with-fontdir=/usr/share/fonts/X11 \
        --with-default-font-path=/usr/share/fonts/X11/misc
    make -j"$JOBS"

    install -D -m 755 kdrive/fbdev/Xfbdev "$DESTDIR/usr/bin/Xfbdev"
    $STRIP "$DESTDIR/usr/bin/Xfbdev"
    ln -sf Xfbdev "$DESTDIR/usr/bin/X"

    # X and a session (~/.xinitrc, the desktop, or twm and an xterm)
    install -m 755 "$ROOT/ports/tinyx/startx" "$DESTDIR/usr/bin/startx"
}
