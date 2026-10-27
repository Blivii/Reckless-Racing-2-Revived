#!/usr/bin/env bash
# One-time toolchain setup on Linux (Debian/Ubuntu, or WSL on Windows). Prints the exports build.sh needs.
# Usage: tools/setup_env.sh [dir]   (default ~/rr2-tools)
set -euo pipefail
D=$(realpath -m "${1:-$HOME/rr2-tools}")
mkdir -p "$D" && cd "$D"
SUDO=; [ "$(id -u)" = 0 ] || SUDO=sudo   # system packages need root; most accounts aren't
command -v apt-get >/dev/null && { $SUDO apt-get update -q >/dev/null && $SUDO apt-get install -y -q libboost-dev cmake ninja-build default-jdk-headless unzip >/dev/null ||
    echo "setup_env: apt-get failed; install libboost-dev cmake ninja-build default-jdk-headless unzip yourself" >&2; }
[ -d android-ndk-r27c ] || { curl -fsSLO https://dl.google.com/android/repository/android-ndk-r27c-linux.zip && unzip -q android-ndk-r27c-linux.zip && rm android-ndk-r27c-linux.zip; }
[ -d android-14 ] || { curl -fsSLO https://dl.google.com/android/repository/build-tools_r34-linux.zip && unzip -q build-tools_r34-linux.zip && rm build-tools_r34-linux.zip; }
[ -d platform-34 ] || { curl -fsSLO https://dl.google.com/android/repository/platform-34-ext7_r03.zip && unzip -q -j platform-34-ext7_r03.zip '*/android.jar' -d platform-34 && rm platform-34-ext7_r03.zip; }   # patcher only
[ -f apktool.jar ] || curl -fsSLo apktool.jar https://github.com/iBotPeaches/Apktool/releases/download/v2.10.0/apktool_2.10.0.jar
mkdir -p boost-inc && ln -sfn /usr/include/boost boost-inc/boost   # NDK build must not see all of /usr/include
echo "export NDK=$D/android-ndk-r27c BOOST=$D/boost-inc BUILD_TOOLS=$D/android-14 APKTOOL_JAR=$D/apktool.jar ANDROID_JAR=$D/platform-34/android.jar"
