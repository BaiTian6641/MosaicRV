# MosaicRV：弹性聚合 RISC-V 处理器规划

正式项目名：**MosaicRV**（Mosaic Processor for RISC-V）。命名含义：多个独立执行 tile 在固定物理硬件上动态分区、租借、聚合，而每个 RISC-V hart 保持精确 architectural semantics。远程仓库历史上登记为 `git@github.com:BaiTian6641/MosaicRV.git`；规划文档不代表已推送的处理器实现。

**当前交付仅为架构审查与实施计划；尚无处理器 RTL、functional prototype、Verilator 运行结果、FPGA bitstream 或实板验证结果。** 后续目标是可证伪的高性能/可扩展非传统执行织构：完整功能原型、XiangShan 辅助程序化正确性校验、GW5A/Zynq/Virtex UltraScale+ 三家族实板验证、可移植 ASIC 转换，以及完整的 ratified RVA23U64/S64 mandatory ISA/执行环境与逐项可选能力。动态调度和聚合收益必须以等资源 fixed/dynamic 对照实测，不能从研究论文直接继承数字。


## 阅读顺序

1. [原始文档库存与逐节覆盖](docs/source-inventory.md)：三份原报告，5,447 行、102 个标题，原文保持字节不变；记录所有思想的接受、修正与分期。
2. [架构审查](docs/architecture-review.md)：固定物理结构/动态资源归属、每 hart 精确提交、tag/credit/恢复、内存顺序、RVV、LLB、cohort 与完整 core-fusion 边界。
3. [细粒度实施计划](docs/implementation-plan.md)：98 个 I 工作包；profile、接口、未来 CLI、跨文档依赖、按声明决定的发布门槛及小模型任务交接。
4. [验证计划](docs/validation-plan.md)：90 个 V 工作包；XiangShan/Difftest/NEMU 来源链、Verilator、独立参考、真实故障注入 replay、formal/litmus 与 ISA 正反例。
5. [平台及 ASIC 计划](docs/platform-plan.md)：51 个 H 工作包；公共 RAM/clock/reset/CDC 合同、三个独立板级 flow、物理证据与 ASIC signoff。
6. 十一个团队执行指南：[第0阶段](docs/stage-0-contracts-bringup.md)、[第1阶段](docs/stage-1-scalar-control.md)、[第2阶段](docs/stage-2-execution-fabric.md)、[第3阶段](docs/stage-3-memory-system.md)、[第4阶段](docs/stage-4-vector-locality.md)、[第5阶段](docs/stage-5-multihart-aggregation.md)、[第6阶段](docs/stage-6-verification-quality.md)、[第7阶段](docs/stage-7-fpga-hardware.md)、[第8阶段](docs/stage-8-asic-release.md)、[第9阶段](docs/stage-9-lockstep-safety.md)、[第10阶段](docs/stage-10-rva23-security.md)。每份含设计、依赖、可交接任务卡、阻断规则与 Track Log；可选 profile 的卡片不能反向阻断 p0。
7. [文档检查与重跑指令](docs/verification.md)：239 个任务（98 I、90 V、51 H）、11 个指南、原文 hash/覆盖、合并依赖图与引用/链接/Git 检查；不包含 CPU 功能/板测/ASIC 成绩。



## 核心修订

