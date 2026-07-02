# LAN File Transfer

一个用 C++20 写的局域网文件传输实验项目。

这个项目的目标不是简单调用 `scp` 或 `rsync`，而是自己实现一套可学习、可演进的传输系统：从 TCP 连接、协议帧、文件分块、断点续传、目录同步、GUI，到 Debian 打包。

当前稳定版本：`v0.1.0`

## 当前能做什么

- 图形界面 `lan-gui`
  - 启动时选择接收目录。
  - 通过 UDP 在局域网发现在线机器。
  - 使用验证码确认两台机器的连接关系。
  - 拖拽文件或文件夹到传输列表即可发送。
  - 任务列表显示文件名、速率、大小/文件数、状态。
  - 支持停止发送任务、清除已结束任务。
  - 接收任务支持打开本机保存目录。
  - 支持托盘运行，关闭窗口时可选择退出或隐藏到托盘。
  - 支持多个已连接设备，发送目标可多选，超过并发限制的任务自动排队。
  - 支持粘贴剪贴板中的本地文件/文件夹和截图图片。

- 命令行工具
  - `sender`：发送文件或目录。
  - `receiver`：监听并接收文件或目录。
  - `local-copy`：本地复制与文件系统基础练习工具。
  - `lan_tests` / `unit_tests`：测试入口。

- 单文件传输
  - 自定义 frame 协议。
  - SHA256 校验。
  - `.part` 临时文件。
  - 断点续传。
  - 已存在相同目标文件时跳过。

- 目录同步
  - 构建 manifest：相对路径、大小、mtime、mode。
  - 使用类似 rsync quick-check 的判断：大小和 mtime 相同则跳过。
  - 目标缺失时 full 传输。
  - 目标已存在且变化时 delta 传输。
  - full 文件已经改成流式发送，避免大文件先完整读入内存。
  - 目录扫描阶段不再预计算每个文件的 SHA256，避免大目录拖拽后长时间无响应。
  - 接收端通过 `.part` 文件落盘，成功后再 rename。
  - 写入后恢复文件 mtime/mode，方便下一次快速跳过。

- 工程能力
  - CMake + Ninja 构建。
  - GoogleTest 单元测试。
  - Qt6 GUI。
  - OpenSSL SHA256。
  - CPack 生成 `.deb` 包。
  - desktop 文件、SVG 图标、中文翻译 `.qm` 文件打包安装。

## 构建依赖

Ubuntu/Debian 系统可以安装：

```sh
sudo apt install \
  build-essential cmake ninja-build pkg-config \
  libssl-dev \
  qt6-base-dev qt6-tools-dev qt6-tools-dev-tools \
  dpkg-dev
```

## 构建

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

生成的程序在：

```txt
build/src/sender
build/src/receiver
build/src/local-copy
build/src/lan-gui
build/src/lan_tests
build/tests/unit_tests
```

## 运行测试

```sh
ctest --test-dir build --output-on-failure
```

当前测试数量：`66` 个。

## GUI 使用方式

两台机器都运行：

```sh
build/src/lan-gui
```

基本流程：

1. 选择本机接收目录。
2. 在机器列表中搜索局域网设备。
3. 点击连接，对端确认验证码。
4. 连接成功后进入传输页面。
5. 把文件或文件夹拖入传输列表。

注意：发送任务的“打开目录”按钮是禁用的，因为目标目录在对端机器；接收任务可以打开本机保存目录。

## 命令行使用

接收端：

```sh
build/src/receiver --dir ~/Downloads/reviewdir --port 39123
```

发送端：

```sh
build/src/sender --host 192.168.1.10 --port 39123 --path ./demo-file.txt
```

发送目录：

```sh
build/src/sender --host 192.168.1.10 --port 39123 --path ./music-folder
```

## 打包

一键生成 Debian 包：

```sh
scripts/package_deb.sh
```

包内会包含：

- `/usr/bin/sender`
- `/usr/bin/receiver`
- `/usr/bin/local-copy`
- `/usr/bin/lan-gui`
- `/usr/share/applications/lan-file-transfer.desktop`
- `/usr/share/icons/hicolor/scalable/apps/lan-file-transfer.svg`
- `/usr/share/lan-file-transfer/translations/lan-file-transfer_zh_CN.qm`

更完整的打包说明见：

- [中文打包文档](docs/packaging.zh_CN.md)
- [English packaging document](docs/packaging.en.md)

协议说明见：

- [协议文档](docs/protocol.md)

技术优化路线见：

- [技术路线文档](docs/technical-roadmap.md)

开发过程复盘见：

- [开发日志](docs/development-log.zh_CN.md)

## 技术设计现状

### 网络层

当前默认网络后端是 POSIX socket。

上层传输逻辑通过 `Connection` / `Listener` / `NetworkBackend` 抽象访问网络，因此后续可以替换为 standalone Asio、QtNetwork 或其他实现。

### 协议层

每个消息都通过统一 frame 发送：

```txt
magic + version + type + flags + body_size + body
```

主要消息类型包括：

- `hello`
- `file_begin`
- `chunk`
- `file_end`
- `manifest`
- `sync_plan`
- `delta_begin`
- `delta`
- `delta_end`
- `ack`
- `error`

### 文件层

已经实现：

- RAII 文件描述符封装。
- `.part` 临时文件保护。
- SHA256 文件校验。
- 本地复制。

