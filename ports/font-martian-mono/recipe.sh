# Martian Mono, TrueType (standard width: regular, medium and bold): the
# terminal's font (st), as on aquilaos.com,
# https://github.com/evilmartians/mono (SIL Open Font License 1.1: the
# release has no copy of it, OFL.txt is the repository's at v1.1.0)

NAME=font-martian-mono
VERSION=1.1.0
URL=https://github.com/evilmartians/mono/releases/download/v$VERSION/martian-mono-$VERSION-ttf.zip
SHA256=483aa2d201a6c44610c0bdcf65eaf425b467d61e9ab606213eacef3e216c2a12
# a zip, which tar doesn't take: unzipped here
PLAIN=1

build()
{
    local f dir=/usr/share/fonts/martian-mono
    unzip -q martian-mono-$VERSION-ttf.zip -x '__MACOSX/*'
    install -d "$DESTDIR$dir"
    for f in StdRg StdMd StdBd; do
        install -m 644 "MartianMono-$f.ttf" "$DESTDIR$dir/"
    done
    install -m 644 "$ROOT/ports/font-martian-mono/OFL.txt" "$DESTDIR$dir/OFL.txt"
}
