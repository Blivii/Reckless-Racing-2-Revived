#!/usr/bin/env bash
# Builds the arm64 loader and repacks the original APK around it.
# Usage: tools/build.sh path/to/Reckless-Racing-2-v1.0.4.apk [path/to/main.103.com.polarbit.RecklessRacing2.obb]
# With the OBB, it goes inside the APK (as the patcher app does); without, it must be in Android/obb as usual.
# Needs: NDK, BOOST (folder containing boost/), BUILD_TOOLS (Android SDK build-tools/<ver>),
#        APKTOOL_JAR, plus cmake, ninja, git and java on PATH.
set -euo pipefail
APK=$(realpath "$1")
cd "$(dirname "$0")/.."
: "${NDK:?set NDK}" "${BOOST:?set BOOST}" "${BUILD_TOOLS:?set BUILD_TOOLS}" "${APKTOOL_JAR:?set APKTOOL_JAR}"

# 1. Native loader
if [ ! -d externals/dynarmic ]; then
    git clone --recursive https://github.com/azahar-emu/dynarmic externals/dynarmic
    git -C externals/dynarmic checkout -q a46601580d5512d324104f985b5f0209dc980ddc
fi
cmake -S . -B build-android -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=29 -DBoost_INCLUDE_DIR="$BOOST"
cmake --build build-android

# 2. Repack: original Java + assets, our loader in front, original 32-bit lib renamed libguest.so
rm -rf out && java -jar "$APKTOOL_JAR" d -s -o out/apk "$APK"   # -s keeps classes.dex as-is
L=out/apk/lib
mkdir -p $L/arm64-v8a
mv $L/armeabi-v7a/libRecklessRacing2.so $L/arm64-v8a/libguest.so
rm -rf $L/armeabi-v7a $L/x86
cp build-android/libRecklessRacing2.so $L/arm64-v8a/
sed -i -E "s/(targetSdkVersion: ).*/\1'24'/" out/apk/apktool.yml   # current Android refuses < 24
python3 tools/patch_dex.py out/apk/classes.dex
java -jar "$APKTOOL_JAR" b -o out/unsigned.apk out/apk
if [ -n "${2:-}" ]; then   # the OBB, stored uncompressed: the loader reads it straight from the APK
    mkdir -p out/obb/rr2 && ln -sf "$(realpath "$2")" out/obb/rr2/main.obb
    (cd out/obb && zip -q -0 ../unsigned.apk rr2/main.obb)
fi

# 3. Align + sign. The repo's key keeps updates installable over earlier builds (and their saves).
KS="${KS:-tools/debug.keystore}"
[ -f "$KS" ] || { mkdir -p "$(dirname "$KS")"; keytool -genkeypair -keystore "$KS" -storepass android \
    -alias androiddebugkey -keypass android -keyalg RSA -validity 10000 -dname "CN=Android Debug"; }
"$BUILD_TOOLS/zipalign" -f -p 4 out/unsigned.apk out/aligned.apk
java -jar "$BUILD_TOOLS/lib/apksigner.jar" sign --ks "$KS" --ks-pass pass:android --out out/RR2-arm64.apk out/aligned.apk
echo "Built out/RR2-arm64.apk"
