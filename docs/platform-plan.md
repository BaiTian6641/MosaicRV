# 可移植实现、三家族实板验证与 ASIC 转换计划

状态：**未来实施工作分解，本轮没有综合、布局布线、下载 bitstream、执行板测或访问 PDK。** 平台研究任务中断后，本文件由集成责任人根据已保存的原文阅读记录与一手资料补完；不把子任务失败当作成功。处理器协议和 profile 见 [implementation-plan.md](implementation-plan.md)，正确性及 reference 见 [validation-plan.md](validation-plan.md)。

## 1. 必须分别完成的三个物理目标

| 目标 | 已知 | 实施前必须补齐 | 不能偷换的验收 |
|---|---|---|---|
| Gowin GW5A / Arora V | 用户指定 family；Gowin 官方 EDA/Programmer 与该器件文档是优先路径 | 完整 part/package/speed grade、开发板/自制板 revision、BSRAM/SSRAM/DSP/PLL 数量、RAM IP 支持、工具版本与许可证、JTAG/UART/电源 | 不能假定 Yosys/nextpnr 支持所选 GW5A 全流程；不能用 GW1N/GW2A 结果替代 |
| Xilinx Zynq | 用户只给 Zynq 名称 | 明确 Zynq-7000（7-series PL）还是 Zynq UltraScale+ MPSoC、完整 part/board、PS 是否参与 clock/reset/DDR/加载 | PL 中的本项目 RISC-V core 必须执行；ARM PS 自己输出 PASS 或启动 Linux 不算 |
| Xilinx Virtex UltraScale+ | 用户指定 family，不是某一块 VCU118 | 完整 part/board、SLR 分布、DDR/板载 oscillator、configuration/JTAG/UART、Vivado/IP license | 不自动指定 VCU118/XCVU9P，更不能把 Zynq UltraScale+ 当第三份 Virtex evidence |

共同首轮 profile 是 p0 ISA，保留真实双 cluster 与动态路径。较小板允许缩 ROB/PRF/队列、改变合法 clock/pipeline 参数，不改变外部 ISA。每个 family 都先 **on-chip RAM + boot ROM + UART/result window**，避免把 DDR/PS 问题与 core 首次正确性混在一起；再增加所需 DDR、cache、p1/p2/p3。若完整 p2/p3 无法在某板容纳，记录哪一 profile 未达 gate，而不是把 p0 的结果扩张为全部处理器已验证。

### 1.1 板卡冻结表

H-001 输出每板一行：`board_id, family, full_part, package, speed_grade, silicon_revision, board_revision, schematic_hash, pinout_hash, clock_source_hz, reset_polarity, rail_voltages, bank_voltages, boot_mode, jtag_chain_id, programmer_serial, uart_device_id, tool_release, license_features, memory_map, config_hash`。原理图与专有文档若禁止再分发，只记录可合法获取的 URL、版本、hash 与访问条件，不提交受限 PDF/许可证/密钥。

数字门槛不能从 SRC-02 的 75–150 MHz 或 LUT 百分比表复制。首次实现前由器件/板卡约束确定所需时钟与资源上限；完整实现至少满足：全部 intended timing paths 已约束，setup/hold 总负 slack 为零或各 tool 等价判据满足，未布通 nets=0，未解释 critical DRC/CDC=0，实际资源不超过目标器件且留给已声明系统 IP 的容量真实存在。放宽频率必须改 profile/实验目标并重新验证，不能用 false path 掩盖真实同步路径。

## 2. 共用 RTL 与厂商边界

- `rtl/common/core/fabric/vector/soc` 内不实例化 Gowin/AMD primitive、不使用 FPGA INIT 属性决定 architectural reset、不依赖 vendor IP 自动初始化。
- `platform/gowin/`、`platform/amd/`、`platform/asic/` 只负责 RAM/clock/reset/IO/PHY/配置/测试接口；wrapper 保持同一可观察协议。
- PRF/VRF/cache 使用同步读 RAM。宽度、深度、读延迟、byte enable、output register、read-during-write/mixed-port collision 都是明确参数或禁止条件；同址写冲突必须仲裁，不依赖模拟器/器件碰巧输出。
- SRAM/BRAM data array 不进行全量 reset；重置 valid/owner/ECC 状态，只有合法初始化/写过的数据可被读取。boot image 初始化文件有 hash，并验证 bitstream 内映像与 host manifest 一致。
- 首轮单 core clock，enable 用同步 clock-enable/数据门控，不以逻辑 AND/OR 自造 clock。PLL lock 与 reset release 每域同步；增加 DDR/UART/JTAG 域时用成熟 synchronizer/async FIFO/handshake。
- AXI master 的 `VALID` 不等待 `READY` 才首次断言；stall 时 address/data/control 保持，write address/data 独立 channel；response IDs 和 errors 回到原请求。AXI transport ordering 不自动等于 RVWMO。[HR-008]
- PS/DMA/JTAG host 修改 shared RAM 要有 ownership handoff/cache maintenance。不能假定非 coherent AXI 端口提供 cache coherence；若选 coherent 端口，实际 shareability/cache attributes 与系统配置必须匹配。
- FPGA 功能签名通过不能证明 ASIC SRAM timing、reset/CDC、clock gating、功耗或 manufacturing test。每一技术转换重跑接口等价与物理门槛。

### 2.1 命令的证据等级

以下是官方工具已提供的命令名/调用模式，不是本项目已创建的脚本或本轮已执行结果；版本和 flags 必须由 H-002/H-022 在安装环境的 help 再确认。[HR-010]

```sh
vivado -version
vivado -mode batch -source platform/amd/build.tcl -tclargs config/boards/selected.json
```

`build.tcl` 与 `selected.json` 是后续任务产物。Vivado 脚本至少生成 `check_timing`、`report_timing_summary`、`report_utilization`、`report_drc`、`report_cdc`、`report_clock_interaction` 与 implementation status 的报告；命令接受的参数使用所锁版本的帮助，失败即停止，不吞 Tcl error。不能对空/open错误 checkpoint 生成报告后算成功。

Gowin 搜索索引提到 `gw_sh`/`run all`，但本次 SUG918/Tcl PDF 获取返回 HTTP 403；因此本文件**不把猜测脚本当 source-verified 可执行例子**。H-011 从已安装且有合法访问的 SUG100/命令手册和 GUI 导出流程，锁定项目与 Tcl 命令；最初可用明确的 GUI 动作完成 synthesis→place/route→timing→bitstream，再把同一参数导出为 batch flow。GW5A RAM 具体 collision 行为同样必须取所选 part 对应 UG300/数据表，不从其他 family 推断。

