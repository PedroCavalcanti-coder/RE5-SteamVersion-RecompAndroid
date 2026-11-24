#!/usr/bin/env bash
# Uso: build_variant.sh <nome> [args do cmake...]
# Compila o código atual com o NDK r27 numa pasta própria (build/android-<nome>), troca o libre5.so do
# jniLibs, empacota com o Gradle e guarda work/android/app-<nome>.apk. Restaura o libre5.so base no fim.
set -uo pipefail
cd /c/Users/Dev_RV01/Documents/Github/re5-recompiled
name="$1"; shift
M="$PWD"
NDK="C:/Program Files/Unity/Hub/Editor/6000.3.27f1/Editor/Data/PlaybackEngines/AndroidPlayer/NDK"
NB="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin"
J=android/app/src/main/jniLibs/arm64-v8a
B=build/android-$name
log=build/variant_$name.log
echo "== $name: $*" > "$log"
cmake -S . -B "$B" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="C:/Users/Dev_RV01/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT.LLVM_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin/ninja.exe" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
  -DANDROID_NDK="$NDK" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRE5_BUILD_TESTS=OFF -DRE5_GENERATED_OPT=-O1 \
  -DRE5_GENERATED_DIR="$M/generated/src" \
  -DRE5_FFMPEG_DIR=C:/Users/Dev_RV01/Documents/Github/re5-ref49/build/ffmpeg-ndk27 -DRE5_DXVK_DIR="$M/build/dxvk" "$@" >> "$log" 2>&1 || { echo "cmake falhou" >> "$log"; exit 1; }
cmake --build "$B" >> "$log" 2>&1 || { echo "build falhou" >> "$log"; exit 1; }
[ -f work/android/libre5.lean_base_ndk27.so ] || cp "$J/libre5.so" work/android/libre5.lean_base_ndk27.so
"$NB/llvm-strip.exe" --strip-unneeded -o "$J/libre5.so" "$B/libre5.so"
(cd android && JAVA_HOME="C:/Program Files/Android/Android Studio/jbr" ./gradlew.bat assembleDebug -q >> "../$log" 2>&1) || echo "gradle falhou" >> "$log"
cp android/app/build/outputs/apk/debug/app-debug.apk "work/android/app-$name.apk"
cp work/android/libre5.lean_base_ndk27.so "$J/libre5.so"
echo "FEITO $name: $(stat -c %s work/android/app-$name.apk) bytes" >> "$log"
