# MosaicRV：弹性聚合 RISC-V 处理器规划

正式项目名：**MosaicRV**（Mosaic Processor for RISC-V）。命名含义：多个独立执行 tile 在固定物理硬件上动态分区、租借、聚合，而每个 RISC-V hart 保持精确 architectural semantics。远程仓库登记为 `git@github.com:BaiTian6641/MosaicRV.git`；本轮只配置 remote，不推送。


**当前交付仅为架构审查与实施计划；尚无处理器 RTL、functional prototype、Verilator 运行结果、FPGA bitstream 或实板验证结果。** 本轮按约定不安装工具、不修改处理器代码、不编程硬件。后续目标仍包括完整功能原型、XiangShan 辅助程序化正确性校验、GW5A/Zynq/Virtex UltraScale+ 三家族实板验证，以及可移植 ASIC 转换。

## 阅读顺序

1. [原始文档库存与逐节覆盖](docs/source-inventory.md)：三份原报告，5,447 行、102 个标题，原文保持字节不变；记录所有思想的接受、修正与分期。
2. [架构审查](docs/architecture-review.md)：固定物理结构/动态资源归属、每 hart 精确提交、tag/credit/恢复、内存顺序、RVV、LLB、cohort 与完整 core-fusion 边界。
3. [细粒度实施计划](docs/implementation-plan.md)：86 个 I 工作包；profile、接口、文件布局、未来 CLI、跨文档依赖和 release gates。
4. [验证计划](docs/validation-plan.md)：80 个 V 工作包；锁定的 XiangShan/Difftest/NEMU 候选来源链、Verilator 接入、独立参考、负控制、有限测试集、formal/litmus 与失败复现。
5. [平台及 ASIC 计划](docs/platform-plan.md)：47 个 H 工作包；公共 RAM/clock/reset/CDC 合同，三个独立板级 flow、真实证据协议与 ASIC SRAM/DFT/STA/physical signoff。
6. [一手参考总账](docs/references.md)：49 个来源 ID，primary URL、版本、支持主张与访问限制；不把原报告聊天引用 token 伪造成文献。
7. 九个团队执行指南：[第0阶段](docs/stage-0-contracts-bringup.md)、[第1阶段](docs/stage-1-scalar-control.md)、[第2阶段](docs/stage-2-execution-fabric.md)、[第3阶段](docs/stage-3-memory-system.md)、[第4阶段](docs/stage-4-vector-locality.md)、[第5阶段](docs/stage-5-multihart-aggregation.md)、[第6阶段](docs/stage-6-verification-quality.md)、[第7阶段](docs/stage-7-fpga-hardware.md)、[第8阶段](docs/stage-8-asic-release.md)。每份含用途、设计、Mermaid、微步骤、阻塞、团队进度表和 Track Log。
8. [本轮验证与重跑指令](docs/verification.md)：文档 hash/覆盖、213 个任务字段、九个指南的结构、合并依赖图、引用/链接和 Git 跟踪检查；不包含伪造的 CPU 测试成绩。

## 核心修订

- **XiangShan 是第二 DUT 与工程参照，不是 ISA 金标准。** 使用其 Difftest 生态及已校准 NEMU/Spike/Sail 参考，逐 architectural event 比较；多 hart RVWMO 另做 memory graph/litmus/formal。
- `execution-done`、`value-visible`、`macro-complete`、`retired` 分开；不可停顿 FU 发射前须有真实结果空间。有限 tag、epoch、lease 必须处理 ABA/晚到响应。
- RVV 指令可以部分完成并通过 `vstart` 重启，不套用标量全有全无提交；物理 lane 数变化不改变 architectural VLEN。
- clean LLB 副本仍需 freshness/invalidation 证明；same-PC cohort 仍需每 hart 独立权限、寄存器、异常与提交。
- 先实现可测双 cluster 标量 fabric，再扩展 A/C/特权/FP/RVV/多 hart/cohort/pod/分布式 ROB。后置不是删除最终研究范围；动态性能收益必须允许被实验否定。

## 开始实施前的决策门槛

- 三块实板的完整 part/package/speed grade、board revision、clock/pins、编程接口；Zynq 必须区分 7000 与 UltraScale+ MPSoC。
- exact-part vendor tool/IP/license 与 RAM 行为；部分 Gowin PDF 本轮 HTTP 403、AMD 动态门户未取得完整正文，文档明确限制，不据此猜脚本或时序。
- 工具/OCI 镜像依赖闭包、新核参考配置/ABI；已列 SHA 是来源核实，尚未构建或执行兼容性验证。
- ASIC 的合法 PDK、库/SRAM/IO、PVT、DFT、foundry signoff 与样片资源。

这些是未来任务的显式输入，不阻止本次完整规划交付，也不能被当成已完成的硬件成果。

## Git 与原文

原始报告位于根目录，保持原名与内容。Git 已初始化；原文与规划文档均需由最终跟踪检查纳入 index。首次提交尝试因本机没有 Git author identity 失败；未伪造姓名/邮箱、未修改全局或本地 author 配置。**Git-tracked/staged 不等于已产生 commit**。仓库作者配置由用户自行设置后，可正常提交已审核文档；本轮没有 push。

更新：2026-09-29，首次完整规划审查；包括对两个失败子任务的产物核对和平台文档补完。后续修改应同步更新 source/task/reference coverage，并重跑 [文档检查](docs/verification.md)。
