# TelePath：面向缓冲、页缓存与 ML 预取的可共享、可观测缓存内核 SDK

<!-- # TelePath：面向异构存储架构的高性能可观测缓存系统框架 SDK -->

<p align="center">
  <img src="https://raw.githubusercontent.com/adj-matrix/adjmatrix-assets/main/TelePath/Telepathy.webp" width="60%">
</p>

<p align="center">
  <a href="../README.md">English</a> | 简体中文
</p>

**TelePath**（Telepathy，Telemetry + Path）是一个围绕**缓存内核**构建的 C++23 SDK。这个内核是建立在后备存储之上、按键寻址、块大小固定的块缓存，上面有三种产品形态：

| 门面（Facade） | 定位 | 访问方式 |
| --- | --- | --- |
| `BufferManager` | PostgreSQL bufmgr 风格的数据库缓冲池 | 在共享或排他 latch 下原地 pin 页面；回写、写前钩子、recLSN、检查点 |
| `PageCache` | 按字节寻址的文件缓存 | 按字节范围 `Read`/`Write`，带预读与后台回写；逻辑文件大小；`Sync` |
| `MLPrefetchBuffer` | 面向 ML 训练的只读数据集缓存 | 读取顺序事先已知（plan），据此预取并按 Belady 淘汰 |

这些门面不需要各自独占一个缓存。它们可以运行在同一个内核的不同**命名空间**里，每个命名空间有自己的置换策略、帧配额和写前钩子；同时共享每个页面唯一且一致的副本、同一组 I/O 与回写线程以及同一份内存预算。内核还可以放进 **POSIX 共享内存**，让多个进程共享同一个缓存。

缓存的一切行为都可以**从外部观测**：带版本号的共享内存观测区发布帧状态与计数器，Python 读取器或 Web 控制台可以直接读取，不触碰引擎的热路径。

TelePath 不是 DBMS：没有 SQL、查询执行、索引、事务，也没有自己的预写日志。它提供的是这些系统所需要的缓冲层，并做成可复用的库。

## 项目动机

数据库缓冲池、操作系统式的页缓存和训练数据缓存解决的是同一个问题：在固定大小的内存里留住慢速存储上的热块。但它们通常是各自独立的实现。于是一台机器同时跑数据库和训练任务时，只能静态切分内存，重复运行后台线程，还会把同一个文件缓存两份且互不一致。

TelePath 探索的是共享一个内核能做到什么：

- **共享但不失隔离**：命名空间在同一个池里拥有保底（`min_frames`）和上限（`max_frames`）份额，一个命名空间的全表扫描驱逐不了其他命名空间的保底页面。
- **跨进程共享**：元数据、帧、置换器状态和配额计数都放在共享段中。某个进程崩溃时，所有挂接进程都会被隔离（fence），脏页按配置抢救或丢弃，做法与 PostgreSQL 在后端崩溃后重置共享内存相同。
- **观测不干扰**：观测面在后台线程上复制状态，页面命中路径不做任何额外工作；外部工具能看到帧归属、脏页与回写状态、pin 以及各进程的计数器。

## 功能

**内核**

- PostgreSQL 风格的缓冲访问：`ReadBuffer` / `ReadBufferExclusive` 返回 RAII 的 `SharedBufferHandle` / `ExclusiveBufferHandle`，通过排他句柄写入即标记为脏页。
- 基于 futex 的 32 位帧控制字（状态、脏、回写、latch、pin 计数），定长开放寻址页表，无锁空闲链表。
- 通过 I/O 引擎实现异步 I/O，提供 POSIX 与原生 `io_uring` 后端：多块读取、`PrefetchBuffer`、`StartReadBuffers` / `WaitReadBuffers`，以及扫描用的环形缓冲等访问策略。
- 回写：前台与后台队列分离的刷写调度器，按脏页水位驱动的后台 cleaner，以及 `FlushAll()` 与 `Close()` 的明确持久性约定。
- 预写日志支持（不含日志本身）：写前钩子（页面写出前先把 WAL 刷到该页的 LSN），通过 `MarkDirty(lsn)` 记录的逐页 recLSN，`MinRecLsn()` 与 `CheckpointBuffers(lsn)`。
- 置换策略：Clock、clock-sweep（PostgreSQL）、SIEVE、LRU、LRU-K、2Q，以及基于计划的 Belady 策略（`PlanReplacer`）；每种策略都能把状态放进共享内存。
- 基于 `std::expected` 的错误模型（`Result<T>`、`Status`），不可恢复的错误使实例进入 Failed 状态。

**共享**

