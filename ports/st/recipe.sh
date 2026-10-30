# st, the simple terminal of suckless, dwm's, https://st.suckless.org/
# (aquila.patch: <pty.h>; TERM=xterm-256color, which ncurses knows here:
# there is no terminfo database, only the descriptions built in; DejaVu
# Sans Mono and the palette of aquilaos.com, dark, or light with
# AQUILA_THEME=light)

NAME=st
VERSION=0.9.2
URL=https://dl.suckless.org/st/st-$VERSION.tar.gz
SHA256=6b215d4f472b21d6232f30f221117a777e24bcfee68955ddefb7426467f9494b
DEPENDS="libx11 libxft font-dejavu"

build()
{
    # openpty() is in the C library (no -lutil or -lrt)
    make CC="$CC" \
        INCS="$($PKG_CONFIG --cflags x11 xft fontconfig freetype2)" \
        LIBS="-lm $($PKG_CONFIG --libs x11 xft fontconfig freetype2)"
    install -D -m 755 st "$DESTDIR/usr/bin/st"
    $STRIP "$DESTDIR/usr/bin/st"
}
