# libjpeg, the Independent JPEG Group's JPEG library (for JWM's
# wallpaper), https://www.ijg.org/ (only the library, for other ports:
# installed into $STAGING)

NAME=libjpeg
VERSION=9f
URL=https://www.ijg.org/files/jpegsrc.v$VERSION.tar.gz
SHA256=04705c110cb2469caa79fb71fba3d7bf834914706e9641a4589485c1f832565b
ARCHIVE_DIR=jpeg-$VERSION

build()
{
    aquila_config_sub

    ./configure --host=$TARGET --prefix=/usr --disable-shared
    # the library, its headers (jconfig.h too) and libjpeg.pc; not the
    # programs (cjpeg, djpeg, ...)
    make -j"$JOBS" libjpeg.la libjpeg.pc
    make DESTDIR="$STAGING" install-libLTLIBRARIES install-data-local \
        install-includeHEADERS install-nodist_pkgconfigDATA
}
