# 原始文档库存、完整阅读与需求覆盖

状态：原始文件只读保留，新结论在独立规划文档中修订。本次范围只有三份原始 Markdown 报告；合计 5,447 行、102 个一至三级标题。没有把原始聊天引用 token 当成可恢复 URL。

## 1. 来源与字节完整性

| Source | 原始文件 | 行数 | 字节 | SHA-256 |
|---|---|---:|---:|---|
| SRC-01 | [deep-research-report.md](../deep-research-report.md) | 1706 | 43644 | `4de278a43e8cbbf0bcc4ca06841a71a946224fa75c595f9fa4cf462dd6cc0364` |
| SRC-02 | [deep-research-report(1).md](../deep-research-report%281%29.md) | 1213 | 42600 | `ff7a2a005a91d2d9b59602479cadcdc4962ad75b68afac0ee7d627028319c6fc` |
| SRC-03 | [deep-research-report(2).md](../deep-research-report%282%29.md) | 2528 | 57353 | `faf4317a72ba9fbd812eeeea7c1bb09e2f0a1519ce21622799cad608137e74fb` |

原作者/原生成时间/原聊天会话不可从文件可信恢复，不伪造作者、发布日期或 token→URL 对应。文件mtime不作为论文时间。原文只含聊天引用的位置保留原样；新引用账本说明支持哪些修订主张及读取限制，不能反向声称找到原 token 的精确来源。

## 2. 全文阅读责任与范围

- SRC-01：架构审查任务完整读取 raw 1–300、301–600、601–900、901–1200、1201–1500、1501–1706；已保存审查文件，虽任务最终异常退出，不抹掉其实际读取记录。集成责任人另读原型/实验/聚合等范围并阅读全文审查。
- SRC-02：平台研究任务完整读取 raw 1–260、261–520、521–780、781–1040、1041–1213；任务在写平台文档前异常退出，集成责任人读取其记录/研究来源并补完平台计划，未把失败任务标为原稿已完成。
- SRC-03：验证任务完整读取 raw 1–350、351–700、701–1050、1051–1400、1401–1750、1751–2100、2101–2528；验证计划含其逐节处置。集成责任人另读memory/LLB/performance/roadmap与全文验证文档。

这些区间并集覆盖各原文全部行，包括代码块、参数表、实验建议和末尾结论，不是只读heading或摘要。集成后用原始SHA-256检查原文未修改。

## 3. 用户要求到交付与执行任务

| Requirement | 本次文档交付 | 后续实现/验收任务 |
|---|---|---|
| U-01 全部原计划/研究与架构修订 | 本库存 + architecture-review.md | I-001, I-002, I-084 |
| U-02 Git跟踪与准确引用 | references.md + verification.md | I-003, V-001, I-086 |
| U-03 极细粒度实现与functional prototype | implementation-plan.md | I-004, I-023, I-080 |
| U-04 测试/程序化正确性/XiangShan/Verilator | validation-plan.md | V-004, V-005, V-008, V-043, V-075 |
| U-05 Gowin GW5A实板 | platform-plan.md §5 | H-011, H-016, H-017 |
| U-06 Xilinx Zynq实板 | platform-plan.md §6 | H-018, H-024, H-025 |
| U-07 Virtex UltraScale+实板 | platform-plan.md §7 | H-026, H-030, H-031 |
| U-08 移植与后续真实ASIC设计 | platform-plan.md §4/§9 | H-004, H-032, H-036, H-047, I-085 |
| U-09 本轮只规划不实现/不碰硬件 | 各文档状态、verification.md边界记录 | 后续开始实施前另行执行上述工作包 |
| U-10 检查失败子任务 | 本文§2与verification.md恢复记录 | 保存架构草稿、补完平台文档、由集成检查验证全部交付 |

## 4. 原文逐标题覆盖

每行范围从本标题到下一个一至三级标题前；因此不把父标题范围与子标题重复计算。代码块不改变源行号。任务ID指向实施/验证/平台计划，处理意见指出接受、纠正、研究分期或不采纳的理由；不以仅存在链接代替技术审查。详细语义依据见 [架构审查](architecture-review.md)；验证细节见 [验证计划](validation-plan.md)。

### SRC-01

