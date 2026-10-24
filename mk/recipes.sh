#!/bin/bash
#
# Build third-party software for AquilaOS from upstream sources.
#
# usage: mk/recipes.sh contrib|ports [name...]   (default: all of the tree)
#
# contrib/ is the third-party part of the base system (make), ports/ is
# optional software (make ports). Both use recipes: <tree>/<name>/recipe.sh
# sets NAME, VERSION, URL and SHA256 and defines a build() function, and
# optionally:
#
#   DEPENDS     recipes to build first (of the same tree, or of contrib)
#   FILE        name of the downloaded archive (default: basename of URL)
#   ARCHIVE_DIR directory the archive extracts to (default: NAME-VERSION)
#   PLAIN       1: the download is a file, not an archive (it's put in an
#               empty NAME-VERSION directory)
#   ARCHS       the architectures it builds for (default: all), it is
#               skipped on the others
#
# Sources that have no usable release archive come from git instead: GIT
# (repository) and COMMIT (full hash, which pins the contents) replace URL
# and SHA256.
#
# build() runs in the extracted source directory (after applying
# <tree>/<name>/*.patch) with CC, AR, RANLIB, STRIP set for the system's
# toolchain ($TARGET: i686-aquila, x86_64-aquila on x86_64, $ARCH), and
# AQUILA_UAPI pointing at the kernel ABI headers (for host
# tools that generate target tables). Programs are installed into
# $DESTDIR (build/<tree>/root), which the initrd includes (make initrd);
# libraries and headers other recipes build against into $STAGING
# (CPPFLAGS and LDFLAGS point there, and pkg-config only looks there).
#
# environment:
#   BUILDDIR    output directory (default: <repo>/build/x86_64, <repo>/build
#               for i386)
#   TOOLCHAIN   the cross toolchain (default: <repo>/build/toolchain)
#   ARCH        x86_64 (default) or i386, TARGET its toolchain's prefix
#   DOWNLOADS   source archives (default: <repo>/build/downloads)
#   JOBS        parallel jobs (default: nproc)
#

set -euo pipefail

# don't inherit the kernel settings exported by make, nor its
# configuration's name (CONFIG), which some builds (Microwindows) use too
unset CFLAGS CXXFLAGS CPPFLAGS ASFLAGS LDFLAGS LDLIBS LD AS CONFIG

ROOT=$(cd "$(dirname "$0")/.." && pwd)

TREE=${1:-}
case "$TREE" in
    contrib|ports) shift ;;
    *) echo "usage: $0 contrib|ports [name...]" >&2; exit 2 ;;
esac

# the architecture (make passes its configuration's)
export ARCH=${ARCH:-x86_64}

case $ARCH in
    i386)   export TARGET=${TARGET:-i686-aquila}; BUILDDIR=${BUILDDIR:-$ROOT/build} ;;
    *)      export TARGET=${TARGET:-$ARCH-aquila}; BUILDDIR=${BUILDDIR:-$ROOT/build/$ARCH} ;;
esac

TOOLCHAIN=${TOOLCHAIN:-$ROOT/build/toolchain}
JOBS=${JOBS:-$(nproc)}
DOWNLOADS=${DOWNLOADS:-$ROOT/build/downloads}
export STAGING=$BUILDDIR/staging

export PATH=$TOOLCHAIN/bin:$PATH
export CC=$TARGET-gcc AR=$TARGET-ar RANLIB=$TARGET-ranlib STRIP=$TARGET-strip
export JOBS
export AQUILA_UAPI=$ROOT/include/uapi
export CPPFLAGS="-I$STAGING/usr/include" LDFLAGS="-L$STAGING/usr/lib"

# pkg-config finds what recipes installed into $STAGING, not the host's
export PKG_CONFIG_LIBDIR=$STAGING/usr/lib/pkgconfig:$STAGING/usr/share/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=$STAGING PKG_CONFIG_PATH=

# libraries are static: what they link with (Requires.private, Libs.private)
export PKG_CONFIG="pkg-config --static"

if ! command -v $CC >/dev/null; then
    echo "error: $CC not found, run 'make toolchain' first" >&2
    exit 1
fi

# recipes are rebuilt when libc changed
LIBC=$($CC -print-sysroot)/usr/lib/libc.a

declare -A built

