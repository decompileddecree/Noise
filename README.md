# Hardware White Noise

A white-noise audio and visual app written in C with SDL2. The same `noise.c`
is used for Windows, Linux, macOS, and Android; SDL2 supplies the audio,
graphics, input, and threading APIs.

## Desktop builds

Install CMake, a C compiler, and the SDL2 development package for your
platform.

### Windows

Install SDL2 with [vcpkg](https://vcpkg.io/), then configure and build from
PowerShell:

```powershell
vcpkg install sdl2:x64-windows
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Release
```

The executable is `build\Release\noise.exe`.

### Linux

On Debian or Ubuntu, install the dependencies and build:

```sh
sudo apt install build-essential cmake libsdl2-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Run it with `./build/noise`.

### macOS

With [Homebrew](https://brew.sh/), install the dependencies and build:

```sh
brew install cmake sdl2
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Run it with `./build/noise`.

## Android APK

The Android app uses SDL2's `SDLActivity` to launch the shared native C
program. It requires Android Studio (or the Android SDK, NDK, and CMake) and
Java 17. The included Gradle wrapper downloads the required Gradle version.

Clone the repository with its SDL2 dependency:

```sh
git clone --recurse-submodules https://github.com/decompileddecree/Noise.git
cd Noise
```

To build a debug APK from PowerShell:

```powershell
.\android\gradlew.bat --project-dir android assembleDebug
```

On Linux or macOS, run `bash android/gradlew --project-dir android assembleDebug`.

The APK is written to
`android/app/build/outputs/apk/debug/app-debug.apk`. You can also open the
`android` folder in Android Studio and build the `app` configuration there.

## Automated builds

GitHub Actions builds the desktop app on Windows, Linux, and macOS and creates
an Android debug APK on pushes and pull requests. The APK is available from the
workflow run's artifacts; it is not automatically published as a release.