本项目未来 `make sim`/`make unit`/`tools/verify.py` 接口沿用 implementation-plan。实板 host runner 在 H-008 定义，不在本轮创建；它必须能逐 case 验证输出并以非零区分 mismatch、timeout、设备/镜像身份错误。

## 3. 共用硬件证据与有限验收集

每板 evidence bundle 至少包含：Git commit/index snapshot hash、tool/IP/license capability、完整 part、constraints、seed、build argv/GUI-exported settings、synthesis/P&R/STA/DRC/CDC reports、bitstream hash、boot image/ELF hash、programmer readback/ID、reset记录、原始 UART/host logs、每 case expected/actual signature、结束码、timeout、可复现命令。敏感 license key 不归档，保留 feature/status 证据即可。

首轮板测计划 `board-common-v1`：由 V-036 物化精确 manifest；采用 12 个 bare-metal 程序×3 输入，加 16 个 fabric/exception/recovery stress 程序；每个程序有 reference 签名与足够小的 common RAM footprint。合计 52 个 case，每板每次 cold boot 全部执行；3 次 cold-power cycle + 10 次 warm reset，各次均核对 build ID 和全部结果。budget 由每 case 的验证周期上界与该板实际时钟/UART 协议计算，并设总超时；不是无限等待。

负控制至少三项：host expected signature 故意改变以验证比较链路；加载带真实 ALU/branch/store 故障的单独 diagnostic bitstream 并证明相应 workload 捕获；加载与 manifest 不同 build ID 确认拒绝。第一项只证明 host checker，不能替代第二项对 processor 错误的检测。故障映像明确标记、单独目录/串口 banner，完成后恢复已知良好映像，不能留在默认 boot flash。

多平台比较固定同一 ELF/input/ISA 的可运行交集；允许 target clock 不同，报告实际 clock、cycles、wall time 和 workload 单位，不用 Verilator wall time 比 FPGA performance。板级电源/温度/电流只有实际仪器、测量点、采样与基线齐全才报告功耗；vendor power estimator 标为估计。

## 4. 共用移植工作包

### H-001 — 确认三板身份与电气边界
- Depends: none
- Inputs: 用户三家族目标、实板/原理图/part marking、板厂手册。
- Action: 为三板分别填 §1.1 冻结表，核对电压、JTAG chain、reset/clock/UART pins 与 boot mode；Zynq 必须选择 7000 或 MPSoC 分支。
- Outputs: 三份 board manifest、合法访问文档清单、未提供项的阻断记录。
- Pass: 每个必需字段有来源且器件/板版本一致；接线电平无未决风险。
- Fail: 只填写 family、猜测 package/pin、电压不明或把示例板当用户实板。
- Covers: GW5A, Zynq, Virtex UltraScale+, hardware decision gates。
- Sources: HR-001, HR-003, HR-005, HR-006。

### H-002 — 冻结工具、器件支持与许可
- Depends: H-001
- Inputs: board manifests、Gowin/Vivado 安装介质与授权条件。
- Action: 在未来授权环境核对 release 支持 exact part、RTL language、IP 和 programming 功能；记录安装 hash、版本和 license feature，锁工具但不提交密钥。
- Outputs: per-family tool-lock、license/part support evidence。
- Pass: 目标 part 可在工具中选择且 required IP 合法可用；container/host 依赖可复现。
- Fail: 假定免费版涵盖所有 UltraScale+，或用另一 part 完成综合代替目标。
- Covers: reproducible vendor flow, legal prerequisites。
- Sources: HR-001, HR-002, HR-010。

### H-003 — 冻结 common top 与 wrapper 接口
- Depends: H-001
- Inputs: I-001/I-002 的 ISA/接口合同、common p0 几何。
- Action: 列出 core clocks/resets/RAM/IO/debug ports 与允许 vendor 替换点；对 vendor-only cells 建目录边界，固定 all family 相同 architectural memory map。
- Outputs: platform interface contract、RTL/source file-list 分层。
- Pass: common core 不要求 vendor library 即可 Verilator elaboration，wrapper 差异不改变 ISA/异常/顺序。
- Fail: common PRF 直接依赖 RAMB/BSRAM primitive 或板名条件分支修改 instruction behavior。
- Covers: portable processor RTL, vendor isolation。
- Sources: implementation-plan.md, architecture-review.md。

### H-004 — 验证 RAM wrapper 的可观察一致性
- Depends: H-002, H-003
- Inputs: I-006、target RAM guides、generic/vendor simulation models。
- Action: 对每个 wrapper 执行全地址 walking pattern、byte mask、同址 read/write、双 port collision、enable/output latency；未定义 collision 由上层仲裁禁止并断言。
- Outputs: RAM semantics matrix、CASE=ram.collision_matrix 的三平台结果。
- Pass: 允许交易的输出值/延迟一致，所有禁止情形可被 assertion 捕获；综合实际推断期望 RAM。
- Fail: 以 vendor simulation 的 X 被 Verilator 变零掩盖问题，或仅比较写后最后值。
- Covers: BRAM/BSRAM/VRF/cache portability。
- Sources: HR-003, HR-004, HR-011, implementation-plan.md。

### H-005 — 验证 clock/reset 与 CDC/RDC
- Depends: H-003, H-004
- Inputs: 每板 clock/reset 电路、PLL wrapper、异步外部输入。
- Action: 同步解除 reset、处理 PLL lock 丢失、UART/JTAG CDC；对多位跨域用 handshake/FIFO，列 CDC/RDC crossings 和例外理由。
- Outputs: clock/reset diagram、CDC/RDC report、reset phase stress log。
- Pass: 每条 crossing 有正确同步策略，无未解释 critical CDC；reset 不产生伪 commit 或设备写。
- Fail: 全局 set_false_path 代替同步器，或 gate clock 用普通 LUT 组合逻辑。
- Covers: safe physical clocks, reset, metastability boundaries。
- Sources: HR-010, platform-plan.md。

### H-006 — 建立安全的 bring-up top
- Depends: H-004, H-005
- Inputs: common RAM、boot ROM、UART/result window、I-080 p0 候选。
- Action: 只用 on-chip RAM 和单 core clock，挂出 build ID、reset cause、cycle/retire counter、last trap；先禁 DDR/复杂 host DMA。
- Outputs: BRAM-only top、boot image mapping、debug observation contract。
- Pass: 所有观察值来自本项目 PL core；诊断不修改正常 architectural state。
- Fail: heartbeat 仅由独立计数器输出却算 processor 执行成功。
- Covers: functional hardware prototype, minimal observability。
- Sources: implementation-plan.md, validation-plan.md。