# for build(): autoconf's config.sub doesn't know aquila, wrap each one in
# the source tree to answer for *-aquila* and ask the original otherwise
aquila_config_sub()
{
    local f

    find . -name config.sub | while read -r f; do
        mv "$f" "$f.orig"
        cat > "$f" <<'EOF'
#!/bin/sh
case "$1" in
    *-aquila*) echo "${1%%-*}-pc-aquila" ;;
    *) exec sh "$(dirname "$0")/config.sub.orig" "$@" ;;
esac
EOF
        chmod +x "$f"
    done
}

# the tree a recipe is in: the one being built, or contrib
recipe_tree()
{
    local tree
    for tree in "$TREE" contrib; do
        if [ -f "$ROOT/$tree/$1/recipe.sh" ]; then
            echo "$tree"
            return
        fi
    done
}

# build_port <name> [dep]: a dependency (dep set) that was built since its
# recipe and libc last changed is not built again
build_port()
{
    local tree
    tree=$(recipe_tree "$1")
    local recipe=$ROOT/$tree/$1/recipe.sh
    local SRC=$BUILDDIR/$tree/src

    [ -n "${built[$1]:-}" ] && return

    if [ ! -f "$recipe" ]; then
        echo "unknown recipe: $1 (not in $TREE/ or contrib/)" >&2
        exit 2
    fi

    local stamp=$BUILDDIR/$tree/$1.built

    if [ -n "${2:-}" ] && [ -f "$stamp" ] &&
       [ -z "$(find "$ROOT/$tree/$1" "$LIBC" -newer "$stamp" 2>/dev/null)" ]; then
        built[$1]=1
        return
    fi

    # not for this architecture
    local archs
    archs=$(ARCHS=; source "$recipe"; echo "$ARCHS")

    if [ -n "$archs" ] && [[ " $archs " != *" $ARCH "* ]]; then
        echo "==> $1 is not built for $ARCH"
        built[$1]=1
        return
    fi

    # dependencies first
    local dep
    for dep in $(DEPENDS=; source "$recipe"; echo "$DEPENDS"); do
        build_port "$dep" dep
    done

    (
        # optional settings start unset
        unset ARCHIVE_DIR FILE PLAIN

        # shellcheck source=/dev/null
        source "$recipe"

        export DESTDIR=$BUILDDIR/$tree/root
        mkdir -p "$DOWNLOADS" "$SRC" "$DESTDIR" "$STAGING"

        if [ -n "${GIT:-}" ]; then
            local archive=$DOWNLOADS/$NAME-$COMMIT.tar
            local repo=$DOWNLOADS/$NAME.git

            # a branch or tag name could change what is built
            if ! [[ $COMMIT =~ ^[0-9a-f]{40}$ ]]; then
                echo "$NAME: COMMIT must be a full commit hash" >&2
                exit 1
            fi

            if [ ! -f "$archive" ]; then
                echo "==> downloading $NAME-$VERSION"
                [ -d "$repo" ] || git clone -q --bare "$GIT" "$repo"
                git -C "$repo" cat-file -e "$COMMIT^{commit}" 2>/dev/null ||
                    git -C "$repo" fetch -q origin
                git -C "$repo" archive --prefix="$NAME-$VERSION/" -o "$archive.part" "$COMMIT"
                mv "$archive.part" "$archive"
            fi
        else
            local archive=$DOWNLOADS/${FILE:-$(basename "$URL")}

            if [ ! -f "$archive" ]; then
                echo "==> downloading $NAME-$VERSION"
                wget -q -O "$archive.part" "$URL"
                mv "$archive.part" "$archive"
            fi

            echo "$SHA256  $archive" | sha256sum -c --quiet -
        fi

        echo "==> building $NAME-$VERSION"
        local srcdir=${ARCHIVE_DIR:-$NAME-$VERSION}
        rm -rf "${SRC:?}/$srcdir"

        if [ "${PLAIN:-0}" = 1 ]; then
            mkdir -p "$SRC/$srcdir"
            cp "$archive" "$SRC/$srcdir/"
        else
            tar -xf "$archive" -C "$SRC"
        fi

        cd "$SRC/$srcdir"

        for p in "$ROOT/$tree/$1"/*.patch; do
            [ -f "$p" ] && patch -p1 -s < "$p"
        done

        build

        # libtool archives name /usr/lib, the build host's: libraries are
        # found without them
        find "$STAGING" -name '*.la' -delete
    )

    touch "$stamp"
    built[$1]=1
}

for port in ${*:-$(cd "$ROOT/$TREE" && ls -d */ | tr -d /)}; do
    build_port "$port"
done
