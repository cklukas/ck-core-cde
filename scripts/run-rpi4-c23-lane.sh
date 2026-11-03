#!/bin/sh

set -eu

if test "$#" -ne 3; then
    echo "usage: $0 CC CXX LANE" >&2
    exit 2
fi

cc=$1
cxx=$2
lane=$3

case $lane in
    ''|*[!A-Za-z0-9_-]*)
        echo "invalid lane name: $lane" >&2
        exit 2
        ;;
esac

: "${TMPDIR:?TMPDIR must name the runner task workspace}"
: "${CKGIT_SISTER_CK_CORE_MOTIF:?missing ck-core-motif sister checkout}"
: "${CKGIT_SISTER_CK_CORE_FS_LIB:?missing ck-core-fs-lib sister checkout}"

case $TMPDIR in
    /*) ;;
    *)
        echo "TMPDIR must be absolute: $TMPDIR" >&2
        exit 2
        ;;
esac

command -v "$cc" >/dev/null
command -v "$cxx" >/dev/null

source_revision=$(git rev-parse HEAD)
motif_revision=$(git -C "$CKGIT_SISTER_CK_CORE_MOTIF" rev-parse HEAD)
filesystem_revision=$(git -C "$CKGIT_SISTER_CK_CORE_FS_LIB" rev-parse HEAD)
test "$motif_revision" = "$(cat .ckgit/motif-revision)"
test "$filesystem_revision" = "$(cat .ckgit/fs-revision)"

root=$TMPDIR/ck-core-cde-c23-$source_revision-$lane
case $root in
    "$TMPDIR"/ck-core-cde-c23-*) ;;
    *)
        echo "refusing unsafe build root: $root" >&2
        exit 2
        ;;
esac

rm -rf "$root"
mkdir -p "$root"

started_epoch=$(date +%s)
started_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
strict_cflags='-O1 -g0 -std=c23 -Werror=strict-prototypes -Werror=old-style-definition -Werror=cast-function-type -Werror=incompatible-pointer-types -Werror=implicit-function-declaration'

echo "CDE source revision: $source_revision"
echo "CK Core Motif revision: $motif_revision"
echo "CK Core filesystem revision: $filesystem_revision"
echo "C23 lane: $lane ($cc / $cxx)"
echo "Build root: $root"
echo "Build started: $started_utc"

xinerama_probe=$root/cde-xinerama-probe
printf '%s\n' \
    'char XineramaQueryScreens(void);' \
    'int main(void) { return XineramaQueryScreens(); }' \
    > "$xinerama_probe.c"
"$cc" "$xinerama_probe.c" -lXinerama -o "$xinerama_probe"

grep -q '^#define XmABI_VERSION 9$' \
    "$CKGIT_SISTER_CK_CORE_MOTIF/lib/Xm/Xm.h.in"
grep -q 'XmToolkitABI' cde/configure.ac

motif_source=$root/motif-source
motif_build=$root/motif-build
motif_prefix=$root/motif-prefix
cde_source=$root/cde-source
cde_build=$root/cde-build
mkdir -p "$motif_source" "$motif_build" "$motif_prefix" \
    "$cde_source" "$cde_build"

git -C "$CKGIT_SISTER_CK_CORE_MOTIF" archive HEAD | tar -x -C "$motif_source"
git archive HEAD | tar -x -C "$cde_source"

(cd "$motif_source" && autoreconf -fi)
(cd "$motif_build" && "$motif_source/configure" \
    --prefix="$motif_prefix" --disable-demos --disable-static \
    --enable-xft --enable-unicode-text --enable-atspi --enable-svg \
    CC="$cc" CXX="$cxx" CFLAGS='-O1 -g0 -std=c23' CXXFLAGS='-O1 -g0')
for component in bindings bitmaps config localized lib include tools/wml clients/uil; do
    make -j4 -C "$motif_build/$component"
    make -j1 -C "$motif_build/$component" install
done
test -e "$motif_prefix/lib/libXm.so.9"
test -x "$motif_prefix/bin/uil"

export PATH="$motif_prefix/bin:$PATH"
export CPPFLAGS="-I$motif_prefix/include $(pkg-config --cflags xft)"
export LDFLAGS="-L$motif_prefix/lib -Wl,-rpath,$motif_prefix/lib"
export LD_LIBRARY_PATH="$motif_prefix/lib"

(cd "$cde_source/cde" && ./autogen.sh)
cd "$cde_build"
if ! "$cde_source/cde/configure" CC="$cc" CXX="$cxx" \
    CFLAGS="$strict_cflags" CXXFLAGS='-O1 -g0'; then
    tail -n 200 config.log
    exit 1
fi
make -j4
make -j4 check
readelf -dW programs/dtwm/.libs/dtwm | grep -F 'libXm.so.9'

fs_prefix=$root/fs-prefix
fs_build=$root/fs-build
private_fixture=$root/private-fixture
mkdir -p "$private_fixture"
export DTFILE_PRIVATE_TEST_ROOT=$private_fixture
cmake -S "$CKGIT_SISTER_CK_CORE_FS_LIB" -B "$fs_build" \
    -DCMAKE_INSTALL_PREFIX="$fs_prefix" -DCMAKE_INSTALL_LIBDIR=lib \
    -DCK_CORE_FS_BUILD_BROWSER=OFF -DBUILD_TESTING=OFF \
    -DCMAKE_CXX_COMPILER="$cxx" -DCMAKE_CXX_FLAGS='-O1 -g0'
cmake --build "$fs_build" --parallel 4
cmake --install "$fs_build"

export LD_LIBRARY_PATH="$motif_prefix/lib:$fs_prefix/lib"
export PKG_CONFIG_PATH="$fs_prefix/lib/pkgconfig"
cde_fs_build=$root/cde-filesystem-build
mkdir -p "$cde_fs_build"
cd "$cde_fs_build"
if ! "$cde_source/cde/configure" --with-ck-core-fs \
    CC="$cc" CXX="$cxx" CFLAGS="$strict_cflags" CXXFLAGS='-O1 -g0'; then
    tail -n 200 config.log
    exit 1
fi
make -j4 -C programs/dtfile
make -j1 -C programs/localized/C/msg dtfile_sync.cat
make -j1 -C programs/dtfile check

finished_epoch=$(date +%s)
finished_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
elapsed_seconds=$((finished_epoch - started_epoch))
dtwm_path=$cde_build/programs/dtwm/.libs/dtwm
dtwm_sha256=$(sha256sum "$dtwm_path" | awk '{print $1}')

{
    printf 'source_revision=%s\n' "$source_revision"
    printf 'motif_revision=%s\n' "$motif_revision"
    printf 'filesystem_revision=%s\n' "$filesystem_revision"
    printf 'lane=%s\n' "$lane"
    printf 'cc=%s\n' "$cc"
    printf 'cxx=%s\n' "$cxx"
    printf 'cflags=%s\n' "$strict_cflags"
    printf 'started_utc=%s\n' "$started_utc"
    printf 'finished_utc=%s\n' "$finished_utc"
    printf 'elapsed_seconds=%s\n' "$elapsed_seconds"
    printf 'dtwm_path=%s\n' "$dtwm_path"
    printf 'dtwm_sha256=%s\n' "$dtwm_sha256"
} > "$root/build-receipt.txt"

cat "$root/build-receipt.txt"