### H-007 — 固定 pin/clock/IO 时序约束
- Depends: H-001, H-005, H-006
- Inputs: 原理图、timing contract、oscillator frequency、IO timing。
- Action: 逐 port 绑定 pin/bank voltage/IO standard；声明 primary/generated clocks、IO delays、reset/crossing exceptions，逐条审阅其结构理由。
- Outputs: versioned CST/SDC 或 XDC、constraint coverage 表。
- Pass: 未定位 IO=0，intended clock/IO path 未约束项=0；任何例外与具体同步结构匹配。
- Fail: 用默认 pin 或 blanket async/false path 隐藏真实路径。
- Covers: physical constraints, portability safety。
- Sources: HR-001, HR-010。

### H-008 — 实现实板 host runner 与签名比对
- Depends: H-006
- Inputs: V-036 corpus manifest、serial/JTAG/device identity、reference expected files。
- Action: runner 读取 build/board ID、加载或选择镜像、reset、采集完整 result records、校验 sequence/CRC/signature/termination；分别拒绝错设备、错镜像与超时。
- Outputs: host runner CLI、52-case manifest、evidence schema。
- Pass: 每 case 唯一 verdict，缺记录不算成功；host mismatch 负控制明确非零。
- Fail: 只匹配一行 PASS、USB重枚举连错板仍通过或串口丢包后补猜数据。
- Covers: real-hardware correctness evidence, automated validation。
- Sources: validation-plan.md。

### H-009 — 校准 timing/resource 报告验收器
- Depends: H-002, H-007
- Inputs: 工具报告格式、明确时钟目标、per-profile resource budget。
- Action: 提取 setup/hold、unconstrained paths、unrouted nets、DRC/CDC 与实际 RAM/DSP 使用；用已知失败报告/错误约束确认不能绿灯。
- Outputs: per-vendor report parser、阈值与必要字段清单。
- Pass: 缺报告/不识别字段/负 slack/critical error 均失败；报告对应同一已实现 netlist hash。
- Fail: 仅检测工具进程 code 0 或 synthesis utilization 即宣布 fit。
- Covers: deterministic physical acceptance。
- Sources: HR-010, HR-001。

### H-010 — 建立跨平台证据归档与负控制流程
- Depends: H-008, H-009
- Inputs: §3 evidence schema、diagnostic bitstream policy。
- Action: 归档每次 program/reset/run 的输入与日志；分离 host-checker 负例、真实 datapath mutation 和 image-ID 错误；明确恢复良好映像步骤。
- Outputs: immutable run manifest、negative-control checklist、restoration record。
- Pass: 所有错误类别均能归因且不能混作正常 PASS；设备最终处于记录的良好映像。
- Fail: 失败后覆盖旧日志、注入未触达或测试结束残留故障 boot image。
- Covers: reproducibility, validation credibility, hardware safety。
- Sources: validation-plan.md。

## 5. Gowin GW5A 独立路径

### H-011 — 建立 exact-GW5A 工程与批处理入口
- Depends: H-002, H-003, H-007
- Inputs: 正确器件的 Gowin EDA、安装的 Tcl/软件手册、source lists。
- Action: GUI 新建 exact part 工程，加入 common+Gowin wrappers/CST/SDC/image，导出当前版本支持的项目/Tcl；在帮助中核实 batch invocation，保留完整命令而不照抄未读取的 PDF 示例。
- Outputs: Gowin project/script、版本化 file list、运行说明。
- Pass: 从空 build 目录可按记录重建同一输入集合，device/constraints 没有隐式 GUI 状态。
- Fail: GUI 工程漏文件，命令行用错误 part 或只在作者机器可用。
- Covers: GW5A reproducible build。
- Sources: HR-001, HR-011。

### H-012 — 验证 GW5A RAM/PLL 推断
- Depends: H-011, H-004, H-005
- Inputs: exact-part BSRAM/SSRAM/PLL 文档、wrapper unit results。
- Action: 单独综合 PRF/cache/boot-RAM/PLL 小实例，检查资源类型、读延迟、mask 粒度和 reset inference；必要的 native IP 只留 wrapper 内。
- Outputs: GW5A memory/clock mapping report、IP config hashes。
- Pass: 没有意外全阵列 FF reset 或容量爆炸；所用模式都有可取得手册/模型证据。
- Fail: 推断成 FF 后仍沿用 BRAM 预算，或从 GW1N RAM 推断 GW5A collision 语义。
- Covers: GW5A portability primitives。
- Sources: HR-001, HR-011。

### H-013 — 运行 GW5A 全核 synthesis
- Depends: H-012, H-006, I-080
- Inputs: p0 candidate、冻结 Gowin project、BRAM-only top。
- Action: 执行 synthesis，检查 latch/unconnected/truncated width、RAM/FU instance count 与两 cluster 连通；按 warning 类别逐条 disposition。
- Outputs: synthesis netlist、utilization/inference/warning reports。
- Pass: exact part 可容纳，两个 cluster 保留，未解释 latch/width/driver 错误=0。
- Fail: 工具优化掉整个 processor、ISA feature被条件编译移除或关键 warning 未处理。
- Covers: GW5A synthesizable processor。
- Sources: HR-001, implementation-plan.md。

### H-014 — 运行 GW5A place/route/STA
- Depends: H-013, H-009
- Inputs: 冻结 clock/CST/SDC、synthesized netlist、resource budget。
- Action: 执行完整 implementation，导出最坏 setup/hold、clock/IO coverage、route/congestion；失效时定位 PRF/wakeup/global fanout，修改架构后重跑正确性。
- Outputs: routed netlist、timing/resource reports、完整决策日志。
- Pass: §1.1 timing/route 门槛均满足，所有 clocks 为实际运行目标，无不实 timing exception。
- Fail: 只降报告目标不降实际 oscillator/divider，或 skip hold/unconstrained 检查。
- Covers: GW5A physical feasibility。
- Sources: HR-001, HR-011。

### H-015 — 生成并核对 GW5A 配置映像
- Depends: H-014, H-010
- Inputs: routed design、boot image hash、Programmer/board ID。
- Action: 生成所选器件/配置方式映像，检查 boot RAM 数据与 build ID；先选择可恢复的易失配置路径，flash 操作需明确范围/备份/授权。
- Outputs: image hash、configuration settings、programming recipe。
- Pass: 映像 part 与硬件 ID 一致、输入来源闭合；不覆盖未知用户 flash 内容。
- Fail: 从旧输出目录取错 bitstream 或 programming 操作超出授权。
- Covers: GW5A programming safety, provenance。
- Sources: HR-001, H-001。

