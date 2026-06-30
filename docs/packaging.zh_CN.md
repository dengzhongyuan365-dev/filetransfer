# Debian 打包

本项目使用 CMake 的 `install()` 规则配合 CPack 生成 `.deb` 包。
包内必须包含命令行二进制、GUI 二进制、desktop 文件、应用图标，以及编译后的 Qt 翻译文件。

## 必需构建工具

先安装常规 C++/Qt 构建依赖：

```sh
sudo apt install \
  build-essential cmake ninja-build pkg-config \
  libssl-dev \
  qt6-base-dev qt6-tools-dev qt6-tools-dev-tools \
  dpkg-dev
```

`qt6-tools-dev-tools` 很重要，因为它提供 `lupdate` 和 `lrelease`。
如果缺少它，源码里的 `.ts` 翻译文件可能存在，但最终包里不会生成并安装 `.qm` 翻译文件。

## 干净打包流程

在项目根目录执行：

```sh
scripts/package_deb.sh
```

这个脚本会自动完成清理构建目录、配置 Release、编译、运行测试、生成 deb，
并检查包内是否包含二进制、desktop 文件、SVG 图标和翻译文件。

脚本内部等价于执行下面这些命令：

```sh
rm -rf build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cpack --config build/CPackConfig.cmake
```

生成的包名类似：

```txt
lan-file-transfer_0.1.0_amd64.deb
```

最后的架构后缀取决于当前构建机器。

## 预期包内容

生成 deb 后，安装前先检查包内容：

```sh
dpkg-deb -c lan-file-transfer_0.1.0_amd64.deb
```

至少必须包含这些文件：

```txt
./usr/bin/sender
./usr/bin/receiver
./usr/bin/local-copy
./usr/bin/lan-gui
./usr/share/applications/lan-file-transfer.desktop
./usr/share/icons/hicolor/scalable/apps/lan-file-transfer.svg
./usr/share/lan-file-transfer/translations/lan-file-transfer_zh_CN.qm
```

可以用下面的命令快速检查关键文件：

```sh
dpkg-deb -c lan-file-transfer_0.1.0_amd64.deb \
  | grep -E 'usr/bin/(sender|receiver|local-copy|lan-gui)|applications/lan-file-transfer.desktop|icons/hicolor/scalable/apps/lan-file-transfer.svg|translations/lan-file-transfer_zh_CN.qm'
```

如果上面的任意文件缺失，不要发布这个包。

`scripts/package_deb.sh` 已经内置了这些关键文件检查。

## 项目里的安装规则

包内容来自 CMake 的 `install()` 规则：

```txt
src/CMakeLists.txt
  sender, receiver, local-copy -> /usr/bin
  lan-gui -> /usr/bin
  lan-file-transfer_zh_CN.qm -> /usr/share/lan-file-transfer/translations

CMakeLists.txt
  packaging/lan-file-transfer.desktop -> /usr/share/applications
  resources/icons/lan-file-transfer.svg -> /usr/share/icons/hicolor/scalable/apps
```

如果后续新增二进制、desktop 文件、图标或翻译文件，必须先更新对应的 `install()` 规则，
然后重新打包并再次检查 deb 内容。

## 翻译更新流程

当 GUI 文案发生变化时，先更新 `.ts` 文件，再通过正常构建生成 `.qm`：

```sh
/usr/lib/qt6/bin/lupdate src/gui_main.cpp src/gui/*.cpp src/gui/*.h \
  -ts translations/lan-file-transfer_zh_CN.ts

cmake --build build
```

检查翻译是否完整：

```sh
grep -nE 'unfinished|vanished' translations/lan-file-transfer_zh_CN.ts
```

没有输出表示没有未完成或已废弃的翻译项。

构建时应该能看到类似输出：

```txt
Generated 81 translation(s) (81 finished and 0 unfinished)
```

## 本地安装验证

安装生成的包：

```sh
sudo apt install ./lan-file-transfer_0.1.0_amd64.deb
```

确认安装后的文件存在：

```sh
command -v sender receiver local-copy lan-gui
test -f /usr/share/applications/lan-file-transfer.desktop
test -f /usr/share/icons/hicolor/scalable/apps/lan-file-transfer.svg
test -f /usr/share/lan-file-transfer/translations/lan-file-transfer_zh_CN.qm
```

启动 GUI：

```sh
lan-gui
```

在中文系统环境下，GUI 应该自动加载中文翻译。
程序也会尝试语言级 fallback，所以 `zh_CN` 找不到时，会继续尝试未来可能存在的
`lan-file-transfer_zh.qm`。

## 常见遗漏

- 没安装 Qt Linguist 工具就打包，导致 `.qm` 文件缺失。
- 添加了 `.desktop` 文件，但没有安装对应 SVG 图标。
- 添加了新的 `.ts` 翻译文件，但没有加入 `qt_add_translation`。
- 添加了新的可执行目标，但忘记写 `install(TARGETS ...)`。
- 修改后没有重新构建就直接 `cpack`，导致包里是旧二进制或旧翻译。
