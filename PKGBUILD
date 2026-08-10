# Maintainer: Will C.
#
# VCS-tracked package: pulls the latest commit from main on every makepkg.
# pkgver() derives the version automatically from CMakeLists.txt's
# `project(ouroboros VERSION X.Y.Z)` line, so bumping the version in the
# build system is the only source of truth. No git tags required, no
# sha256 updates, no pkgver edits in this file. Commit, push, makepkg -si.

# Upstream project name; pkgname is suffixed `-git` per Arch VCS guidelines
# so this can coexist with a future stable `ouroboros` AUR package via
# provides/conflicts. Internal references (srcdir, source array, install
# paths under /usr/share/{licenses,doc}/) all use $_pkgname so the layout
# matches what the stable package would produce.
_pkgname=ouroboros
pkgname="${_pkgname}-git"

# Auto-relocate BUILDDIR off any path containing spaces. CMake's try_compile
# and makepkg's CFLAGS expansion (-ffile-prefix-map, -fdiagnostics-color)
# word-split on spaces and fail. By the time this PKGBUILD is sourced, makepkg
# has canonicalized BUILDDIR to the user's explicit value (env or
# ~/.makepkg.conf) or to $startdir as fallback. We mirror makepkg's own `-ef`
# test to detect the fallback case and only override when the fallback path
# also contains spaces -- explicit user overrides are honored unchanged.
if [[ $BUILDDIR -ef "$startdir" && "$startdir" == *" "* ]]; then
    BUILDDIR=/tmp/makepkg-$pkgname
fi

pkgver=4.1.0
pkgrel=1
pkgdesc='Offline, metadata-driven terminal music player for modern Linux (C++23, PipeWire, waterfall spectrogram)'
arch=('x86_64')
url='https://github.com/wllclngn/OUROBOROS'
license=('GPL-2.0-only')
depends=(
    'glibc'
    'gcc-libs'
    'pipewire'
    'mpg123'
    'libsndfile'
    'libvorbis'
    'ffmpeg'
    'icu'
)
makedepends=(
    'git'
    'cmake'
    'gcc'
    'pkgconf'
)

# Coexistence with a future stable AUR `ouroboros` package.
provides=("$_pkgname")
conflicts=("$_pkgname")

# Two sources: OUROBOROS itself, and montauk -- the sort/search core
# (sublimation) lives at montauk/sublimation and is built in-tree. build()
# points the CMake build at $srcdir/montauk via -DMONTAUK_DIR; there is no
# fallback and no build-time toggle.
source=(
    "$_pkgname::git+$url.git"
    "montauk::git+https://github.com/wllclngn/montauk.git"
)
# git branches are dynamic -- checksums change every commit. SKIP is correct
# for a -git VCS package; integrity is the source URLs themselves.
sha256sums=('SKIP'
            'SKIP')

pkgver() {
    cd "$srcdir/$_pkgname"

    # Locals are underscore-prefixed to keep them out of any namespace
    # makepkg internals might touch.
    local _cmake_ver _commits _short

    # Capture X.Y.Z directly from `project(ouroboros VERSION X.Y.Z LANGUAGES ...)`.
    _cmake_ver=$(sed -nE 's/^project\(ouroboros VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' \
                 CMakeLists.txt | head -n1)

    # Append the total commit count + short hash so every push produces a
    # unique sequential Arch version string. Reinstalling unchanged HEAD is
    # a no-op.
    _commits=$(git rev-list --count HEAD)
    _short=$(git rev-parse --short=8 HEAD)
    printf '%s.r%s.g%s' "${_cmake_ver:-0.0.0}" "$_commits" "$_short"
}

build() {
    cd "$srcdir/$_pkgname"

    cmake -B build -S . \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DMONTAUK_DIR="$srcdir/montauk"

    # No -j flag -- makepkg injects MAKEFLAGS from /etc/makepkg.conf, which
    # is where parallelism belongs.
    cmake --build build
}

package() {
    cd "$srcdir/$_pkgname"

    # Binary + manpage.
    install -Dm755 build/ouroboros "$pkgdir/usr/bin/ouroboros"
    install -Dm644 ouroboros.1     "$pkgdir/usr/share/man/man1/ouroboros.1"

    # License + docs under the upstream name so users can `pacman -Ql ouroboros`
    # equivalently whether they installed -git or stable.
    install -Dm644 LICENSE   "$pkgdir/usr/share/licenses/$_pkgname/LICENSE"
    install -Dm644 README.md "$pkgdir/usr/share/doc/$_pkgname/README.md"
}