### H-016 — 执行 GW5A cold/warm board regression
- Depends: H-015, H-008
- Inputs: 实际 GW5A 板、52-case corpus、host runner。
- Action: 下载后核对 ID，按 §3 运行 3 次 cold power 和 10 次 warm reset 的完整集合；保留 UART/JTAG 与板时钟测量。
- Outputs: GW5A physical evidence bundle、全部 expected/actual signatures。
- Pass: 每次每 case 结果匹配，无 hang/CRC/timeout；负控制检出并恢复良好映像。
- Fail: 只有 simulation/log复制、LED心跳或单次 arithmetic demo。
- Covers: mandatory GW5A real-hardware validation。
- Sources: validation-plan.md, H-010。

### H-017 — 扩展 GW5A 外部存储与高 profile
- Depends: H-016, I-048
- Inputs: 所选板实际存储类型/接口、vendor controller、p1/p2/p3需要。
- Action: 先 controller calibration/地址walking/burst边界/错误注入，再 core 接入；无 DDR 的板保持有据的 RAM profile，不能凭 family 名称假定有 DDR。
- Outputs: external-memory adapter、memory-test evidence、per-profile fit matrix。
- Pass: 只有真实存在且验证过的 memory 才进平台描述；每新增 profile 重新跑 timing与corpus。
- Fail: calibration成功即当CPU正确，或额外存储不可用仍宣称Linux/RVV容量已满足。
- Covers: GW5A scaling, memory integration。
- Sources: HR-001, implementation-plan.md。

## 6. Zynq 独立路径：7000 与 UltraScale+ 分支

### H-018 — 冻结 Zynq PS/PL 职责
- Depends: H-001, H-003
- Inputs: selected Zynq branch、board clock/reset/DDR连接、UG585或UG1085。
- Action: 明确 BRAM-only 是否用板 oscillator 或 PS FCLK；PS 若只加载/串口服务，仍由PL RISC-V执行测试；列 PS boot、DDR init、clock/reset release顺序。
- Outputs: Zynq boot ownership diagram、PL/PS memory map。
- Pass: 不存在未启动PS却依赖其FCLK/DDR的隐含循环；branch特有boot组件明确。
- Fail: 把Zynq-7000 FSBL与MPSoC PMU/firmware链路混用。
- Covers: Zynq-7000, Zynq UltraScale+ MPSoC decision gate。
- Sources: HR-005, HR-006。

### H-019 — 建立 Zynq wrapper 与 RAM profile
- Depends: H-018, H-004, H-005
- Inputs: 7-series 或 UltraScale+ PL 类型、BRAM resources、clock/reset source。
- Action: 选择相应memory primitive/IP wrapper，移除不适用URAM/clock属性；BRAM-only路径先不接PS DDR，测所有RAM collision合同。
- Outputs: selected-Zynq top、RAM/clock unit evidence。
- Pass: synthesis绑定所选family的正确cells，不能把UltraScale-specific primitive带入Zynq-7000。
- Fail: 利用错误memory读延迟使模拟通过但实板失败。
- Covers: Zynq-specific portability。
- Sources: HR-003, HR-004, HR-005。

### H-020 — 定义 Zynq AXI 与非一致性 handoff
- Depends: H-019, I-047
- Inputs: PS/PL所选AXI端口、数据宽度/ID/outstanding、host loader。
- Action: 对load/reset/signature memory分别定义owner；PS cache clean/invalidate和barrier按所选端口协议执行；测试backpressure/错误response/未对齐禁止条件。
- Outputs: AXI adapter、CASE=zynq.ps_pl_handoff、cache-maintenance protocol。
- Pass: host写入的镜像和core写出的签名真实可见；不存在PS stale cache掩盖core错误。
- Fail: 将HP等普通端口自动当coherent，或ARM执行期望值代替读PL结果。
- Covers: Zynq PS integration, memory visibility。
- Sources: HR-005, HR-008。

### H-021 — 建立 Zynq 可复现 Vivado 工程
- Depends: H-019, H-002, H-007
- Inputs: exact part/board、common file list、必要PS/block-design配置。
- Action: 生成batch Tcl及IP配置，固定所有IP version和board preset；校验block design/地址分配，避免必须手点未记录GUI配置。
- Outputs: Zynq build Tcl、IP locks、recreation instructions。
- Pass: 干净目录重建所选branch相同逻辑配置，工具报告没有自动upgrade后未记录变化。
- Fail: 本地board_files未跟踪或预设隐含错误DDR/pin。
- Covers: reproducible Zynq implementation。
- Sources: HR-010, HR-005。

### H-022 — 校准 Vivado build/report Tcl
- Depends: H-002, H-003, H-007, H-009
- Inputs: 所锁Vivado help、tool capability、report acceptance schema。
- Action: 通过`vivado -version`与Tcl帮助确认命令；脚本运行synth/opt/place/route并生成§2.1报告，error立即非零，保留journal和checkpoint。
- Outputs: validated Tcl runner、all mandatory reports、script failure controls。
- Pass: checkpoint stage/hash与报告对应；缺run/时序失败不会因Tcl流程正常退出误报成功。
- Fail: catch吞error、空设计report、仅synth report充当post-route证据。
- Covers: AMD implementation automation, timing evidence。
- Sources: HR-010。

### H-023 — 验收 Zynq routed design
- Depends: H-022, I-080
- Inputs: p0 RTL、XDC、selected-Zynq wrapper。
- Action: 完成synthesis/P&R，检查clock interaction、CDC、RAM inference、setup/hold与IO constraints；读取full utilization而非只看LUT。
- Outputs: Zynq routed checkpoint、timing/resource/DRC bundle。
- Pass: §1.1门槛全部满足，PS/PL接口和真实core clock被约束。
- Fail: 未约束FCLK路径、incorrect async grouping或physically unrouted design。
- Covers: Zynq physical implementation。
- Sources: HR-003, HR-005, HR-010。

### H-024 — 下载 Zynq 并运行共同板测
- Depends: H-023, H-008, H-010
- Inputs: 真实Zynq板、正确boot/PS初始化链、bitstream/ELF hashes。
- Action: 先JTAG/受控易失加载，核对PL build ID；按§3 cold/warm回归和负控制运行，不让PS替代执行；需要持久启动时另冻结boot image构成。
- Outputs: Zynq physical evidence bundle、reset/boot/serial记录。
- Pass: 52-case每次全过，真实PL cycles/retire前进且签名来自PL core；错误映像可恢复。
- Fail: Linux在ARM上启动或软件host打印PASS被计为RISC-V验证。
- Covers: mandatory Zynq real-hardware validation。
- Sources: HR-005, HR-006, validation-plan.md。

