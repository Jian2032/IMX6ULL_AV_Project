#!/bin/sh
# Build a private ARM static libjpeg-turbo without root privileges.
#
# The Ubuntu host does not provide CMake or an ARM JPEG library.  This script
# therefore downloads a pinned CMake binary for the x86_64 build host and uses
# it to cross-compile a pinned libjpeg-turbo release.  Nothing is installed in
# /usr and no file is copied into the BusyBox root filesystem.

set -eu

CMAKE_VERSION=3.28.6
JPEG_VERSION=3.0.4

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# Support both layouts used by this project:
#   formal tree: <root>/src/apps/http_mjpeg
#   NFS tree   : <root>/http_mjpeg
case "$SCRIPT_DIR" in
	*/src/apps/http_mjpeg)
		DEFAULT_PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
		;;
	*)
		DEFAULT_PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
		;;
esac

PROJECT_ROOT=${PROJECT_ROOT:-$DEFAULT_PROJECT_ROOT}
THIRD_PARTY_DIR="$PROJECT_ROOT/third_party"
DOWNLOAD_DIR="$THIRD_PARTY_DIR/downloads"
TOOLS_DIR="$THIRD_PARTY_DIR/tools"
SOURCE_DIR="$THIRD_PARTY_DIR/src/libjpeg-turbo-$JPEG_VERSION"
BUILD_DIR="$THIRD_PARTY_DIR/build/libjpeg-turbo-arm-$JPEG_VERSION"
INSTALL_DIR="$THIRD_PARTY_DIR/libjpeg-turbo-arm"

CMAKE_ARCHIVE="cmake-$CMAKE_VERSION-linux-x86_64.tar.gz"
CMAKE_URL="https://ghfast.top/https://github.com/Kitware/CMake/releases/download/v$CMAKE_VERSION/$CMAKE_ARCHIVE"
CMAKE_FALLBACK_URL="https://github.com/Kitware/CMake/releases/download/v$CMAKE_VERSION/$CMAKE_ARCHIVE"
CMAKE_SHA256=931e3c0d546ee03ca72bb147ccd9b49e3b6252f765f66bf21b9d165519940458
CMAKE_HOME="$TOOLS_DIR/cmake-$CMAKE_VERSION-linux-x86_64"

JPEG_ARCHIVE="libjpeg-turbo-$JPEG_VERSION.tar.gz"
JPEG_URL="https://ghfast.top/https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$JPEG_VERSION/$JPEG_ARCHIVE"
JPEG_FALLBACK_URL="https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$JPEG_VERSION/$JPEG_ARCHIVE"
JPEG_SHA256=99130559e7d62e8d695f2c0eaeef912c5828d5b84a0537dcb24c9678c9d5b76b

TARGET_CC=${CROSS_COMPILE:-arm-linux-gnueabihf-}gcc
TARGET_AR=${CROSS_COMPILE:-arm-linux-gnueabihf-}ar
TARGET_RANLIB=${CROSS_COMPILE:-arm-linux-gnueabihf-}ranlib
TARGET_FLAGS="-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard"
JOBS=${JOBS:-4}

download_file()
{
	url=$1
	fallback_url=$2
	destination=$3
	expected_sha256=$4
	actual_sha256=

	if [ -s "$destination" ]; then
		actual_sha256=$(sha256sum "$destination" | awk '{print $1}')
		if [ "$actual_sha256" = "$expected_sha256" ]; then
			printf 'Using verified local archive: %s\n' "$destination"
			return
		fi
		printf 'Discarding invalid archive: %s\n' "$destination" >&2
		rm -f "$destination"
	fi

	printf 'Downloading through domestic accelerator: %s\n' "$url"
	if ! wget -O "$destination.part" "$url"; then
		echo "Domestic accelerator failed; trying the official URL." >&2
		rm -f "$destination.part"
		wget -O "$destination.part" "$fallback_url"
	fi

	actual_sha256=$(sha256sum "$destination.part" | awk '{print $1}')
	if [ "$actual_sha256" != "$expected_sha256" ]; then
		echo "ERROR: SHA-256 verification failed for $destination.part" >&2
		echo "expected: $expected_sha256" >&2
		echo "actual  : $actual_sha256" >&2
		rm -f "$destination.part"
		exit 1
	fi
	mv "$destination.part" "$destination"
}

