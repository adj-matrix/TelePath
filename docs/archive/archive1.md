# 选题意义

理论与工程价值：
在现代数据密集型系统中，缓冲池位于内存与外存之间，是决定系统吞吐能力、访问延迟与资源利用率的关键基础组件。随着多核 CPU、NVMe SSD 与分层存储设备的普及，传统教学型缓冲池在并发访问、异步写回和观测能力方面逐渐暴露出局限：一方面，其实现往往偏向功能正确性验证，难以反映现代存储环境下的真实瓶颈；另一方面，系统内部状态通常以黑盒形式存在，开发者难以对缺页、淘汰、刷盘、锁竞争与脏页累积等关键行为进行持续观测和归因分析。

本课题以数据库系统中的 Buffer Pool 为研究对象，但有意识地剥离 SQL、事务、执行器等复杂上层模块，聚焦于底层页面缓存、并发控制与存储 I/O 调度问题，力图构建一个兼顾工程可实现性与系统研究价值的实验平台。项目当前以 C++ 存储引擎内核为主体，在此基础上引入可观测性设计，强调“数据面与观测面解耦”，使遥测能力在架构层面成为内生能力，而非调试阶段临时附加的日志手段。

本课题的特色与创新点主要体现在：
1. 面向现代存储环境的并发缓冲池设计：围绕页面级缓存、替换策略、缺页协调、异步写回和脏页管理构建轻量级实验内核，作为理解现代数据库存储子系统的重要基础。
2. 可观测性优先的架构思路：在系统设计阶段预留遥测接口与状态导出路径，使后续控制台、仪表盘或图形化可视化界面能够在不侵入数据面热路径的前提下获取关键运行状态。
3. 强调从“可运行”到“可验证”的系统实现：不仅实现并发 Buffer Pool 本身，还通过缺页协同、完成事件分发、后台清理与回归测试机制提升系统的内部一致性和可实验性。
4. 面向后续扩展的研究平台属性：为共享内存 IPC、异构延迟模拟、自动化调优与监控平面留出清晰接口，使本项目既能够完成毕业设计目标，也具有继续演进为长期开源基础设施的潜力。

# 国内外研究现状概述

理论研究：
缓存置换算法一直是数据库系统与存储系统中的经典研究问题。从基础的 LRU 到适用于数据库工作负载的 LRU-K [1]，再到近年来围绕分层内存、异构存储和机器学习辅助缓存管理的研究 [7-10][15][17]，学术界始终在探索如何在容量、延迟与并发访问压力之间取得更优平衡。与此同时，高并发数据系统的研究重点也逐渐从粗粒度串行化转向更细粒度的同步控制、锁竞争削减与异步 I/O 组织方式优化 [3][6][11]。

技术实现现状：
在工业界与教学系统中，基于页的缓冲池管理仍然是主流架构。MySQL InnoDB 等成熟系统围绕缓冲池已经形成复杂的并发控制和刷盘策略 [4]；Bustub 等教学系统则为现代 C++ 数据库内核实验提供了清晰的接口范式 [5]。然而，前者实现复杂度极高、可裁剪性有限，不利于本科毕业设计在有限时间内构建可控实验平台；后者更强调课程教学与模块练习，在写回策略、完成事件协调、后台清理和系统可观测性方面通常保留了较多简化假设。

随着 NVMe SSD 普及与 Linux 异步 I/O 机制演进，现代存储设备已能提供远高于传统磁盘时代的并发能力 [6]。这使得缓冲池不再只是一个简单的内存缓存容器，而需要在页面访问、缺页安装、脏页写回和后台清理等方面体现更精细的组织能力。与此同时，数据库与系统软件领域越来越重视可观测性、性能归因与配置适配问题 [12]。相比“只看最终吞吐量”的粗粒度评估，能够解释系统为何出现瓶颈、瓶颈出现在何处、以及不同策略如何影响内部状态变化，正成为系统研究的重要要求。

