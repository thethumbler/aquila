# IBM Plex Sans, TrueType (regular, medium, semibold and bold, and their
# italics): the desktop's font (title bars, menus, the taskbar), as on
# aquilaos.com, https://github.com/IBM/plex (SIL Open Font License 1.1)

NAME=font-ibm-plex-sans
VERSION=1.1.0
URL=https://github.com/IBM/plex/releases/download/%40ibm/plex-sans%40$VERSION/ibm-plex-sans.zip
SHA256=fb365d910566e6d199cc2c15579a7dd9a267128e18431a394ed81f1970c69200
# a zip, which tar doesn't take: unzipped here
PLAIN=1

build()
{
    local f dir=/usr/share/fonts/ibm-plex-sans
    unzip -q ibm-plex-sans.zip
    install -d "$DESTDIR$dir"
    for f in Regular Italic Medium MediumItalic SemiBold SemiBoldItalic Bold BoldItalic; do
        install -m 644 "ibm-plex-sans/fonts/complete/ttf/IBMPlexSans-$f.ttf" "$DESTDIR$dir/"
    done
    install -m 644 ibm-plex-sans/fonts/complete/ttf/license.txt "$DESTDIR$dir/OFL.txt"
}
