# TelePath：面向现代存储的可观测 Buffer Pool SDK

<!-- # TelePath：面向异构存储架构的高性能可观测缓存系统框架 SDK -->

<p align="center">
  <img src="https://raw.githubusercontent.com/adj-matrix/adjmatrix-assets/main/TelePath/Telepathy.webp" width="60%">
</p>

<p align="center">
  <strong>!!!🤖开发中🤖!!! 非常抱歉，为了满足学术诚信要求，七月前暂时不接受任何 Issue 和 Pull Request (PR)</strong>
</p>

<p align="center">
  <a href="../README.md">English</a> | 简体中文
</p>

**TelePath**（Telepathy, Telemetry + Path）是一个面向现代存储环境与系统研究场景设计的 C++17 SDK，目标是构建一个具备可观察性的 Buffer Pool / Page Cache 引擎。

它并不是一个完整的数据库系统，而是一个可复用、可扩展、可实验的底层系统库，重点包括：

- 并发 Buffer Pool 核心
- 可插拔置换策略
- 稳定的 POSIX fallback 存储后端
- 面向原生 Linux 的 `io_uring` 后端路径
- 尽量不干扰热路径的低开销遥测接口

TelePath 从设计一开始就强调**数据面与观测面解耦**，希望把可观察性作为架构的一部分，而不是事后补充的调试手段。

## 项目动机

现代存储设备能够提供远高于传统教学型 Buffer Manager 的并发能力，但很多原型系统仍然难以在不引入重型日志或侵入式跟踪的情况下观察其内部状态。

TelePath 想要提供一个更小、更清晰、也更容易审计的实验底座，用于研究例如：

- Buffer Pool 在不同线程数下如何扩展
- 不同置换算法在偏斜负载下的行为差异
- 可观察性引入后对数据面热路径的影响
- 在 WSL2 等现实约束下如何分阶段推进 async-ready 设计

## 当前边界

目前 TelePath 的工程边界是：

- 它是一个 C++ Library
- 它管理内存中的页块，并通过存储后端协调持久化
- 它通过 telemetry sink 暴露可观察性
- 它被设计为未来 benchmark 与系统实验的基础设施

目前**不打算**纳入：

- SQL 解析
- 查询执行
- B+Tree 索引
- MVCC / 事务系统
- WAL / 崩溃恢复
- 首阶段就接入生产级监控平台

## State 3 当前实现

当前仓库已经从最初的 State 1/2 骨架推进到了一个可用的 State 3 写回核心，包括：

- `BufferManager`、`BufferHandle`、`BufferDescriptor`
- 连续 frame memory pool
- `DiskBackend`、`PosixDiskBackend`、`IoUringDiskBackend` 与 `DiskBackendFactory`
- 同页并发 miss 的显式协调
- 已提交磁盘请求的 centralized completion dispatch
- 基于 worker 的异步 flush scheduler
- 前台/后台 flush 队列分离与可配置 batching
- 基于 dirty watermark 的 background cleaner
- `ClockReplacer`、`LruReplacer`、`LruKReplacer`
- counter/no-op 两种 telemetry sink
- fallback 与 native 验证并存的 benchmark / CI 路径

这意味着项目当前不仅具备基础 buffer 生命周期闭环，还已经具备：

1. 对外保持同步语义、对内具备异步风格的磁盘调度路径
2. 同页并发 miss 的 ownership 协调，而不是重复加载
3. 带 completion routing 的真实 writeback 流程
4. cleaner 辅助的脏页管理
5. 更强的 flush 正确性、失败处理与调度行为回归覆盖

## 架构图

<p align="center">
  <img src="https://raw.githubusercontent.com/adj-matrix/adjmatrix-assets/main/TelePath/Architecture.png" width="60%">
</p>

## 构建方式

环境准备脚本放在 `support/` 中，日常构建/测试/基准入口脚本放在 `scripts/` 中。

### 最小构建依赖

```bash
sudo ./support/install_build_deps.sh
```

### 推荐开发工具

```bash
sudo ./support/install_dev_tools.sh
```

这个脚本会安装 `clang`、`clang-format`、`clang-tidy`、`lldb`、`valgrind` 等开发增强工具，但它们不是最小构建闭环的硬依赖。

### Debug 构建

```bash
./scripts/build/debug.sh
./scripts/test/debug.sh
```

### ASAN 构建

```bash
./scripts/build/asan.sh
./scripts/test/asan.sh
```

### Native `io_uring` 验证

在支持的原生 Linux 内核上，可以单独构建并测试 `io_uring` 路径：

```bash
./scripts/build/io_uring_debug.sh
./scripts/test/io_uring_native.sh
```

### LSAN 说明

LSAN 的构建入口为：

```bash
./scripts/build/lsan.sh
```

但在部分受限环境或 WSL 风格环境中，LeakSanitizer 在执行阶段可能因 `ptrace` 限制而失败。这更偏向运行环境限制，而非当前已知 TelePath 代码错误。

### Web 控制台

TelePath 现在包含一个浏览器端 Console，用于运行负载并查看引擎最近结果。

启动方式：

```bash
./scripts/web/serve.sh
```

它会启动一个本地 HTTP 服务，并提供：

- 浏览器 UI，
- 英文与简体中文运行时切换，
- 单次 benchmark 运行，
- 线程 sweep 实验，
- 进程内 recent run / recent sweep 历史，
- 以及基于当前 TelePath debug 构建与 benchmark 可执行文件的薄 API 层。

前端多语言资源位于 `localization/web-console/`。

## 仓库结构

仓库目录规范见：

- [Repository Tree Guide](../docs/tree.md)

当前主要实现位于：

- `src/cpp/include/telepath`
- `src/cpp/lib`
- `test/cpp`
- `web/frontend`
- `web/backend`
- `gui`
- `support`
- `scripts`

## 路线图

- Phase 1：建立稳定、可测试的 Buffer Pool 骨架
- Phase 2：推进并发生命周期、async-ready I/O 边界与 benchmark 骨架
- Phase 3：完成 centralized completion ownership、async writeback scheduling 与 cleaner-backed dirty-page management
- Phase 4：继续推进 shared-memory telemetry、native backend 深化优化与更大规模实验平台

当前阶段的重点仍然是架构正确性、接口稳定性与可扩展性，而不是过早宣称性能极限。

## 文档入口

对外文档位于 `docs/`。

推荐阅读：

- [State Summary](../docs/state.md)
- [CI Guide](../docs/ci.md)
- [Test Guide](../docs/test.md)
- [Repository Tree Guide](../docs/tree.md)
- [Architecture Mermaid](../docs/mermaid/architecture.mmd)

> 📜 **许可证**
>
> TelePath 基于 MIT License 发布。
>
> 🎨 **说明**
>
> 封面图来自 "テレパシ (Telepathy)", 为该项目名称的灵感来源，版权归原作者所有。