| Source range | 原始标题 | 处置 | Tasks |
|---|---|---|---|
| SRC-01:1-2 | 面向动态资源织构的高性能 RISC-V 微架构研究：从 XiangShan、BOOM、OpenC910 到 SIMT、Core Fusion 与 FPGA 原型 | 保留总体研究目标；不承诺性能收益 | I-001, I-084 |
| SRC-01:3-71 | 结论与总体判断 | 接受逻辑统一物理分布；数量与收益作为假设 | I-023, I-029, I-077 |
| SRC-01:72-73 | 从 XiangShan、BOOM 与 OpenC910 能学到什么 | 比较设计与本项目分离，XiangShan不是语义oracle | I-001, V-005, V-075 |
| SRC-01:74-142 | XiangShan 是最值得作为第一参考对象的设计 | 锁版本，不从阶段文档拷贝容量预算 | V-001, V-002 |
| SRC-01:143-198 | XiangShan 的 Issue Queue 已经接近你的“资源感知调度”入口 | ready与真实资源grant分离 | I-022, I-024 |
| SRC-01:199-230 | BOOM 恰好指出了你的核心研究问题 | 动态RF端口是需验证的优化 | I-015, I-032 |
| SRC-01:231-240 | OpenC910 提供另一个实际 OoO RISC-V 参考点 | 保留SoC分离启示；C910板卡数字不重述为已核实事实 | I-047, H-003 |
| SRC-01:241-244 | 建议的动态执行织构 | 采用有界分层执行织构 | I-023, I-028 |
| SRC-01:245-323 | uOP 应成为真正的内部 ISA | 内部协议不是外部ISA；补attempt/generation/闭合展开 | I-002, I-016 |
| SRC-01:324-396 | Resource Scheduler 应与传统 Issue Scheduler 分离 | 原子准入，拒绝部分占用无限等待 | I-024, I-030 |
| SRC-01:397-476 | Functional Array 最好是 Clustered，而不是完全统一 | 保留local fast path与remote bandwidth | I-027, I-028 |
| SRC-01:477-570 | SIMT、ILP、SMT 与动态聚合应该如何组合 | 区分ILP/SMT/RVV/透明cohort，不混淆架构身份 | I-064, I-067, I-071 |
| SRC-01:571-625 | 不要一开始让每个 SIMT lane 都成为独立 OoO thread | 一个warp ROB不适用独立标准harts；选择per-member commit | I-067, I-068 |
| SRC-01:626-681 | 最好的统一抽象可能是 Context Group | Context Group只共享资源，不合并architectural state | I-064, I-071 |
| SRC-01:682-683 | Unified Memory 与 Unified Write Back 应该怎样实现 | 统一服务与返回；顺序域仍按hart | I-034, I-026 |
| SRC-01:684-764 | Unified Memory 应统一“资源调度”，而不是统一“内存顺序状态” | 保留per-hart LSQ、共享端口 | I-035, I-065 |
| SRC-01:765-796 | XiangShan 很适合观察“为什么 memory 不容易完全统一” | 内存复杂性保留；upstream队列数字不作本机预算 | I-036, I-043, V-018 |
| SRC-01:797-960 | Unified Write Back 很有价值，但要把 Completion 与 Writeback 分开 | 区分执行、值可见、macro complete、retire | I-025, I-026, I-017 |
| SRC-01:961-1005 | Unified Writeback 不等于 Global CDB | 拒绝global CDB，采用分层completion | I-028, I-072 |
| SRC-01:1006-1051 | Core Aggregation 最好理解为资源聚合，而不是完整 Core Fusion | 先借用资源，再完整core fusion | I-066, I-075 |
| SRC-01:1052-1123 | 把 Core 拆成 Context Frontend 与 Backend Resource Tiles | home control与backend分离 | I-064, I-066 |
| SRC-01:1124-1172 | 不应该每周期“无限动态缩放” | 快仲裁慢ownership；必须drain | I-031, I-071 |
| SRC-01:1173-1215 | Resource Fabric 需要 credit-based flow control | credit不足以证明死锁自由，补wait graph | I-024, I-030, V-042 |
| SRC-01:1216-1289 | FPGA 验证路线与建议的第一版参数 | 保留FPGA-first，改为三family独立门槛 | I-080, I-081, H-032 |
| SRC-01:1290-1382 | RTL 数据结构应该围绕 FPGA 做修改 | banked PRF和小IQ，语义跨vendor一致 | I-015, I-032, H-004 |
| SRC-01:1383-1435 | 验证不能只靠程序跑通 | 守恒按attempt/packet/macro分层；程序与不变量联合 | V-027, V-032, V-041, V-042 |
| SRC-01:1436-1453 | 我认为最值得做成论文或真实架构的版本 | 研究贡献作为可证伪假设 | I-077, I-084 |
| SRC-01:1454-1509 | 最有潜力的是“资源虚拟化” | capability来自已存在FU，不创造硬件 | I-024, I-029 |
| SRC-01:1510-1556 | 第二个强点是“Execution 与 Completion 解耦” | execution/completion分离并保证真实结果容量 | I-025, I-026 |
| SRC-01:1557-1632 | 第三个强点是“Core 只是逻辑边界” | logical hart稳定，资源可聚合 | I-064, I-071, I-075 |
| SRC-01:1633-1706 | 但第一代实现应刻意限制动态性 | 小原型不等于删除最终目标 | I-080, I-082, I-083, I-084 |

