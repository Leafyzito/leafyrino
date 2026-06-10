# Linux

For all dependencies below we use Qt 6. Our minimum supported version is Qt 5.15.2, but you are on your own.

## Install dependencies

### Ubuntu 22.04

```sh
sudo apt install qt6-base-dev qt6-svg-dev qt6-image-formats-plugins libboost-dev libboost-json-dev libnotify-dev libssl-dev libsecret-1-dev pkg-config cmake g++ git hunspell libhunspell-dev
```

Qt Charts is not in the default 22.04 repositories; install it with [aqt](https://github.com/miurahr/aqtinstall) or use the same [install-qt-action](https://github.com/jurplel/install-qt-action) modules as CI (`qtimageformats`, `qtcharts`). Jammy ships Boost 1.74 without JSON; CI builds Boost 1.83 for `boost/json.hpp`.

CI builds on `ubuntu-22.04` with Qt 6.7.2 and produces the AppImage and `.deb` packages.

### Ubuntu 24.04 and later

```sh
sudo apt install qt6-base-dev qt6-svg-dev qt6-charts-dev qt6-image-formats-plugins libboost-dev libboost-json-dev libnotify-dev libssl-dev libsecret-1-dev pkg-config cmake g++ git hunspell libhunspell-dev
```

CI builds on `ubuntu-24.04` with Qt 6.9.3 using [install-qt-action](https://github.com/jurplel/install-qt-action) with the `qtcharts` module. No Chatterino/docker image is required.

### Debian 13 (trixie) or later

```sh
sudo apt install qt6-base-dev qt6-svg-dev qt6-charts-dev qt6-image-formats-plugins libboost-dev libnotify-dev libssl-dev libsecret-1-dev pkg-config cmake g++ git hunspell
```

### Arch Linux

```sh
sudo pacman -S --needed qt6-base qt6-tools qt6-charts boost-libs openssl qt6-imageformats qt6-svg boost libnotify rapidjson pkgconf cmake hunspell
```

If you use Wayland, you will also need to ensure `qt6-wayland` is installed.

Alternatively you can use the [chatterino2-git](https://aur.archlinux.org/packages/chatterino2-git/) package to build and install Chatterino for you.

### openSUSE

```sh
sudo zypper install cmake pkgconf boost-devel libboost_json1_89_0-devel desktop-file-utils libappstream-glib8 hunspell ninja doxygen qt6-tools-devel qt6-charts-devel
```

### Gentoo Linux

```sh
doas emerge dev-libs/openssl dev-qt/qt5compat dev-qt/qtbase dev-qt/qtsvg dev-qt/qtcharts dev-qt/qtimageformats x11-libs/libnotify dev-libs/qtkeychain dev-libs/boost dev-build/cmake app-text/hunspell
```

### Fedora 39 and above

_Most likely works the same for other Red Hat-like distros. Substitute `dnf` with `yum`._

```sh
sudo dnf install qt6-qtbase-devel qt6-qtcharts-devel qt6-qtimageformats qt6-qtsvg-devel qt6-qtbase-private-devel g++ git openssl-devel libnotify-devel boost-devel cmake hunspell
```

### NixOS 18.09+

```sh
nix-shell -p openssl boost qt6.full pkg-config cmake libnotify hunspell
```

## Compile

### Windows + WSL (Ubuntu 22.04)

Moltorino ships helper scripts for Linux packaging from Windows through WSL:

```bat
build_linux_appimage.bat
build_linux_flatpak.bat
```

Those wrappers call the WSL scripts in `.CI/` and place finished artifacts in `releases/linux/`.

Prerequisites for the WSL path:

```sh
sudo apt install ninja-build build-essential pkg-config libgl1-mesa-dev libxkbcommon-dev libdbus-1-dev libxcb-cursor-dev libx11-xcb-dev libxkbcommon-x11-dev libwayland-dev libsecret-1-dev libpulse-dev libasound2-dev libfontconfig1-dev libfuse2 desktop-file-utils patchelf flatpak flatpak-builder rsync python3-pip
python3 -m pip install --user aqtinstall
~/.local/bin/aqt install-qt linux desktop 6.10.2 linux_gcc_64 -O /home/$USER/Qt --modules qtimageformats qt5compat qtcharts
flatpak remote-add --if-not-exists --user flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install --user flathub org.kde.Platform//6.10 org.kde.Sdk//6.10
```

The WSL scripts assume Qt is installed at `/home/<user>/Qt/6.10.2/gcc_64`. Set `QT_ROOT_DIR` before running the AppImage script if your Qt path differs.

## Manually

1. In the project directory, create a build directory and enter it
   ```sh
   mkdir build
   cd build
   ```
1. Generate build files. To enable Lua plugins in your build add `-DCHATTERINO_PLUGINS=ON` to this command.
   ```sh
   cmake -DBUILD_WITH_QTKEYCHAIN=OFF -DCHATTERINO_SPELLCHECK=On ..
   ```
   To enable the embedded Twitch/Kick stream player, add `-DCHATTERINO_WITH_STREAM_PLAYER=ON` to this command (requires Qt WebEngine, e.g. `qt6-webengine-dev` on Debian/Ubuntu).
1. Build the project
   ```sh
   cmake --build .
   ```

### Through Qt Creator

1. Install C++ IDE Qt Creator by using `sudo apt install qtcreator` (Or whatever equivalent for your distro)
1. Open `CMakeLists.txt` with Qt Creator and select build