### H-025 — 扩展 Zynq DDR 与 OS/vector profile
- Depends: H-024, H-020, I-048
- Inputs: PS DDR初始化、core AXI、firmware/DTB、p1/p2/p3。
- Action: 独立DDR压力与PS/PL共享一致性检查后引入RISC-V boot；测试page faults、interrupt、atomic、DMA/coherence维护与长运行。
- Outputs: DDR/boot adapter、profile-specific timing和program evidence。
- Pass: RISC-V用户程序在PL核完成预期签名/退出；各profile fit和ISA支持逐项记录。
- Fail: DDR可读就跳过cache coherence、以PS/Linux console冒充PL/OS日志。
- Covers: Zynq advanced system validation。
- Sources: HR-005, HR-008, implementation-plan.md。

## 7. Virtex UltraScale+ 独立路径

### H-026 — 冻结 Virtex 板级 clock/配置/SLR 路径
- Depends: H-001, H-002, H-003
- Inputs: exact Virtex UltraScale+ part、board manual、SLR/DDR/IO布局。
- Action: 不依赖Zynq PS，选择板clock/PLL、JTAG和UART/host桥；记录跨SLR链路以及configuration flash边界。
- Outputs: Virtex top contract、floorplan建议与pin/clock来源。
- Pass: 所有平台服务有该板真实提供路径，所需IP/工具授权匹配part。
- Fail: 复制VCU118 pinout到未知板，或假定每款器件有相同URAM/DDR能力。
- Covers: Virtex UltraScale+ board specificity。
- Sources: HR-002, HR-007, HR-004。

### H-027 — 验证 Virtex BRAM/URAM adapter
- Depends: H-026, H-004
- Inputs: selected-part memory resources、p0 PRF/cache latency contract。
- Action: 先BRAM实现共同profile；URAM作为独立容量优化，显式处理其端口/byte write/reset/读延迟限制，不以同一wrapper名字隐藏额外cycle。
- Outputs: memory mapping/inference report、adapter equivalence cases。
- Pass: 每种允许memory实现均满足core contract；不支持模式通过外置逻辑实现或拒绝配置。
- Fail: 将URAM当任意多口BRAM的直接替代。
- Covers: Virtex storage scaling, RAM portability。
- Sources: HR-004, platform-plan.md。

### H-028 — 构建 Virtex Vivado flow 与约束
- Depends: H-027, H-007, H-022
- Inputs: exact part/file list、Virtex XDC、已校准Vivado runner。
- Action: 为Virtex单独生成工程和IP配置，保留共同runner而不是共同pinout；必要时给cluster和RAM软floorplan，先不以过紧pblock隐藏问题。
- Outputs: reproducible Virtex batch flow、constraints、IP locks。
- Pass: 无Zynq PS cell/地址依赖；build标识清楚区分Zynq/Virtex。
- Fail: 三个平台共用一个手改project而无法恢复各自配置。
- Covers: Virtex reproducibility, vendor wrapper reuse。
- Sources: HR-010, HR-007。

### H-029 — 验收 Virtex route 与网络时序
- Depends: H-028, I-080, H-009
- Inputs: p0候选、clock target、inter-cluster/SLR crossings。
- Action: synth/P&R/STA，检查长线、fanout、SLR crossings与completion回路；必要pipeline cut回到Verilator回归，不仅调实现seed。
- Outputs: routed/timing/resource/CDC report、pipeline与配置hash。
- Pass: §1.1门槛满足，physical增加latency后architectural suites仍通过。
- Fail: 高IPC但实际时钟失守、跨SLR关键路径被错标false path。
- Covers: Virtex physical scalability。
- Sources: HR-010, architecture-review.md。

### H-030 — 下载 Virtex 并执行共同板测
- Depends: H-029, H-008, H-010
- Inputs: 真实Virtex UltraScale+板、合法programmer、52-case corpus。
- Action: 核对hardware ID和image hash后编程，执行cold/warm回归、真实mutation负例及恢复；采集实际core clock/retire/签名。
- Outputs: Virtex physical evidence bundle。
- Pass: 完整回归零mismatch/timeout，证据属于该板该bitstream且host独立核对。
- Fail: 用其他UltraScale+板日志、软件仿真或只跑CoreMark作为完整证明。
- Covers: mandatory Virtex UltraScale+ real-hardware validation。
- Sources: validation-plan.md, HR-007。

### H-031 — 扩展 Virtex DDR 与宽 fabric
- Depends: H-030, I-048, I-072
- Inputs: 该板实际DDR/controller、p1/p2/p3、wide/pod profile。
- Action: 先独立controller校准和地址/byte/burst检查，再连接processor；2/4/8 lane/pod每一配置独立route与回归，量化interconnect成本。
- Outputs: advanced-profile fit表、DDR/controller与程序证据。
- Pass: 每个被宣称支持profile有对应硬件/时序/correctness记录，失败配置保持失败状态。
- Fail: 单个小profile成功后外推所有宽度线性扩展。
- Covers: Virtex advanced fabric/DDR validation。
- Sources: HR-007, HR-010, implementation-plan.md。

## 8. 跨平台、DFX 与交付门槛

### H-032 — 比较三平台共同 corpus
- Depends: H-016, H-024, H-030
- Inputs: 三份真实board bundles、相同ELF/input/reference manifest。
- Action: 对齐case IDs/ISA/字节映像与defined signatures，分别报告频率、周期和几何；不对允许nondeterminism盲目bitwise比较。
- Outputs: three-family portability evidence matrix。
- Pass: 三family全部52-case及cold/warm集合闭合；几何差异不产生architectural差异。
- Fail: 少一family、静默删不通过case或只比较不同程序的PASS字符串。
- Covers: end-to-end portability validation, user three-platform criterion。
- Sources: validation-plan.md, implementation-plan.md。

### H-033 — 测量板上性能而非推断功耗
- Depends: H-032, I-077
- Inputs: paired workloads、实际clock、计数器校准、可选仪器。
- Action: 在fixed/dynamic同等资源和已route频率下量周期/时间；有仪器才测电压电流/温度，记录idle subtraction/rails/采样误差；估算与实测分列。
- Outputs: reproducible performance与可选power报告。
- Pass: useful-work/second与完整成本同报，所有energy结论有真实measurement来源或明确estimate标签。
- Fail: IPC×假定Fmax、vendor power estimate冒充实测、删除负收益样本。
- Covers: performance hypotheses, physical evidence integrity。
- Sources: SRC-02:932-1032, architecture-review.md。

### H-034 — 定义可选 DFX 研究 gate
- Depends: H-032, I-031
- Inputs: 真正需要coarse功能替换的研究理由、selected device/tool DFX support与许可。
- Action: 先记录runtime routing为何不足；若值得做，划region/interface、isolation、clock/reset、inflight drain、partial image identity；否则保留量化no-go结论，不把DFX混进per-uOP路由。
- Outputs: DFX architecture decision、isolated-region contract或有据拒绝结果。
- Pass: 决策明确功能/资源收益与切换成本；任何实现需证明旧事务排空、隔离与恢复后签名相同。
- Fail: 为instruction N动态生成ALU的说法、在有inflight memory时直接重配区域。
- Covers: SRC-02 partial reconfiguration, advanced research preservation。
- Sources: architecture-review.md, SRC-02:1033-1070。