- 命名空间（每个内核最多 16 个）：各自拥有置换器、`min_frames` / `max_frames` 和写前钩子。页面归加载它的命名空间所有，其他命名空间的命中不会改变归属。
- 门面挂接到已有内核：`PageCache::Create(options, manager, space)` 与 `MLPrefetchBuffer::Create(options, manager, space)`。
- 跨进程模式（`BufferManagerOptions::shared_memory`）：
  - 任一进程都可以创建或挂接共享段，挂接时校验配置。
  - 各进程声明完全相同的命名空间，配额和淘汰顺序在进程之间同样生效。
  - 后台回写由选举出的一个 cleaner 进程负责。
  - 进程崩溃在几十毫秒内被检测到，随后共享段被隔离。

**观测**

- 共享内存中的观测区（`ObservationOptions`），格式公开发布于 `telepath/observation/observation_format.h`：
  - 汇总信息；
  - 各进程的遥测计数器；
  - 每帧一条记录（状态、标志、pin、页面、所属命名空间）。
- `web/backend/observation.py` 直接解析该格式，Web 控制台通过 `GET /api/observation` 提供。
- 可插拔的 `TelemetrySink` 计数器，以及带延迟分位数和运行期采样快照的基准测试工具。

## 架构

<p align="center">
  <img src="https://raw.githubusercontent.com/adj-matrix/adjmatrix-assets/main/TelePath/Architecture.png" width="80%">
</p>

## 总体概览

供存储引擎使用的缓冲池：

```cpp
#include "telepath/buffer/buffer_manager.h"
#include "telepath/io/disk_backend_factory.h"
#include "telepath/replacer/replacer.h"
#include "telepath/telemetry/telemetry_sink.h"

telepath::BufferManagerOptions options;
options.pool_size = 4096;   // 帧数
options.page_size = 8192;   // 字节，[4 KiB, 4 MiB] 内的 2 的幂
options.pre_write_hook = [&](const telepath::BufferTag &tag, std::span<const std::byte> page) {
  return wal.FlushUpTo(PageLsn(page));  // WAL 规则
};
auto manager = telepath::BufferManager::Create(
    options, telepath::CreateDiskBackend("/data", options.page_size).value(),
    telepath::MakeClockSweepReplacer(options.pool_size), telepath::MakeNoOpTelemetrySink()).value();

{
  auto page = manager->ReadBufferExclusive(/*file_id=*/1, /*block_id=*/42).value();
  std::span<std::byte> bytes = page.mutable_data();  // 标记为脏页
  // ... 修改 bytes，并在 lsn 处追加日志记录 ...
  page.MarkDirty(lsn);                               // 记录页面的 recLSN
}
manager->CheckpointBuffers(redo_lsn);                // redo_lsn 之前记录的修改全部持久化
manager->Close();
```

文件缓存与训练数据集共享同一个内核：

```cpp
telepath::NamespaceOptions files;
files.name = "files";
files.min_frames = 1024;           // 训练遍历驱逐不了这些页面
auto cache = telepath::PageCache::Create({}, *manager, std::move(files)).value();

telepath::NamespaceOptions training;
training.name = "training";
training.max_frames = 2048;        // 数据集最多占用这么多帧
auto dataset = telepath::MLPrefetchBuffer::Create({}, *manager, std::move(training)).value();

cache->Write(/*file_id=*/7, /*offset=*/0, bytes);
dataset->SetPlan(epoch_order);     // std::vector<telepath::BufferTag>
while (auto next = dataset->Next().value()) Consume(next->handle.data());
```

多个进程共享一个缓存，每个进程运行相同的代码：

```cpp
options.shared_memory.name = "/telepath_cache";  // 创建或挂接
std::vector<telepath::NamespaceOptions> spaces;
spaces.push_back({.name = "oltp", .replacer = telepath::MakeLruKReplacer(options.pool_size, /*k=*/2), .min_frames = 1024});
spaces.push_back({.name = "scan", .replacer = telepath::MakeSieveReplacer(options.pool_size), .max_frames = 256});
auto shared = telepath::BufferManager::Create(
  options,
  std::move(backend),
  telepath::MakeClockSweepReplacer(options.pool_size),
  telepath::MakeNoOpTelemetrySink(), std::move(spaces)
).value();
telepath::BufferNamespace scan = shared->GetNamespace("scan").value();
auto page = scan.ReadBuffer(/*file_id=*/3, /*block_id=*/0).value();
```

用 Python 读取观测面：

```python
from web.backend.observation import read_observation  # 在仓库根目录运行
snapshot = read_observation("/telepath_observation")
print(snapshot["summary"], len(snapshot["frames"]), snapshot["counters"])
```

