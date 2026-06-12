#!/bin/sh

set -e

breakline() {
    printf "================================================================================\n\n"
}

# Configured in the CI step
install_prefix="appdir/usr"

# The directory we finally pack into our .deb package
packaging_dir="package"

# Get the Ubuntu Release (e.g. 20.04, 22.04 or 24.04)
ubuntu_release="$(lsb_release -rs)"

# The final path where we'll save the .deb package
deb_path="Chatterino-ubuntu-${ubuntu_release}-x86_64.deb"

# Refactor opportunity:
case "$ubuntu_release" in
    22.04)
        # Qt6 static-linked deb, see https://github.com/Chatterino/docker
        dependencies="libc6, libstdc++6, libglx0, libopengl0, libpng16-16, libharfbuzz0b, libfreetype6, libfontconfig1, libjpeg-turbo8, libxcb-glx0, libegl1, libx11-6, libxkbcommon0, libx11-xcb1, libxkbcommon-x11-0, libxcb-cursor0, libxcb-icccm4, libxcb-image0, libxcb-keysyms1, libxcb-randr0, libxcb-render-util0, libxcb-shm0, libxcb-sync1, libxcb-xfixes0, libxcb-render0, libxcb-shape0, libxcb-xkb1, libxcb1, libbrotli1, libglib2.0-0, zlib1g, libicu70, libpcre2-16-0, libssl3, libgraphite2-3, libexpat1, libuuid1, libxcb-util1, libxau6, libxdmcp6, libffi8, libmount1, libnotify4, libselinux1, libpcre3, libbsd0, libblkid1, libpcre2-8-0, libmd0"
        ;;
    24.04)
        # Qt6 static-linked deb, see https://github.com/Chatterino/docker
        dependencies="libc6, libstdc++6, libglx0, libopengl0, libpng16-16, libharfbuzz0b, libfreetype6, libfontconfig1, libjpeg-turbo8, libxcb-glx0, libegl1, libx11-6, libxkbcommon0, libx11-xcb1, libxkbcommon-x11-0, libxcb-cursor0, libxcb-icccm4, libxcb-image0, libxcb-keysyms1, libxcb-randr0, libxcb-render-util0, libxcb-shm0, libxcb-sync1, libxcb-xfixes0, libxcb-render0, libxcb-shape0, libxcb-xkb1, libxcb1, libbrotli1, libglib2.0-0, zlib1g, libicu74, libpcre2-16-0, libssl3, libgraphite2-3, libexpat1, libuuid1, libxcb-util1, libxau6, libxdmcp6, libffi8, libmount1, libnotify4, libselinux1, libpcre3, libbsd0, libblkid1, libpcre2-8-0, libmd0"
        ;;
    *)
        echo "Unsupported Ubuntu release $ubuntu_release"
        exit 1
        ;;
esac

echo "Building Ubuntu .deb file on '$ubuntu_release'"
echo "Dependencies: $dependencies"

if [ -f ./bin/Leafyrino ] && [ -x ./bin/Leafyrino ]; then
    app_executable="Leafyrino"
elif [ -f ./bin/chatterino ] && [ -x ./bin/chatterino ]; then
    app_executable="chatterino"
else
    echo "ERROR: No Leafyrino or chatterino binary file found. This script must be run in the build folder, and the app must be built first."
    exit 1
fi

chatterino_version=$(git describe --tags 2>/dev/null) || true
if [ "$(echo "$chatterino_version" | cut -c1-1)" = 'v' ]; then
    chatterino_version="$(echo "$chatterino_version" | cut -c2-)"
else
    chatterino_version="0.0.0-dev"
fi

# Make sure no old remnants of a previous packaging remains
rm -vrf "$packaging_dir"

mkdir -p "$packaging_dir/DEBIAN"

echo "Making control file"
cat >> "$packaging_dir/DEBIAN/control" << EOF
Package: chatterino
Version: $chatterino_version
Architecture: amd64
Maintainer: Leafyzito <https://github.com/leafyzito>
Depends: $dependencies
Section: net
Priority: optional
Homepage: https://github.com/leafyzito/leafyrino
Description: Leafyrino - chat client for Twitch (built for $ubuntu_release)
EOF
cat "$packaging_dir/DEBIAN/control"
breakline


echo "Running make install"
make install
find "$install_prefix"
breakline


echo "Merge install into packaging dir"
cp -rv "$install_prefix/" "$packaging_dir/"
find "$packaging_dir"
breakline

if ! command -v strip >/dev/null 2>&1; then
    echo "ERROR: strip was not found; refusing to package an unstripped .deb."
    exit 1
fi
main_binary="$packaging_dir/usr/bin/$app_executable"
if [ ! -f "$main_binary" ]; then
    echo "ERROR: Installed binary '$main_binary' was not found; refusing to package without stripping it."
    exit 1
fi
strip --strip-unneeded "$main_binary"
for strip_dir in "$packaging_dir/usr/bin" "$packaging_dir/usr/lib"; do
    if [ -d "$strip_dir" ]; then
        find "$strip_dir" \
            -type f ! -path "$main_binary" \
            \( -perm -111 -o -name '*.so' -o -name '*.so.*' \) \
            -exec strip --strip-unneeded {} + 2>/dev/null || true
    fi
done
breakline


echo "Building package"
dpkg-deb --build "$packaging_dir" "$deb_path"
breakline


echo "Package info"
dpkg --info "$deb_path"
breakline


echo "Package contents"
dpkg --contents "$deb_path"
breakline
