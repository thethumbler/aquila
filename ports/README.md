# Ports

Optional third-party software, built for AquilaOS from upstream sources.
Third-party software that is part of the base system is in
[contrib/](../contrib/) instead; both are recipes built by
[mk/recipes.sh](../mk/recipes.sh).

```
make ports                  # build the ports that are out of date
mk/recipes.sh ports lua     # build some ports, even up to date ones
ARCH=i386 mk/recipes.sh ports lua   # for i386 (make ports CONFIG=i386-pc for all)
make initrd                 # the initrd includes the ports once built
```

A port is out of date when its recipe or patches, `mk/recipes.sh`, the C
library or a port it depends on (`DEPENDS`, linked statically) changed
since it was built.

Ports are installed into `build/<arch>/ports/root` (`build/ports/root`
for i386), which `make initrd` copies into the root filesystem after the
base system.

## Recipes

A port is `ports/<name>/recipe.sh` (a contrib/ recipe is the same):

```sh
NAME=foo
VERSION=1.0
URL=https://example.org/foo-$VERSION.tar.gz
SHA256=<sha256 of the archive>

build()
{
    make CC="$CC" AR="$AR" RANLIB="$RANLIB"
    install -D -m 755 foo "$DESTDIR/usr/bin/foo"
}
```

Recipes may call `aquila_config_sub` in `build()` to let an autoconf
`configure` accept `--host=$TARGET` (`i686-aquila`, `x86_64-aquila` on
x86-64, `aarch64-aquila`).

`build()` runs in the extracted source directory, after applying any
`<name>/*.patch` (`patch -p1`), with `CC`, `AR`, `RANLIB` and
`STRIP` set for the system's toolchain (`$TARGET`, `$ARCH` is `i386`,
`x86_64` or `aarch64`) and `JOBS` for parallel builds. The archive must extract to `<name>-<version>/`, or to the
directory `ARCHIVE_DIR` names.

A port without a usable release archive comes from git: `GIT` (the
repository) and `COMMIT` (a full commit hash, which pins the contents)
replace `URL` and `SHA256`, and the sources are exported as
`<name>-<version>/`.