`src/cpp/include/telepath` 下的公开头文件精确说明了每个调用，包括线程安全性、错误码与持久性。

## 进度

TelePath 分阶段开发，记录在 [`docs/design/kernel-refactor.md`](../docs/design/kernel-refactor.md)：

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| 0–4 | C++23 工具链、错误模型、帧控制字、句柄、页表、策略层、异步 I/O、回写与持久性 | 完成 |
| 5 | 门面：`PageCache`、带 `PlanReplacer` 的 `MLPrefetchBuffer` | 完成 |
| 6 | 跨进程模式、观测面、recLSN 与检查点、cleaner 选举、崩溃检测与恢复 | 完成 |
| 7a–7c | 带配额的命名空间；门面共享一个内核；跨进程命名空间 | 完成 |
| 7d–7e | 按命名空间的统计与淘汰事件流；Web 命名空间视图 | 下一步 |
| 之后 | 支持不同块大小的缓存组；可选的通用日志模块 | 计划中 |

每个阶段都同步进行正确性工作：

- **随机化混沌测试**：并发运行大部分 API，对照每个页面的模型检查，并注入 I/O 故障；在单进程、双进程以及 io_uring 上都会运行。
- **置换器模型测试**：把淘汰结果与 LRU、LRU-K、Belady 参考模型逐一对照。
- **撕裂读测试**：检查观测面。
- **崩溃测试**：杀死已挂接的进程。
- **Sanitizer 与编译器**：整个测试集在 ASAN 与 TSAN 下运行，并用 GCC 14 与 Clang 19 构建。

测试清单见 [`docs/test.md`](../docs/test.md)。

## 构建

环境初始化脚本位于 `support/`，日常入口脚本位于 `scripts/`。项目使用 C++23，在 Linux 上需要 **GCC 14 或 Clang 19**。

```bash
sudo ./support/install_build_deps.sh   # GCC 14 与 Clang 19（Ubuntu 22.04 上会添加 toolchain PPA 与 apt.llvm.org）
sudo ./support/install_dev_tools.sh    # 可选：clang-format、clang-tidy、lldb、valgrind
```

使用预设构建与测试：

```bash
./scripts/build/debug.sh   && ./scripts/test/debug.sh
./scripts/build/asan.sh    && ./scripts/test/asan.sh
./scripts/build/tsan.sh    && ./scripts/test/tsan.sh
./scripts/build/release.sh && ctest --preset release
```

在支持 `io_uring` 的原生 Linux 内核上，可以单独构建并测试该路径：

```bash
./scripts/build/io_uring_debug.sh
./scripts/test/io_uring_native.sh
```

`./scripts/build/lsan.sh` 用于准备 LeakSanitizer 构建。在受 ptrace 限制的环境（例如部分 WSL 环境）中 LSAN 可能无法运行，这是环境限制，并非已知的 TelePath 缺陷。

基准测试（`scripts/bench/`）和 Web 控制台使用 release 构建。

## Web 控制台

```bash
./scripts/web/serve.sh
```

该命令启动一个本地 HTTP 服务，可以：

- 提供浏览器界面，支持中英文切换；
- 运行单次基准测试与线程扫描；
- 以帧图展示最终和运行期采样的缓冲池快照；
- 在会话中保留最近的运行记录；
- 通过 `/api/observation?name=/name` 提供实时观测区。

前端本地化资源位于 `localization/web-console/`。命名空间视图计划在阶段 7e 加入。

## 仓库结构

- `src/cpp/include/telepath`：公开头文件（buffer、facade、io、replacer、observation、telemetry、options）
- `src/cpp/lib`：实现
- `test/cpp`：单元、压力、混沌、模型与多进程测试
- `web/backend`、`web/frontend`：Web 控制台与 Python 观测读取器
- `scripts`、`support`：构建、测试、基准与环境脚本

目录归属规则见 [Repository Tree Guide](../docs/tree.md)。

## 文档

- [内核设计与路线图](../docs/design/kernel-refactor.md)
- [并发不变量](../docs/concurrency.md)
- [测试指南](../docs/test.md)
- [CI 指南](../docs/ci.md)
- [实验指南](../docs/experiment.md)
- [Repository Tree Guide](../docs/tree.md)
- [Architecture Mermaid](../docs/mermaid/architecture.mmd)
- [历史文档](../docs/state.md)

> 📜 **许可证**
>
> TelePath 基于 MIT License 发布。
>
> 🎨 **说明**
>
> 封面图来自 "テレパシ (Telepathy)", 为该项目名称的灵感来源，版权归原作者所有。