本课题综合吸收上述研究思路，但采取更务实的实现路线：以轻量级单机缓冲池内核为主体，优先完成并发控制、异步式 I/O 边界、写回路径、后台清理与观测接口的基础架构，再在后续阶段扩展共享内存状态导出、控制台/GUI 可视化、异构延迟注入与自动调优等能力。这样既保持研究方向与“可观测存储内核”的原始目标一致，也避免在本科毕设阶段因系统边界过大导致实现目标失焦。

# 主要研究内容

本课题围绕“高并发缓冲池核心 + 可观测接口 + 后续扩展观测平面”的整体思路展开，具体包含三个核心模块：

C++存储引擎内核（Core Engine）：
异构 I/O 适配：
1) 设计磁盘管理抽象层，提供基于页（Page）的读写接口，并以统一的异步式提交/完成边界组织底层 I/O。
2) 在当前阶段完成稳定的 POSIX 后备后端，并实现面向支持环境的 `io_uring` 后端路径，为后续更深入的原生异步 I/O 优化提供实验基础。
3) 面向后续研究保留异构延迟模拟与软件定义延迟注入能力，以支持构造更稳定、可重复的性能实验环境。

并行缓冲池核心：
1) 设计并实现页面粒度的缓冲池管理机制，重点解决页表访问、Pin/Unpin、缺页装入与淘汰安装过程中的线程安全问题。
2) 实现 LRU、LRU-K 与 Clock 等页面置换算法，并在页面映射表与替换器交互过程中保证并发语义正确。
3) 针对同一页面被多个线程同时请求的场景，引入显式缺页协调机制，以减少重复装载和竞争性安装。
4) 设计集中式完成事件分发路径，避免磁盘完成事件被不同等待者无序消费，增强异步读写过程中的一致性。
5) 构建异步写回调度子系统，支持前台显式刷盘、后台写回、脏页重新置脏处理、批量提交与失败传播。
6) 设计后台清理机制，根据脏页水位和可淘汰状态触发预清理，为高并发缓冲池提供更稳定的写回行为。
7) 在内核中保留可调参数接口，如页表分片数、刷盘工作线程数、批量提交上限与脏页高低水位，为后续性能实验和自动化调优提供基础。

内核状态导出机制（State Exporter）：
1) 在缓冲池关键路径中保留轻量级遥测埋点接口，用于记录命中、未命中、磁盘读写、淘汰与脏页刷盘等核心事件。
2) 当前阶段先完成进程内 Telemetry Sink 与计数器快照能力，用于验证观测接口的低侵入性与可集成性。
3) 后续阶段拟实现基于 Shared Memory、Socket 或无锁环形队列的状态导出机制，将关键运行状态从 C++ 内核导出到独立监控平面，同时尽量避免阻塞数据面热路径。

Python可视化分析台（Visual Dashboard）：
1) 本模块仍作为项目的重要后续方向保留，其目标是构建面向缓冲池内部状态的监控与分析平面。
2) 具体实现形式不限定为 PySide，也可以根据工程适配性采用控制台、TUI、Web Dashboard 或桌面 GUI 等方案；核心目标是不改变“观测面独立于数据面”的总体架构。
3) 后续可视化内容包括：缓冲池帧状态看板、冷热分布、缺页与淘汰生命周期、脏页积压情况、刷盘任务队列、吞吐与命中率曲线等。
4) 在条件允许的情况下，可进一步扩展锁竞争热点展示、后台清理活动轨迹、以及参数切换前后的策略对比分析。

系统集成与并发测试：
1) 编写多线程测试程序与基准工作负载，对并发读取、同页竞争、异页并行、淘汰、刷盘与失败恢复路径进行验证。
2) 基于基准测试与回归测试评估不同替换算法、不同并发配置和不同刷盘策略下的命中率、吞吐量与稳定性。
3) 在后续阶段结合状态导出与可视化平面，进一步构建面向锁竞争、脏页累积和写回行为的可解释实验环境。

