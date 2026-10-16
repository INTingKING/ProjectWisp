# ProjectWisp Android spike (build only)

SDL3 `android-project` template wrapping the unchanged desktop app: `libmain.so` (wisp) + `libSDL3.so`, arm64-v8a, minSdk 29, Vulkan 1.1.

Toolchain used: Android SDK cmdline-tools, NDK 27.3.13750724, SDK CMake 3.31.6, platform android-35, build-tools 35.0.0, OpenJDK 21.

1. Cross-build htslib 1.21 (static, PIC) with the NDK's zlib:

       ./configure --host=aarch64-linux-android --disable-libcurl --disable-bz2 --disable-lzma \
         --disable-gcs --disable-s3 --disable-plugins CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang
       make lib-static CFLAGS="-g -O2 -fPIC"
       # copy libhts.a -> $PREFIX/lib, htslib/ -> $PREFIX/include

   (export TMPDIR=/tmp if your TMPDIR does not exist; clang fails with "unable to make temporary file".)
2. `WISP_ANDROID_HTS_ROOT=$PREFIX ./gradlew assembleDebug` (libBigWig builds via FetchContent with WITH_CURL=OFF -> -DNOCURL).

Output: `app/build/outputs/apk/debug/app-debug.apk`.

Data: htslib/libBigWig need real paths (no content:// URIs). Fonts + data_release.conf are copied from the APK into
`SDL_GetAndroidInternalStoragePath()/assets`; the data root is `SDL_GetAndroidExternalStoragePath()/data`
(getExternalFilesDir, no storage permission). Populating it (adb push / download) is not done yet.

Known gaps: the rsID download and checksum shell out to curl/sha256sum via popen (not present on Android);
no touch UI; not run on a device or emulator.