### H-035 — 冻结 FPGA release evidence
- Depends: H-032, H-033, H-017, H-025, H-031
- Inputs: common与已支持advanced profiles、全部锁定输入/报告/physical logs。
- Action: 独立重建与重放每family，列supported/blocked/failed profile；文档写明program/run/recover步骤，不发布许可证/受限IP源。
- Outputs: FPGA release manifest、board operation guide、完整已知限制。
- Pass: 每claim可定位同part/tool/image/ELF/signature，未知板/未fit profile不算成功。
- Fail: 仅一份bitstream无源配置，或使用未经授权的第三方IP归档。
- Covers: reproducible real prototype deliverable。
- Sources: references.md, validation-plan.md。

## 9. 后续 ASIC 转换：不是把 bitstream 改名为芯片

### H-036 — 冻结 ASIC 产品/工艺输入
- Depends: H-003
- Inputs: 目标用途、工艺/PDK合法访问、standard cell/SRAM/IO/PLL库、工作电压温度、封装与test预算。
- Action: 记录exact process/library revisions、PVT/corner、foundry签核要求与license；若未提供则明确阻断physical signoff，不假设SKY130等开源PDK生产就绪。
- Outputs: ASIC technology manifest、corner/mode/signoff matrix、外部依赖。
- Pass: 每必需库/模型可合法取得且覆盖目标corner；缺项有明确owner/action而不是伪fallback。
- Fail: FPGA成功直接推断ASIC可制造，或把教学PDK运行当production signoff。
- Covers: later real-design conversion, ASIC prerequisites。
- Sources: HR-009, HR-012。

### H-037 — 实现 SRAM macro adapter 与 BIST 访问合同
- Depends: H-036, H-004
- Inputs: macro端口/latency/mask/ECC/repair/test规格、PRF/VRF/cache需求。
- Action: 映射RAM到合法macro组合，处理width/depth banking、collision与reset差异；boot ROM方式与power-up unknown显式化，测试端口不得破坏功能协议。
- Outputs: SRAM/ROM adapters、macro placement list、functional equivalence cases。
- Pass: 所有合法memory访问与generic模型一致；macro不可支持模式通过明确逻辑解决且面积时序计入。
- Fail: 继续依赖FPGA INIT或BRAM read-first默认行为。
- Covers: ASIC memory portability, MBIST foundation。
- Sources: implementation-plan.md, HR-009。

### H-038 — 定义 ASIC clock/reset/DFT modes
- Depends: H-036, H-005
- Inputs: functional/test clocks、reset sources、PLL/clock-gating cells、power domains。
- Action: 使用library ICG和test enable，定义reset sequencing/RDC/scan override；单电源单频为默认，DVFS/多电源必须单独新增mode/UPF契约。
- Outputs: clock/reset architecture、mode table、gating checks。
- Pass: functional/scan/reset模式无时钟毛刺、每域解除受控，所有clock crossing有策略。
- Fail: 普通组合门代替ICG或DFT模式未纳入时序。
- Covers: ASIC clocks, reset, CDC/RDC。
- Sources: HR-009, platform-plan.md。

### H-039 — 进行 technology-mapped synthesis 与等价
- Depends: H-037, H-038
- Inputs: common RTL release、libraries、SDC、blackbox/macro功能模型。
- Action: lint/elaboration/synthesis，检查latches/multidrivers/uninitialized controls；RTL→gate sequential equivalence覆盖真实reset和macro假设，明确cutpoints。
- Outputs: gate netlist、area/timing、equivalence proof/failed obligations。
- Pass: 等价范围闭合、没有用blackbox屏蔽架构状态；综合资源映射与合同一致。
- Fail: 只verilog编译成功或数百unproven points仍称等价。
- Covers: ASIC functional preservation, synthesis correctness。
- Sources: HR-009, validation-plan.md。

### H-040 — 插入 scan 并验证模式隔离
- Depends: H-039
- Inputs: scan-cell library、clock/reset/test mode、chain length/IO预算。
- Action: 用所选DFT流程做scan replacement/chain stitching，定义跨clock lockup/test ordering；验证functional scan-enable=0等价和shift/capture模式。
- Outputs: scan netlist、chain map、DFT DRC与功能等价结果。
- Pass: 所有目标flops有明确scan/合法例外，chain连接和capture可达；scan改动不改变functional逻辑。
- Fail: 仅OpenROAD插入chain就宣称ATPG/制造测试完成。
- Covers: DFT, scan, functional/test separation。
- Sources: HR-009。

### H-041 — 制定 ATPG/MBIST/repair 验收
- Depends: H-040, H-037
- Inputs: stuck-at/transition fault模型、SRAM test/repair能力、manufacturing要求。
- Action: 生成ATPG fault list/patterns与覆盖分母，审查untestable/excluded faults；MBIST覆盖地址/数据/byte masks/repair路径，定义test-time与tester接口。
- Outputs: ATPG/MBIST报告、fault coverage目标与签核阈值、pattern provenance。
- Pass: 数值目标在运行前按产品要求冻结且实际报告达标；所有排除有理由，repair后功能复验。
- Fail: 只pattern数量非零即通过，或把functional程序跑通代替manufacturing coverage。
- Covers: real ASIC manufacturing test。
- Sources: HR-009, H-036。

### H-042 — 实施 floorplan/PDN/macro placement
- Depends: H-039, H-040
- Inputs: macro/IO尺寸、utilization目标、power grid规则、cluster locality。
- Action: 将PRF/FU/IQ/completion置于局部岛，约束pod间长线；布PDN/macro halos/channels并检查拥塞、pin access与供电连通。
- Outputs: floorplan/PDN数据库、拥塞/连接报告、面积分解。
- Pass: 无macro重叠/未供电cell/不可布区域，预计长路径与协议pipeline一致。
- Fail: 用零wire延迟推定fabric Fmax或只看standard-cell面积忽略RAM/路由。
- Covers: ASIC physical architecture, locality cost。
- Sources: HR-012, architecture-review.md。

### H-043 — 完成 CTS、route 与多角 STA
- Depends: H-042, H-038
- Inputs: MCMM modes/corners、clock uncertainties、derates、IO/false/multicycle exceptions。
- Action: placement/CTS/route/extraction后检查setup/hold、recovery/removal、pulse width、gating、max transition/capacitance；逐条审查exceptions，处理OCV及foundry要求。
- Outputs: routed netlist/parasitics、全corner timing与clock reports。
- Pass: 所有签核mode/corner达冻结目标，intended paths无未约束，例外有结构证明。
- Fail: 只typical corner满足、pre-route timing冒充signoff或为绿灯放宽真实路径。
- Covers: ASIC STA, timing closure。
- Sources: HR-012, H-036。