实验预期数据与评估指标：
本研究计划从“功能正确性、并发行为与策略效果”三个层面展开定量实验，预期包括以下指标：
1. 吞吐量对比图：比较不同线程数、不同缓冲池配置和不同置换策略下的访问吞吐量变化。
2. 命中率与未命中率统计：观察负载变化、热点变化与缓存容量变化对替换效果的影响。
3. 脏页与写回行为统计：分析前后台刷盘调度、批量写回与后台清理机制对脏页积压和淘汰压力的影响。
4. 正确性与鲁棒性验证：通过失败路径、同页缺页协调、重置脏竞争和等待/淘汰交错等测试验证系统一致性。
5. 后续条件具备时，再补充状态热图、事件流视图和更细粒度的尾延迟观测结果。

图1 基于 LRU-K 算法的页面访问与置换流程

图2 系统架构图

如图2所示，本系统总体上采用“数据面 + 观测面”的解耦思路。C++ 内核负责高并发页面管理、替换与刷盘调度；观测面通过遥测接口与状态导出机制获取运行时信息，并在后续阶段由控制台或可视化界面负责展示。

# 拟采用的研究思路

技术路线：
内核开发：采用现代 C++17 与 CMake 构建项目，围绕 `BufferManager`、`DiskBackend`、`Replacer` 与 `TelemetrySink` 等模块组织系统边界。在实现路径上优先保证接口清晰、并发语义正确与测试可复现，再逐步推进异步写回、后台清理、`io_uring` 后端与观测面扩展。

监控开发：监控与可视化平面继续保留，但采用分阶段推进策略。当前阶段先在内核中稳定遥测埋点和快照接口，后续再根据工程可行性实现基于 Shared Memory 或 Socket 的状态导出，并选择合适的控制台、Web 或 GUI 技术栈实现数据展示。

测试方法：使用回归测试与多线程场景测试验证系统正确性，使用 Benchmark 评估不同工作负载、不同线程数与不同配置下的系统行为；对于 `io_uring` 路径，在支持的原生 Linux 环境下开展额外验证。对于异构延迟模拟、可视化与自动调优等后续功能，则采取“先接口预留，后阶段落地”的方式逐步推进。

总的技术路线图如图3所示。

图3 技术路线图

可行性分析：
1. 申请人具备 C++ 与系统课程基础，已能够围绕页面缓存、替换策略、并发控制和测试框架实现可运行原型。
2. 当前项目已完成缓冲池核心、替换器、后端抽象、缺页协调、完成事件分发、异步写回调度、后台清理与基础遥测等关键模块，说明整体技术路线具备可实现性。
3. 课题继续遵循“先完成内核主体，再扩展观测平面”的分阶段策略，有助于在毕业设计时间约束下控制风险，同时保留后续继续完善系统的空间。
4. 项目边界明确，不涉及 SQL 执行、事务管理与恢复系统等复杂上层模块，能够将实现重点集中在底层缓存、并发与存储调度问题上。
5. 对于 Shared Memory、Socket IPC、可视化界面、异构延迟模拟与自动调优等功能，当前阶段可先完成设计预留与接口规划，后续依据时间安排逐步实现，不会破坏现有核心架构。

# 工作进度安排

阶段	起止日期	任务	提交的阶段成果
1	1月15日—3月14日	系统调研、文献查询、确定总体方向	调查报告、论文综述、初步架构方案
2	3月15日—3月21日	系统分析，明确缓冲池、替换器、后端抽象与观测接口边界	系统业务模型、功能模型、数据模型
3	3月22日—3月31日	完成并发缓冲池核心的主体设计与实现	Buffer Pool 原型、替换算法、测试基础
4	4月1日—4月25日	完善缺页协调、完成事件分发、异步写回与后台清理机制	经过回归测试的核心源程序
5	4月26日—5月16日	开展基准测试、文档整理与观测面接口规划，视进度补充状态导出或轻量监控原型	系统程序、测试结果、设计说明
6	5月17日—5月23日	整理撰写论文，归纳系统设计、实现与实验结果	论文初稿
7	5月24日—5月30日	修改完善论文与补充图表说明	论文终稿
8	5月31日—6月6日	准备答辩材料、演示说明与后续工作总结	答辩材料

