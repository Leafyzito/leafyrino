#!/bin/sh

set -eu

# Print all commands as they are run
set -x

_QT_DIR=""

if [ -f ./bin/Moltorino7 ] && [ -x ./bin/Moltorino7 ]; then
    app_binary="./bin/Moltorino7"
    app_executable="Moltorino7"
elif [ -f ./bin/chatterino ] && [ -x ./bin/chatterino ]; then
    app_binary="./bin/chatterino"
    app_executable="chatterino"
else
    echo "ERROR: No built app binary found. This script must be run in the build folder after the Linux app build succeeds."
    exit 1
fi

if [ "${QT_ROOT_DIR:-}" != "" ]; then
    echo "Using Qt root from QT_ROOT_DIR: $QT_ROOT_DIR"
    _QT_DIR="$QT_ROOT_DIR"
elif [ "${Qt5_DIR:-}" != "" ]; then
    echo "Using Qt DIR from Qt5_DIR: $Qt5_DIR"
    _QT_DIR="$(dirname "$(dirname "$(dirname "$Qt5_DIR")")")"
elif [ "${Qt6_DIR:-}" != "" ]; then
    echo "Using Qt DIR from Qt6_DIR: $Qt6_DIR"
    _QT_DIR="$(dirname "$(dirname "$(dirname "$Qt6_DIR")")")"
fi

if [ -n "$_QT_DIR" ]; then
    export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:${_QT_DIR}/lib"
    export PATH="${_QT_DIR}/bin:$PATH"
else
    echo "No Qt environment variable set, assuming system-installed Qt"
fi

if [ -n "${OPENSSL_1_1_1_DIR:-}" ]; then
    export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:$OPENSSL_1_1_1_DIR/lib"
    export PATH="$OPENSSL_1_1_1_DIR/bin:$PATH"
else
    echo "No OpenSSL environment variable set, assuming system-installed OpenSSL"
fi

script_path=$(readlink -f "$0")
script_dir=$(dirname "$script_path")
chatterino_dir=$(dirname "$script_dir")

echo "Running LDD on app binary:"
ldd "$app_binary"
echo ""

rm -rf appdir

echo "Running cmake install into appdir"
cmake --install . --prefix "$PWD/appdir/usr"
find appdir/
echo ""

desktop_file_name=$(find "$PWD/appdir/usr/share/applications" -maxdepth 1 -name '*.desktop' | head -n 1)
if [ -z "$desktop_file_name" ]; then
    echo "ERROR: No installed desktop file found in appdir/usr/share/applications/"
    exit 1
fi
desktop_file_base=$(basename "$desktop_file_name")

cp "$chatterino_dir"/resources/icon.png ./appdir/"$app_executable".png

linuxdeployqt_path="${LINUXDEPLOYQT_PATH:-linuxdeployqt-x86_64.AppImage}"
linuxdeployqt_url="https://github.com/probonopd/linuxdeployqt/releases/download/continuous/linuxdeployqt-continuous-x86_64.AppImage"

if [ ! -f "$linuxdeployqt_path" ]; then
    echo "Downloading LinuxDeployQT from $linuxdeployqt_url to $linuxdeployqt_path"
    curl --location --fail --silent "$linuxdeployqt_url" -o "$linuxdeployqt_path"
    chmod a+x "$linuxdeployqt_path"
fi

case "$linuxdeployqt_path" in
    /* | ./* | ../*) ;;
    *) linuxdeployqt_path="./$linuxdeployqt_path" ;;
esac

appimagetool_path="${APPIMAGETOOL_PATH:-appimagetool-x86_64.AppImage}"
appimagetool_url="https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage"

if [ ! -f "$appimagetool_path" ]; then
    echo "Downloading AppImageTool from $appimagetool_url to $appimagetool_path"
    curl --location --fail --silent "$appimagetool_url" -o "$appimagetool_path"
    chmod a+x "$appimagetool_path"
fi

case "$appimagetool_path" in
    /* | ./* | ../*) ;;
    *) appimagetool_path="./$appimagetool_path" ;;
esac

# For some reason, the copyright file for libc was not found. We need to manually copy it from the host system
mkdir -p appdir/usr/share/doc/libc6/
cp /usr/share/doc/libc6/copyright appdir/usr/share/doc/libc6/

echo "Run LinuxDeployQT"
host_runtime_exclude_libs="libglib-2.0.so.0,libgobject-2.0.so.0,libgio-2.0.so.0,libgmodule-2.0.so.0,libgthread-2.0.so.0,libffi.so.8,libpcre.so.3,libpcre2-8.so.0,libselinux.so.1,libmount.so.1,libsystemd.so.0,libgcrypt.so.20,libgpg-error.so.0,libcap.so.2,liblz4.so.1,liblzma.so.5,libzstd.so.1,libdbus-1.so.3,libblkid.so.1"
"$linuxdeployqt_path" \
    --appimage-extract-and-run \
    "appdir/usr/share/applications/$desktop_file_base" \
    -no-translations \
    -bundle-non-qt-libs \
    -exclude-libs="$host_runtime_exclude_libs" \
    -unsupported-allow-new-glibc

rm -rf appdir/home
rm -f appdir/AppRun

if ! command -v strip >/dev/null 2>&1; then
    echo "ERROR: strip was not found; refusing to package an unstripped AppImage."
    exit 1
fi
strip --strip-unneeded "appdir/usr/bin/$app_executable"
find appdir/usr/bin appdir/usr/lib appdir/usr/plugins \
    -type f ! -path "appdir/usr/bin/$app_executable" \
    \( -perm -111 -o -name '*.so' -o -name '*.so.*' \) \
    -exec strip --strip-unneeded {} + 2>/dev/null || true

echo "Run AppImageTool"

# shellcheck disable=SC2016
echo '#!/bin/sh
here="$(dirname "$(readlink -f "${0}")")"
export QT_QPA_PLATFORM_PLUGIN_PATH="$here/usr/plugins"
cd "$here/usr"
exec "$here/usr/bin/'"$app_executable"'" "$@"' > appdir/AppRun
chmod a+x appdir/AppRun

linuxdeployqt_basename=$(basename "$linuxdeployqt_path")
appimagetool_basename=$(basename "$appimagetool_path")

# find -name matches basenames only, not paths like ./foo.AppImage
find . -maxdepth 1 -type f -name '*.AppImage' \
    ! -name "$linuxdeployqt_basename" \
    ! -name "$appimagetool_basename" \
    -delete
export ARCH=x86_64
"$appimagetool_path" \
    --appimage-extract-and-run \
    appdir

created_appimage=$(find . -maxdepth 1 -type f -name '*.AppImage' \
    ! -name "$linuxdeployqt_basename" \
    ! -name "$appimagetool_basename" \
    ! -name "${app_executable}-x86_64.AppImage" \
    | head -n 1)
if [ -n "$created_appimage" ]; then
    mv "$created_appimage" "./${app_executable}-x86_64.AppImage"
fi

# TODO: Create appimage in a unique directory instead maybe idk?
rm -rf appdir