Other optional settings: `DEPENDS` (recipes to build first, ports or
contrib/ ones), `ARCHS` (the architectures it builds for, all if
unset), `FILE` (name
of the downloaded archive), `PLAIN=1` (the download is a single file, not
an archive: it's copied into an empty source directory). Libraries and headers other ports build
against go into `$STAGING`, which `CPPFLAGS` and `LDFLAGS` point at,
and `pkg-config` only looks there (`--static`: libraries are static).
Libtool archives (`.la`) are removed from it, they name the build
host's `/usr/lib`.

## Ports

| port       | version  |                                                     |
|------------|----------|-----------------------------------------------------|
| bash       | 5.3.20   | `bash` in `/bin` (5.3 and its official patches to 20): its own readline over ncurses, the C library's malloc; process substitution through named pipes (there is no `/dev/fd`) |
| ca-certificates | 2026-10-20 | `/etc/ssl/certs/ca-certificates.crt` (also `/etc/ssl/cert.pem`): the CA certificates Mozilla trusts, as curl extracts them |
| curl       | 8.23.0   | `curl` in `/usr/bin`: HTTP, HTTPS (TLS 1.2 and 1.3 through Mbed TLS, servers verified with ca-certificates), FTP and more; gzip and deflate; names resolved one at a time; no HTTP/2, IDN or LDAP |
| doomgeneric | e257a09 | `doom` and `xdoom` in `/usr/bin`: Doom on the framebuffer (`/dev/fb0`, 4 times over at 1280x800, Limine's mode; the keyboard of the terminal in front, in `K_MEDIUMRAW`), and in an X window (twice over); Freedoom's IWAD, or another in `/usr/share/games/doom` or given with `-iwad`. No sound |
| dmenu      | 5.3      | `dmenu` and `dmenu_run` in `/usr/bin`, the menu of dwm (Alt+p) |
| dwm        | 6.5      | `dwm` in `/usr/bin`, a tiling window manager (its default configuration: Alt is the modifier, Alt+Shift+Enter opens st) |
| expat      | 2.6.4    | library only (XML, for fontconfig)                  |
| freedoom   | 0.13.0   | `/usr/share/games/doom/freedoom1.wad` (28 MB): Freedoom: Phase 1, free (BSD) game data for Doom |
| font-cursor-misc | 1.0.4 | the cursor font, for X servers                 |
| font-dejavu | 2.37    | DejaVu Sans and Sans Mono (regular and bold, TrueType) in `/usr/share/fonts/dejavu`, the fonts of Xft programs |
| font-misc-misc | 1.1.3  | the fixed width fonts of X (`fixed`, `6x13`, `9x15`, ...) in `/usr/share/fonts/X11/misc` (BDF, there is no `bdftopcf`): Unicode, and ISO 8859-1 (made by the recipe) which the usual names are |
| fontconfig | 2.15.0   | library, `/etc/fonts`, and `fc-cache`, `fc-list` and `fc-match` in `/usr/bin` |
| freetype   | 2.13.3   | library only (the font rasterizer)                  |
| kilo       | 323d93b  | `kilo` in `/usr/bin`                                |
| libfontenc | 1.1.8    | library only (for libXfont)                         |
| libice     | 1.1.2    | library only (for libSM)                            |
| libpthread-stubs | 0.5 | `pthread-stubs.pc` only (the pthread functions are in the C library), for libxcb |
| libsm      | 1.2.6    | library only (session management, for Xt)           |
| libtermkey | 0.22     | library only (for vis)                              |
| libx11     | 1.8.12   | library only (Xlib), and its data in `/usr/share/X11` (locales, error messages) |
| libxau     | 1.0.12   | library only (X authorization)                      |
| libxaw     | 1.0.16   | library only (Athena widgets)                       |
| libxcb     | 1.17.0   | library only (the X protocol, under Xlib)           |
| libxdmcp   | 1.1.5    | library only (for TinyX)                            |
| libxext    | 1.3.6    | library only (X extensions)                         |
| libxfont   | 1.5.4    | library only, bitmap fonts (for TinyX)              |
| libxft     | 2.3.8    | library only (text through FreeType and fontconfig) |
| libxinerama | 1.1.5   | library only (for dwm)                              |
| libxmu     | 1.2.1    | library only (miscellaneous utilities of X clients) |
| libxpm     | 3.5.17   | library only (X pixmaps)                            |
| libxrender | 0.9.12   | library only (RENDER, for Xft)                      |
| libxt      | 1.3.1    | library only (the X Toolkit Intrinsics)             |
| lua        | 5.5.1    | `lua` and `luac` in `/usr/bin`                      |
| mbedtls    | 3.6.7    | library only (TLS and its cryptography, for curl): the kernel's `/dev/urandom` for entropy; without its own sockets and timers |
| microwindows | 969eae6 | `microwindows` in `/usr/bin`: Nano-X, a small windowing system on the framebuffer, and its demos (Nuklear among them) in `/usr/lib/microwindows`; `microwindows` opens the demos' launcher, `microwindows nxterm` a terminal. The mouse is `/dev/mouse`, the keyboard the terminal it runs in, in the foreground. x86 only. MPL 1.1 |
| ncurses    | 6.6      | library only, wide characters, the vt100, ansi, linux, xterm and xterm-256color descriptions built in (no terminfo database) |
| st         | 0.9.2    | `st` in `/usr/bin`, the terminal of dwm (Xft fonts, TERM=xterm-256color) |
| tcc        | mob e23dabe | `tcc` (also `cc`) in `/usr/bin`, compiling static programs on AquilaOS; with the C library's headers in `/usr/include` and archives in `/usr/lib`; no `-run` (no `mprotect()`) |
| tinyx      | 1.2.0 (feab72c) | `Xfbdev` (also `X`) and `startx` (X and twm with an xterm, or `~/.xinitrc`) in `/usr/bin`: X on the framebuffer, the keyboard and mouse of a virtual terminal; clients through a Unix socket (`:0`). Its changes to X.Org's code are GPLv3 |
| unixbench  | 6.0.1    | UnixBench's index tests in `/usr/lib/unixbench`, and `ubench` in `/usr/bin` (its own `ubench.c`): what UnixBench's `Run` does for the index, without Perl; `ubench -c 4` runs 4 copies of each test at once. No graphics tests. GPLv2 |
| twm        | 1.0.13.1 | `twm` in `/usr/bin`, the window manager of X        |
| vis        | 0.9      | `vis` in `/usr/bin`, without Lua (no syntax highlighting) |
| xcb-proto  | 1.17.0   | for libxcb: the protocol in XML, generated on the build host |
| xorgproto  | 2024.1   | headers only (X protocol)                           |
| xterm      | 401      | `xterm` in `/usr/bin`, the terminal emulator of X (bitmap fonts, no setuid, utmp or luit) |
| xtrans     | 1.5.2    | headers only (X transports)                         |
| zlib       | 1.3.1    | library only                                        |
