# Debian Packaging

This project uses CMake install rules plus CPack to build a `.deb` package.
The package must include command line binaries, the GUI binary, the desktop
entry, the application icon, and compiled Qt translation files.

## Required Build Tools

Install the normal C++/Qt build dependencies first:

```sh
sudo apt install \
  build-essential cmake ninja-build pkg-config \
  libssl-dev \
  qt6-base-dev qt6-tools-dev qt6-tools-dev-tools \
  dpkg-dev
```

`qt6-tools-dev-tools` is important because it provides `lupdate` and `lrelease`.
Without it, the `.ts` source file may exist, but the package will not contain the
compiled `.qm` translation file.

## Clean Package Build

Run these commands from the project root:

```sh
scripts/package_deb.sh
```

The script removes the build directory, configures a Release build, builds the
project, runs tests, creates the deb package, and checks that the package
contains the binaries, desktop file, SVG icon, and translation file.

Internally, it is equivalent to running:

```sh
rm -rf build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cpack --config build/CPackConfig.cmake
```

The generated package should be named like:

```txt
lan-file-transfer_0.1.0_amd64.deb
```

The exact architecture suffix depends on the build machine.

## Expected Package Contents

After building the package, inspect it before installing:

```sh
dpkg-deb -c lan-file-transfer_0.1.0_amd64.deb
```

At minimum, the package must contain these files:

```txt
./usr/bin/sender
./usr/bin/receiver
./usr/bin/local-copy
./usr/bin/lan-gui
./usr/share/applications/lan-file-transfer.desktop
./usr/share/icons/hicolor/scalable/apps/lan-file-transfer.svg
./usr/share/lan-file-transfer/translations/lan-file-transfer_zh_CN.qm
```

Use this shortcut to check the important entries:

```sh
dpkg-deb -c lan-file-transfer_0.1.0_amd64.deb \
  | grep -E 'usr/bin/(sender|receiver|local-copy|lan-gui)|applications/lan-file-transfer.desktop|icons/hicolor/scalable/apps/lan-file-transfer.svg|translations/lan-file-transfer_zh_CN.qm'
```

If any of those files are missing, do not publish the package.

`scripts/package_deb.sh` already performs this required package content check.

## Install Rules In The Project

The package contents come from the CMake `install()` rules:

```txt
src/CMakeLists.txt
  sender, receiver, local-copy -> /usr/bin
  lan-gui -> /usr/bin
  lan-file-transfer_zh_CN.qm -> /usr/share/lan-file-transfer/translations

CMakeLists.txt
  packaging/lan-file-transfer.desktop -> /usr/share/applications
  resources/icons/lan-file-transfer.svg -> /usr/share/icons/hicolor/scalable/apps
```

When adding a new binary, desktop file, icon, or translation, update the matching
`install()` rule first, then rebuild the package and re-run the content check.

## Translation Update Flow

When GUI text changes, update the `.ts` file and regenerate the `.qm` through the
normal build:

```sh
/usr/lib/qt6/bin/lupdate src/gui_main.cpp src/gui/*.cpp src/gui/*.h \
  -ts translations/lan-file-transfer_zh_CN.ts

cmake --build build
```

Check that the generated translation is complete:

```sh
grep -nE 'unfinished|vanished' translations/lan-file-transfer_zh_CN.ts
```

No output means there are no unfinished or obsolete translations.

The build should print a line similar to:

```txt
Generated 81 translation(s) (81 finished and 0 unfinished)
```

## Local Install Test

Install the generated package:

```sh
sudo apt install ./lan-file-transfer_0.1.0_amd64.deb
```

Confirm the installed files:

```sh
command -v sender receiver local-copy lan-gui
test -f /usr/share/applications/lan-file-transfer.desktop
test -f /usr/share/icons/hicolor/scalable/apps/lan-file-transfer.svg
test -f /usr/share/lan-file-transfer/translations/lan-file-transfer_zh_CN.qm
```

Launch the GUI:

```sh
lan-gui
```

On a Chinese locale, the GUI should load the Chinese translation automatically.
The application also tries the language-only fallback, so `zh_CN` falls back to
`zh` if a future `lan-file-transfer_zh.qm` is provided.

## Common Omissions

- Building without Qt Linguist tools: the `.qm` file will be missing.
- Adding a `.desktop` file but not installing the matching SVG icon.
- Adding a new translation `.ts` file but not adding it to `qt_add_translation`.
- Adding a new executable target but forgetting the `install(TARGETS ...)` rule.
- Running `cpack` before rebuilding, which can package stale binaries or stale
  translations.
