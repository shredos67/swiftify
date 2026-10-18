#!/usr/bin/env bash
# builds a self contained linux x86_64 tarball of swiftify, with qt and its
# libraries bundled, against a qt kit from the qt installer or aqtinstall
#
# usage
#   ./tools/build-linux-tarball.sh --qt ~/Qt/6.12.0/gcc_64
#   ./tools/build-linux-tarball.sh --qt ~/Qt/6.12.0/gcc_64 --out dist --jobs 8
#
# needs cmake, ninja, git, curl, a c++20 compiler (CC and CXX are used when
# set), rust from rustup, and the dev packages for libpulse, alsa, openssl and
# libsecret. the tarball still needs alsa, libsecret, libpulse, glib, dbus and
# systemd's libraries on the machine it runs on, every desktop has them

set -euo pipefail

QT=""
OUT="dist"
JOBS="$(nproc 2>/dev/null || echo 4)"

while [[ $# -gt 0 ]]; do
    case $1 in
        --qt) QT="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) echo "unknown option $1" >&2; exit 1 ;;
    esac
done

fail() { echo "error, $1" >&2; exit 1; }

[[ "$(uname -s)" == Linux && "$(uname -m)" == x86_64 ]] || fail "this builds linux x86_64 only"
[[ -n "$QT" ]] || fail "pass --qt with the path of a qt 6.9 or newer gcc_64 kit"
QT="$(cd "$QT" && pwd)"
[[ -x "$QT/bin/qmake" ]] || fail "no qmake in $QT/bin, pass the gcc_64 folder of the kit"
for tool in cmake ninja git curl cargo; do
    command -v "$tool" >/dev/null || fail "$tool is missing"
done

SOURCE="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(sed -n '/^project(Swiftify/,/VERSION/s/^ *VERSION \([0-9.]*\).*/\1/p' "$SOURCE/CMakeLists.txt")"
[[ -n "$VERSION" ]] || fail "could not read the version from CMakeLists.txt"
NAME="swiftify-$VERSION-linux-x86_64"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
WORK="$SOURCE/build-linux-tarball"
DEPS="$WORK/deps"
APPDIR="$WORK/AppDir"
mkdir -p "$WORK/sources" "$WORK/tools"

echo "qt $QT"
echo "version $VERSION"
echo "output $OUT/$NAME.tar.gz"

# pinned, and checked against these hashes before they run
LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_SHA256="c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d"
PLUGIN_QT_URL="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-qt-x86_64.AppImage"
PLUGIN_QT_SHA256="15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724"

fetch_tool() {
    local url="$1" sha256="$2" file
    file="$WORK/tools/$(basename "$1")"
    if [[ ! -f "$file" ]] || ! echo "$sha256  $file" | sha256sum --check --status; then
        curl -sSfL --retry 3 -o "$file" "$url"
    fi
    echo "$sha256  $file" | sha256sum --check --status || fail "$(basename "$file") does not match its pinned hash"
    chmod +x "$file"
}
fetch_tool "$LINUXDEPLOY_URL" "$LINUXDEPLOY_SHA256"
fetch_tool "$PLUGIN_QT_URL" "$PLUGIN_QT_SHA256"

common=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF "-DCMAKE_PREFIX_PATH=$QT;$DEPS")
[[ -n "${CC:-}" ]] && common+=("-DCMAKE_C_COMPILER=$CC")
[[ -n "${CXX:-}" ]] && common+=("-DCMAKE_CXX_COMPILER=$CXX")

# qtkeychain and qcoro are built against the same qt, at tags known to work
build_dependency() {
    local name="$1" url="$2" tag="$3"
    shift 3
    if [[ ! -d "$WORK/sources/$name" ]]; then
        git clone --quiet --depth 1 --branch "$tag" "$url" "$WORK/sources/$name"
    fi
    cmake -S "$WORK/sources/$name" -B "$WORK/$name" "${common[@]}" \
        "-DCMAKE_INSTALL_PREFIX=$DEPS" -DCMAKE_INSTALL_LIBDIR=lib "$@"
    cmake --build "$WORK/$name" --parallel "$JOBS"
    cmake --install "$WORK/$name"
}
build_dependency qtkeychain https://github.com/frankosterfeld/qtkeychain.git 0.17.0 \
    -DBUILD_WITH_QT5=OFF -DBUILD_WITH_QT6=ON -DBUILD_TEST_APPLICATION=OFF -DBUILD_TRANSLATIONS=OFF
build_dependency qcoro https://github.com/qcoro/qcoro.git v0.13.0 \
    -DUSE_QT_VERSION=6 -DQCORO_WITH_QTDBUS=OFF -DQCORO_WITH_QTWEBSOCKETS=OFF -DQCORO_WITH_QTQUICK=OFF \
    -DQCORO_WITH_QML=OFF -DQCORO_WITH_QTTEST=OFF -DQCORO_BUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF

cmake -S "$SOURCE" -B "$WORK/swiftify" "${common[@]}" -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$WORK/swiftify" --parallel "$JOBS"
rm -rf "${APPDIR:?}"
DESTDIR="$APPDIR" cmake --install "$WORK/swiftify"

# linuxdeploy only brings x11 by itself. wayland needs its platform plugin and
# the shell, decoration and graphics plugins beside it, or it falls back to
# xwayland
mkdir -p "$APPDIR/usr/plugins"
wayland=()
for folder in wayland-shell-integration wayland-decoration-client wayland-graphics-integration-client; do
    if [[ -d "$QT/plugins/$folder" ]]; then
        cp -r "$QT/plugins/$folder" "$APPDIR/usr/plugins/"
        wayland+=(--deploy-deps-only "$APPDIR/usr/plugins/$folder")
    fi
done

export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE="$QT/bin/qmake"
export QML_SOURCES_PATHS="$SOURCE/qml"
export EXTRA_PLATFORM_PLUGINS="libqwayland.so"
export LD_LIBRARY_PATH="$DEPS/lib:$QT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# the linuxdeploy plugin is found next to linuxdeploy, by its name.
# qtkeychain is named outright, linuxdeploy's own pass does not look in
# LD_LIBRARY_PATH and drops a library it cannot find without a word
PATH="$WORK/tools:$PATH" "$WORK/tools/linuxdeploy-x86_64.AppImage" \
    --appdir "$APPDIR" --executable "$APPDIR/usr/bin/swiftify" \
    --library "$DEPS/lib/libqt6keychain.so.1" "${wayland[@]}" --plugin qt

# the tarball is the usr tree: bin, lib, plugins, qml and share. the fonts are
# found from bin/../share and qt from bin/qt.conf, so it runs from anywhere
[[ -f "$APPDIR/usr/bin/qt.conf" ]] || fail "linuxdeploy wrote no qt.conf"
[[ -f "$APPDIR/usr/plugins/platforms/libqwayland.so" ]] || fail "the wayland platform plugin is missing"
[[ -f "$APPDIR/usr/plugins/tls/libqopensslbackend.so" ]] || fail "the openssl tls plugin is missing"

# what the desktop itself is built on comes from the machine, never the
# build's ubuntu. an old libsystemd or glib in lib/ is found first by the
# machine's own libraries too: libsecret then fails to load and no login is
# kept, and two copies of a library in one process can corrupt its heap
system_libraries=(
    'libsystemd.so*' 'libglib-2.0.so*' 'libgthread-2.0.so*' 'libgio-2.0.so*' 'libgobject-2.0.so*'
    'libdbus-1.so*' 'libgcrypt.so*' 'libapparmor.so*' 'libcap.so*' 'libffi.so*' 'libpcre.so*'
    'liblz4.so*' 'liblzma.so*' 'libzstd.so*' 'libbsd.so*' 'libmd.so*' 'libpng16.so*' 'libbrotli*.so*'
    'libpulse*.so*' 'libasyncns.so*' 'libsndfile.so*' 'libFLAC.so*' 'libogg.so*' 'libopus.so*' 'libvorbis*.so*'
    'libgssapi_krb5.so*' 'libkrb5*.so*' 'libk5crypto.so*' 'libkeyutils.so*'
    'libwayland-*.so*' 'libxkbcommon*.so*' 'libXau.so*' 'libXdmcp.so*'
)
for pattern in "${system_libraries[@]}"; do
    find "$APPDIR/usr/lib" -maxdepth 1 -name "$pattern" -delete
done

rm -rf "${WORK:?}/${NAME:?}"
cp -a "$APPDIR/usr" "$WORK/$NAME"

# everything in it has to find its libraries in the tarball or the system,
# with none of the build's own paths
missing=""
while IFS= read -r -d '' file; do
    if file "$file" | grep -q ELF; then
        unresolved="$(env -u LD_LIBRARY_PATH ldd "$file" 2>/dev/null | grep "not found" || true)"
        [[ -z "$unresolved" ]] || missing+="${file#"$WORK/"}"$'\n'"$unresolved"$'\n'
    fi
done < <(find "$WORK/$NAME" -type f \( -path '*/bin/*' -o -name '*.so*' \) -print0)
[[ -z "$missing" ]] || fail "libraries missing from the tarball"$'\n'"$missing"
tar -C "$WORK" -czf "$OUT/$NAME.tar.gz" "$NAME"
echo "built $OUT/$NAME.tar.gz"