if [ "$(uname -m)" != "x86_64" ]; then
	echo "ERROR: the bundled CMake download is for an x86_64 Ubuntu host." >&2
	exit 1
fi

if ! command -v "$TARGET_CC" >/dev/null 2>&1; then
	echo "ERROR: cross compiler $TARGET_CC was not found in PATH." >&2
	exit 1
fi
if ! command -v sha256sum >/dev/null 2>&1; then
	echo "ERROR: sha256sum is required to verify downloaded archives." >&2
	exit 1
fi

mkdir -p "$DOWNLOAD_DIR" "$TOOLS_DIR" \
	"$THIRD_PARTY_DIR/src" "$THIRD_PARTY_DIR/build" "$INSTALL_DIR"

if command -v cmake >/dev/null 2>&1; then
	CMAKE_BIN=$(command -v cmake)
else
	download_file "$CMAKE_URL" "$CMAKE_FALLBACK_URL" \
		"$DOWNLOAD_DIR/$CMAKE_ARCHIVE" "$CMAKE_SHA256"
	if [ ! -x "$CMAKE_HOME/bin/cmake" ]; then
		tar -xzf "$DOWNLOAD_DIR/$CMAKE_ARCHIVE" -C "$TOOLS_DIR"
	fi
	CMAKE_BIN="$CMAKE_HOME/bin/cmake"
fi

download_file "$JPEG_URL" "$JPEG_FALLBACK_URL" \
	"$DOWNLOAD_DIR/$JPEG_ARCHIVE" "$JPEG_SHA256"
if [ ! -f "$SOURCE_DIR/CMakeLists.txt" ]; then
	tar -xzf "$DOWNLOAD_DIR/$JPEG_ARCHIVE" -C "$THIRD_PARTY_DIR/src"
fi

printf 'Host CMake    : %s\n' "$CMAKE_BIN"
printf 'Target CC     : %s\n' "$TARGET_CC"
printf 'Source         : %s\n' "$SOURCE_DIR"
printf 'Build          : %s\n' "$BUILD_DIR"
printf 'Install        : %s\n' "$INSTALL_DIR"

"$CMAKE_BIN" -S "$SOURCE_DIR" -B "$BUILD_DIR" \
	-G "Unix Makefiles" \
	-DCMAKE_SYSTEM_NAME=Linux \
	-DCMAKE_SYSTEM_PROCESSOR=arm \
	-DCMAKE_C_COMPILER="$TARGET_CC" \
	-DCMAKE_AR="$TARGET_AR" \
	-DCMAKE_RANLIB="$TARGET_RANLIB" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_C_FLAGS="$TARGET_FLAGS" \
	-DCMAKE_ASM_FLAGS="$TARGET_FLAGS" \
	-DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
	-DCMAKE_INSTALL_LIBDIR=lib \
	-DENABLE_SHARED=OFF \
	-DENABLE_STATIC=ON \
	-DWITH_TURBOJPEG=ON \
	-DWITH_TOOLS=OFF \
	-DWITH_TESTS=OFF \
	-DWITH_JAVA=OFF \
	-DWITH_SIMD=ON

"$CMAKE_BIN" --build "$BUILD_DIR" --parallel "$JOBS"
"$CMAKE_BIN" --install "$BUILD_DIR"

if [ ! -f "$INSTALL_DIR/lib/libturbojpeg.a" ] || \
	[ ! -f "$INSTALL_DIR/include/turbojpeg.h" ]; then
	echo "ERROR: the expected static library or header was not installed." >&2
	exit 1
fi

echo
echo "[PASS] ARM static libjpeg-turbo is ready."
file "$INSTALL_DIR/lib/libturbojpeg.a"
ls -lh "$INSTALL_DIR/lib/libturbojpeg.a" "$INSTALL_DIR/include/turbojpeg.h"
echo "Next: cd $SCRIPT_DIR && make clean && make"
