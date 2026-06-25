#!/usr/bin/env bash

# Bundle relevant Qt & system dependencies into the app bundle.

set -eo pipefail

_app_bundle="${APP_BUNDLE_PATH:-Leafyrino.app}"

if [ -d "bin/${_app_bundle}" ] && [ ! -d "${_app_bundle}" ]; then
    >&2 echo "Moving bin/${_app_bundle} down one directory"
    mv "bin/${_app_bundle}" "${_app_bundle}"
elif [ -d bin/chatterino.app ] && [ ! -d "${_app_bundle}" ]; then
    >&2 echo "Moving legacy bin/chatterino.app to ${_app_bundle}"
    mv bin/chatterino.app "${_app_bundle}"
elif [ -d bin/Leafyrino.app ] && [ ! -d "${_app_bundle}" ]; then
    >&2 echo "Moving bin/Leafyrino.app to ${_app_bundle}"
    mv bin/Leafyrino.app "${_app_bundle}"
fi

if [ ! -d "${_app_bundle}" ]; then
    echo "ERROR: No '${_app_bundle}' dir found in the build directory."
    exit 1
fi

if [ -n "$Qt5_DIR" ]; then
    echo "Using Qt DIR from Qt5_DIR: $Qt5_DIR"
    _QT_DIR="$Qt5_DIR"
    _img_version="5.15.2"
elif [ -n "$Qt6_DIR" ]; then
    echo "Using Qt DIR from Qt6_DIR: $Qt6_DIR"
    _QT_DIR="$Qt6_DIR"
    _img_version="6.9.3"
fi

if [ -n "$_QT_DIR" ]; then
    export PATH="${_QT_DIR}/bin:$PATH"
else
    echo "No Qt environment variable set, assuming system-installed Qt"
fi

echo "Running MACDEPLOYQT"

_macdeployqt_args=()

if [ -n "$MACOS_CODESIGN_CERTIFICATE" ]; then
    _macdeployqt_args+=("-codesign=$MACOS_CODESIGN_CERTIFICATE")
fi

if [ -f kimg.zip ]; then
    echo "Extracting kimageformats plugins"
    rm -rf kimg
    7z x -okimg kimg.zip

    _karchive_dylib="$(find kimg -type f \( -name 'libKF6Archive.6.dylib' -o -name 'libKF5Archive.5.dylib' \) | head -n 1)"
    _kimg_avif_plugin="$(find kimg -type f \( -name 'kimg_avif.dylib' -o -name 'kimg_avif.so' \) | head -n 1)"

    if [ -z "${_karchive_dylib}" ]; then
        echo "ERROR: Could not find libKF Archive dylib in kimageformats archive."
        find kimg -type f
        exit 1
    fi

    if [ -z "${_kimg_avif_plugin}" ]; then
        echo "ERROR: Could not find kimg_avif plugin in kimageformats archive."
        find kimg -type f
        exit 1
    fi

    if [ -n "$MACOS_CODESIGN_CERTIFICATE" ]; then
        echo "Codesigning $(basename "${_karchive_dylib}")"
        codesign -s "$MACOS_CODESIGN_CERTIFICATE" --force "${_karchive_dylib}"
        echo "Codesigning kimg_avif"
        codesign -s "$MACOS_CODESIGN_CERTIFICATE" --force "${_kimg_avif_plugin}"
    fi

    mkdir -p "${_app_bundle}/Contents/Frameworks"
    mkdir -p "${_app_bundle}/Contents/PlugIns/imageformats"
    cp "${_karchive_dylib}" "${_app_bundle}/Contents/Frameworks/"
    cp "${_kimg_avif_plugin}" "${_app_bundle}/Contents/PlugIns/imageformats/"
else
    echo "No kimageformats archive provided; relying on Qt imageformats."
fi

macdeployqt "${_app_bundle}" "${_macdeployqt_args[@]}" -verbose=1

if [ -n "$MACOS_CODESIGN_CERTIFICATE" ]; then
    # Validate that the app bundle was codesigned correctly
    codesign -v "${_app_bundle}"
fi