### H-044 — 验证 power intent、IR/EM 与热边界
- Depends: H-043
- Inputs: workload activity、power model/corners、PDN、可选UPF。
- Action: 用真实activity与保守worst-case评估功耗、IR drop/EM/thermal；若多电源，检查isolation/level shifting/retention及掉电恢复，与动态lease协议联动。
- Outputs: power/IR/EM/thermal reports、UPF equivalence与power-state tests（适用时）。
- Pass: 按所选工艺/封装限制全部达标；未使用多电源时明确not applicable并提供单域结构证据。
- Fail: FPGA板功耗直接当ASIC功耗，或隔离单元缺失仍允许运行时power gating。
- Covers: ASIC power integrity, dynamic resource safety。
- Sources: H-036, architecture-review.md。

### H-045 — 完成 DRC/LVS/ERC 与 post-route 等价
- Depends: H-043, H-044, H-041
- Inputs: foundry规则deck、extracted layout、schematic/netlist、scan/functional modes。
- Action: 执行DRC/LVS/ERC/antenna及需要的可靠性检查；post-route网表等价与必要SDF reset/IO smoke；全waiver逐项审查。
- Outputs: foundry-required signoff reports、GDS/netlist checksum闭合。
- Pass: 无未批准的signoff error/waiver，layout与提交netlist/库一致，新增buffer/scan不破坏功能。
- Fail: 开源flow的“flow complete”替代foundry认可签核。
- Covers: real physical-design conversion, tapeout boundary。
- Sources: HR-009, HR-012, H-036。

### H-046 — 冻结封装、板测与首硅 bring-up 计划
- Depends: H-045
- Inputs: pad/package/rail/clock/debug/test接口、ATPG/MBIST、RISC-V corpus。
- Action: 定义power sequencing、电气测量、JTAG ID、scan/MBIST、ROM/UART、同一architectural signatures、PVT/frequency sweep与failure binning。
- Outputs: silicon bring-up runbook、tester/board需求、acceptance matrix。
- Pass: 每个制造/功能/时序目标有测点与pass/fail，工具/板/样片access明确；样片未回则状态仅为计划。
- Fail: FPGA实板测试被称为真实ASIC硅后验证。
- Covers: later physical silicon validation, processor lifecycle。
- Sources: H-036, validation-plan.md。

### H-047 — 审核 ASIC-ready 与 tapeout 声明
- Depends: H-046, H-035
- Inputs: RTL/profile/source locks、三板证据、全部ASIC signoff与license记录。
- Action: 逐claim检查对应artifact/责任人/approval；区分portable RTL、ASIC synthesized、routed、signoff complete、taped out、silicon validated六种状态。
- Outputs: release readiness report、已完成/外部阻断清单。
- Pass: 每声明不超证据范围；未获PDK/许可/硅样品的阶段明确阻断而非伪完成。
- Fail: 把某工具返回0作为跨阶段总完成标准。
- Covers: portability to real design, honest end-to-end evidence。
- Sources: references.md, verification.md。

### H-048 — 验证三家族 lockstep 物理分离与故障注入
- Depends: H-010, I-089
- Inputs: 每个family的lockstep-capable build、fault-injection控制、physical placement/clock/reset约束、故障动作。
- Action: 对GW5A、Zynq、Virtex UltraScale+分别约束main/shadow物理分区与共同资源边界；执行正常corpus和真实fault注入，记录检测延迟、阻断副作用、资源/时序差异。
- Outputs: 每family lockstep evidence bundle、placement/timing/fault reports。
- Pass: 三家族都证明redundant logic实际存在、输入同步、fault被检测且未发布副作用；资源不足或无时序则该family profile blocked。
- Fail: 仅Verilator fault injection充当实板证据、shadow被优化移除、比较器无法阻断设备写。
- Covers: optional FPGA lockstep, physical common-mode protection。
- Sources: AR-019, AR-020, platform-plan.md。

### H-049 — 审核 ASIC lockstep 的物理与制造测试边界
- Depends: H-038, H-041, I-090
- Inputs: main/shadow floorplan、共享SRAM/clock/power、scan/MBIST/ATPG、fault model、目标安全认证边界。
- Action: 对replica放置/时钟树/电源/共享宏做common-mode审查；扫描链覆盖replica和checker，MBIST/ECC覆盖shared memories；把lockstep状态纳入function/test/power mode等价与签核。
- Outputs: ASIC lockstep physical/signoff report、fault coverage scope、residual safety analysis。
- Pass: 每个声称的安全机制有设计、仿真、形式/物理或制造测试证据；没有正式评估时不发布ASIL/SIL合规结论。
- Fail: 将DCLS当作TMR纠错、FPGA fault campaign替代ASIC signoff、共享SRAM/clock未纳入风险。
- Covers: ASIC lockstep, DFT, physical safety boundary。
- Sources: AR-019, AR-020, AR-021, HR-009。

### H-050 — 验证 RVA23 软件包在三家族的物理执行
- Depends: H-032, I-098
- Inputs: RVA23 Core/Secure acceptance bundle、Linux/RVV/crypto/CFI/PM软件包、三家族实际资源和memory map。
- Action: 在每块真实板执行RVA23-directed corpus、Linux应用、RVV workloads、pointer masking、CFI、vector crypto和故障负例；记录binary hash、device tree、capability discovery和性能。
- Outputs: three-family RVA23 evidence bundles、capability/runtime logs。
- Pass: 每家族支持声明与实际执行一致；软件发现机制读到真实能力；无RVA23能力时不能运行profile binary并称成功。
- Fail: 只在host模拟器运行RVA23，或用不支持VLEN/crypto/CFI的板宣称完整commercial profile。
- Covers: RVA23 hardware portability, real-board commercial evidence。
- Sources: AR-022, AR-023, AR-024, AR-025, AR-026。

### H-051 — 验证 ASIC security features 的物理与制造边界
- Depends: H-049, I-097, I-098
- Inputs: RVA23 Secure候选、RoT/IOPMP/AIA接口、SRAM/ECC、DFT/scan、物理安全约束。
- Action: 把pointer masking、CFI、vector crypto、entropy、debug trigger、AIA和lockstep纳入ASIC physical/DFT/security审查；明确RoT/TPM/secure boot/IOPMP的外部owner；执行综合后故障和side-channel观测。
- Outputs: ASIC security signoff bundle、platform dependency ledger、residual risk report。
- Pass: core内功能有硅级证据路径，平台责任明确；无正式side-channel/security evaluation时不宣称合规。
- Fail: 用FPGA软件通过代替ASIC安全验证，或把vector crypto DIEL说成完整物理侧信道免疫。
- Covers: ASIC commercial security, physical signoff boundary。
- Sources: AR-024, AR-025, AR-026, AR-027, HR-014。

