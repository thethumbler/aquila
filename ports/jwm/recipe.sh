# JWM, Joe's Window Manager, https://joewing.net/projects/jwm/: title
# bars, a taskbar with a menu, the windows and a clock, and a wallpaper,
# in C on Xlib. AquilaOS's desktop: system.jwmrc, its themes in the
# palette of aquilaos.com (dark and light, aquila-theme switches), and
# the wallpaper (wallpaper.jpg, stretched to the screen), are in
# ports/jwm/.
#
# 2.3.7, the last of 2.3: from 2.4 on, JWM draws antialiased text only
# through Pango (glib, and HarfBuzz, in C++), and with core X fonts
# without it; 2.3 draws it with Xft. (aquila.patch: its configuration
# mapped MAP_PRIVATE; JPEG images decoded at a scale too small)

NAME=jwm
VERSION=2.3.7
URL=https://joewing.net/projects/jwm/releases/jwm-$VERSION.tar.xz
SHA256=745d2f5df3a4ce9ecc8e75445edb3fbe31dd46c0c26d129cc14de7ad8b665326
DEPENDS="libx11 libxext libxft libxrender libxpm libjpeg libxmu libxinerama font-ibm-plex-sans st"

build()
{
    aquila_config_sub

    # text through Xft, images in XPM and JPEG (no PNG, SVG or Cairo), no
    # translations; the libraries are static, what they need comes after
    # them (configure's checks link with LIBS)
    local libs="$($PKG_CONFIG --libs xft xrender xpm libjpeg xmu xinerama xext x11)"
    LIBS="$libs" ./configure --host=$TARGET --prefix=/usr --sysconfdir=/etc \
        --disable-png --disable-cairo --disable-rsvg \
        --disable-fribidi --disable-nls
    # jwm itself links with LDFLAGS alone, which configure fills in an
    # order that only suits shared libraries
    make -j"$JOBS" LDFLAGS="$LDFLAGS $libs"
    install -D -m 755 src/jwm "$DESTDIR/usr/bin/jwm"
    $STRIP "$DESTDIR/usr/bin/jwm"

    local port=$ROOT/ports/jwm share=$DESTDIR/usr/share/aquila
    install -D -m 644 "$port/system.jwmrc" "$DESTDIR/etc/system.jwmrc"
    install -D -m 755 "$port/aquila-theme" "$DESTDIR/usr/bin/aquila-theme"
    install -d "$share"

    # aquilaos.com's palette (sass/style.scss): paper, ink, text, muted,
    # rule, accent, fill, fill-strong; dark, then light
    theme dark  '#191917' '#e8e5de' '#cfccc4' '#9a978f' '#383631' '#d19a6c' '#232120' '#302b26'
    theme light '#fcfcfa' '#2a2824' '#3d3b35' '#6f6d66' '#dddbd3' '#a2643a' '#f4f1ec' '#ebe3d8'

    # the wallpaper, both themes'
    install -m 644 "$port/wallpaper.jpg" "$share/wallpaper.jpg"

    # the menu's icon: the logo in the accent, on nothing (None: the
    # background shows through)
    "$port/logo-xpm" aquila 24 24 3 None '#d19a6c' > "$share/aquila.xpm"
}

# a theme: its JWM style
theme()
{
    sed -e "s/@theme@/$1/g" -e "s/@paper@/$2/g" -e "s/@ink@/$3/g" \
        -e "s/@text@/$4/g" -e "s/@muted@/$5/g" -e "s/@rule@/$6/g" \
        -e "s/@accent@/$7/g" -e "s/@fill@/$8/g" -e "s/@fill_strong@/$9/g" \
        "$port/theme.jwmrc.in" > "$share/theme-$1.jwmrc"
}