- **最终 ISA 目标是完整 RVA23S64，而非 RV64IM/RVV 部分实现。** RVA23S64 继承 U64 mandatory：完整 V（VLEN≥128）、Zkt/Zvkt 对已实现清单指令的 data-independent latency、coherent PMA/CMO、Supm/Ssnpm 最少 PMLEN=0/7、Sha=H+七项 guest/CSR/translation 子义务。必须按条款对同一构建验实现、负例和软件发现；远程 FU、lease/replay、lane 迁移与两级 VM 不得改变任一 architectural 行为。详见 [RVA23 逐项矩阵](docs/implementation-plan.md) §3.2.1。
- **完整 profile ≠ 实现全部可选扩展，也不存在 ratified “RVA23 Secure” profile。** 2 个 localized、5 个 development、8 个 U expansion、8 个 S expansion 选项均保留 owner；只有本次选定且独立验收的能力才能宣传。Zvkng/Zvksg、CFI、Sv48/Sv57 等只是可选项；AIA/secure boot/RoT 与 Linux 属各自平台/软件声明，不以 Core ISA 验收冒充。主源为 [RVA23 v1.0](https://docs.riscv.org/reference/rva23/v1.0/index.html) 与 [ratified text](https://raw.githubusercontent.com/riscv/riscv-profiles/rva23-rvb23-ratified/src/rva23-profile.adoc)。

- **XiangShan 是第二 DUT 与工程参照，不是 ISA 金标准。** 使用其 Difftest 生态及已校准 NEMU/Spike/Sail 参考，逐 architectural event 比较；多 hart RVWMO 另做 memory graph/litmus/formal。
- `execution-done`、`value-visible`、`macro-complete`、`retired` 分开；不可停顿 FU 发射前须有真实结果空间。有限 tag、epoch、lease 必须处理 ABA/晚到响应。
- RVV 指令可以部分完成并通过 `vstart` 重启，不套用标量全有全无提交；物理 lane 数变化不改变 architectural VLEN。
- clean LLB 副本仍需 freshness/invalidation 证明；same-PC cohort 仍需每 hart 独立权限、寄存器、异常与提交。
- **Lockstep 是可选 profile，不是默认处理器行为。** 一个 main/shadow fault-containment pair 只构成一个 logical hart；DCLS 做检测与阻断，不自动纠错；TMR 投票是后续研究。共享 RAM/cache/clock/bus 必须单独保护或列为残余 common-mode 风险。
- 先实现可测双 cluster 标量 fabric，再扩展 A/C/特权/FP/RVV/多 hart/cohort/pod/分布式 ROB。后置不是删除最终研究范围；动态性能收益必须允许被实验否定。

## 如何把计划交给小模型

1. 从 [实施计划](docs/implementation-plan.md) §3.1 的 `Depends` 与 §3.2 的声明矩阵选**一个已满足前提的任务 ID**，而不是一次派发整个 stage。先确定构建的 p0/p1/p2/p3、RVA23U64/S64 **全部 mandatory**、所选 ratified optional/项目安全组合、可选 Safety，以及是否声称 FPGA/ASIC；未声称的选项保持 `NOT_CLAIMED`，缺任一 mandatory 则整个 RVA23 Core `BLOCKED`，不能把该项标 optional。
2. 把该任务的 `Inputs/Action/Outputs/Pass/Fail` 和对应 stage 指南的前置/微步骤/停止求助条件一并给执行者；冻结输入 hash、接口 owner、有限 case、预期观测和产物目录。大型 I-092/I-093/I-094/I-096/I-083/I-084 按 §3.2.1/§3.3 的**卡内纵切**逐条签收；Sha 子项、Zkt/Zvkt、PMAs/CMO 和 S/U privilege 不得靠一个 Linux boot 或 V crypto KAT 代替。
3. 集成负责人核对回传的程序退出码、逐事件/逐 byte 证据、负控制与覆盖计数；任何不能重放的输入或版本差异返回上游 owner 修复。先闭合 p0 的真实双 cluster + cacheless ordered-memory 功能，再单独宣称 Linux/RVV/多 hart、三家族 FPGA、RVA23 和 ASIC。**P0 发布不是全项目完成**，core fusion、scale/性能实验与商业目标仍需各自完成。

## 开始实施前的决策门槛

- 三块实板的完整 part/package/speed grade、board revision、clock/pins、编程接口；Zynq 必须区分 7000 与 UltraScale+ MPSoC。
- exact-part vendor tool/IP/license 与 RAM 行为；部分 Gowin PDF 本轮 HTTP 403、AMD 动态门户未取得完整正文，文档明确限制，不据此猜脚本或时序。
- 工具/OCI 镜像依赖闭包、新核参考配置/ABI；已列 SHA 是来源核实，尚未构建或执行兼容性验证。
- ASIC 的合法 PDK、库/SRAM/IO、PVT、DFT、foundry signoff 与样片资源。

这些是未来任务的显式输入，不阻止本次完整规划交付，也不能被当成已完成的硬件成果。

## Git 与原文

原始报告位于根目录，保持原名与内容。Git 已初始化；原文与规划文档均需由最终跟踪检查纳入 index。首次提交尝试因本机没有 Git author identity 失败；未伪造姓名/邮箱、未修改全局或本地 author 配置。**Git-tracked/staged 不等于已产生 commit**。仓库作者配置由用户自行设置后，可正常提交已审核文档；本轮没有 push。

更新：2026-09-29，按审查意见细化 p0 cacheless 依赖、Core/Secure/Safety 与物理声明门槛、真实故障重放、pointer masking 起源/设备边界、vector crypto 扩展和小模型交接。原报告保持原样；后续每项实现仍需同步检查 source/task/reference coverage 并重跑 [文档检查](docs/verification.md)。
