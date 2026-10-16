# ProjectWisp on Android (spike)

SDL3 `android-project` template (Gradle) wrapping the app: `libmain.so` (wisp) + `libSDL3.so`, arm64-v8a,
**minSdk 29, Vulkan 1.1 required** (`android.hardware.vulkan.version` 0x401000), portrait only, phone touch layout.

Toolchain used: Android SDK cmdline-tools, NDK 27.3.13750724, SDK CMake 3.31.6, platform android-35, build-tools 35.0.0, OpenJDK 21.

1. Cross-build htslib 1.21 (static, PIC) with the NDK's zlib:

       ./configure --host=aarch64-linux-android --disable-libcurl --disable-bz2 --disable-lzma \
         --disable-gcs --disable-s3 --disable-plugins CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang
       make lib-static CFLAGS="-g -O2 -fPIC"
       # copy libhts.a -> $PREFIX/lib, htslib/ -> $PREFIX/include

   (export TMPDIR=/tmp if your TMPDIR does not exist; clang fails with "unable to make temporary file".)
2. `WISP_ANDROID_HTS_ROOT=$PREFIX ./gradlew assembleDebug` (libBigWig builds via FetchContent with WITH_CURL=OFF -> -DNOCURL).

Output: `app/build/outputs/apk/debug/app-debug.apk`.

## Data

htslib/libBigWig need real paths (no content:// URIs), so everything lives in app-specific storage (no permissions):
fonts + `data_release.conf` + `data_packs.tsv` are copied from the APK into `SDL_GetAndroidInternalStoragePath()/assets`;
the data root is `SDL_GetAndroidExternalStoragePath()/data` (getExternalFilesDir).

First start without `tracks/overview.wov` shows the download screen. Packs (`assets/data_packs.tsv`, files + sizes + SHA-256):
core 71 MB (required), SVs 87 MB, CpG methylation 169 MB, genome sequence 933 MB (optional now: without it the
Sequence lane is empty), rsID 378 MB. Files are fetched from `DATA_BASE_URL` (the GitHub data release) with
`DownloadManager` through `org.projectwisp.app.WispActivity` (Java) + `src/util/download.cpp` (JNI): it resumes, survives
backgrounding, and the request id is kept as `<file>.part.dmid` so a restarted app re-attaches. The finished file is
hashed in-process (`src/util/sha256.cpp`, public domain) and renamed into place.

**Not yet published:** release `data-v1` only has `rsid.idx` and `sv.hprc.vcf.gz(.tbi)`. The core, methylation and
genome assets must be uploaded under the names in `data_packs.tsv` before a phone can fetch them.

## Phone UI

`--phone` on desktop renders the same layout at 1080x2340 (360x780 dp at 3x) for shots: `scripts/shots/run_phone.sh`.
Script touch commands: `tap`, `swipe`, `pinch`, `hold`/`up`.

Not tested on a device or emulator (no arm64 emulator on this x86 box): DownloadManager shim, dp scaling of SDL touch
coordinates, the Android back key, soft keyboard for search.