# 参考文献目录
[1] O'Neil E J, O'Neil P E, Weikum G. The LRU-K page replacement algorithm for database disk buffering [C] // Proceedings of the 1993 ACM SIGMOD International Conference on Management of Data. Washington D C: ACM, 1993: 297-306.
[2] Hellerstein J M, Stonebraker M, Hamilton J. Architecture of a database system [J]. Foundations and Trends in Databases, 2007, 1(2): 141-269.
[3] Leis V, Haubenschild M, Kemper A, et al. LeanStore: In-memory data management beyond main memory [C] // 2018 IEEE 34th International Conference on Data Engineering (ICDE). Paris: IEEE, 2018: 185-196.
[4] MySQL. MySQL 8.0 Reference Manual: The InnoDB Buffer Pool [EB/OL]. https://dev.mysql.com/doc/refman/8.0/en/innodb-buffer-pool.html/.
[5] Pavlo A. Bustub: The BusTub Relational Database Management System (Educational) [EB/OL]. https://github.com/cmu-db/bustub/.
[6] Haas G, Leis V. What modern NVMe storage can do, and how to exploit it: High-performance I/O for high-performance storage engines [J]. Proceedings of the VLDB Endowment, 2023, 16(11): 2090-2102.
[7] Leis V, Bernhard A, Haubenschild M, et al. Virtual-memory assisted buffer management [C] // Proceedings of the 2023 ACM SIGMOD International Conference on Management of Data. Seattle: ACM, 2023: 1505-1518.
[8] Hao X, Li Z, Leis V, et al. Towards buffer management with tiered main memory [C] // Proceedings of the 2024 ACM SIGMOD International Conference on Management of Data. Santiago: ACM, 2024: 506-519.
[9] Yao F, Yang X, Gong S, et al. GeoLayer: Towards Low-Latency and Cost-Efficient Geo-Distributed Graph Stores with Layered Graph [R/OL]. arXiv preprint arXiv:2509.02106, 2025.
[10] Yamada H, Kitsuregawa M, Goda K. LakeHarbor: Making Structures First-Class Citizens in Data Lakes [C] // 2024 IEEE 40th International Conference on Data Engineering (ICDE). Utrecht: IEEE, 2024: 1-12.
[11] Ravella C S, Faleiro J M, Minhas U F, et al. Optimized locking in SQL Azure [C] // 2024 IEEE 40th International Conference on Data Engineering (ICDE). Utrecht: IEEE, 2024: 2977-2989.
[12] He H, Gao Y, Li Z, et al. When database meets new storage device: Understanding and exposing performance mismatches via configurations [J]. Proceedings of the VLDB Endowment, 2023, 16(7): 1712-1725.
[13] Dageville B, Cruanes T, Zukowski M, et al. The snowflake elastic data warehouse [C] // Proceedings of the 2016 International Conference on Management of Data (SIGMOD). San Francisco: ACM, 2016: 215-226.
[14] Feng L. OceanBase: The Fastest Distributed Database for Transactional, Analytical, and AI Workloads [EB/OL]. https://github.com/oceanbase/oceanbase.
[15] Van Aken D, Pavlo A, Gordon G J, et al. Automatic database management system tuning through large-scale machine learning [C] // Proceedings of the 2017 ACM International Conference on Management of Data. Chicago: ACM, 2017: 1009-1024.
[16] Qt Company. Qt for Python (PySide6) Documentation [EB/OL]. https://doc.qt.io/qtforpython-6/.
[17] Kraska T, Beutel A, Chi E H, et al. The case for learned index structures [C] // Proceedings of the 2018 International Conference on Management of Data (SIGMOD). Houston: ACM, 2018: 489-504.
