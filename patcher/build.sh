#!/usr/bin/env bash
# Builds the on-device patcher app around the arm64 loader. No game code goes in it.
# Usage: patcher/build.sh   (after tools/build.sh, or set SO=path/to/libRecklessRacing2.so)
# Needs: BUILD_TOOLS (Android build-tools 34), ANDROID_JAR (platform android.jar), java, javac, openssl, zip.
set -euo pipefail
cd "$(dirname "$0")"
: "${BUILD_TOOLS:?set BUILD_TOOLS}" "${ANDROID_JAR:?set ANDROID_JAR}"
SO=$(realpath "${SO:-../build-android/libRecklessRacing2.so}")
KS=../tools/debug.keystore
O=build && rm -rf $O && mkdir -p $O/assets $O/classes $O/dex

# Loader + the repo signing key (so patched games install over earlier builds and keep saves)
cp "$SO" $O/assets/libRecklessRacing2.so
openssl pkcs12 -in $KS -passin pass:android -nodes -nocerts | openssl pkcs8 -topk8 -nocrypt -outform DER -out $O/assets/key.pk8
openssl pkcs12 -in $KS -passin pass:android -nokeys | openssl x509 -outform DER -out $O/assets/cert.der

APKSIG="$BUILD_TOOLS/lib/apksigner.jar"   # contains apksig, which signs on the phone
"$BUILD_TOOLS/aapt2" compile --dir res -o $O/res.zip   # the icon
"$BUILD_TOOLS/aapt2" link -o $O/unsigned.apk -I "$ANDROID_JAR" --manifest AndroidManifest.xml -A $O/assets -R $O/res.zip
javac -nowarn --release 8 -cp "$ANDROID_JAR:$APKSIG" -d $O/classes src/rr2/patcher/*.java
java -cp "$BUILD_TOOLS/lib/d8.jar" com.android.tools.r8.D8 --release --min-api 29 --lib "$ANDROID_JAR" \
    --output $O/dex $(find $O/classes -name '*.class') "$APKSIG"
(cd $O/dex && zip -q ../unsigned.apk classes.dex)
"$BUILD_TOOLS/zipalign" -f -p 4 $O/unsigned.apk $O/aligned.apk
java -jar "$APKSIG" sign --ks $KS --ks-pass pass:android --out $O/RR2-Patcher.apk $O/aligned.apk
echo "Built patcher/$O/RR2-Patcher.apk"