### GUI 架构

GUI 主窗口现在只负责页面组织、用户交互和控件刷新，部分可维护性边界已经拆出：

- `DeviceManager`：管理设备列表、在线状态、连接状态、当前活动设备和发送目标。
- `DiscoveryController`：管理 UDP discovery socket、广播探测、announce 回复和控制消息发送。
- `TransferListModel`：管理传输快照、任务所属设备、按设备过滤和用户隐藏的任务。
- `control_message`：统一 UDP 发现、连接请求、连接响应、断开连接的 JSON 编解码。
- `target_dialogs`：封装发送目标选择和排队任务更换目标设备弹窗。
- `TransferScheduler`：管理多设备发送队列、全局并发和单设备并发，并支持注入 fake runner 做稳定测试。
- 文件大小格式化。
- 路径校验和规范化。
- 取消信号。

### 传输层

单文件传输和目录同步已经分开：

- 单文件传输偏向可靠完整传输，带 SHA256 和断点续传。
- 目录同步偏向批量同步，使用 manifest + sync plan + delta/full 流。

目录同步中，full 文件和 delta 路径都已经改成流式发送；delta 路径使用 rolling checksum 滑动窗口生成操作并立即发送，不再先构建完整 delta plan。

## 已完成的路线图节点

- 基础 C++ 工程
  - CMake 工程、多个可执行目标、目录结构拆分。

- 通用基础设施
  - `Result<T>` 错误返回。
  - 参数解析。
  - 文件大小和速率格式化。
  - Stopwatch。
  - Buffer/字节数据处理。
  - CancellationToken。

- 文件系统能力
  - 路径校验。
  - 文件大小、mtime、mode 读取。
  - 文件 hash。
  - RAII fd。
  - 临时文件和 rename commit。

- 网络能力
  - TCP listen/connect/accept。
  - send_all/recv_exact。
  - 网络后端抽象层。
  - UDP 局域网发现。

- 协议能力
  - frame 编解码。
  - hello 协商。
  - 单文件 metadata。
  - manifest/sync plan/delta 编解码。
  - 错误帧和 ack。

- 传输能力
  - 单文件发送/接收。
  - 单文件断点续传。
  - 目录 manifest。
  - skip/full/delta 计划。
  - full 文件流式传输。
  - delta 操作使用 rolling checksum 流式生成和发送。
  - 大目录扫描不预计算 hash。
  - 进度事件和 GUI 日志。

- GUI 和发布能力
  - Qt6 GUI。
  - 机器发现列表。
  - 验证码连接确认。
  - 拖拽发送。
  - 传输任务列表。
  - 托盘模式。
  - 中文翻译。
  - desktop 文件和图标。
  - Debian 打包脚本。

## 规划过但还没有完成的内容

- 安全和认证
  - 当前只有验证码确认连接，还没有真正的加密认证。
  - 后续可以做设备身份、公钥指纹、信任列表。
  - 传输内容目前不是端到端加密。

- 更完整的同步算法
  - 当前 delta 已经避免把源文件读入内存并构建完整 plan。
  - 当前 delta 已经支持 rolling checksum 的 O(1) 滑动更新和 SHA256 强校验确认。
  - 后续还可以继续优化签名索引、批量读缓冲、统计日志和大文件基准测试。

- 更强的数据完整性策略
  - 单文件有 SHA256。
  - 目录 full 流目前主要依赖大小校验和传输协议，未做每块 checksum。
  - 后续可以增加 per-chunk checksum 或 full 文件结束 hash 模式。
  - 可以提供 `--checksum` 强校验模式。

- 目录语义
  - 当前主要同步普通文件。
  - 对空目录、符号链接、删除同步、文件重命名检测还不完整。
  - 权限、所有者、扩展属性没有完整同步。

- 并发和性能
  - 当前一次主要处理一个发送任务。
  - 还没有多文件并发传输。
  - 没有限速、暂停、恢复整个目录任务。
  - GUI 速率还不是滑动窗口瞬时速率。

- 服务化
  - 目前可以启动 GUI 或 receiver，但没有 systemd service。
  - 后续可以提供开机自启服务、用户级 systemd unit。

- 网络适配层后续实现
  - 抽象已经有了。
  - 但还没有 Asio backend、QtNetwork backend 或 TLS backend。

- 跨平台
  - 当前主要面向 Linux。
  - Windows/macOS 还没有系统性适配。

- 发布流程
  - 已支持本地 `.deb`。
  - 还没有 GitHub Release 自动上传 deb。
  - 没有 CI 自动构建和测试。

- GUI 体验
  - 任务列表还有继续打磨空间。
  - 还没有任务详情页。
  - 还没有历史记录。
  - 还没有多设备发送队列。

## 当前版本边界

`v0.1.0` 可以作为一个稳定学习版本：

- 能在两台局域网 Linux 机器之间传文件和目录。
- 能观察网络协议、文件落盘、分块发送、断点续传、目录同步的基本流程。
- 能打包成 `.deb` 安装运行。

但它还不是生产级替代 rsync/scp 的工具。后续重点应该放在：

1. delta 性能基准测试和大文件优化。
2. 每块校验和强校验模式。
3. 设备认证和加密。
4. GUI 任务体验和多任务队列。
5. CI 和 GitHub Release。