### SRC-02

| Source range | 原始标题 | 处置 | Tasks |
|---|---|---|---|
| SRC-02:1-2 | 面向动态聚合流水线与 uOP 执行的 RISC‑V 处理器架构研究 | 保留动态聚合目标，建立分阶段确定性验收 | I-001, I-086 |
| SRC-02:3-30 | 执行摘要 | 静态硬件/动态归属；DFX不用于per-uOP | I-031, H-034 |
| SRC-02:31-102 | 设计边界与架构原则 | control/data plane分离且接口固定 | I-002, H-003 |
| SRC-02:103-153 | uOP 应成为真正的内部协议 | 补macro/uop/attempt与side-effect授权 | I-002, I-016, I-034 |
| SRC-02:154-173 | “动态流水级数”最好改成 elastic latency | 改称variable effective latency | I-027, I-028 |
| SRC-02:174-208 | XiangShan 与参考高性能 RISC‑V 核的比较 | 参考核数字需版本；不移植未经证明结论 | V-001, V-005 |
| SRC-02:209-210 | uOP、Rename、队列和动态执行骨架 | 可实施rename/ROB/IQ/steering骨架 | I-013, I-016, I-022 |
| SRC-02:211-263 | Rename 与 ROB 不要一开始分布化 | 首版central per-hart control | I-013, I-018 |
| SRC-02:264-299 | 第二代再考虑 Hierarchical ROB | 后续distributed ROB有独立证明与成本gate | I-074 |
| SRC-02:300-345 | Queue 不要做一个巨大 Unified IQ | 小队列+真实backpressure | I-022, I-030 |
| SRC-02:346-388 | Dispatch 可以变成 cost-based routing | 先确定性locality/load策略再predictor | I-029, I-063 |
| SRC-02:389-390 | 三种具体动态聚合流水线方案 | 三方案分阶段保留，非一次性全加 | I-023, I-066, I-075 |
| SRC-02:391-447 | 弹性 Clustered OoO | 双cluster可执行对照基线 | I-023, I-080 |
| SRC-02:448-514 | Distributed Dataflow / Completion 架构 | completion守恒与fanout ack明确 | I-025, I-026, V-032 |
| SRC-02:515-665 | Aggregate Tile + SIMT-like Cohort | same PC不充分；cohort成员独立state | I-066, I-067, I-069 |
| SRC-02:666-667 | 内存、一致性、分支预测与统一写回 | 内存顺序/coherence/预测/WB分开验收 | I-021, I-026, I-065 |
| SRC-02:668-727 | Unified Memory 最好是服务层，而不是统一 L1 | 统一服务层而非巨型统一cache | I-042, I-043, I-062 |
| SRC-02:728-766 | RVWMO 与 cache coherence 是两回事 | RVWMO不同于coherence，独立合法结果检查 | I-065, V-065, V-066 |
| SRC-02:767-834 | Unified Write-Back 是最大潜在陷阱之一 | 不可停顿FU要真实result credit | I-024, I-025, I-026 |
| SRC-02:835-871 | Branch predictor 是动态宽机器的供给瓶颈 | 保留常规BPU与恢复，不任意fabric化前端 | I-021 |
| SRC-02:872-910 | FPGA 原型路线与实验设计 | 不指定用户未选板，不继承假设频率 | H-001, H-009, H-032 |
| SRC-02:911-931 | 原型步骤 | 分解为implementation/validation/platform三DAG | I-080, I-081, I-082, I-083 |
| SRC-02:932-981 | 测试 workload 应专门针对假设 | 采用针对假说的有限实验矩阵 | I-077, I-078, I-079 |
| SRC-02:982-1032 | 关键指标不要只收 IPC | IPC/useful work/实际clock/area分列，功耗要测 | I-076, H-033 |
| SRC-02:1033-1070 | Partial Reconfiguration 应放到最后 | DFX为独立coarse-grain决策gate | H-034 |
| SRC-02:1071-1213 | 行动建议与最终架构判断 | 保留H1/H2/H3与advanced fusion，负结果可交付 | I-077, I-079, I-084 |