## 10. 已读取来源、版本与访问限制

所有访问日期为 2026-09-29；只记录一手来源，不把搜索生成总结当规范。外部文档未复制进仓库，以URL/版本/取得状态记录，避免未经许可再分发。

| ID | Primary source / version | 本次支持的事实与限制 |
|---|---|---|
| HR-001 | [Gowin EDA download/support](https://www.gowinsemi.com/en/support/download_eda/)、[Arora V guide catalog](https://www.gowinsemi.com/en/support/database/2913/) | 官方EDA/license/programmer/器件资料入口已读。具体release/part支持必须H-002冻结，family不是完整board规格。 |
| HR-002 | [AMD Vivado licensing options](https://www.amd.com/en/products/software/adaptive-socs-and-fpgas/vivado/vivado-licensing-options.html)，页面更新2026-07-30 | 2026.1开始分BASIC/CORE/PRO等；BASIC需年度免费续期，all UltraScale+不能默认免费；IP许可另核对。不把价格/政策当永久常量。 |
| HR-003 | [7 Series FPGAs Memory Resources, UG473 v1.14, 2019-07-03](https://docs.amd.com/api/khub/documents/9gZGbqBxtlKXxBfkBt~lAg/content) | 7-series RAM模式、byte enable与collision条件需按文档及wrapper核对；不是UltraScale+/Gowin统一memory规范。 |
| HR-004 | [UltraScale Memory Resources, UG573 official portal](https://docs.amd.com/r/en-US/ug573-ultrascale-memory-resources) | family-specific RAM审查入口；动态页面/所选release未在本轮固定全文，具体URAM/BRAM行为为H-027实施门槛，不据此声称已验证。 |
| HR-005 | [Zynq UltraScale+ Device TRM, UG1085 v2.5, 2025-03-21](https://docs.amd.com/api/khub/documents/xzMsp_c5sG9J6A3u7NkJYQ/content) | 已读document identity、PS/PL/boot/clock接口章节定位；MPSoC的PS/boot路径不等同Zynq-7000。exact board配置仍需实施核对。 |
| HR-006 | [Zynq-7000 TRM UG585: Boot and Configuration](https://docs.amd.com/r/en-US/ug585-zynq-7000-SoC-TRM/Boot-and-Configuration) | 官方入口已定位，静态读取返回应用shell；不可据此宣称已核对所有boot步骤，H-018需要对应release完整TRM。 |
| HR-007 | [AMD Virtex UltraScale+ product family](https://www.amd.com/en/products/adaptive-socs-and-fpgas/fpga/virtex-ultrascale-plus.html) | 已读取家族说明；板卡、DDR/SLR/URAM数与license均以exact part为准，不从VCU118范例泛化。 |
| HR-008 | [Arm AMBA AXI/ACE specification IHI0022H](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/IHI0022H_amba_axi_protocol_spec.pdf) | primary协议已由研究任务读取handshake条款；VALID/READY和channel规则不能代替CPU memory consistency。具体AXI版本/子集由adapter声明。 |
| HR-009 | [OpenROAD DFT documentation](https://openroad.readthedocs.io/en/latest/main/src/dft/README.html)，访问日latest | 已读scan replacement/config/plan流程；scan insertion不是ATPG、MBIST或foundry signoff全套能力证明。实现时锁commit。 |
| HR-010 | [Vivado Tcl Command Reference UG835 v2018.3](https://docs.amd.com/api/khub/documents/wNJReNjblikQ29AHV1THwg/content)、[Vivado Quick Reference](https://docs.amd.com/api/khub/documents/aNBqzHrLSGHsSXaindgD5Q/content) | 已读命令目录与batch invocation；这是已取得版本，不冒称最新2026.1全文已读。H-022对所选release检查help/flags。 |
| HR-011 | [Gowin Quick Start SUG918](https://cdn.gowinsemi.com.cn/SUG918E.pdf)、[Arora V BSRAM/SSRAM UG300](https://cdn.gowinsemi.com.cn/UG300E.pdf) | 本轮获取返回HTTP403；仅为待取得的primary pointers。没有凭搜索摘要发布可执行GW5A Tcl或具体collision保证；阻断只在未来相应实现gate。 |
| HR-012 | [OpenROAD project flow](https://openroad.readthedocs.io/en/latest/main/README.html)、[SkyWater PDK status](https://skywater-pdk.readthedocs.io/en/main/status.html) | OpenROAD可用于physical-flow研究；SkyWater状态页明确experimental preview而非保证production使用。本项目不预选该PDK，也不把开放工具当foundry认可。 |
| HR-015 | [RVA23 ratified profile](https://docs.riscv.org/reference/rva23/v1.0/index.html)、[ratified source](https://raw.githubusercontent.com/riscv/riscv-profiles/rva23-rvb23-ratified/src/rva23-profile.adoc) | 支持商业RVA23基线、V mandatory、pointer masking、Sha、crypto/CFI选项边界；不把可选安全扩展当强制。 |
| HR-016 | [RISC-V Server Platform v1.0](https://docs.riscv.org/reference/server-platform/v1.0/server_platform_requirements.html) | 支持server/平台责任的RoT、TPM、Secure Boot、AIA、debug trigger、SEE一致性边界；不是core-only合规证明。 |
| HR-013 | [VeeR EL2 DCLS documentation](https://chipsalliance.github.io/Cores-VeeR-EL2/html/main/docs_rendered/html/dual-core-lock-step.html)、[Antmicro DCLS article](https://antmicro.com/blog/2026/04/dual-core-lockstep-in-veer-el2) | 支持synthesis barrier、delayed shadow、error injection和物理集成风险。VeeR是小规模RISC-V参考，不替代MosaicRV的宽OoO/fabric证明。 |
| HR-014 | [TI SDAA393, June 2026](https://www.ti.com/lit/pdf/sdaa393) | 支持DCLS检测-only/common-mode限制与安全等级需系统论证；不把DCLS当作fault-tolerant TMR或自动认证。 |

SRC-02 完整阅读证据：1–260、261–520、521–780、781–1040、1041–1213，均显式raw范围；补充读取SRC-03的370–539、790–1123、1997–2346。原报告的初期VCU118建议改为三family独立gate；FPGA预算数值不继承为事实；DFX保留为粗粒度可选研究；实板与ASIC成果均必须有独立证据。
