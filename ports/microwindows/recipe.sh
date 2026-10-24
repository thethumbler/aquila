# Microwindows and Nano-X, a small windowing system on the framebuffer,
# https://github.com/ghaerr/microwindows (no releases: pinned to a commit)
#
# Upstream has an AquilaOS target (Configs/config.aquila: the framebuffer,
# /dev/mouse, the keyboard through the terminal). aquila.patch:
# - reads the framebuffer's mode with Linux's ioctls (<sys/fb.h>), which
#   upstream now does on Linux only (scr_fb.c);
# - builds the terminal emulator (nxterm, which AquilaOS's ptys can run);
# - skips the C++ demos (there's no C++ compiler);
# - calls time() with a time_t pointer in nxroach (i386's is 64 bits).
# Everything goes in /usr/lib/microwindows, as upstream runs it from its
# own directory: the demos find their files as bin/...; `microwindows`
# starts it there. The framebuffer is x86's (VESA) only.

NAME=microwindows
VERSION=969eae602a8a5a022ea978da4016f97409d4421e
GIT=https://github.com/ghaerr/microwindows.git
COMMIT=969eae602a8a5a022ea978da4016f97409d4421e
ARCHS="x86_64 i386"

build()
{
    cd src
    cp Configs/config.aquila config
    # its configuration, not AquilaOS's (make ports CONFIG=... passes that
    # on in MAKEFLAGS)
    make CONFIG="$PWD/config" TOOLSPREFIX=$TARGET- -j"$JOBS"

    local dir=$DESTDIR/usr/lib/microwindows

    install -d "$dir/bin"
    cp bin/* "$dir/bin/"

    # strip the programs, not the images and configuration files
    for f in "$dir"/bin/*; do
        if head -c 4 "$f" | grep -q ELF; then
            $STRIP "$f"
        fi
    done

    install -d "$DESTDIR/usr/bin"
    cat > "$DESTDIR/usr/bin/microwindows" <<'SH'
#!/bin/sh
# Microwindows: the demos' launcher (nxstart), or the given program
# (microwindows nxterm), from its directory; a client starts the nano-X
# server (bin/nano-X) if none runs. The keyboard is this terminal's, so it
# runs in the foreground (in the background, SIGTTOU stops it)
cd /usr/lib/microwindows || exit 1
prog=${1:-nxstart}
[ $# -gt 0 ] && shift
exec "bin/$prog" "$@"
SH
    chmod 755 "$DESTDIR/usr/bin/microwindows"
}