### SRC-03

| Source range | 原始标题 | 处置 | Tasks |
|---|---|---|---|
| SRC-03:1-2 | 面向 RISC-V / RVV 的动态聚合处理器架构研究：从高 IPC CPU 到 GPU-like SIMT 的弹性执行 Fabric | 保留MosaicRV（原EAF-V）完整目标而非神奇GPU加速；新增optional lockstep不改变原ISA目标 | I-001, I-084, I-087 |
| SRC-03:3-54 | 研究结论与架构定位 | 论文数字与本设计结果分开，primary追溯 | I-077, I-078 |
| SRC-03:55-56 | 从 XiangShan 与现有 RISC-V 设计推导新的 Pipeline | 顺序control plane+弹性backend | I-002, I-017 |
| SRC-03:57-129 | 不要把 Frontend 也完全 Fabric 化 | latency-sensitive frontend保留常规结构 | I-009, I-021 |
| SRC-03:130-229 | uOP 不应该只是传统 CPU 的 micro-op | 有界metadata与身份生命周期 | I-002, V-008 |
| SRC-03:230-281 | 关键不是一个 Global Scheduler，而是 Scheduler Hierarchy | 局部调度和分层broker | I-022, I-030, I-072 |
| SRC-03:282-323 | Dynamic scaling 必须有两个时间尺度 | 准入仲裁与ownership重配分时间尺度 | I-031, I-071 |
| SRC-03:324-369 | Pipeline depth 因此自然变成 Variable，而不是硬做 Variable Pipeline | 固定路径上可变延迟，不动态制造寄存器级 | I-027, I-028 |
| SRC-03:370-395 | RVV 与 CPU—GPU 连续体 | 保留RVV/SMT/cohort不同工作点 | I-051, I-071 |
| SRC-03:396-448 | RVV 应该成为 Fabric 的第一等公民 | RVV完整能力验收后发布 | I-051, I-082 |
| SRC-03:449-539 | 不要把一个 RVV instruction 完全展开成数百个 ROB uOP | descriptor保存element progress，ROB不按element爆炸 | I-051, I-057 |
| SRC-03:540-576 | RVV 和 SIMT 不能简单画等号 | RVV不是多hart SIMD，cohort不是单hart vector | I-064, I-067 |
| SRC-03:577-663 | Dynamic Cohort Fusion | 修正same-PC eligibility与成员独立fault | I-067, I-069 |
| SRC-03:664-726 | 但是这里存在一个不可消除的极限 | 拒绝凭空产生软件并行度 | I-079, I-084 |
| SRC-03:727-789 | 我建议定义五个 Execution Personality | 各personality保留，改变资源不改变ISA | I-071 |
| SRC-03:790-807 | Memory Fabric 与 Per-Lane Cache 的重新设计 | LLB/shared-memory/coalescing分开验证 | I-060, I-061, I-062 |
| SRC-03:808-888 | 为什么完整 per-lane cache 容易失败 | full per-lane coherent cache仅作独立有成本对照 | I-078 |
| SRC-03:889-952 | Lane Locality Buffer | LLB是受freshness约束的微结构 | I-060 |
| SRC-03:953-985 | LLB 最适合作为非 coherent microarchitectural structure | 纠正clean副本仍需coherence/invalidation | I-060, I-065 |
| SRC-03:986-1041 | 不应该简单做“预测 LLB 或 L1 二选一” | predictor只影响性能，错误不能绕过权限 | I-063 |
| SRC-03:1042-1123 | Memory Operation 也应该成为 Packet，而不只是 uOP | memory packets保留每element错误和身份 | I-056, I-061 |
| SRC-03:1124-1160 | 更进一步：Cross-Hart Coalescing | 跨hart合并须有合法observation/permissions | I-070 |
| SRC-03:1161-1199 | Memory Fabric 可以做三层调度 | address/transaction/QoS三层有边界 | I-056, I-061, I-062 |
| SRC-03:1200-1256 | Critical Load 不应该排在 100 个 GPU-like Load 后面 | critical优先需aging与bulk进展 | I-062, V-033 |
| SRC-03:1257-1272 | Ara-Opt 给这个方向提供了非常强的证据 | Ara-Opt摘要支持研究方向，未复现收益 | I-063, I-078 |
| SRC-03:1273-1274 | Dynamic Execution、Unified Writeback 与 Core Aggregation | 分布completion+per-hart control | I-026, I-064 |
| SRC-03:1275-1342 | Unified Writeback 的概念是对的，但不能是一根 Bus | 不是一根统一WB总线 | I-025, I-026, I-072 |
| SRC-03:1343-1424 | Result Locality 应成为 Scheduling 的第一等指标 | locality加入有界策略与对照 | I-027, I-029 |
| SRC-03:1425-1488 | 从 Giant ROB 转向 Per-Hart Commit Domain | 每hart退休顺序，不建全局指令ROB | I-064, I-074 |
| SRC-03:1489-1536 | Core 也不应该消失，而应该变成 Commit / Control Domain | core是control domain，borrow不迁移hart | I-066, I-075 |
| SRC-03:1537-1595 | 真正可扩展的单位应当叫 Slice 或 Tile | slice/pod分层而非无限crossbar | I-072, I-073 |
| SRC-03:1596-1669 | IPC、Throughput 与这类架构真正能赢在哪里 | IPC不是vector useful work/sec | I-076, I-078 |
| SRC-03:1670-1734 | 传统 High-IPC CPU 的根本问题之一是资源被静态 provision | 资源ownership可变的收益需实验 | I-031, I-077 |
| SRC-03:1735-1818 | 但是 Dynamic Fabric 本身也可能让 IPC 更低 | 承认远端延迟可能更差，保留local island | I-027, I-077 |
| SRC-03:1819-1877 | 可以做 Execution Locality Predictor | locality predictor可关闭且不得参与正确性判断 | I-029, I-063 |
| SRC-03:1878-1928 | Branch Prediction 不应该被过度“Fabric 化” | BPU+年龄化flush，older response不误kill | I-018, I-021, V-030 |
| SRC-03:1929-1996 | 这里最大的创新点其实可能是 “Elastic Window” | 压缩metadata不放弃宏指令顺序与有界inflight | I-016, I-051, I-074 |
| SRC-03:1997-2016 | FPGA 原型与验证路线 | 先scalar再全研究路线，三板+ASIC有独立证据 | I-080, I-081, H-047 |
| SRC-03:2017-2077 | 最小原型应该是 Scalar Elastic Backend | 真正双cluster scalar RTL，不以解释器替代 | I-023, I-080 |
| SRC-03:2078-2128 | 下一版加入 RVV Lane Fabric | 固定VLEN，2/4/8物理lane分配 | I-059, I-078 |
| SRC-03:2129-2171 | 然后验证 Lane Locality Buffer | LLB实验必须先通过coherence gate | I-060, I-078 |
| SRC-03:2172-2234 | 再加入多 Hart 与 Cohort Fusion | 保留multihart与cohort split/rejoin | I-064, I-069 |
| SRC-03:2235-2270 | 最后才做 Pod Aggregation | 先coarse inter-pod，再考虑细粒度成本 | I-072, I-073 |
| SRC-03:2271-2346 | XiangShan 的工程方法很适合借鉴 | XiangShan第二DUT；Difftest/reference是真实校验路径 | V-005, V-075 |
| SRC-03:2347-2528 | 推荐的最终 EAF-V 架构 | 所有核心原则进入MosaicRV最终实现与验收gate；lockstep作为可选安全profile独立验收 | I-082, I-083, I-084, I-085, I-091 |

## 5. 未继承为事实的原报告断言

XiangShan queue/PRF/FU/ROB具体数值与版本绑定不足；PULP-C910/BOOM/RSD/CVA6/Vortex/Ventus/FireSim及Ara2/AraXL/Spatz/MemPool/TeraPool的原文数字与特定板卡结果，未在本轮逐项复现或全量核验，不作为本机预算、性能承诺或成功证据。相应比较思路保留为背景，实施不依赖其数值正确。Ara-Opt/SEAM-V仅核到primary摘要主张，Core Fusion/TRIPS语义差异已在架构审查限定。每条性能假说最终依靠本项目等资源、实际时钟、正确性先行的实验。

## 6. 后续新增需求：optional lockstep

用户在初次三份报告之外明确提出 lockstep。该需求不改变原始文档范围：在 [architecture-review.md](architecture-review.md) §11.1 中定义为可选安全 profile，由 I-087–I-091 实现、V-081–V-084 验证、H-048/H-049 完成三FPGA/ASIC物理边界。它不是原报告的隐藏假设，也不是默认处理器模式；DCLS检测与TMR纠错分开，lockstep pair不是第二个 architectural hart。
