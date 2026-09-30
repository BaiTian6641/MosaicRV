# 验证计划：可追溯的 RISC-V 语义、XiangShan 对照与弹性 Fabric 不变量

## 0. 文档状态与结论边界

本文是**实施前的研究与工作分解**，不是已有验证系统的说明。仓库目前没有本文约定的 RTL、C++ harness、适配器、测试生成器或 CLI。本轮只阅读原始报告及上游主源；没有安装、克隆、构建、运行模拟器、运行处理器测试或触碰硬件。文中数量是未来有限验收集的设计，不是已运行结果、错误率保证或覆盖率宣传。项目集成关系见 [实施计划](implementation-plan.md)、[架构审查](architecture-review.md)、[平台计划](platform-plan.md)。

核心关系必须保持如下：

- **新核是 DUT-A，XiangShan 是 DUT-B / 设计及工程参照。** 两者可以执行相同兼容程序，各自接受参考模型校验；XiangShan 的相同结果不能证明新核正确，二者不同也不自动说明新核错误。
- **Difftest 是观测、传输、同步及比较框架；NEMU、Spike、Sail 是不同参考实现/可执行语义来源。** 参考模型也会有缺陷、配置差异、未实现功能与共同来源风险；最终争议回到已冻结的规范条款，而不是多数投票。[VR-001–VR-007]
- Verilator 证明的是指定离散模型在指定激励上的行为；不能替代时序收敛、CDC/RDC、FPGA 板测、模拟器四态语义复核、形式证明或 ASIC sign-off。[VR-008]
- 初始 `p0` 的候选 ISA 是 **RV64IM_Zicsr_Zifencei + M-mode、1 hart**。A、C、S/U/Sv39、F/D、V、多 hart、cohort 与 pod 是后续必须规划和验收的阶段，不在未通过阶段前宣称已支持。完整 V 不是几个整数向量指令；VLEN/ELEN、依赖扩展及必选指令均须冻结。只有 lane 数变化，不能偷偷改变某 hart 的 architectural VLEN。[VR-013]
- 原报告的“vector macro-uOP 在 ROB 只占一项”可以保留；“整条向量指令一定原子完成”必须改为 **ISA 规定的部分完成、精确 trap、`vstart` 重启和可允许的后续元素更新**。非一致性 LLB 也不能绕过架构一致性：若保存普通共享内存副本，必须有可证明的失效/版本检查；仅仅叫 clean copy 不够。[VR-013、VR-014]

## 1. 已核实的上游版本与可冻结组合

以下是检索日 **2026-09-29** 的主源快照。表中的 SHA 已由上游 API/gitlink/提交消息核实；“候选一致组合”只说明来源闭合，**尚未验证可构建、可加载及可执行**。实施时 V-001–V-006 负责完成这些检查。

| 组件 | 候选冻结值 | 来源关系及边界 |
|---|---|---|
| XiangShan | `kunminghu-v2` → `e7bab53e66dfb3c4a1d11cf9519b0396f8576cae`，提交 2026-09-24 | 当前默认分支 README（维护表更新 2026-06-30）推荐研究优先 V2；冻结分支 README 尚有旧的 master 文案，不能拿其文案覆盖 gitlink 事实。[VR-001、VR-002] |
| Difftest | `3729300ae233816d332d057f170472ebe35147b0` | 上述 XiangShan 树中的 gitlink；接口、生成器、C++ 比较器必须一起冻结，不能只复制 bundle 名。[VR-002、VR-003] |
| ready-to-run | `c4114ce3fffcd5c147c525014b40f1c841347238` | 同一 XiangShan gitlink；NEMU `.so` 和样例 workload 可用于上游环境校准，不能直接作为新核 RV64IM 程序。[VR-004] |
| NEMU | `f39e3077d7bac3cd9a3a853a9300a5f8f0293a2c` | ready-to-run 提交明确给出的参考库来源。单 hart 配置是 `riscv64-xs-ref_defconfig`；目录核实的双 hart 文件是 `riscv64-xs-dual-ref_defconfig`，不能照抄提交消息中顺序不同的文件名。配置启用 RVV/H/Sv48 等许多功能，不是 p0 配置。[VR-004、VR-005] |
| XiangShan Mill | `0.12.3` | 冻结 `.mill-version`。Dockerfile 基镜像是 `ghcr.io/openxiangshan/xs-env:latest`，这**不是可重现 pin**；实施时解析镜像 digest、所有包版本、JDK/Chisel/firtool 依赖闭包。[VR-002] |
| Verilator | `v5.052`，tag object `efa4927be48e75c3cd08fc848b198d1d9d237f00` → commit `ea338be98e1e838d3518809ce8899f85a009963c`，tag 日期 2026-09-05 | 当前在线手册标示 5.052；是原生 SV+C++ 的候选基线，不声称与 XiangShan 候选组合已实测兼容。[VR-008] |
| Spike 主线 | `0bff12123b1fd510e19e19634dd997dbade70e54`，2026-09-28 | 主线 CLI/语义对照候选；C++ 内部接口不受公开 API 稳定性保证。XiangShan 的 `riscv64-spike-so` 来自 OpenXiangShan fork，**不是**主线 Spike 原生提供 NEMU ABI。[VR-004、VR-006] |
| Sail RISC-V model | release `0.14.1` → `e4b243f4eb5d1ed05bbbc030ad338c2a32c45d72` | 与下面 ACT4 当前 README 指定的版本配对；这是模型版本，不是 Sail 编译器版本。该 README 要求 Sail compiler ≥0.20.2；冻结具体 compiler/release binary 校验和。[VR-007、VR-009] |
| riscv-arch-test / ACT4 | `6e8a45123f14cebfb3df151a0e7b849b4389b33b`，2026-09-08 | 当前主流程是 ACT4，不是已弃用 RISCOF；Make/Python、UDB、Sail 0.14.1 生成自检 ELF。默认扩展排除项需要展开审计；并非完整微架构验证。[VR-009] |

其余 XiangShan 子模块按完整树递归锁定；未展开在表中不等于允许浮动。规范使用正式批准版本及对应文件 hash；当前 main/snapshot 的 normative 文本只用于本次研究定位，不自动选作产品 ISA 版本。HERD `.cat`、riscv-formal、生成器、交叉工具链在进入对应阶段前固定 SHA/配置/许可证及依赖，不在此伪造未核实版本。

#### 环境与容器约束

保留两个隔离环境：A 为 XiangShan/其 gitlink Difftest/NEMU 的上游校准环境；B 为可移植 SV+C++ 新核环境。共享的只是经过 hash 的程序、平台描述、参考语义配置和结果 schema，不让 A 的 Chisel 工具链成为 B 的强制依赖。Linux x86-64、GCC/Clang C++ 工具链、Make/Python、Verilator 源构建所需 Perl/autoconf/flex/bison、压缩库按上游依赖配置；Spike 需要其 DTC/Boost 等依赖；NEMU 单独确认 readline/SDL/zstd 等配置相关依赖；Sail 源构建确认 compiler/GMP，或使用经校验的 Linux release 包。ACT4 还需 Python、Ruby/Bundler/UDB（可通过 mise/uv 管理）。[VR-005–VR-009]

容器禁止只记录 `latest`、隐式联网下载和主机 `/usr/local` 的偶然工具；记录 OCI digest、arch、locale、uid、编译器、libc、RAM/存储峰值和线程数，源码/输入只读挂载，输出单独目录。资源需求由首次可重放校准测量，不编造“需要若干 GB”硬指标。多 hart `dlmopen` 路径依赖 Linux/glibc 语义，不能假设 macOS 上相同；自定义适配器可以用进程隔离参考实例，但必须证明共享内存事件同步。[VR-003]

## 2. 命令证据等级与未来 CLI 契约

#### A. 已在上游原文核实的示例；本轮没有执行

在冻结的 XiangShan 工作目录，完成它的依赖、绝对路径 `NEMU_HOME`/`NOOP_HOME`/`AM_HOME` 与子模块初始化后，上游 README 原文给出：[VR-002]

```sh
make init
make verilog
make emu CONFIG=MinimalConfig EMU_THREADS=2 -j10
./build/emu -b 0 -e 0 -i ./ready-to-run/coremark-2-iteration.bin --diff ./ready-to-run/riscv64-nemu-interpreter-so
./build/emu --help
```

`MinimalConfig` 是该版本的上游配置名，不等于本项目 p0；`-j10` 只是原例，不是主机资源建议。预编译 coremark 的 ISA/设备/退出协议须另查。

Difftest 原文允许非 Chisel 设计配置 `src/test/scala/DifftestMain.scala` 后生成匹配的 Verilog 和 C++：[VR-003]

```sh
make
```

这不是“把任意 SV 文件放进来即可自动正确对接”。生成器定义的 hart 数、commit 宽度、PRF/VRF 形状和软件结构必须同时匹配。

NEMU 原文给出 `make menuconfig`、`make xxx-ref_defconfig`、`make -j`；`xxx` 是上游原文本身的占位参数，不能直接执行。冻结目录已核实 `riscv64-xs-ref_defconfig`，因此实施时选该配置进行上游校准；新核配置另外冻结，不能复用它并掩盖额外扩展。[VR-005]

Verilator 5.052 原文的小示例（要求该例的 `our.v` 与 `sim_main.cpp` 已存在，不是处理器命令）：[VR-008]

```sh
verilator --cc --exe --build -j 0 -Wall sim_main.cpp our.v
obj_dir/Vour
```

Spike 原文 `spike pk hello` 需要对应 pk、编译后的 `hello` 和完整 ISA/runtime，**不适用于尚无 S/U 的 p0**。Sail 0.14.1 原文源构建入口 `./build_simulator.sh`，执行入口 `build/c_emulator/sail_riscv_sim <elf-file>`，另提供 `--print-default-config`、`--config`；主线参考模型不是自动可插拔 Difftest `.so`。[VR-006、VR-007]

ACT4 原文配置后生成入口为 `CONFIG_FILES=<your_config_directory>/test_config.yaml make`，生成结果在 `WORKDIR/<config_name>/elfs`；这里路径是用户需要替换的上游占位参数。全部产生的适用 ELF 都需运行，不能只看到生成成功便记通过。[VR-009]

#### B. 本项目未来实现契约；现在不存在、不可当作可执行工具

```sh
make sim PROFILE=p0
make unit CASE=<case-id> PROFILE=p0
python3 tools/verify.py --profile <p0|p1|p2|p3> --suite <suite-id> --seed <n> --out <dir>
python3 tools/experiment.py --matrix <json> --out <dir>
build/<profile>/eaf-sim --elf <path> --platform <json> --ref <so> --seed <u64> --events <jsonl> --max-cycles <u64> --max-retire <u64> --out <dir>
```

- `profile` 查版本化配置清单，不按名称猜扩展；`platform` 固定 reset/ISA/CSR/PMA/内存/设备；`events` 固定外部刺激，无事件也用明确空清单。
- `--elf` 初版只接受静态 little-endian ELF64 RISC-V、PT_LOAD；检查入口、地址溢出、重叠段、`filesz<=memsz`、RAM 范围；PIE、动态解释器、未知重定位拒绝并说明。raw binary 仅通过显式已记录的 ELF→image 转换适配上游，不假装 NEMU standalone 可直接执行任意 ELF。
- `--ref` 是经过 ABI/capability 校验的 NEMU-compatible adapter 库；Sail/Spike 可用独立 replay/签名通道，不伪造它们天然实现此 ABI。
- 退出码固定：0 所有适用检查通过；1 DUT/参考或不变量 mismatch；2 配置/镜像/ABI 不合法；3 预算超限或 no-progress；4 工具/内部异常。空套件、缺失输出、被杀进程、未完成 stop、测试进程 crash 均不能变成 0。
- 输出至少含 `run.json`、原始事件流、比较摘要、签名/最终内存、排除账本、首差异与最近上下文；波形只在 ROI 打开但原始刺激必须可重放。退出码只是入口证据，不能代替完整计数闭合。

## 3. 原生 SV 接入方案与 Architectural Event v1

#### 两条可落地路线

**默认路线 A：SV 顶层退休探针 → C++ canonical event → reference adapter。** SV 保持可综合，探针在验证构建启用；C++ harness 用顶层定长数据或显式 DPI 参数传输，不依赖不稳定的层次名称。canonical event 与 upstream bundle 不是同一 ABI。单 hart 先采用每条已提交指令执行一步 reference，独立验证 memory side effect；扩展后引入 vector partial-trap、外部事件及多 hart 内存追踪。

**可选路线 B：冻结 Difftest 生成器 → 生成匹配的 DPI Verilog/C++ → SV wrapper 连接新核。** 只有完成 V-004/V-008 的字节布局/时序/字段测试才切换；保留与路线 A 同一 canonical 事件用于审计。上游支持生成非 Chisel 接口，但高级优化偏 Chisel；不要把 Chisel 的 rename 连接示例当成 SV core 即插即用方案。[VR-003]

从已读 `refproxy.h` 可确认的重要 ABI 事实：`difftest_regcpy(void*, bool, bool)` 不是常见教学示例的两参数版；`difftest_memcpy(uint64_t, void*, size_t, bool)`、`difftest_exec(uint64_t)`、`difftest_raise_intr(uint64_t)`、`difftest_csrcpy(void*, bool)` 均需锁定。方向枚举 `REF_TO_DUT` / `DUT_TO_REF`、packed `ref_state_t` 受编译宏影响；`difftest_init_v2(unsigned)` 仅在库提供时用于结构大小初始化，不能仅凭大小相同推断布局相同。加载缺符号即失败，不“降级为无差分”。多 hart 上游路径要求 `difftest_set_mhartid`、`difftest_put_gmaddr` 及 shared golden memory，绝非 N 个互不通信的单 hart NEMU 即完成验证。[VR-003]

冻结代码中 `ArchVecRegState` 是 **64 个 64-bit 段，即 32×128-bit**；不能据此宣称任意 VLEN 支持。宽 VLEN 阶段必须修改/选择明确兼容的序列化及参考模型配置，然后重新负向校准；不允许截断高位。`refproxy.h` 的 fallback `skip_one` 明确不支持 vector skip。NEMU README 的概述存在“V 将来支持”旧句，但同页及冻结 config 已启用 RVV；实际能力以冻结配置、实现与针对性执行三者核对，不能只取对自己有利的句子。[VR-003、VR-005]

#### 项目 canonical event 字段（未来接口，不声称上游已有）

| 字段组 | 精确定义与比较方式 |
|---|---|
| Header | `schema_version:u32, run_id, hart_id:u32, event_id:u64, retire_order:u64, cycle:u64, slot:u16, kind`；kind 为 RETIRE、SYNC_TRAP、ASYNC_INTERRUPT、VECTOR_PARTIAL_TRAP、MEM_VISIBLE、EXTERNAL_INPUT、HALT。每 hart event_id 单调，retire_order 只对退休递增；同时提交按 hart 内 slot 顺序，不用跨 hart host 回调顺序伪造全局 ISA 顺序。 |
| Instruction | `pc_before:u64, pc_after:u64, insn_bits:u32, insn_len:u8, priv_before/after:u8`；禁止 blanket PC mask、压缩指令低位丢失或把 fetch PC 当 commit PC。异常指令可能有 fetched-byte-valid mask，取指故障时不能编造完整 opcode。 |
| Scalar state | `gpr_write_valid, rd:u5, rd_value:u64`，可选多个 FP/vector/CSR effect；`x0` 永为 0。比较顺序快照的全部 GPR，不仅当前 destination，能发现无意破坏。多退休周期须按 architectural 写集重建每步快照，不用周期末快照比较第一条。 |
| CSR | 列表 `{csr_addr:u12, old:u64, new:u64, implemented_mask:u64, compare_rule_id}`；M 基础至少 mstatus/misa/mie/mip/mtvec/mscratch/mepc/mcause/mtval/medeleg/mideleg（存在才列）、PMP 与计数器；S 阶段加入 sstatus/sie/sip/stvec/sscratch/sepc/scause/stval/satp/权限；F/V 加 fcsr/fflags/frm、vl/vtype/vlenb/vstart/vxrm/vxsat/vcsr、FS/VS/SD。CSR 别名不能双算两个独立状态。 |
| Trap/interrupt | `cause:u64, tval:u64, tval_valid_mask, fault_pc, target_pc, target_priv, interrupt_id, source_event_id, boundary_retire_order`；trap 本身不假装成功退休、不重复 reference step。同步 trap 在 fault instruction 上执行参考语义；异步事件在记录的可接受指令边界注入，先核对 pending/enable/delegation，再比较 epc/cause/status/handler PC。 |
| Memory operation | `{mem_id, parent_event, element_index, segment_field, VA, PA, size_bytes, read/write_byte_mask, read/write_bytes, kind, aq, rl, pma_class, translation_id, exception}`；跨 cache line/page 必须展开子访问，但 architectural 聚合关系可逆。commit-store 与 store-buffer drain/MEM_VISIBLE 是不同事件；load 来源含 store forwarding/store-version 或 reads-from 证据。AMO 一次 read-modify-write 不拆成可穿插的两次普通访问。 |
| Vector | 每个 32-register vector byte 的 pre/post 值、写 mask、defined-value rule；`VLEN, ELEN, vl_before/after, vtype_before/after, vstart_before/after, vm, original_mask_bits, operand_EEW/EMUL, element/field completion`。位精确保留 mask register。一个 macro-uOP 对应一条 ISA 指令，但可产生多个 memory/trap 事件，不把 element completion 当额外 retired instruction。 |
| Debug-only identity | `uop_id, rob_tag, epoch_generation, lease_generation, producer_tag, route, cohort_id, packet_id`；用于不变量/三角定位，不要求 NEMU/Spike/Sail 拥有这些微架构字段。物理 tag 回收后必须靠 generation 区分旧响应。 |
| External/termination | 设备读值、IRQ assert/deassert、DMA 写入、time/随机输入都带事件序号；HALT 区分正常程序退出、测试失败、WFI、预算超限。程序写入 pass magic 不绕过此前 mismatch；必须 drain 与比对最终 memory/signature。 |

#### 允许差异不是任意掩码

1. 掩码/关系规则必须由**规范条款 + profile + opcode + pre-state**生成，不能从 DUT/REF 已经不同的位反推出“don't care”。记录每次应用的位数、事件和 rule ID；规则也是被测对象。
2. GPR、确定性 PC、有效 memory byte 默认全位比较。CSR WARL 是合法值关系，WPRI 是指定读写行为；不能一律屏蔽“保留位”。允许 FS/VS 提前 Dirty、mtval 可选值、misa 固定实现等须精确限制到规范允许的字段。
3. RVV `tu/mu` 必须检查旧值不变；普通 agnostic destination element 通常只能保留旧值或全 1，不是任意噪声。mask destination tail 有额外合法规则；`vl=0`、`vstart>=vl`、prestart 必须按指令规则处理。FOF/partial fault 可以有特定 spurious writes，不得直接把 `vstart` 之后全丢弃而失去边界检查。[VR-013]
4. `cycle/time/instret` 不能拿两台不同微架构的 wall clock 做逐值比较。确定性功能套件用事件驱动虚拟时间；单独计数器测试检查规范增量/权限/暂停语义。性能工作负载真实 cycle 值记入结果但不进入跨 DUT architectural-equality 比较。
5. MMIO 读值由同一外部设备模型向两方提供，地址/宽度/byte-enable/顺序/副作用仍独立比较。禁止常态 `skip=isMMIO` 后把整个 DUT state 拷给参考。无法建模的设备案例标 `UNSUPPORTED_ENV` 并阻断该功能验收，不得默默绿灯。
6. SC 可合法失败；参考实例若选择不同 SC 结果，不直接强行同步 rd。单独建立预约/冲突/可失败性检查，再以记录的允许选择 replay；强制要求成功的受限循环按规范与 PMA 另测 forward progress。随机中断接受延迟、FOF 无故缩短 vl、浮点 unordered reduction 等也用明确合法结果集合/关系，不做统一 bitwise equality。
7. 单 hart retirement lockstep 不充分验证 RVWMO。Spike 自述 SC，只能产生合法执行子集；多 hart 不能因未观察到弱结果就宣称顺序正确。Golden-memory load patching 检查指令计算时可能掩盖内存系统错误，必须有独立 reads-from/coherence/RVWMO checker 与 litmus/formal 证据。规范当前对 I/O、取指、page walk、SFENCE.VMA 及向量 memory model 的形式化仍有范围限制，按各自 ISA 语义单列，而非把一份 `.cat` 当全系统证明。[VR-006、VR-014]

## 4. 有限验收清单、预算与证据格式

所有集合在运行前物化为逐 case `suite-manifest.json`，每行含 case ID、生成器 SHA、ELF hash、ISA/ABI、profile、reset image、reference SHA/config、seed、外部事件 hash、预算、预期结束、比较规则、功能 bin。数量变化需版本化审批，不能失败后删项。**PASS + FAIL + UNSUPPORTED + INFRA_ERROR + TIMEOUT = manifest 总数**；没有 “没跑所以通过”。未知能力也不能冒充不适用。

| 有限 suite | 固定枚举/预算设计 | 未来 gate |
|---|---|---|
| `calibration-v1` | 12 个正控制（算术、跳转、CSR、load、store、trap、interrupt、MMIO、双提交、退出、压缩、vector 高位；后两项按已启用能力），24 个真实 mismatch 注入点 | 适用正控制全过、24 项中的适用故障全部被指定检查器捕获；未启用项明确 deferred，启用时必须补齐 |
| `scalar-directed-v1` | 256 个独立 case：64 整数边界、32 M、32 branch/fetch、32 load/store、32 CSR/trap、32 rename/flush、32 completion/backpressure | 每 case 至少一个事先定义的 signature/首差异断言；全部通过 |
| `scalar-random-v1` | seeds 0–999，每 seed 2,048 条有意义目标指令，另记录 setup/handler/termination 数；四类内存延迟按 seed mod 4 选择 | 1000 个完整终止，无无声 reference skip；目标 bin 未命中仍失败，不能以退休数替代 |
| `metamorphic-v1` | 64 个确定性程序 × fixed/local/remote/reconfigured 四种调度；vector/cohort 启用后各扩展同样 64 组 | 对齐 external stream 后签名与合法 architectural 结果关系一致；允许的差异列账 |
| `extension-directed-v1` | C 64、A 128、S/U/Sv39 192、F/D 256、V 512 个不同 case；不是按单个 opcode 重复凑数 | 每扩展在宣称支持前完成全部 case + 当期全部适用 ACT ELF |
| `fabric-transition-v1` | 32 rollback 时点 × 4 种延迟排列，16 drain 状态 × 8 种刺激，16 cohort 模式 × 8 种刺激 | 每个命名转移/不变量有 observed 证据；公平性假设及超时明确 |
| `litmus-v1` | 48 个计划程序（SB/MP/LB/IRIW/WRC/RWC、依赖、fence、aq/rl、mixed-size/同址等变体；V-036 在运行前冻结全部名称/内容）× seeds 0–199；每次 1,000 iterations | 禁止结果 0 次；有限集合结果均在模型允许集内；不要求所有允许结果一定观察到 |
| `programs-v1` | 12 个自包含 bare-metal 程序：insertion-sort、heapsort、CRC32、SHA-256、linked-list、binary-search、整数矩阵乘、memcpy、重叠 memmove、有限状态 parser、prime-sieve、byte-RLE；每个固定小型、正常与边界/对抗三组输入 | 36 个实例用同 ELF 或可证明等价的 PT_LOAD image、同输入、同参考；F/V 后另增清单；不能用 XiangShan coremark 能跑推出 p0 Linux 能跑 |
| `act-v1` | 冻结 ACT4/UDB/Sail 下**全部适用**自检 ELF，事先导出精确 N 与 hash；上游默认 exclusion 展开为逐项记录 | 适用项全过，若已宣称功能的 normative bin 缺失则由定向测试补齐并追踪，不能记“ACT 100% = CPU 100%” |

`calibration-v1` 的 24 个负控制固定为：N01 ALU add、N02 x0 写保护、N03 load/W sign-extension、N04 CSR 位、N05 branch target、N06 mepc、N07 mcause、N08 IRQ enable、N09 younger-retire、N10 store byte-enable、N11 store PA、N12 store data、N13 MMIO 重复副作用、N14 vector 高段、N15 vector mask、N16 vstart、N17 undisturbed element、N18 FOF vl、N19 FP sticky fflags、N20 丢 commit、N21 重复 store event、N22 同 hart 事件交换、N23 错 hart ID、N24 trace 截断。N14–N18/N19 在 V/F 启用前保留 deferred 行，启用时全部要求 detection；其余不得缺失。每个注入必须改变实际 DUT 结果/设备副作用或真实 trace，不能只伪造比较器输出。

短 directed 的默认预算提案为 200,000 cycles/50,000 retire，random 为 5,000,000 cycles/200,000 retire；不得截断程序后以“预算内无 mismatch”判成功。长 Linux/workload 用独立明确预算，由首次参考运行及最坏延迟合同制定。no-progress 的设计 watchdog 初始取 4,096 cycles，仅适用于环境每 64 cycles 内响应且没有 WFI 的有限微架构实例；其他环境必须使用计算出的 bound 或明确不适用标签。数字是测试工程限额，不是硬件承诺。

每个 run 输出 schema：`source_lock, profile_hash, suite_hash, case_id, seed, input_hashes, command_argv, tool_versions, env_digest, started/completed, exit_status, retire_count_per_hart, event_count_per_kind, compared_fields, semantic_relations_used, skip_ledger, functional_bins_hit/missed, first_mismatch, logs_hashes`。功能 coverage 追踪 requirement×事件×异常×重配的义务；line/toggle coverage 仅定位遗漏，不能替代这些义务。负控制和 timeout 注入与普通功能回归是独立套件，结果不能混在“成功率”分母内。

#### 小模型验证交接协议（实施时逐任务执行）

1. **先定范围，不先运行。** 接单者写明任务 ID、目标 claim/profile、前置 `Depends` 的 artifact hash、实现里程碑、冻结规范条款与 capability 交集；从 V-036 的 `suite-manifest.json` 选定适用 case/seed/预算/预期退出，未支持的功能登记原因和受影响 claim，不从 p0 负例推断可选扩展已通过。输入不齐则记 `Blocked` 并请求明确 owner/决议，不能猜默认配置。
2. **成对验证。** 按 manifest 跑无注入正例并检查退出协议/最终状态，再在相同 ELF、seed、事件和规则下跑真实 RTL 路径故障负例（按任务指定 V-021/022/023）；保存注入 site、激活证据、首次异常 architectural event 与非零退出。只有 timeout、mock compare、被 skip 的故障不是 mismatch；若正例失败先保留现场，不能删 case 或改规则凑 PASS。
3. **闭合并交给下一人。** 每一交付附 `evidence-manifest`：`task_id, claim, profile_hash, source_lock, suite_hash, case_id, seed, elf_hash, external_events_hash, injected_rtl_hash/site(or none), snapshot_hash(or none), command_argv, env_digest, reference_config, compare_rule_hash, exit_status, first_difference(hart/event/field), output_hashes, exclusion_ledger_hash, counts(PASS/FAIL/UNSUPPORTED/INFRA_ERROR/TIMEOUT), upstream_artifact_hashes, downstream_owner`。核对计数等于冻结总数；负控制在单独分母中核对预期被抓，不把其非零误记正常 FAIL。交接方从空输出目录用锁定输入重放选中的正/负例并核对首差异；不能重放、字段/哈希缺失或必须能力被 UNSUPPORTED 时只阻断受影响 claim，回退最后一个已验收配置/规则并附最小事实。

## 5. 细粒度验证任务

下面每项是一项未来可执行工作；`Inputs` 中的“实现里程碑”由父实施计划连接，不表示该实现现在存在。任务不要求现在执行工具。每项继承第 4 节证据 schema，`Fail` 为阻断条件而非可以自动忽略的 warning。

### V-001 — 冻结上游闭包与来源账本
- Depends: none
- Inputs: 第 1 节 SHA、上游主源、架构 ISA 决议里程碑。
- Action: 在实施环境收集 XiangShan 递归 gitlinks、NEMU config、参考库构建 provenance、工具版本及文件 hash；解析容器 tag 为 digest，区分源码重建库与预编译库。
- Outputs: 不含浮动 branch/latest 的 source-lock 清单及来源证据。
- Pass: 每个可执行输入都有不可变标识、来源和许可证；表内 SHA 对应实际 checkout；未核实组合明确不进入执行 gate。
- Fail: 缺失子模块、配置混版、库来源未知、tag/digest 不一致或靠主机偶然 PATH。
- Covers: SRC-03 §XiangShan 的工程方法很适合借鉴；可重现性。
- Sources: VR-001, VR-002, VR-003, VR-004, VR-008。

### V-002 — 建立 capability 交集而非扩展并集
- Depends: V-001
- Inputs: ISA/privilege/profile 决议、编译器选项、各模型精确配置。
- Action: 为 DUT、XiangShan、NEMU、Spike、Sail、ACT4 分别列 XLEN、扩展版本、CSR、PMP/PMA、地址宽度、VLEN/ELEN、misaligned policy、trap 可选行为；计算逐 workload 可比交集及明确 unsupported 原因。
- Outputs: capability-matrix 与每 suite 的适用/拒绝集合。
- Pass: p0 只生成 RV64IM_Zicsr_Zifencei/M-mode 程序；交集之外的测试不能执行后才悄悄 skip；后续扩展保留待验收行。
- Fail: 把 `G` 当 IM、把 upstream config 的 V/H/Sv48 当本核承诺、缺功能被误记通过。
- Covers: SRC-03 §最小原型应该是 Scalar Elastic Backend、§RVV 与 CPU—GPU 连续体。
- Sources: VR-003, VR-005, VR-006, VR-007, VR-009, VR-013。

### V-003 — 校准隔离环境与失败退出
- Depends: V-001
- Inputs: 容器/主机依赖清单、未来 CLI 契约。
- Action: 在未来实施环境建立 A/B 两套锁定环境，运行版本/帮助检查及缺库、错误架构库、无写权限输出目录三个失败场景；记录构建资源峰值但不改硬件预算。
- Outputs: 环境证据包与 CLI 退出码对照。
- Pass: 所有工具与锁一致；三种故障明确非零并有诊断；从空输出目录开始可独立重建同一输入集合。
- Fail: 自动回退无 reference、联网获取未锁依赖、把工具 crash 记为 DUT PASS。
- Covers: SRC-03 §FPGA 原型与验证路线；可复现宿主环境。
- Sources: VR-002, VR-005, VR-006, VR-007, VR-008, VR-009。

### V-004 — 验证 NEMU ABI 与序列化布局
- Depends: V-001, V-002, V-003
- Inputs: 冻结 refproxy、NEMU reference 库、canonical-state 定义。
- Action: 逐符号核对原型、bool/enum/struct 布局和宏；用独立状态写入/读回、已知两条算术指令及一次 store 验证方向；再加载缺符号与错误布局库确认 fail-closed。
- Outputs: ABI manifest、状态 round-trip、实际 reference-step 语义与负控制日志。
- Pass: 所有字段按预期读回且 reference 真正执行产生已知结果；错库在任何 DUT 执行前拒绝；不能只做 mock echo。
- Fail: 两参数 regcpy 误接三参数 ABI、状态截断、大小相等但字段顺序错误、缺扩展符号静默忽略。
- Covers: SRC-03 §XiangShan 的工程方法很适合借鉴；reference adapter。
- Sources: VR-003, VR-004, VR-005。

### V-005 — 执行 XiangShan 上游环境正控制
- Depends: V-003, V-004
- Inputs: 来源闭合的上游组合、ready-to-run coremark、上游 README 示例。
- Action: 未来按第 2 节原例构建并运行 XiangShan+NEMU；保存完整 build 配置、Difftest 开启证据、退出原因及首尾 commit；不将样例迁就成 p0。
- Outputs: 独立 DUT-B 环境校准记录。
- Pass: 样例按其退出协议完成、比较器确实工作且无 mismatch；结果只证明该环境路径。
- Fail: 没加载 reference、只看进程退出 0、替换 workload/配置后仍称上游原例通过。
- Covers: SRC-03 §XiangShan 的工程方法很适合借鉴。
- Sources: VR-002, VR-004。

### V-006 — 校准独立 Spike 与 Sail 语义
- Depends: V-002, V-003
- Inputs: 主线 Spike pin、Sail 0.14.1、12 个无外设依赖的小程序。
- Action: 使用各模型官方 CLI/配置运行同一 ISA 交集程序，比较寄存器/签名与明确语义；记录 C++ API 或日志 adapter 的版本边界，遇分歧最小化并查规范。
- Outputs: 三参考三角对照表、模型差异记录。
- Pass: 12 个程序在确定性字段一致；允许差异由规范条款解释且有独立合法性检查。
- Fail: 把主线 Spike 当 NEMU ABI `.so`、默认 Sail 最大配置冒充 p0、用二比一投票解决规范争议。
- Covers: SRC-03 §Retire/architectural correctness 建议。
- Sources: VR-005, VR-006, VR-007。

### V-007 — 固定通用 ELF 与平台入口
- Depends: V-002, V-006
- Inputs: 交叉工具链、p0 平台描述、linker/startup/退出协议实现里程碑。
- Action: 生成 freestanding/static LP64 ELF，反汇编审计实际 ISA，固定 entry、stack、BSS、RAM、trap vector；导出 PT_LOAD 内容及 binary 转换 provenance，禁用未经审计的 libc/编译器扩展。
- Outputs: 兼容 ELF 契约及首个跨环境程序包。
- Pass: 指令、初始化字节、entry 与终止协议均在 DUT/ref 交集；原 ELF 与转换 image 在声明加载地址的字节一致。
- Fail: 默认编译器悄悄使用 C/A/F、把用户态 ELF 当 bare-metal、假设不同 reset ROM 相同。
- Covers: SRC-03 §software sees normal RISC-V、§最小原型应该是 Scalar Elastic Backend。
- Sources: VR-002, VR-005, VR-006, VR-007, VR-009。

### V-008 — 冻结 architectural event 接口
- Depends: V-002, V-004
- Inputs: 第 3 节 event schema、退休接口实现里程碑、路线 A/B 选择。
- Action: 编写每字段宽度、有效性、边界顺序与采样时刻；对两条同周期指令加 trap、store drain 的实际序列手工推导事件，分别经 SV/C++ 序列化校验。
- Outputs: 版本化 schema、探针映射与逐步期望事件表。
- Pass: 每个 architectural effect 可定位到唯一 hart/指令；没有无定义字段或跨周期错位；路线 B 生成物与软件端完全同版。
- Fail: 只暴露执行结果不暴露退休状态、多个退休共享一份错误快照、vector 被固定截为低 128 位。
- Covers: SRC-03 §uOP 不应该只是传统 CPU 的 micro-op、§Per-Hart Commit Domain。
- Sources: VR-003, VR-008, VR-011。

### V-009 — 证明 reset 与初始状态可重放
- Depends: V-007, V-008
- Inputs: SV reset 实现、明确同步/异步极性、平台初值。
- Action: 在 1/2/5/17 个有效时钟 reset 长度和不同解除相位运行同一程序；reset 中断正在进行的仿真并重启；分别检查 SRAM 非复位内容的初始化合同。
- Outputs: reset 波形片段、首条合法退休与状态证据。
- Pass: reset 期间无提交/设备写；解除后 PC/权限/CSR/queue ownership 确定；同 seed 重放状态一致。
- Fail: 依赖 C++ 内存碰巧为零、reset 期间伪提交、旧事务穿越 reset 污染新 run。
- Covers: SRC-03 §Architectural State 不动态、§precise retire。
- Sources: VR-008, VR-011。

### V-010 — 校准 Verilator 时钟与采样循环
- Depends: V-008, V-009
- Inputs: C++ harness 实现里程碑、Verilator 5.052。
- Action: 明确低相驱动、高相 eval、稳定后事件采样；使用 `eval()`/`final()`，需要延迟时才启用并正确推进 `--timing` 事件；逐项施加 backpressure 检查一次握手仅记录一次。
- Outputs: 单周期时间线、重复采样检测记录。
- Pass: 整数时间单位固定；每次 accept/retire 与 RTL 事件一一对应；end-of-run final assertion 执行。
- Fail: eval 被误作时钟、DPI 回调出现双计数、开启 timing 后不推进 pending event、结束前漏 final。
- Covers: SRC-03 §Elastic Latency Execution、§Completion Fabric。
- Sources: VR-008。

### V-011 — 验证 ELF 装载与内存边界
- Depends: V-007, V-010
- Inputs: harness loader、稀疏/连续 RAM 模型、平台 PMA。
- Action: 对正常多 PT_LOAD/BSS/非零 entry 和损坏 magic、端序、溢出、重叠、越界、filesz>memsz、动态 ELF 逐项执行；以已知 byte pattern 检查 load/store 字节序及边界 fault。
- Outputs: loader case 清单与加载后内存摘要。
- Pass: 正常段逐字节一致、BSS 为规定零、拒绝项在开始运行前 code 2；地址错误不绕回 RAM。
- Fail: 只加载第一个 segment、忽略 ELF entry、silent truncation、错误映像继续跑。
- Covers: SRC-03 §Memory Fabric；程序镜像可移植性。
- Sources: VR-003, VR-007, VR-008, VR-009。

### V-012 — 固定 signature 与终止判断
- Depends: V-010, V-011
- Inputs: startup/退出协议、signature 区间、外部设备模型。
- Action: 分别运行正常完成、显式 FAIL、无退出循环、WFI、写过 PASS 后出现 mismatch、signature 越界与末尾 pending store 的程序。
- Outputs: 终止状态机证据及最终 signature/memory 文件。
- Pass: 只有完整协议完成、检查器无误且规定 drain 完成才返回 0；每种错误获得约定非零。
- Fail: timeout 当成功、只匹配 UART 文本、未完成 store 就读 signature、空程序算通过。
- Covers: SRC-03 §precise retire、§FPGA 原型与验证路线。
- Sources: VR-003, VR-008, VR-009。

### V-013 — 比较多退休宽度与顺序
- Depends: V-008, V-012
- Inputs: 1/2-wide 退休实现、GPR 全快照重建。
- Action: 执行同周期相同 rd 两次写、x0 写、branch 后指令、slot0 trap/slot1 普通指令、store/load 相邻，按每 hart program order 逐条 step reference。
- Outputs: 每 slot architectural delta 与比较记录。
- Pass: older-before-younger、无重复/跳漏 retire_order；slot0 异常阻止非法 younger retirement。
- Fail: 周期末状态污染早 slot、slot 顺序靠回调偶然排列、x0 被改、trace gap 未报错。
- Covers: SRC-03 §从 Giant ROB 转向 Per-Hart Commit Domain。
- Sources: VR-003, VR-011。

### V-014 — 比较同步 trap 与精确状态
- Depends: V-013
- Inputs: illegal/ecall/ebreak/对齐/访问错误注入程序、M trap 实现。
- Action: 将每种 trap 放在 ROB 头/非头、长延迟前后与同周期完成场景；核对 fault PC、cause、tval、mepc、mstatus、handler PC 和 mret 恢复。
- Outputs: 逐种 trap 的 pre/post state、被 squash 的 younger 证据。
- Pass: fault 指令不记成功退休；older effect 完整、younger 不可见；规定 CSR 和 PC 全部正确。
- Fail: exception 写回先污染 rd、错误 tval 被全屏蔽、reference 多走一步、trap 后重放丢指令。
- Covers: SRC-03 §Ordered Commit / Precise State、§No instruction retires before older unresolved exception。
- Sources: VR-003, VR-007, VR-012。

### V-015 — 驱动可重放的异步中断
- Depends: V-014
- Inputs: event 文件、M interrupt 控制器模型、WFI 合同。
- Action: 在已编号退休边界 assert/deassert timer/software/external IRQ，覆盖 pending-but-disabled、使能 CSR 紧邻、同步 trap 竞争、WFI 唤醒；分别记录 asserted 与 accepted 边界。
- Outputs: 可重放 IRQ 时间线及优先级/屏蔽测试记录。
- Pass: 接受满足 profile 规范优先级、epc/目标权限正确；同刺激 replay 同 architectural 接受边界；未接受 IRQ 不被丢失。
- Fail: 用 host wall time 注入、绕过 DUT enable 判定给参考补状态、把 IRQ 当一条普通退休指令。
- Covers: SRC-03 §compatible exception/control state、§precise exception。
- Sources: VR-003, VR-012。

### V-016 — 校验 PC、取指及 fence.i
- Depends: V-014
- Inputs: p0 fetch/branch/fence.i 实现、自修改代码测试。
- Action: 覆盖 JAL/JALR bit0 清除、正负分支、取指跨行/页前边界、对齐 fault、修改代码后 fence.i，以及错误路径取指异常。
- Outputs: PC 转移与取指可见性用例证据。
- Pass: 逐条 pc_before/after 正确；fence.i 后观察到规定的新代码；错误路径异常不泄漏。
- Fail: 避免 mismatch 而 mask PC 高位/低位、将 cache 命中等价于正确取指、p0 私自执行 C。
- Covers: SRC-03 §不要把 Frontend 也完全 Fabric 化、§Branch Prediction 不应该被过度 Fabric 化。
- Sources: VR-012, VR-014。

### V-017 — 建立 CSR 合法关系比较器
- Depends: V-014, V-015
- Inputs: CSR 实现表、规范版本、comparison-rule schema。
- Action: 每个实现 CSR 覆盖合法写、保留/WARL 值、只读写、权限错误、alias；独立测试计数器读写/暂停/权限与 FS/VS 合法提前 Dirty 规则。
- Outputs: CSR 逐位规则账本、每条规则的正反例。
- Pass: 确定字段严格比较；允许关系能接受至少一个合法不同结果并拒绝相邻非法结果；规则带条款和适用前提。
- Fail: 整个 mstatus/mip 不比较、从差异自动生成 waiver、未实现 CSR 按零无条件接受。
- Covers: SRC-03 §Architectural State 不动态；CSR 可观测行为。
- Sources: VR-003, VR-007, VR-012, VR-013。

### V-018 — 分开 architectural memory 与可见 store
- Depends: V-011, V-013
- Inputs: p0 ordered LSU/store-buffer（I-035）、memory-operation schema；若启用可选 speculative load replay（I-036），另附 late-alias/回滚里程碑。
- Action: p0 覆盖所有 load/store 宽度、符号扩展、部分 byte mask、同 hart forwarding、跨行、错误路径 store；记录 store commit 与真正 drain，比较 reference memory 及最后 byte signature。I-036 启用时重跑这些检查并注入 late-alias replay/kill，与 V-029 的 stale completion 证据交叉核对。
- Outputs: 有因果 ID 的 load/store/visibility trace；可选 I-036 另附 replay/rollback trace。
- Pass: p0 每个可见 byte 有合法已提交 producer，load 返回值与允许来源相符，取消 store 不外泄；声明 I-036 时还须证明 late alias 重放后无旧 load/错误 store 可见。
- Fail: 仅比较 rd 漏掉错误 store、将 cache refill 当 load effect、重复 drain、用 DUT memory 覆盖 reference 后宣称一致；可选 replay 失败不得阻断已验收的保守 p0 配置。
- Covers: SRC-03 §Memory Operation 也应该成为 Packet、§Store visibility obeys selected model。
- Sources: VR-003, VR-014。

### V-019 — 实现独立 MMIO 副作用模型
- Depends: V-015, V-018
- Inputs: UART/timer/测试结束寄存器模型、PMA 非幂等区域表。
- Action: 运行 destructive-read、partial-write、非法宽度、未映射地址、flush 中 speculative MMIO；同一个已锁事件给 DUT/reference，独立计数实际副作用。
- Outputs: 设备 I/O 事务日志与差分设备状态。
- Pass: 每项副作用仅一次且顺序/字节使能正确；被取消访问无副作用；MMIO rd 数据相同不掩盖错误地址。
- Fail: 读两次破坏设备状态、默认所有 MMIO skip、错误路径设备写、未建模设备默认为零。
- Covers: SRC-03 §Memory Fabric、§coalesced memory 的例外边界。
- Sources: VR-003, VR-005, VR-012, VR-014。

### V-020 — 禁止静默 exclusions 与 reference skip
- Depends: V-017, V-019
- Inputs: 每个 source/profile 比较规则、ACT4 默认 exclusion、Difftest waive/skip 路径。
- Action: 枚举所有禁用 checker、skip、CSR waive、未实现 reference feature、超时与生成失败；每项记录原因、规范依据、替代验证、受影响 case/字段和结束条件；未登记项故意触发拒绝。
- Outputs: 机器可读 exclusion ledger 与计数闭合报告。
- Pass: 未登记排除为 0；任何已承诺功能不能仅以 waiver 获得验收；新规则必须通过负控制。
- Fail: 用 broad skip/ignore_illegal_mem_access 掩盖错误，测试被 filter 后不在总数，waive 同步 DUT→REF 后缺独立检查。
- Covers: SRC-03 §architectural verification retirement boundary；验证可信性。
- Sources: VR-003, VR-009。

### V-021 — 注入真实整数与 CSR 差异
- Depends: V-013, V-017, V-020
- Inputs: 可运行 DUT 及独立故障注入构建、calibration-v1。
- Action: 在已执行路径翻转 ALU add 结果、x0 写保护、sign extension、CSR 一位；F 启用后额外破坏 sticky fflags；同程序运行无故障/有故障两构建，保存首次受影响 retire。
- Outputs: 故障 site→checker→首次差异矩阵。
- Pass: 无故障正控制全过；每个注入确实改变 DUT architectural state，且比较器在首个应检查边界报非零。
- Fail: 只改 expected 文件、mock mismatch、注入点没执行、错误被状态同步消掉、仅检测最终 checksum 却漏首差异。
- Covers: SRC-03 §Retire architectural destination/CSR effects。
- Sources: VR-003, VR-011。

### V-022 — 注入 PC、trap 与 interrupt 错误
- Depends: V-014, V-015, V-016, V-020
- Inputs: branch/trap/IRQ 正控制、可定位 RTL 注入点。
- Action: 分别改 branch target 一位、mepc 偏移、mcause、IRQ enable 判断、trap 后 younger-retire；对每项证明路径激活后检查器报告对应字段。
- Outputs: 控制流故障检测证据。
- Pass: 所有实际注入均在正确事件定位；trap 没有成功退休时仍能检出。
- Fail: checker 只看 rd 无法发现错误 PC/trap，缺 IRQ event 被当合法延迟，timeout 被当 mismatch 的替代证据。
- Covers: SRC-03 §No older unresolved exception、§Branch recovery。
- Sources: VR-003, VR-012。

### V-023 — 注入 store、MMIO 与 vector 高位错误
- Depends: V-018, V-019, V-020
- Inputs: memory/设备正控制；vector 里程碑启用时追加高位正控制。
- Action: 修改 store byte-enable、PA、数据，制造重复 MMIO 写；V 启用后改变向量高 64-bit、mask bit、vstart、被保护 inactive element 与 FOF 最终 vl，分别运行。
- Outputs: 不依赖普通 GPR 的故障检测矩阵。
- Pass: 可见内存/设备/向量错误全部被对应 checker 检出；未启用 vector 明确为 deferred 且阻止 V gate。
- Fail: 仅最终 GPR 一致便通过、vec trace 截断、全屏蔽 agnostic region 导致非法值通过。
- Covers: SRC-03 §Memory Fabric、§Vector Macro-uOP、§precise semantics。
- Sources: VR-003, VR-013, VR-014。

### V-024 — 注入 trace 丢失、重复与乱序
- Depends: V-008, V-020
- Inputs: 真实 DUT canonical event trace、transport checker。
- Action: 在 DUT 事件传输边界丢一个 commit、复制一个 store、交换同 hart 相邻事件、错 hart ID、截断文件；与原始 trace 分别 replay。
- Outputs: 事件完整性故障检测记录。
- Pass: 五类故障均因明确完整性/语义检查失败；不能通过补齐猜测事件得到 PASS。
- Fail: 只以末尾 pass magic 判成功、按 PC 去重合法重复 loop、跨 hart 事件串扰未报。
- Covers: SRC-03 §Hierarchical completion fabric、§per-hart retirement。
- Sources: VR-003, VR-011。

### V-025 — 执行 RV64I 与 M-mode 定向基础集
- Depends: V-012, V-021, V-022, V-023, V-024
- Inputs: scalar-directed-v1 的整数、branch、memory、CSR/trap 子集。
- Action: 逐 case 执行固定输入的 0/−1/min/max、shift 边界、32-bit W sign-extension、立即数、分支目标与异常；每个 case 使用独立期望值而非 DUT 生成 expected。
- Outputs: 256-case 清单中已完成子集与功能 bins。
- Pass: 全部适用条目成功终止，规定结果/异常与 reference 一致，边界 bins 实际命中。
- Fail: 部分 opcode 未生成、signed/unsigned 混淆、只记录“跑过若干指令”。
- Covers: SRC-03 §最小原型应该是 Scalar Elastic Backend。
- Sources: VR-007, VR-009, VR-012。

### V-026 — 执行 M 的长延迟与边界测试
- Depends: V-025
- Inputs: 32 个 M 定向 case、可变延迟 MUL/DIV 实现。
- Action: 覆盖 high multiply signed 组合、W 结果、除零、min/−1、商余符号、相同 tag 资源复用；运算中插入 flush/reset/重配请求。
- Outputs: M 语义结果及 long-latency cancellation 记录。
- Pass: 结果符合指令规则，取消运算不写新 owner，单位恢复接受后续合法操作。
- Fail: 除零 host exception、溢出处理不符、旧 DIV 完成命中新 tag、资源永不释放。
- Covers: SRC-03 §Elastic Latency Execution、§Dynamic FU routing。
- Sources: VR-007, VR-012。

### V-027 — 验证 rename、PRF 与唯一 producer
- Depends: V-013, V-025
- Inputs: rename/PRF/free-list 实现里程碑、小容量与研究容量两组配置。
- Action: 构造 RAW/WAR/WAW、同周期 rename bypass、PRF bank 冲突、free-list 满/空、ROB wrap；追踪每个活跃物理目的的 producer 与回收时刻。
- Outputs: tag ownership invariant 与 architectural signature。
- Pass: 每个活跃目的恰有一个合法 producer，旧值存活到最后消费者；bank stall 不丢写；容量参数变动不改语义。
- Fail: false-ready、提前 free、回收后旧响应污染、仲裁下无声 drop。
- Covers: SRC-03 §Every physical destination has exactly one valid producer、§Elastic Window。
- Sources: SRC-03:130-229, SRC-03:1929-1996, VR-011。

### V-028 — 对比 fixed/local/remote 调度语义
- Depends: V-026, V-027
- Inputs: 同资源固定与弹性后端里程碑、64 个无竞争确定性程序。
- Action: 对每程序固定输入和 external stream，在 fixed/local/remote/reconfigured 四种调度下运行；故意改变完成先后与网络延迟，比较 canonical architectural trace。
- Outputs: 64×4 metamorphic 结果及 route 延迟记录。
- Pass: 规定确定性架构状态和内存结果一致；只有 cycle/route 等微架构指标变化。
- Fail: 以不同调度不同结果当合法、通过禁用远程模式规避失败、同程序多次运行却未改变拓扑。
- Covers: SRC-03 §architectural order 固定 execution topology 动态、§Fast Local Execution Island。
- Sources: SRC-03:55-369, SRC-03:1735-1818, VR-014。

### V-029 — 在回滚后投递 stale completion
- Depends: V-028
- Inputs: epoch/lease/producer generation 实现、可延迟 completion transport。
- Action: 分别将 ALU、DIV、load、vector packet（启用后）完成延迟到 branch kill 后、新 owner 租用后与 tag 重用后；对应 32 个命名回滚时点×4 排列。
- Outputs: 被拒绝 stale message 计数与无 architectural 污染证据。
- Pass: stale 消息不能改 PRF/VRF/ready/ROB/memory；被丢弃事务仍正确释放信用；所有活跃新指令可继续。
- Fail: 只检查 ROB 但错误写回 PRF、旧 ready 唤醒新消费者、无限占用 credit。
- Covers: SRC-03 §No wrong-epoch uOP can update architectural state、§Branch Mask/Epoch。
- Sources: SRC-03:1878-1928, VR-011。

### V-030 — 检验 checkpoint 回收与嵌套分支
- Depends: V-027, V-029
- Inputs: branch checkpoint/ROB/free-list 实现。
- Action: 覆盖两层及最大配置深度嵌套、checkpoint 耗尽、oldest/youngest mispredict、异常与 mispredict 同周期、回滚后立即重命名。
- Outputs: rename map/free-list/branch-mask 恢复对照。
- Pass: 恢复目标精确、存活 older producer 保留、被杀 younger 所有资源最终回收且无双 free。
- Fail: 全清导致 older 丢失、checkpoint 污染下一分支、回收不足形成永久假满。
- Covers: SRC-03 §Branch Prediction 不应该被过度 Fabric 化、§Elastic Window。
- Sources: SRC-03:1878-1996, VR-011。

### V-031 — 检验 epoch 与 lease 计数回绕
- Depends: V-029, V-030
- Inputs: generation 位宽与最迟响应寿命合同、缩小位宽验证配置。
- Action: 让回滚/租约切换超过两轮完整 generation 空间，保留最旧消息直到同低位 tag 再现；证明 drain 或足够宽计数阻止 ABA。
- Outputs: 回绕反例搜索及寿命界证明记录。
- Pass: 不能把跨完整回绕旧事务识别为新事务；位宽约束由最大存活时间/排空协议推导。
- Fail: “64 位不会回绕”替代参数化证明、小配置 alias 未检查、overflow 静默复用。
- Covers: SRC-03 §Epoch ID、§coarse-grain resource reconfiguration。
- Sources: SRC-03:282-323, SRC-03:1878-1928, VR-011。

### V-032 — 检验网络 credit 守恒和背压
- Depends: V-028
- Inputs: router/IQ/result buffer credit 协议、2-entry local result buffer 候选。
- Action: 填满每一输入/输出队列，逐个释放，覆盖同时 push/pop、kill+pop、重复返回信用、下游长期 stall；逐端口核算 issued−returned。
- Outputs: credit 守恒证明与饱和状态 trace。
- Pass: 对每条通道 free+occupied+inflight 恒等于容量；不丢包、不重复、无组合 ready 环依赖。
- Fail: 信用超发、kill 双还、packet 永久滞留、绕过 backpressure 覆盖未消费结果。
- Covers: SRC-03 §Scheduler Hierarchy、§Completion Fabric、§distributed waiting state。
- Sources: SRC-03:230-281, SRC-03:1273-1342, VR-011。

### V-033 — 在有界环境下验证 forward progress
- Depends: V-026, V-032
- Inputs: 环境响应上界、仲裁公平性合同、wait-for graph。
- Action: 为 ALU/DIV/load/return/commit 等资源构造循环等待压力；在每 64 cycles 至少一个响应的约束下检查有界完成；另注入永久不响应确认 code 3 与挂起资源诊断。
- Outputs: 环境假设、计算的进展界与 watchdog 命中证据。
- Pass: 有界场景中每个 eligible oldest 工作在合同内进展；永久不响应归类环境 timeout 而非通过。
- Fail: 只有平均吞吐、用不现实公平性 assume 掩盖设计饥饿、无法区分合法 WFI 与 deadlock。
- Covers: SRC-03 §Criticality/age scheduling、§Elastic Latency Execution。
- Sources: SRC-03:1200-1256, SRC-03:1343-1424, VR-011。

### V-034 — 验证资源重配置 drain 与原子发布
- Depends: V-031, V-033
- Inputs: lease 协议状态机、重配实现里程碑、16 drain 状态×8 刺激矩阵。
- Action: 在 IQ/PRF consumer/result buffer/LSQ/store buffer/vector descriptor 各有未完成事务时请求重配；观察停止接收、完成/取消、依赖与数据保留、ownership 新代发布。
- Outputs: 每类资源 drain ledger 与 reconfiguration transition trace。
- Pass: 所有规定旧代引用解除后才授予新 owner；未迁移 architectural state 不丢失；确认后旧代请求全部拒绝。
- Fail: 只看 IQ 空忽略 memory/return in-flight，非幂等访问重做，资源一度同时归两 hart。
- Covers: SRC-03 §Dynamic scaling 两个时间尺度、§Execution Ownership 动态。
- Sources: SRC-03:282-323, SRC-03:2347-2528, VR-011。

### V-035 — 验证重配置与异常/取消的竞争
- Depends: V-014, V-015, V-034
- Inputs: 重配协议、trap/IRQ/reset/软件取消请求。
- Action: 在 prepare/drain/publish/ack 四边界分别注入 trap、IRQ、请求撤销与 reset；验证失败重配保留旧配置或进入明确恢复状态。
- Outputs: 竞争优先级表及逐分支 state trace。
- Pass: 没有半发布配置；trap 精确且资源无泄漏；取消不吞已接受事务；所有失败路径可继续运行。
- Fail: cancel 后永久等 ack、IRQ 错 hart、恢复路径只在普通成功时有效。
- Covers: SRC-03 §precise exceptions、§resource leasing、§five personalities。
- Sources: SRC-03:282-323, SRC-03:727-789, VR-012。

### V-036 — 物化 seed、刺激与套件 manifest
- Depends: V-002, V-020
- Inputs: 第 4 节有限集合、generator/tool locks、future verify CLI。
- Action: 运行前展开每 case 及 seed，分离生成 seed、内存延迟 seed、外部事件 seed；冻结程序 hash、预算与 expected termination，验证重复/缺失 ID 被拒绝。
- Outputs: 可审计 suite-manifest 与覆盖分母。
- Pass: 每个计划实例唯一可定位；种子足以配合锁版本重建输入；运行后不能改 manifest 解释失败。
- Fail: 只保留一个 PRNG seed 却漏 host nondeterminism、生成失败未计数、case collision。
- Covers: SRC-03 §FPGA 原型与验证路线；确定性测试管理。
- Sources: VR-009, VR-010。

### V-037 — 执行标量 constrained fuzz
- Depends: V-025, V-026, V-036
- Inputs: scalar-random-v1、能力交集与已校准比较器。
- Action: 生成 seeds 0–999 的合法/定向非法混合程序，控制可终止分支、地址区域、陷阱恢复与寄存器依赖；每例 2,048 目标指令，运行四种确定性 memory-delay 配置。
- Outputs: 所有 ELF/输入 hash、功能 bin 及每例结果。
- Pass: 所有实例完成或给出可定位失败；非法指令按预期 trap 而非删去；达到预先声明的依赖/异常 bins。
- Fail: RISCV-DV README 未承诺的 RVV 能力被假定存在、UVM generator 未验证可在 Verilator 使用、超时 program 被重抽 seed 替换。
- Covers: SRC-03 §Scalar Elastic Backend、§useful work in flight。
- Sources: VR-010；若不用 UVM，项目生成器必须另行独立校准。

### V-038 — 最小化失败而不改变故障性质
- Depends: V-021, V-024, V-037
- Inputs: V-021 或 V-022 已激活 RTL 注入路径的真实 mismatch（p0 可选 V-021 ALU/CSR），原始 ELF/seed/事件、注入构建 hash/site、首差异指纹与 immutable manifest；V-037 的自然失败不是必要输入。
- Action: 固定注入版本/开关与比较规则，先以同一输入跑无注入正例 PASS、有注入负例在目标 architectural event FAIL；删除无关指令/数据/刺激，保持目标 opcode、trap 前提、hart 数与调度故障可达；每次缩减用实际 DUT+REF 复核，禁止以改变 skip/mask 换取“复现”。
- Outputs: 最小 ELF、原始到最小化 provenance、注入 site/seed/首差异与 reduction log。
- Pass: 最小 case 的无注入正例通过、有注入负例在固定环境重复三次产生同一首差异类别和相关字段；原始 case 永久保留。无自然 fuzz 缺陷时仍可凭可重放的 RTL 注入完成 p0 此项。
- Fail: mismatch 缩成另一个 timeout、只在 reference 运行、只改比较器或 trace、改平台/异常前提却宣称同一 bug。
- Covers: SRC-03 §XiangShan 工程方法；failure triage。
- Sources: VR-003, VR-010。

### V-039 — 验证从零与 checkpoint 的 replay
- Depends: V-038
- Inputs: V-038 固定的已激活 RTL 注入构建/site、无注入正例、最小 ELF/seed/外部事件、DUT/reference/RAM/device/PRNG/lease/event cursor 状态、波形 ROI。
- Action: 对同一个注入故障分别从 reset 和首差异之前最近 snapshot 重放；固定注入开关及事件 cursor，逐事件比较 event hash 到同一首差异；关闭注入后两路径都须完成正例；若采用 LightSSS，遵循其禁止混用的 debug flags 并验证 fork/thread 行为。
- Outputs: reset/snapshot 双路径 replay bundle、正负对照与状态完整性证明。
- Pass: 两条注入重放路径复现同一首差异，关闭注入均 PASS；snapshot 显式包括 RTL 注入状态及全部外部非确定性和事务状态；p0 无自然故障仍可凭此负控制通过。
- Fail: 只保存 CPU register、遗漏注入开关/seed、fork 后线程丢失、device/host time 未固定、打开 trace 改变故障但无解释。
- Covers: SRC-03 §XiangShan 的工程方法很适合借鉴。
- Sources: VR-003, VR-008。

### V-040 — 关闭 functional coverage 空洞
- Depends: V-028, V-035, V-037
- Inputs: requirement→case→bin 映射、实际 hit/miss 数据。
- Action: 审查 opcode×依赖×异常×route×backpressure 的必需组合，逐一补充最小确定性 case；对不可达 bin 给出设计/形式证据，不用代码覆盖百分比替代。
- Outputs: 功能覆盖义务闭合表与不可达证明。
- Pass: 所有声明 mandatory bin 均已命中或有可审阅的不可达证据；未达项明确阻断相关 gate。
- Fail: 用 line/toggle 大数字代替异常/恢复覆盖、将失败 bin 标为不可达、只统计总退休数。
- Covers: SRC-03 §全部验证 invariants、§IPC/throughput 指标区别。
- Sources: VR-009, VR-010, VR-011。

### V-041 — 绑定 RVFI 并验证标量形式性质
- Depends: V-008, V-025
- Inputs: RVFI wrapper 实现、riscv-formal/Yosys/solver 锁、RV64I 支持范围。
- Action: 对已覆盖指令与 commit 数据建立 RVFI，运行指令/寄存器/PC/唯一性/因果检查；明确每项 bounded/unbounded、深度、环境 assume，并加入 reachability cover 防 vacuity。
- Outputs: proof manifest、proof/cex、assumption/coverage 报告。
- Pass: 声称证明的范围均有有效 proof，关键状态可达；未知/timeout 独立失败；trace wrapper 不改变功能有证据。
- Fail: 一次 bounded PASS 称全处理器证明、假设 DUT 结论本身、宣称现成 riscv-formal 完整证明 RVV/多 hart。
- Covers: SRC-03 §architectural order、§precise retire。
- Sources: VR-011。

### V-042 — 证明 Fabric 局部协议不变量
- Depends: V-031, V-032, V-034
- Inputs: 小实例 formal wrappers、tag/credit/lease 不变量。
- Action: 分别证明无双 owner、无 stale write、credit conservation、kill 后不能 commit、重配发布前旧代排空；用非空 traffic cover 和故意移除 guard 的变异版检验性质非空。
- Outputs: 每不变量独立 proof/反例与参数推广边界。
- Pass: 小实例性质证明且 mutant 至少产生预期反例；大容量参数推广有数学/归纳说明，否则只宣称小实例。
- Fail: 所有输入 assume 无冲突、无 cover、只检查 final output 忽略中间 architectural 污染。
- Covers: SRC-03 §uOP tags、§Completion Fabric、§Dynamic reconfiguration。
- Sources: SRC-03:130-369, SRC-03:1273-1595, VR-011。

### V-043 — 运行 ACT4 全部适用 architectural tests
- Depends: V-007, V-017, V-020, V-025, V-036
- Inputs: ACT4/UDB/Sail 版本闭包、DUT linker/rvmodel_macros、实际支持 profile。
- Action: 对齐 UDB/sail.json/rvtest_config 与平台；展开默认 EXCLUDE_EXTENSIONS 与 include_priv_tests，生成全部适用 ELF，记录精确 N 后在 DUT 执行并捕获每个 pass/fail。
- Outputs: N 个 ELF hash/结果、签名来源、规范 coverpoint 与排除 ledger。
- Pass: N 个实际运行全部通过，N 与生成清单闭合；自检 PASS 宏已校准；不把官方测试视作充分验证/认证授权。
- Fail: 沿用废弃 RISCOF 命令而不锁旧流程、只运行样例、默认排除已承诺 S 功能、Sail 配置不符。
- Covers: SRC-03 §software sees normal RISC-V、§retirement verification。
- Sources: VR-007, VR-009。

### V-044 — 验收压缩指令与混合长度取指
- Depends: V-016, V-043
- Inputs: C 实现里程碑、64 个 C 定向 case、C 扩展能力更新。
- Action: 执行每个必需 C 指令和 reserved encoding，覆盖 16/32-bit 混排、跨 fetch line/页、JALR/branch/exception epc、instruction length trace。
- Outputs: C 指令/长度/边界 coverage 与 capability 更新证据。
- Pass: 64 个 case 和全部适用 C ACT 通过；非法 encodings 正确分类；退休长度正确。
- Fail: 以 isRVC 猜 PC 而不核实 target、跨页第二半取指故障丢失、编译器启 C 早于硬件验收。
- Covers: SRC-03 §普通 RISC-V binary 兼容。
- Sources: VR-003, VR-006, VR-007, VR-012。

### V-045 — 验收 AMO 原子性与 aq/rl
- Depends: V-018, V-043
- Inputs: A 实现里程碑、128-case A 清单中的 AMO 子集、共享内存操作记录。
- Action: 测 W/D、signed/unsigned min/max、原值返回、对齐/PMA fault、aq/rl 四组合；竞争 hart 在 RMW 中间尝试写同地址。
- Outputs: AMO 原子事务 trace、返回值与最终 memory 对照。
- Pass: AMO 是单一合法线性化操作，W sign-extension 正确，禁止访问按 PMA trap，排序规则成立。
- Fail: RMW 暴露中间值、错误原值、aq/rl 被当性能 hint 忽略、AMO merge 成普通 store。
- Covers: SRC-03 §Store visibility obeys memory model、§shared MEF。
- Sources: VR-012, VR-014。

### V-046 — 验收 LR/SC 预约与有限进展
- Depends: V-045, V-033
- Inputs: reservation-set/PMA 规范、A 清单 LR/SC 子集、允许失败策略。
- Action: 覆盖成功、无 LR、同/异址 SC、他 hart/设备冲突、context switch/异常、虚实地址别名与部分重叠写；对规范受限 LR/SC 循环检验在公平环境下进展。
- Outputs: 每次 SC 的可成功/必须失败/允许失败判据及结果。
- Pass: 成功 SC 有合法预约且原子写；失败 SC 不产生 store；受限循环满足实现给定有界测试目标并由协议论证补充。
- Fail: 强制所有 SC 匹配参考同一次成功选择、无限“允许失败”掩盖 livelock、失败仍写 memory。
- Covers: SRC-03 §per-hart store ordering state、§forward progress。
- Sources: VR-003, VR-012, VR-014。

### V-047 — 验收 S/U 权限与委托
- Depends: V-014, V-015, V-017, V-043
- Inputs: S/U 实现里程碑、192-case privileged 清单中的控制子集。
- Action: 逐层验证 ecall、mret/sret、medeleg/mideleg、S/U 非法 CSR、MPRV/SUM/MXR、WFI/TW 等 profile 规定字段；同 hart 在权限切换前后执行相同地址访问。
- Outputs: 权限转移矩阵与 trap state。
- Pass: 每种来源/目标权限、epc/cause/status 与访问许可准确；没有把 M-mode 访问规则泄到 U-mode。
- Fail: 通过全权限 reference 绕过错误、delegate priority 错、错误 CSR 用零值应答而非 trap。
- Covers: SRC-03 §正常 harts/virtual memory/Linux 路线。
- Sources: VR-007, VR-009, VR-012。

### V-048 — 验收 Sv39 page walk 与 fault
- Depends: V-018, V-047
- Inputs: Sv39/PTW 实现、PA/ASID/PMP/PMA/A-D policy、privileged 清单 VM 子集。
- Action: 覆盖 4 KiB/2 MiB/1 GiB leaf、非 canonical VA、非法 PTE、superpage 对齐、R/W/X/U、A/D 位策略、page-table access fault、跨页 load/store 与 PMP 优先级。
- Outputs: VA→PA/permission/fault 逐项 trace。
- Pass: 所有翻译和 fault 与同配置参考一致；不支持的 satp mode 按 WARL 处理；页表物理访问也受规定权限限制。
- Fail: 把 upstream NEMU Sv48 config 直接当 Sv39 验证、只测普通命中、跨页前半错误可见而不按平台合同处理。
- Covers: SRC-03 §normal virtual memory、§memory packets。
- Sources: VR-005, VR-007, VR-012。

### V-049 — 验收 TLB shootdown 与地址域隔离
- Depends: V-048
- Inputs: TLB/ASID/SFENCE.VMA、后续多 hart shootdown 协议。
- Action: 对页表映射/权限修改运行有/无 SFENCE.VMA 对照，覆盖 rs1/rs2 各种零/非零组合、global mapping、ASID 复用；多 hart 阶段用软件同步执行远程 fence。
- Outputs: 翻译缓存一致性及旧响应失效记录。
- Pass: 必须失效的条目不可继续使用；晚到旧 PTW response 不重新插入 stale translation；未承诺的自动全系统 fence 不被假设。
- Fail: 将 SFENCE.VMA 当普通 memory fence、只清当前 TLB 忽略 PTW、cohort 共用错误 ASID。
- Covers: SRC-03 §per-hart architectural state、§Cross-Hart Coalescing 的地址前提。
- Sources: VR-012, VR-014。

### V-050 — 验收 F/D 数值与舍入
- Depends: V-025, V-043
- Inputs: F/D 实现里程碑、256-case FP 清单、参考 FP 配置。
- Action: 覆盖 ±0、subnormal、±∞、qNaN/sNaN、NaN-boxing、五种合法 rounding mode、动态 frm、FMA 非融合对照、转换边界、比较/min/max；D 前确保 F 依赖。
- Outputs: 数值/flags/rounding 功能矩阵。
- Pass: 确定结果与 IEEE/RISC-V 指定行为一致，合法 NaN 选择按精确规则比较；所有必需操作有定向与 ACT 证据。
- Fail: 使用 host double 默认舍入当全部 oracle、FTZ 偷改语义、只比较近似误差不比较 architectural bits/fflags。
- Covers: SRC-03 §FP/FMA slices、§共享 datapath。
- Sources: VR-006, VR-007, VR-012。

### V-051 — 验收 FP 状态、异常与共享隔离
- Depends: V-050, V-017
- Inputs: fcsr/FS/context save-restore、共享 FPU 调度。
- Action: 交错两种 rounding context、fs=Off、sticky fflags、异常/flush 中的长延迟 FP；测试 CSR 写与 FP 完成同周期顺序。
- Outputs: 每 hart FP state 与 event trace。
- Pass: fflags/frm/FS 归属准确、被取消 FP 不更新 architectural flags，allowed Dirty 关系不屏蔽数值错误。
- Fail: 跨 hart frm 串扰、错误路径累计 flags、CSR 更新顺序取决于物理完成。
- Covers: SRC-03 §Execution resources shared retirement local。
- Sources: VR-003, VR-007, VR-012。

### V-052 — 冻结 V 完整能力与宽状态传输
- Depends: V-002, V-008, V-050, V-051
- Inputs: V 实现里程碑、VLEN/ELEN/LMUL 能力、必需指令列表。
- Action: 固定每 hart VLEN/ELEN、完整 V 所需 F/D 等依赖与全部指令覆盖表；验证所有 vector bits 的参考传输，超过冻结 Difftest 128-bit 形状时先完成适配器扩展校准。
- Outputs: V capability manifest 与高位负控制证据。
- Pass: 全 32 个 vector register 的完整 VLEN 均可比较；lane 数变化不改 vlenb；未完成完整 V 只能称明确子集实验。
- Fail: 只实现 vadd 就广告 RVV、默认任何 NEMU 构建支持任意 VLEN、忽略高位、把 lane 数写成 VLEN。
- Covers: SRC-03 §RVV 第一等公民、§runtime lane aggregation。
- Sources: VR-003, VR-005, VR-007, VR-013。

### V-053 — 验证 vset、vl/vtype 与重启入口
- Depends: V-052
- Inputs: V 512-case 清单配置子集、vector CSR 实现。
- Action: 覆盖 AVL=0/1/VLMAX−1/VLMAX/VLMAX+1/2VLMAX−1/2VLMAX、rd/rs1 零组合、各合法/非法 SEW/LMUL、vill、vstart 非零与 VS=Off。
- Outputs: vl 选择合法区间、vtype/vstart/异常状态表。
- Pass: 选择满足规范并在同配置要求下可重放；非法 vtype 按 vill/vl 规则；vstart 重置/保留符合指令规定。
- Fail: 以参考某个合法 vl 选择强迫所有实现完全相同、非法配置静默当 m1、配置动作更改 architectural VLEN。
- Covers: SRC-03 §VectorDescriptor 中 vl/sew/mask、§RVV lane allocation。
- Sources: VR-007, VR-013。

### V-054 — 验证 RVV 算术、mask 与 tail
- Depends: V-053, V-023
- Inputs: V 清单算术/固定点子集、逐元素合法关系比较器。
- Action: 交叉 SEW/合法 LMUL、vl=0/短/满、四种 vta/vma、全零/全一/交替/单 bit mask；测 v0 alias、prestart、saturation/vxrm/vxsat，并对非法 agnostic 值注入故障。
- Outputs: active/inactive/tail/prestart 分区逐元素结果。
- Pass: active 值准确，tu/mu/prestart 保留，agnostic 仅接受规范允许值；无效元素不产生 memory fault；flags 正确。
- Fail: inactive 全不比较、vl=0 仍改 tail、v0 位顺序错误、以“vector 宏提交”跳过 element 结果。
- Covers: SRC-03 §Vector Macro-uOP、§packet completion semantics。
- Sources: VR-013。

### V-055 — 验证跨 lane 排列、归约与重叠
- Depends: V-054
- Inputs: V 清单 permutation/reduction/widen/narrow 子集。
- Action: 覆盖 slide/gather/compress、mask 生成、整数/FP reduction、widen/narrow 的 EEW/EMUL 与寄存器重叠限制；验证有序 FP reduction 与无序 reduction 的不同合法性关系。
- Outputs: 跨 lane 数据映射及合法结果集合检查。
- Pass: index 边界与 illegal overlap 正确，ordered reduction 顺序精确；unordered 不被错误要求与另一树 bitwise 相同但仍满足规范算法约束。
- Fail: 动态 lane 分配丢 carry/排列、mask tail 普通化、非零 vstart 对不支持重启的指令被随意接受。
- Covers: SRC-03 §lane groups、§chaining/forwarding、§shared ALU/FMA。
- Sources: VR-007, VR-013。

### V-056 — 验证 vector memory packet 与权限
- Depends: V-048, V-054
- Inputs: V 清单 unit/strided/indexed/segment/whole-register/mask memory 子集、MEF packetizer。
- Action: 覆盖正/负/零 stride、ordered/unordered index、重复地址、跨 line/page、EEW≠SEW、EMUL 边界与 masked-off 无效地址；逐 element/segment 保留原始事务对应关系。
- Outputs: instruction→element→memory transaction→result 的可逆映射。
- Pass: 每 active 元素读取/写入其允许值，inactive/tail 不访问禁区；fault 归属正确；同址写入按该指令定义的顺序/无序范围检查。
- Fail: coalescing 丢元素、误合跨权限请求、将 unordered 结果硬排成参考顺序或任意放行。
- Covers: SRC-03 §Memory Operation 也应该成为 Packet、§VectorDescriptor。
- Sources: VR-013, VR-014。

### V-057 — 验证 fault-only-first 的三种终局
- Depends: V-053, V-056
- Inputs: FOF load 实现、可控 page/access fault 与 IRQ。
- Action: 分别在元素 0、后续 active 元素、被 mask 元素和 segment 内 field 制造 fault；再无 fault 运行允许主动缩短 vl 的关系测试，及 IRQ 中断后恢复。
- Outputs: FOF 的 trap/vl-trim/正常完成分类与部分写入证据。
- Pass: 元素 0 同步 fault 保留 vl 并 trap；后续同步 fault 按规范截短 vl 不取同类 trap；mask 不造 fault；IRQ 与同步缩短分开处理，spurious writes 仅限允许范围。
- Fail: 所有 fault 都 suppress、错把“首个 active 元素”普遍替代规范元素 0、vl=0 被当成功处理所有错误、非幂等访问可重做。
- Covers: SRC-03 §RVV packet completion 的 ISA 例外修正。
- Sources: VR-013。

### V-058 — 验证部分完成、vstart 与精确重启
- Depends: V-014, V-056, V-057
- Inputs: vector partial-trap event、fault handler、可恢复页面映射。
- Action: 在每个 packet 边界及 segment 中间触发可恢复 fault，检查 pre-vstart 已完成内容、epc/vstart、允许后续更新，修复 fault 后从同指令重启；追加不支持非零 vstart 的指令非法测试。
- Outputs: trap 前后及 restart 后 vector/memory 的三阶段证据。
- Pass: older 指令已提交、younger 无影响，完整重启结果符合规范；store/非幂等副作用不非法重复；合法部分状态没有被 reference 全指令 step 覆盖。
- Fail: 宣称 vector instruction 全有或全无、清空整个 destination 后重跑、丢失 vstart、借 mask 屏蔽 fault element 错误。
- Covers: SRC-03 §一个 ROB Entry/VectorDescriptor、§precise vector completion 修正。
- Sources: VR-013。

### V-059 — 验证 vector chaining 与回滚隔离
- Depends: V-029, V-055, V-058
- Inputs: chaining/VRF rename 实现、completion bitmap、vector descriptor epoch。
- Action: 构造 producer-consumer 部分 forwarding、WAW、v0 dependency、packet 乱序返回；producer partial fault 或 branch squash 后晚到旧 packet，检验 consumer 禁止使用未定义数据。
- Outputs: element-ready/producer 关系及 fault/kill trace。
- Pass: 只有同代合法已可用元素可 forwarding；bitmap 不能因重复 packet 提前完成；squash 不污染新 descriptor。
- Fail: 一个 packet done 当整条指令 done、partial producer fault 后 younger 退休、跨 hart vector tag alias。
- Covers: SRC-03 §Vector completion bitmap、§chaining/forwarding、§No wrong-epoch update。
- Sources: SRC-03:449-539, SRC-03:2078-2128, VR-013。

### V-060 — 验证 lane partition 与迁移语义不变
- Depends: V-034, V-059
- Inputs: 2/4/8 lanes 研究配置、固定 architectural VLEN、64 个 vector 程序。
- Action: 对同 ELF/输入在不同物理 lane 数、静态分区及安全边界重配下运行；两 hart/四 hart partition 的上下文所有权逐项保存；执行中请求迁移必须 drain 或拒绝。
- Outputs: vector metamorphic 64 组结果与 lane/VRF ownership 记录。
- Pass: 规定 architectural 结果关系不变，VLEN/ELEN 不变化，少 lane 只是更多时间片；重配不丢 mask/vstart。
- Fail: 2 lanes 不够就缩小 vlenb、迁移时 vector state 混 hart、只比较吞吐不检查部分故障。
- Covers: SRC-03 §RVV 第一等公民、§one×8 vs two×4 vs four×2。
- Sources: SRC-03:396-448, SRC-03:2078-2128, VR-013。

### V-061 — 验证 LLB 新鲜性、失效与 ownership
- Depends: V-018, V-034
- Inputs: LLB 实现、共享 L1 一致性/版本协议、L1-only 对照。
- Action: 对 LLB hit/miss/bypass、他 hart store、DMA、同址别名、PMA 不缓存区域、lane reassignment、错预测与 invalidation 竞争执行定向序列。
- Outputs: LLB 数据来源/版本链与对照结果。
- Pass: 每次 hit 都能证明与允许 memory observation 一致；失效确认/版本检查完整；不可缓存与非幂等地址不进入 LLB。
- Fail: clean copy 被误当无需一致性、重配后旧 owner 数据泄漏、predictor 错误导致错误值而非仅延迟变化。
- Covers: SRC-03 §LLB 最适合作为非 coherent structure、§reuse-directed routing。
- Sources: SRC-03:790-1041, VR-014。

### V-062 — 验证 coalescer 拆合与地址域边界
- Depends: V-056, V-061
- Inputs: line/byte coalescer、translation/PMA/访问权限标签。
- Action: 枚举相同 line 不同 byte、重复 byte、跨 line/page、权限不同、VA 相同 PA 不同、PA 相同 VA 不同；合并 load 必须保留每请求独立 fault/返回，store 仅按已定义规则合并。
- Outputs: merge/split 关系及多播逐消费者校验。
- Pass: 合并前后 architectural observation 等价；MMIO/AMO/不兼容排序不合并；取消一消费者不取消其他合法消费者。
- Fail: 只按 VA/低地址匹配、丢 byte-enable、把请求减少当正确性、错误消费者共享 fault。
- Covers: SRC-03 §Memory Packetizer、§Cross-Hart Coalescing。
- Sources: SRC-03:1042-1160, VR-013, VR-014。

### V-063 — 验证 MEF QoS 不饥饿及关键负载
- Depends: V-033, V-062
- Inputs: 地址/transaction/criticality 三层调度实现、age 公平性合同。
- Action: 同时注入依赖 scalar load、bulk vector、prefetch 和热点 bank 流，固定每流有限输入；观察优先级提升、age 升级、取消、queue full 与返回背压。
- Outputs: 每类请求等待界、实际服务次序、结果正确性。
- Pass: critical path 获得规定服务；非 critical 合法需求也在公平合同内完成；无请求被优先级永久饿死。
- Fail: CPU 延迟改善靠吞 vector 请求、无限 prefetch 抢占、平均等待掩盖单项 starvation。
- Covers: SRC-03 §Memory Fabric 三层调度、§Critical Load、§Ara-Opt 归因假设。
- Sources: SRC-03:1161-1272, VR-014。

### V-064 — 验证多 hart architectural context 隔离
- Depends: V-017, V-034, V-047
- Inputs: 多 hart 实现里程碑、hart ID/CSR/reset/interrupt 分配。
- Action: 在 2/4-hart 配置交错相同 PC 不同 GPR/权限/ASID/FP/vector 状态，分别触发 trap、IRQ、WFI 与 halt；参考实例必须具备独立 state 与已定义共享 memory。
- Outputs: hart-context 对照、state isolation trace。
- Pass: 每 hart PC/rename/commit/trap/CSR 独立，mhartid 稳定，某 hart 停止不隐式停止其他 hart。
- Fail: 把多个软件线程当一个 RVV hart、共用参考寄存器状态、按 host 调度创建伪全局 retire 顺序。
- Covers: SRC-03 §Per-Hart Commit Domain、§RVV 和 SIMT 不能等同。
- Sources: SRC-03:540-663, SRC-03:1425-1536, VR-003, VR-012。

### V-065 — 建立独立的多 hart memory trace checker
- Depends: V-045, V-046, V-062, V-064
- Inputs: per-hart program order、reads-from/version、store visibility、coherence 事件。
- Action: 将每个内存原语映射到 po/rf/co/依赖/fence/aq/rl 约束；明确 Difftest golden-memory patch 的使用点，独立重验 load 值与 store 来源，不以 patched reference state 当 memory proof。
- Outputs: 可审计执行图及内存约束检查结果。
- Pass: 每个观察到的值有合法来源与满足适用模型的执行见证；缺失 provenance 阻断多 hart memory 验收。
- Fail: 简单按 retirement 排列全局 store、SC 模型拒绝合法弱行为、patch DUT load 到 REF 后就跳过 memory 检查。
- Covers: SRC-03 §Store visibility obeys selected RISC-V model、§cross-hart fusion。
- Sources: VR-003, VR-006, VR-014, VR-015。

### V-066 — 执行 RVWMO litmus 并标明形式化边界
- Depends: V-065, V-036
- Inputs: 48 个 litmus-v1 程序、冻结 herd7/`.cat`、生成的允许结果集。
- Action: 先以 model checker 求每例允许/禁止结果；在 DUT 2/4 hart 上对 seeds 0–199 各运行 1,000 iterations；采样 store-buffer、延迟、bank conflict 变化；另把一个禁止结果注入结果检查器做负控制。
- Outputs: 精确运行矩阵、outcome histogram、模型见证与禁止结果检测日志。
- Pass: 所有观测结果在允许集，禁止结果注入被拒绝；报告“未观察到”不等于“不可能”；I/O/PTW/vector 未形式化范围单列语义检查。
- Fail: 以 Spike SC 对照替代 RVWMO、仅跑 fenced case、必须观察每个允许结果才通过、隐藏未支持 litmus。
- Covers: SRC-03 §memory ordering、§多 hart 与 cohort。
- Sources: VR-006, VR-014, VR-015。

### V-067 — 验证共享缓存一致性与外部写入
- Depends: V-061, V-065
- Inputs: shared-L1/multi-pod coherence 实现里程碑、DMA/缓存维护平台协议。
- Action: 构造 dirty eviction、ownership transfer、同址读写竞争、失效与 fill 竞争、false sharing、DMA 写入后 CPU 读取；无硬件一致性的 DMA profile 必须执行明确软件维护协议。
- Outputs: 每 cache line ownership/version history 与最终 memory。
- Pass: 单写者/允许多读者、不丢 dirty 数据、失效后 stale hit 不可见；DMA 协议按实际平台而不是默认 coherent。
- Fail: “统一 L1”被当一致性证明、返还旧 fill 覆盖新值、不同 FPGA 平台的 DMA 假设混用。
- Covers: SRC-03 §per-lane cache 风险、§shared banked L1、§pod memory sharing。
- Sources: SRC-03:790-985, SRC-03:2235-2270, VR-014。

### V-068 — 验证 cohort eligibility 不是 PC 相等
- Depends: V-051, V-060, V-064
- Inputs: cohort eligibility predicate、融合实现里程碑。
- Action: 对相同 PC 不同指令字、自修改代码、privilege/ASID/CSR/rounding、异常待决、memory ordering、操作宽度分别生成相容/不相容 pair；最初仅融合已证明的纯算术类。
- Outputs: eligibility 真值表与实测接受/拒绝证据。
- Pass: 每个物理 lane 绑定唯一 hart/指令/目标；仅已证明相容 pair 被融合；共享取指不是必需前提，也不能只信 PC。
- Fail: opcode 相同但 frm 不同仍错误共享、fault 状态兼容未检查、为了融合改变软件可见 PC。
- Covers: SRC-03 §Dynamic Cohort Fusion、§same PC/same instruction/compatible state。
- Sources: SRC-03:577-663, SRC-03:2172-2234, VR-012, VR-013。

### V-069 — 验证 divergence、独立 trap 与再汇合
- Depends: V-068, V-015
- Inputs: cohort split/merge/active-mask 实现、16 模式×8 刺激矩阵。
- Action: 在一个 lane branch、trap、IRQ、WFI、memory fault、长延迟时分裂 cohort，其他 hart 独立进展，再在相容 PC/状态重合；覆盖非连续 hart mask。
- Outputs: per-hart architectural trace 与 cohort 生命周期。
- Pass: 一个 hart 的异常不杀其他 hart 的合法工作；每 hart 精确顺序保留；inactive lane 不写回；reconvergence 不重复执行。
- Fail: warp 全停导致无必要死锁、复制一个 trap 给所有 hart、mask 错位、split 后漏 credit。
- Covers: SRC-03 §divergence/reconvergence、§transparent micro-SIMT。
- Sources: SRC-03:540-789, VR-012。

### V-070 — 验证跨 hart 合并 load 的内存语义
- Depends: V-062, V-065, V-069
- Inputs: cross-hart coalescer、独立权限与 ordering tags。
- Action: 同 line 多 hart load 之间插入第三方 store/fence/AMO/失效，覆盖只有一 hart 权限失败与一 hart 被 squash；检查每消费者可见值是否有合法内存执行见证。
- Outputs: 合并读取的每 hart 返回/reads-from/fault 证据。
- Pass: 物理请求减少但每个 architectural load 仍满足其 program order 与内存模型；不强制本可不同的值永远相同。
- Fail: coalescing 成为非法 broadcast、合并跨 MMIO/权限边界、参考 patch 掩盖 stale read。
- Covers: SRC-03 §Cross-Hart Coalescing、§A coalesced load returns allowed value。
- Sources: SRC-03:1124-1160, VR-014。

### V-071 — 执行 cohort 开关 metamorphic 对照
- Depends: V-069, V-070
- Inputs: 64 个 data-race-free 多线程程序、固定输入、相同规范可见同步。
- Action: cohort off/on 与不同融合宽度各运行；对无数据竞争程序比较最终结果及每 hart defined state，对允许非确定性程序比较结果集合而非强行相同 trace。
- Outputs: fusion 语义对照与每 workload 兼容说明。
- Pass: 稳定结果程序一致；非确定结果合法；至少命中形成、分裂、重合及部分异常四类行为。
- Fail: 用禁止多 hart 交错维持相同结果、只比较融合率、cohort off 本身未经参考校验。
- Covers: SRC-03 §A fused cohort is architecturally identical to independent execution。
- Sources: SRC-03:577-663, SRC-03:2172-2234, VR-014。

### V-072 — 验证 pod 间任务与资源归还
- Depends: V-034, V-060, V-067, V-071
- Inputs: 多 pod 实现里程碑、粗粒度 task/vector 租约合同。
- Action: 任务跨 pod 执行/窃取、远程 memory return、目的 pod 停止接收、超时与 lease 回收逐项测试；task identity 与 source hart commit domain 保留。
- Outputs: inter-pod migration/return trace 与最终状态。
- Pass: 恰一次完成、无跨 pod stale reply、源 hart architectural state 不变归属；失败远程请求有明确恢复且无重复副作用。
- Fail: 任意单 cycle 跨 pod ALU 借用未经合同即进入基线、远程异常丢 source identity、无限等待构成环。
- Covers: SRC-03 §Slice/Tile/Pod hierarchy、§最后才做 Pod Aggregation。
- Sources: SRC-03:1537-1595, SRC-03:2235-2270, VR-014。

### V-073 — 验证 p1 单 hart Linux 与可选 SMP 运行环境
- Depends: V-043, V-044, V-046, V-047, V-049
- Inputs: 满足选定 Linux/OpenSBI ISA/平台需求的实现里程碑、锁定 firmware/kernel/rootfs/DTB；声明 SMP Linux 时另附 V-064/V-065/V-067 多 hart memory/coherence 证据。
- Action: 先审计实际二进制所需扩展及设备，p1 单 hart 从 reset 启动到 init，运行固定用户程序、异常/系统调用、timer、进程切换及页表/上下文切换；保存 boot milestone 与退出签名。仅当声明 SMP Linux 时增加多 hart 启动、同步、共享内存观测与 RVWMO/coherence 检查。
- Outputs: 单 hart 可重放 OS 镜像闭包和程序结果；SMP claim 另附多 hart 同步/内存证据；必要额外扩展用 capability gate 补齐。
- Pass: 对所声明 Linux claim，boot 到用户程序终止且差分/设备检查有效；进程上下文、页表和 IRQ 正确；SMP 需 V-064/V-065/V-067 及真实同步程序证据，不能用单 hart 结果代替。
- Fail: 在 p0 宣称 Linux、只靠 I-048 启动烟测宣称 p1 Linux 已验收、使用未审计 prebuilt linux.bin、屏蔽 boot 全段差分、单 hart 证据冒充 SMP。
- Covers: SRC-03 §normal Linux/virtual memory、多线程软件路线。
- Sources: VR-004, VR-005, VR-007, VR-012。

### V-074 — 执行应用级有限程序集合
- Depends: V-025, V-036
- Inputs: programs-v1 的 12 程序×3 输入、reference 独立期望、ISA audit。
- Action: 运行整数排序/CRC/hash/链表/整数矩阵/memory overlap/分支状态机等程序；后续按 capability 加 F/V/SMP 变体，固定 work count、校验输出与内存 guard。
- Outputs: 程序级结果矩阵、ELF/source/input provenance。
- Pass: 36 个 baseline 实例完整退出且结果正确；扩展变体单列，数据边界和错误输入有明确期望。
- Fail: 只跑空 loop、用 benchmark score 代替计算结果、不同 binary/输入混成“同程序”。
- Covers: SRC-03 §ILP/MLP/DLP/TLP 工作负载、§FPGA 原型验证路线。
- Sources: SRC-03:370-395, SRC-03:664-726, VR-005, VR-006, VR-007。

### V-075 — 同一参考下运行 XiangShan 与新核
- Depends: V-005, V-007, V-074
- Inputs: 交集 workload 清单、两 DUT 平台适配、相同 reference source SHA 与共同 ISA 语义，分别声明的实现参数。
- Action: 对相同 ELF PT_LOAD 内容/输入分别运行 XiangShan+Difftest+NEMU 与新核+adapter+同源 NEMU；优先同一可配置参考二进制，若 ISA/CSR 参数是编译期配置则记录两个库 hash/配置差异并校准共同语义；必要 boot shim 单独 hash，在共同 GPR/内存/入口已核实相同的边界开始比较；全部 36 baseline 再用 Spike/Sail 独立核对。
- Outputs: DUT-A/REF、DUT-B/REF、A/B defined-result 三份结果及不可比原因。
- Pass: 每 DUT 独立参考检查通过；共同程序可见起点、ISA/ABI、输入与内存语义一致；misa/mhartid/实现相关 CSR 分别对自身 profile 核对，不强求两 DUT 相同，也不屏蔽各自实现错误；周期/内部 uOP 不比较。
- Fail: XiangShan 直接当 oracle、把不同 reset/设备差异归咎 DUT、给不同 ISA 的二进制贴“同 ELF”标签、共用错误 adapter 没独立校准。
- Covers: SRC-03 §XiangShan 工程方法与高性能基线；用户要求的 programmatic correctness validation。
- Sources: VR-002, VR-003, VR-005, VR-006, VR-007。

### V-076 — 分离正确性 gate 与性能归因
- Depends: V-028, V-060, V-063, V-071, V-075
- Inputs: 已全部通过正确性检查的 workload、固定资源对照、未来 experiment CLI。
- Action: 固定 resource/compiler/input 后比较 fixed vs elastic、L1-only vs LLB vs LLB+coalescer、cohort off/on、1×8/2×4/4×2；记录 useful work、cycles、retire/hart、network bytes、wait/occupancy、frequency 来源与能耗代理范围。
- Outputs: 带置信边界和失败样例的因果实验矩阵。
- Pass: 每个性能点附相同输入的正确性证据；注明 Verilator cycle 不等于板上频率、toggle proxy 不等于真实能耗；负收益同样保留。
- Fail: 比较不同资源/时钟却声称调度改进、把源报告 1.33/1.38/95% 等未复现实验变本项目 gate。
- Covers: SRC-03 §IPC/Throughput、§Ara-Opt/SEAM-V 论据、§各 FPGA A/B 方案。
- Sources: SRC-03:1596-1877, SRC-03:2017-2270；外部性能原始文献由架构/参考总账核验。

### V-077 — 将同一证据协议带到真实 FPGA
- Depends: V-012, V-024, V-039, V-075
- Inputs: 此次广告的 GW5A、Zynq 或 Virtex UltraScale+ 目标及其平台实现/板卡决议、硬件 trace/装载协议；其他目标留待各自 claim 前单独运行。
- Action: 对每个此次声明的真实平台分别运行其容量 profile 的同一 ELF/输入；板上保存 signature、首末 commit、trap/IRQ、overflow flags，经 host 离线 reference replay；高带宽 trace 不足时分窗口重跑并保留窗口连接 checkpoint。
- Outputs: 按目标板卡/run/profile 关联的硬件证据包与未声明目标的 deferred 账本。
- Pass: 每个已声明真实器件运行可确认、trace 无丢失或显式 overflow 失败，结果与仿真及 reference 一致；无法全 trace 的结论范围明确降为已观察窗口/签名；不能以一块板证据覆盖其他两块。
- Fail: 只综合便称板测、JTAG 下载成功当程序通过、带宽不足无声抽样却声称完整差分。
- Covers: SRC-03 §FPGA 原型与验证路线；GW5A/Zynq/Virtex UltraScale+ 用户平台要求。
- Sources: VR-003, VR-008；具体器件与工具主源见 platform-plan.md。

### V-078 — 生成分阶段验收证据与阻断结论
- Depends: V-040, V-041, V-042, V-043, V-075
- Inputs: 全部运行 manifests、失败 triage、源覆盖图、阶段 capability。
- Action: 对每个阶段计算计划/执行/通过/失败/unsupported/infra/timeout 闭合；列未完成后续功能，不允许为了发布缩小已承诺能力；每项性能实验也链接 correctness run。
- Outputs: 阶段验收报告、可复现命令和 artifact hash 索引。
- Pass: baseline 必需项全过且零未登记 exclusions；后续 V/C/A/priv/FP/cohort/pod/硬件阶段各引用对应 V 任务证据后才放行。
- Fail: 以本任务 Depends 的最小基础列表代替后续阶段全部义务、未完成任务说成 future optional、任何未知状态被统计为通过。
- Covers: SRC-03 全部架构与验证路线；完整交付边界。
- Sources: SRC-03:1-2528, VR-001–VR-015。

### V-079 — 验证观测逻辑可移除与平台可移植
- Depends: V-008, V-042
- Inputs: 含/不含探针 RTL、此次声明目标的 RAM wrapper/复位/端口模式实现里程碑；声明 FPGA 目标另附对应 V-077 真实板证据，ASIC 目标附独立物理证据。
- Action: 对探针开关进行顺序等价或相同输入 trace 比较；针对声明的目标对 RAM read-during-write、端序/byte-enable、reset 值做合同测试；ASIC memory wrapper 同样复用此接口但独立进行时序签核。
- Outputs: instrumentation noninterference 与逐目标 wrapper 行为报告。
- Pass: 观测不反馈功能路径，已声明 wrapper 实现满足同一逻辑合同；FPGA 广告必须对应目标 V-077 板证据，ASIC 广告必须独立签核；厂商特定能力显式封装，不进入 core 语义。
- Fail: 仿真 DPI RAM 隐藏 FPGA collision 行为、删探针改变 arbitration、将 Verilator 测试当 ASIC equivalence/timing proof。
- Covers: SRC-03 §same architectural machine/dynamic physical substrate；可移植性与 ASIC 后续转换。
- Sources: VR-003, VR-008, VR-011。

### V-080 — 防止扩展广告超出实际验收
- Depends: V-002, V-043, V-078
- Inputs: 每阶段 capability/ISA 字符串、misa、compiler flags、OS DTB/hwprobe 描述。
- Action: 逐项核对声明与通过证据；对 A/C/F/D/V/S/U/Sv39 尚未启用的指令/CSR/mode 执行规定非法或 WARL 测试；启用后撤销对应拒绝预期并运行全套扩展义务。
- Outputs: 软件可发现能力与验证证据的闭合矩阵。
- Pass: 所有声明均有对应阶段完整证据；非法/保留行为符合冻结规范；完整 V/Linux 所需能力不由少数 demo 推断。
- Fail: 提前标 RV64GCV/RVA profile、misa 与编译器/设备树互相矛盾、缺少必选 V 指令仍声明 V、把 deferred 当 rejected 消失。
- Covers: SRC-03 §保持标准 software model、§最终 EAF-V/MosaicRV 架构。
- Sources: VR-007, VR-009, VR-012, VR-013。

### V-081 — 校准 lockstep 观测点与输入等价
- Depends: V-008, V-020, V-033
- Inputs: lockstep profile manifest、D值、同步输入合同、observation schema。
- Action: 为每个profile列出比较字段、合法delay、异步输入同步规则、main/shadow资源公平性和不比较字段；同一程序分别运行normal/DCLS候选，证明无注入时 architectural event 序列合法一致。
- Outputs: observation manifest、delay calibration、normal-vs-lockstep metamorphic报告。
- Pass: 所有输入在同步后一次进入，输出按D对齐；无mismatch时安全信号不触发，两模式输出属于同一ISA合法结果。
- Fail: 异步输入分别驱动副本、debug-only信号参与关键比较、或比较周期末粗糙状态。
- Covers: optional DCLS, delayed lockstep, metamorphic equivalence。
- Sources: AR-019, AR-020, validation-plan.md。

### V-082 — 注入 replica 内部状态与输出故障
- Depends: V-081, V-021, V-027, V-042
- Inputs: main/shadow独立寄存器/CSR/ROB/LSQ/FU/vector状态和故障注入接口。
- Action: 在shadow输入、main输出、register file、CSR、branch target、LSQ address/data、FU result、vector element和comparator control注入单点错误；每个注入在实际执行路径上生效。
- Outputs: fault site→observation point→detection latency→blocked effect矩阵。
- Pass: 每个声明故障在配置latency内检测，未发布store/MMIO/CSR/commit被阻断；无注入负控制不触发。
- Fail: 用比较器mock代替真实副本、故障未到达执行路径、mismatch后副作用仍然发布。
- Covers: lockstep fault containment, negative controls。
- Sources: AR-019, AR-020。

### V-083 — 验证 reset、debug、reconfiguration 与资源公平
- Depends: V-015, V-034, V-082
- Inputs: reset/debug/scan/DFT/lockstep enable/disable状态机、dynamic broker。
- Action: 覆盖两副本同时reset、shadow延迟退出、debug进入/退出、配置切换、lease drain、shadow长期得不到资源；确认任何非lockstep状态显式发布且不会静默恢复比较。
- Outputs: mode transition trace、resource grant histogram、fault-controller状态证据。
- Pass: shadow与main获得有界服务；任一mode切换前旧结果完成或丢弃；debug/scan关闭检测时capability显式变化。
- Fail: shadow永久饥饿、一个副本独自复位、debug后检测静默关闭仍宣称DCLS。
- Covers: optional lockstep mode transitions, fairness, debug/test safety。
- Sources: AR-019, AR-020, AR-021。

### V-084 — 证明 shared-domain 保护与综合冗余存活
- Depends: V-083
- Inputs: DCLS 已验收的 shared p0 RAM/bus/clock/debug 保护图；对所声明 FPGA/ASIC 目标附 V-079 观测/物理 wrapper 证据、综合 netlist/平台约束；若声明 shared multi-hart/coherence/LLB 再附 V-067 等对应内存证据。
- Action: 从共享 RAM/总线注入可检测错误，核对 ECC/parity/CRC/address binding 和 monitor 路径；仅在物理目标声明时检查综合后 shadow、delay、checker、barrier 仍存在并运行 post-synthesis/implemented 对比测试；增加 cache/LLB/coherence 时扩大共享保护域。
- Outputs: p0 common-mode fault ledger；物理 claim 分别附 netlist survivability evidence、physical protection report。
- Pass: 每个声明范围内的 shared architectural path 有保护或残余风险记录且 fault campaign 能触发；物理 DCLS claim 另证明冗余未被综合优化移除，不能借 p0 RTL 证据宣称物理冗余存活。
- Fail: 共享 RAM corruption 无检测、遗漏已声明 cache/LLB 保护；物理 claim 综合掉 duplicate logic，或仅 RTL 仿真证明物理冗余存活。
- Covers: common-cause faults, synthesis survival, shared-domain protection。
- Sources: AR-019, AR-020, HR-009。

### V-085 — 闭合 RVA23U64/RVA23S64 全部 mandatory 条款与 reference 覆盖
- Depends: V-002, V-020, V-043, V-080
- Inputs: ratified RVA23 profile 的 U64/S64 mandatory 条款与批准的 ISA 扩展版本/hash、逐扩展实现里程碑、V-044–V-059/V-045–V-049 等已有证据、ACT/Sail/Spike/NEMU 逐项能力表；可选项独立账本。
- Action: 按下列分组将**每个**命名 mandatory 扩展展开为独立 requirement×规范条款×正例/负例×oracle×DUT 事件×结果行；分组只是小模型派工，不可用分组 PASS 替代成员 PASS。U64 基础：little-endian RV64I/ECALL→V-025/V-041/V-043/V-047；M、A、F、D、C、B、Zicsr→V-043–V-051/V-044/V-045/V-046 的逐指令/CSR 定向与 ACT；Zicntr、Zihpm→计数器权限/增量/实现值、只读零行为（V-017/V-047）；Ziccif、Ziccrse、Ziccamoa、Zicclsm、Za64rs、Zic64b、Zicbom、Zicbop、Zicboz→V-086 的各自 PMA/atomic/fetch/cache 行；Zihintpause、Zihintntl、Zimop、Zcmop→逐合法编码/长度、定义的 hint/MOP 效果或无效果、非法编码负例（V-016/V-044）；Zfhmin、Zfa→FP 数值/舍入/flags/NaN-boxing 与不合法编码（V-050/V-051）；Zkt→适用标量指令的 mandatory 数据独立执行延迟（DIEL）单列，固定控制量改变数据值、记录可重复测量窗口与边界，**不**依赖可选密码套件；V（每 hart VLEN≥128，完整 V 而非子集）→V-052–V-058 的完整逐指令、宽状态、mask/tail/vstart/内存；Zvfhmin、Zvbb→向量半精度最小集/位操作逐指令值及配置；Zvkt→适用向量指令 mandatory DIEL，固定 `vl/vtype/mask` 等控制量分别改变 active/inactive data operand，独立于 V-089；Zicond、Zcb、Zawrs→逐指令条件值/压缩解码/预约等待与故障边界；Supm→V-087 的 U-mode PMLEN=0/7 与环境选择。

S64 先继承上述**全部** U64 obligations，再将 Zifencei→改代码后取指可见性；Ss1p13→CSR/权限/trap 条款；Svbare、Sv39、Svade、Ssccptr、Svpbmt、Svnapot→Bare/页表/权限/PBMT/NAPOT/页表 PMA 与 A/D fault（V-047–V-049）；Svinval→按地址/ASID 失效及旧 PTW 回应（V-049）；Sstvecd、Sstvala、Sscounterenw、Sstc、Sscofpmf、Ssu64xl→Direct 任意有效 4B 对齐 BASE、各类 fault 的 `stval`、非零 `hpmcounter` enable 可写、timer/overflow/过滤/UXL=64 的 CSR 与 interrupt 正负例；Ssnpm→`senvcfg.PME`/`henvcfg.PME` PMLEN=0/7（V-087）；Sha **逐成员** H、Ssstateen、Shcounterenw、Shvstvala、Shtvala、Shvstvecd、Shvsatpa、Shgatpa→HS/VS/VU 两阶段翻译及虚拟异常/注入/陷阱、`sstateen0–3`/`hstateen0–3`、非零 HPM 的 `hcounteren` 可写、`vstval`/`htval` 定值、Direct `vstvec` 任意有效 4B 对齐 BASE、`vsatp` 支持所有 `satp` 模式、`hgatp` 支持对应 SvNNx4 及 Bare；参照 V-047–V-049 的模式与异常 trace，但补专用 H/guest directed oracle，不把一条 S-mode 页表测试当 H 测试。每个无 reference 支持的 mandatory 行必须有独立第二 oracle 或规范驱动定向/形式证据；ACT exclusion、skip、只读实现值均逐项登记，缺少证据为 UNSUPPORTED 而非 PASS。

Mandatory `Zkt` 只约束规范表内**已实现**的标量指令（含适用 I/M/C/B 等；不因此要求实现标量密码）；loads/stores/conditional branches 不在其范围，OoO 的融合、拆分、路由优化不得依赖操作数**数据**。`Zvkt` 约束适用向量指令的所有 data operands，包括 inactive 数据；`vl/vtype/mask` 用作控制时不属于 DIEL 数据约束。对各自指令清单逐项冻结可观察计时边界及重复输入对照，不将 DIEL 宣称为整个处理器恒时或完整侧信道安全。

- Outputs: U64/S64 逐条 clause ledger、独立 H/guest 与 Zkt/Zvkt DIEL evidence、逐工具 capability intersection、missing coverage/exclusion ledger、每行输入 hash/trace/正负控制及 pass/fail/unsupported。
- Pass: U64 和 S64 **各自**全体 mandatory 行均有符合其条款的真实执行/状态/时序证据及有效 oracle；mandatory 负控制能检出错误；不从 ISA 字符串、ACT 通过率或分组名推断覆盖。
- Fail: 任一 mandatory 成员缺证、完整 V 被少量指令代替、Sha 被当可选 H、Zkt/Zvkt DIEL 推迟到可选密码、参考模型不支持便跳过，或把可选项当 Core 门禁。
- Covers: ratified RVA23U64/RVA23S64 mandatory conformance、binary compatibility；profile 不等于 OS/server 平台。
- Sources: AR-022, AR-023, VR-005, VR-007, VR-009；[ratified RVA23U64/S64 profile §Mandatory](https://raw.githubusercontent.com/riscv/riscv-profiles/rva23-rvb23-ratified/src/rva23-profile.adoc)、[Zkt §31.1.5](https://docs.riscv.org/reference/isa/v20260120/unpriv/scalar-crypto.html#crypto_scalar_zkt)、[Zvkt](https://docs.riscv.org/reference/isa/v20260120/unpriv/vector-crypto.html#zvkt)。

### V-086 — 验证 RVA23 fetch/cache/PMA/atomic/misaligned 合同
- Depends: V-018, V-045, V-046, V-085
- Inputs: Ziccif/Ziccrse/Ziccamoa/Zicclsm/Za64rs/Zic64b/Zicbom/Zicbop/Zicboz 与 Ssccptr 的区域 PMA、fetch/cache/预约实现、CPU 与**独立可控 coherent agent**；非一致性设备/区域另标；声明 multi-hart 时附 V-065/V-067。
- Action: 在同时具有 cacheability+coherence PMA 的 main-memory 区域分别测：Ziccif 的自然对齐 power-of-two 取指（16-bit 与**完整 32-bit**）不可撕裂；通过规范允许的写入与指令同步在两个完整编码版本间切换，观察结果只能为完整旧/新编码（含跨 fetch line 的 32-bit、页边界对照；不是“取指单周期”）。Ziccrse 在公平环境、符合规范的受限 LR/SC 循环检验 RsrvEventual，SC 自发失败不要求与 oracle 同周期一致；Za64rs 测预约集连续、自然对齐、最大 64B；Ziccamoa 测 A 全体 W/D atomics 的 PMA 支持、原值/原子性/aq/rl（V-045/046），**不**把可选 Zacas/Zabha 算入 A；Zicclsm 测 main memory 的 misaligned scalar 与适用 FP/vector load/store 成功及字节/异常边界，**不要求 misaligned AMO 或原子 misaligned load/store**；Ssccptr 测页表硬件读取 PMA。每项分别构造负例：撕裂取指、无限允许失败、超界预约、拒绝合法 A 原子、错误的 misaligned 数据/意外陷阱、拒绝合法 PTW，预期被 checker 检出。

对每个有效 64B 自然对齐 block 测 `cbo.inval/clean/flush/zero`（Zicbom/Zicboz）及 Zicbop prefetch 的规范允许可见性、PMA/权限/fault/ordering；跨块边界和错误权限对照，prefetch 不强制产生数据/缓存命中。CPU 与独立 coherent agent 在广告 coherent 区域交错读/写及 CBO，核对代理读值、旧行失效、dirty 数据不丢；noncoherent 设备只按明确软件维护协议判断，不能偷用 coherent 期望。分别记录不适用区域的 PMA 拒绝，不能把所有非幂等访问一律定义为相同 trap。multi-hart claim 才追加 V-065/V-067 memory/coherence traces；单 hart Core 不需要 SMP、密码或 FPGA。

Zawrs 单列 `WRS.NTO`/`WRS.STO`，在 LR 建立 reservation 后比较无 store、reservation 被 store 失效、pending interrupt、`WRS.STO` 实现定义短超时，以及 S/VS `TW/VTW` 的受限异常；允许规范规定的自发提前结束，不能把每次唤醒硬绑定为 store，也不能强求 NTO 固定时间退出。

- Outputs: 32-bit fetch atomicity、RsrvEventual、64B reservation/block、A atomics、misaligned load/store、PTW、CBO/agent 各自正/负 case 的事件与 PMA/区域属性/最终 bytes/错误原因；multi-hart 独立证据。
- Pass: 所有 mandatory PMA 区域义务及权限/可见性/进展有可观测合法结果且负控制拒绝违规，未声明的 misaligned AMO/Zama16b 不计入 Core；multi-hart 仅按声明追加。
- Fail: 撕裂 fetch、受限循环永不进展、预约集越界、合法 A/非原子 misaligned load/store 被拒、CMO 静默 noop 掩盖应见副作用或权限错误、cache hit 被当 agent 证据、强制 Core 实现可选 misaligned AMO。
- Covers: RVA23 mandatory memory-region PMA, cache/CMO, fetch atomicity, RsrvEventual。
- Sources: AR-022, AR-024, VR-014；[ratified RVA23U64/S64 profile §Mandatory](https://raw.githubusercontent.com/riscv/riscv-profiles/rva23-rvb23-ratified/src/rva23-profile.adoc)、[Zawrs §13.1](https://docs.riscv.org/reference/isa/v20260120/unpriv/zawrs.html)。

### V-087 — 验证 mandatory Supm/Ssnpm pointer masking 全访问覆盖
- Depends: V-049, V-056, V-085
- Inputs: U64 Supm 用户执行环境的 PMLEN 选择、S64 Ssnpm 的 `senvcfg.PME`/`henvcfg.PME`、PMLEN=0/7 至少两档、U/VS/VU（及实际支持的更高权限）有效权限/地址转换模式、CPU 显式访问与设备/DMA 起源表（含 MMIO PMA）；可选 Sspm 单列 V-090。
- Action: 对 mandatory U/VS/VU 配置 0/7 各运行启用/禁用成对 tagged/untagged CPU 显式 scalar/FP/compressed/vector/AMO/CMO/HLV/HSV（适用时）访问及指向 MMIO 的 CPU 访问，按是否虚拟地址符号扩展、Bare/guest-physical 高位清零计算预期地址，再核对读写值、权限/副作用、fault、硬件写 `stval`/`vstval` 与 debug trigger；CFI shadow-stack 访问**仅声明 Zicfiss 时**纳入。MPRV/SPVP 按有效权限选择对应配置；MXR 生效则不 masking，即使 Bare。对设备/IOMMU/DMA、CPU implicit fetch/PTW 使用同 tag 确认不 mask；CSR 软件写不转换，适用的硬件异常写与地址 trigger 转换；用错 tag/禁止区域/非法模式提供可检测负例。Supm/Sspm 是执行环境提供的选择承诺，不能从 Supm 推出 M-mode masking 或把可选 Sspm 强加到 S64。
- Outputs: 起源×访问×有效模式×PMLEN=0/7 的有限正/负矩阵、原/转换地址、读写副作用/异常及 CSR trace，optional Sspm 另册。
- Pass: Supm 与 Ssnpm 的 mandatory 0/7 选择可达并有实测对照；CPU MMIO 仍按有效权限转换且副作用恰一次，DMA/fetch/PTW 不转换；CSR/trigger/MXR/MPRV/SPVP 遵循冻结规范。
- Fail: 只测 RAM load/store、把 CPU MMIO 归为设备起源、误 mask DMA/fetch/PTW、PMLEN=0 吞高位、未测 VS/VU/Ssnpm 或擅自要求 Sspm。
- Covers: mandatory Supm/Ssnpm pointer masking 与 H guest access；Sspm 仅可选。
- Sources: AR-024（[Pointer Masking §17.1.2.2/§17.1.2.6–8](https://docs.riscv.org/reference/isa/v20260120/priv/zpm.html)）；[ratified RVA23U64/S64 profile](https://raw.githubusercontent.com/riscv/riscv-profiles/rva23-rvb23-ratified/src/rva23-profile.adoc)。

### V-088 — 验证 CFI landing pad 与 shadow stack
- Depends: V-016, V-044, V-047, V-087
- Inputs: **仅声明** RVA23U64 扩展选项 Zicfilp/Zicfiss 时的实现与合法/非法 indirect control flow、SS PTE/PMA/PMP、trap/debug 状态；两选项独立声明与记账。
- Action: Zicfilp 测 LPAD/label/ELP、trap save/restore、间接跳转正负例；Zicfiss 测 SSPUSH/SSPOPCHK/SSRDP/SSAMOSWAP、错误页面/非幂等 memory/跨权限、direct call/return 与 speculation；另按 V-087 测已声明 shadow-stack 显式访问的 pointer masking。
- Outputs: 每个已声明选项的 directed suite、fault/trap evidence、speculation boundary report；未声明选项为 deferred。
- Pass: 已声明的合法路径通过；非法 landing/shadow-store/return mismatch 按正确异常优先级失败；speculative 错误路径不改变 architectural state。
- Fail: LPAD 被全局当 hint、SS page 被普通 store 写入、trap 丢失 ELP/ssp、以其中一个选项通过冒充另一个通过。
- Covers: RVA23U64 **expansion options** Zicfilp/Zicfiss；非 mandatory。
- Sources: AR-025；[ratified RVA23U64 §Expansion Options](https://raw.githubusercontent.com/riscv/riscv-profiles/rva23-rvb23-ratified/src/rva23-profile.adoc)。

### V-089 — 验证**可选** vector crypto 结果与其 DIEL
- Depends: V-052, V-059, V-085
- Inputs: **实际声明**的 RVA23U64 localized options Zvkng/Zvksg 或独立 development option Zvbc、官方 known-answer/KAT 与规范逐指令 oracle、DIEL instrumentation；mandatory Zvbb/Zvkt 和 Zkt 的独立 V-085 结果不得在这里首次生成。
- Action: 按声明拆套件：Zvkng 的 Zvkn + Zvkg（GHASH `vghsh`/`vgmul`），Zvksg 的 Zvks + Zvkg；另行声明 development option Zvbc 才加 `vclmul[h]`/CLMUL（若另外宣称 Zvknc/Zvksc，则单列各自超出 profile 选项的 claim）。仅对已声明密码扩展逐指令覆盖 AES/SM4/SHA/SM3、EGW/EEW/EGS、LMUL/vstart/mask/tail/overlap；区分规范强制 illegal-instruction 的 `LMUL×VLEN<EGW`（即使 `vl=0`）与仅 reserved 的 SEW、vl/vstart 编码：reserved 记录实现选择，不凭空强制 trap。固定 `vl/vtype/mask` 等控制量，扫描活动/不活动 data operand 测**可选密码扩展适用范围** DIEL；不是替代 mandatory Zkt/Zvkt 的 DIEL 或完整侧信道免疫结论。
- Outputs: 每个声明→逐指令 crypto correctness matrix、GHASH/另行声明 CLMUL 账本、illegal/reserved 判定、可选密码 DIEL evidence 与侧信道范围声明。
- Pass: 各已声明扩展全部适用指令/KAT 正确；强制 illegal 情形 trap，reserved 不错误规定必须 trap；可选密码 DIEL 范围内的数据值不改变测量延迟。
- Fail: 少数 AES KAT 冒充 Zvkng/Zvksg、将独立 Zvbc/CLMUL 写成 Zvkng/Zvksg 成员、将 mandatory Zkt/Zvkt 推给 optional crypto、reserved 一概判 illegal、inactive 数据改变应受 DIEL 约束的时间、DIEL 被宣传为全侧信道免疫。
- Covers: RVA23U64 localized options Zvkng/Zvksg 与单独 development option Zvbc；mandatory DIEL 仍由 V-085 验收。
- Sources: AR-026（[Vector Cryptography §32.1.1.5/§32.1.2](https://docs.riscv.org/reference/isa/v20260120/unpriv/vector-crypto.html)）；[ratified RVA23U64 §Options](https://raw.githubusercontent.com/riscv/riscv-profiles/rva23-rvb23-ratified/src/rva23-profile.adoc)。

### V-090 — 验证逐项 RVA23 optional claim / 项目自选 Secure / Server Platform 边界
- Depends: V-085
- Inputs: V-085 mandatory 已闭合的目标 RVA23U64/S64、**每个**选项的 yes/no claim 与前置实现里程碑；只有另行声明 Linux/server 平台才输入 V-073 与平台 RoT/TPM/secure boot/IOPMP owner 和平台规范版本。`RVA23 Secure` 仅是项目自选 bundle 名称，**不是** ratified RVA23 profile。
- Action: 为 ratified profile 的**每个**选项各开独立可发现行，保留分类与未声明 `deferred`：U64 localized **Zvkng、Zvksg**（各走 V-089）；U64 development **Zabha、Zacas、Ziccamoc、Zvbc、Zama16b**（byte/half AMO、CAS、主存 AMOCASQ PMA、vector CLMUL、16B 原子性 granule 各测独立正负例）；U64 expansion **Zfh、Zbc、Zicfilp、Zicfiss、Zvfh、Zfbfmin、Zvfbfmin、Zvfbfwma**（各自 scalar/vector 半精度、carryless multiply、CFI V-088、BF16 convert/FMA 数值与非法边界）；S64 **没有** privileged localized/development；S64 expansion **Sv48、Sv57、Zkr、Svadu、Sdtrig、Ssstrict、Svvptc、Sspm**（每种分页翻译模式/故障、entropy CSR 行为、硬件 A/D 更新与 mandatory Svade 的切换关系、trigger、标准/保留空间非法指令和 CSR contained trap 但不强加到 custom 空间、invalid→valid PTE 有界可见性、S-mode PMLEN=0/7 分别做正负例）。每项记录 `claim, category, normative clause, dependency, oracle, positive, negative, observed state, evidence/UNSUPPORTED`；扩展依赖不自动等于另一个选项已被声明；额外实现如 Ssaia 另列平台/非 profile claim，不混入上述 ratified inventory。

仅若项目**选择** `RVA23 Secure` bundle，冻结该项目 bundle 的逐项成员/版本并对每个所选选项闭合上表证据；仅若单独声明 Linux/server 平台，才联结 V-073 与该平台自己的规范、设备/固件、RoT/TPM/secure boot/IOPMP owner 和可运行的 server-style 综合程序（仅覆盖所声明功能）。平台安全设备不是 ISA 扩展，也不因 ratified profile 而自动存在；选项、安全 bundle、平台是三个不同 claim，不得互相代理。

- Outputs: ratified option 分类完整 inventory（每项 selected/未选 deferred/UNSUPPORTED/FAIL/PASS）、项目自选 bundle manifest（若声明）、独立平台 owner/evidence matrix 与适用程序执行证据（若声明）；未声明的平台/选项不得冒充 Core PASS。
- Pass: U64/S64 mandatory 不依赖任何可选行；所有**声明**选项有独立规范闭合与正负执行证据，所选 bundle 成员全通过才签项目自选 bundle，平台有自身完整证据才签平台；未声明选项留 deferred 而不阻断 mandatory Core。
- Fail: 将任一选项当 mandatory、将 `RVA23 Secure` 称 ratified profile、遗漏可选行、Zama16b 错算 Core misaligned AMO、以 ISA Core 代替 Server Platform/安全设备验收、将未实现选项写成支持。
- Covers: RVA23U64/S64 ratified localized/development/expansion options 的逐项可发现性；项目自选 Secure 与 Server Platform 分离。
- Sources: AR-022, AR-027, HR-014；[ratified RVA23U64/S64 §Optional Extensions](https://raw.githubusercontent.com/riscv/riscv-profiles/rva23-rvb23-ratified/src/rva23-profile.adoc)。源边界：Ziccamoc、Zama16b、Ssstrict 与 Sha 均由 ratified profile 定义，其文本说明相关定义将收入 ISA 手册；在该文本与批准 ISA 手册版本正式对齐前，以批准的 ratified profile 版本逐条冻结，不猜测尚未收入手册的替代条款。

#### RVA23 验证交接微案例（内部非任务 ID；按 V-085–V-090 各行派独立小模型 owner）

先冻结目标 U64 或 S64、规范条款/hash、区域 PMA/特权模式、程序/外部事件/hash、预算和 oracle；每一行仅处理一个有限案例族，下面「反例」是 checker/真实实现注入的**应检出错误**，不是要求规范合法 DUT 抛异常。执行者按第 4 节小模型协议保存正例结果、反例激活及首次差异/非零状态；reference 不支持时先换独立 oracle，仍无判据记 UNSUPPORTED 并阻断对应 mandatory/selected claim，绝不自行 skip。矩阵为未来工作设计，**没有**已运行 PASS：

| 微案例族（每个族内逐成员开行） | 固定正例及明确 PASS | 应检出反例 / FAIL | 证据或 UNSUPPORTED 条件 |
|---|---|---|---|
| U 基础/整数/压缩 `RV64I,M,B,C,Zicsr,Zicond,Zcb,Zimop,Zcmop` | 对每种规范指令/CSR 编码、RV64 little-endian 读写及合法 MOP 执行一项既定签名；每项值、PC 长度/权限合法才 PASS | 改写算术结果、压缩 PC、合法 hint/MOP 取指或非法编码处理；错误逃过检查即 FAIL | ELF/encoding、pre/post GPR/PC/CSR、ACT coverpoint；未覆盖 opcode→UNSUPPORTED |
| U FP/vector `F,D,Zfhmin,Zfa,V,Zvfhmin,Zvbb` | 对每扩展逐指令数值/flags，V 每 hart VLEN≥128、32 寄存器全宽和合法 mask/tail/vstart/element fault 全覆盖才 PASS | 高段截断、错误舍入、mask 后误写；未检出即 FAIL | 各指令 oracle/完整 VRF 与 trap trace；只有向量子集→UNSUPPORTED |
| U 计数器与 hints `Zicntr,Zihpm,Zihintpause,Zihintntl` | 每项权限/增量/可见实现值及合法 hint 的保留状态满足规范才 PASS | 注入误增量/异常/错序；未检出即 FAIL | CSR/opcode trace 与合法结果集合；仅 cycle 等于墙钟不构成证据 |
| U mandatory DIEL `Zkt,Zvkt` | Zkt 的**已实现且列于规范表**的标量指令和 Zvkt 的适用向量指令分别固定控制条件后改变数据（Zvkt 包括 inactive 数据），规定范围内延迟数据独立才 PASS | 数据相关执行路线/延迟差异未报警即 FAIL | 指令集合/独立计时窗口/对照；Zkt 不覆盖 load/store/conditional branch；控制用 `vl/vtype/mask` 排除；无 instrumentation→UNSUPPORTED，V-089 不可代偿 |
| Fetch/atomic `Ziccif,Ziccrse,Ziccamoa,Za64rs,A,Zawrs` | 自然对齐 16/32-bit fetch 不撕裂、受限 LR/SC 在公平环境进展、A 全体原子值/排序、≤64B 连续自然对齐预约集、`WRS.STO` 有界等待且 `WRS.NTO` 支持 reservation/interrupt 事件，各自满足才 PASS | 撕裂指令、SC 永远失败、RMW 半步暴露、预约超界、STO 无界等待或 TW/VTW 陷阱错误未检出即 FAIL | fetch/内存/预约/等待事件和公平刺激 hash；WRS 可自发醒，NTO 不要求固定 timeout；无独立 agent/oracle→UNSUPPORTED |
| PMA/misaligned/CMO `Zicclsm,Zic64b,Zicbom,Zicbop,Zicboz,Ssccptr` | coherent+cacheable 主存 misaligned scalar/FP/vector load/store 的 bytes，64B block 的 CBO/PREFETCH 允许结果与页表硬件读，各自满足才 PASS | 拒绝合法 misaligned load/store、非法权限仍 zero、dirty 数据丢失、PTW PMA 错误未检出即 FAIL | CPU+独立 coherent agent 和 PMA trace；不要求 misaligned AMO 或 cache hit |
| S 模式/翻译 `Zifencei,Ss1p13,Svbare,Sv39,Svade,Svpbmt,Svinval,Svnapot` | code modify/fence 后取指、Bare/Sv39/页表 fault、PBMT/NAPOT 翻译和 invalidate 对照各项合法才 PASS | stale 指令、A/D=0 时未 fault、无效 PTE 被接受、该失效未失效未检出即 FAIL | fetch/PTW/fault/TLB trace；省略任何命名成员→UNSUPPORTED |
| S trap/counters/timer `Sstvecd,Sstvala,Sscounterenw,Sstc,Sscofpmf,Ssu64xl` | Direct BASE 任意有效 4B 对齐值、逐 fault `stval`、非零 HPM enable、timer/overflow/filter、UXL=64 各测合法值才 PASS | 硬编码 BASE、错误 fault 地址/计数/溢出或 UXL 非 64 未检出即 FAIL | CSR/trap/IRQ 事件与实现 HPM 清单；缺实测成员→UNSUPPORTED |
| H/guest `Sha:H,Ssstateen,Shcounterenw,Shvstvala,Shtvala,Shvstvecd,Shvsatpa,Shgatpa` | HS/VS/VU guest 两阶段读写/fault/trap 与每个具名 CSR/模式承诺独立通过才 PASS | 缺 VS mode、缺 `hgatp` Sv39x4/Bare、`htval` 错地址或 stateen 越权未检出即 FAIL | 两阶段 VA→GPA→PA 与权限/CSR/trap trace；仅 S-mode 测试→UNSUPPORTED |
| PM `Supm,Ssnpm` | U/VS/VU 的 PMLEN=0、7 分别在虚拟与 Bare/guest-physical，CPU MMIO/CMO 与 DMA/fetch/PTW 对照及 MXR/MPRV/SPVP 合法才 PASS | tagged DMA 被 mask、CPU MMIO 绕过 mask、PMLEN=0 误抹高位未检出即 FAIL | 起源/转换地址/权限/副作用日志；Sspm 未声明不参与 |
| Ratified options `localized/development/expansion` | V-090 **每一具名选项**独立选中后按其条款运行对应功能正例，结果合法才给该选项 PASS；未选 `deferred` | 选中 option 的数值/PMA/异常/CFI/模式或 DIEL 错误逃过检测即 FAIL | 每项 claim/spec/trace/负控制；已声明但缺 oracle→UNSUPPORTED，不从别的选项继承 |
| 自选 Secure / 独立 Server Platform | 仅分别声明时，bundle 所选成员全部通过；平台按自身规范实跑所声明 boot/设备/安全接口才分别 PASS | 将缺 RoT/设备写成平台合规或用 Core ISA 冒充平台验证即 FAIL | 独立 manifest 与 owner/运行签名；不声明则 deferred，绝不拖累 U64/S64 |

## 6. 阶段 gate 与非确定性的具体裁决

基线 p0 gate 使用 V-001–V-043 中适用的全部义务及 V-074/V-075/V-078/V-080；V-038/039 可用 V-021 的 seeded RTL ALU/CSR 注入完成无自然 defect 时的真实 mismatch/replay，不需要意外故障。p0 的 V-018 只需 I-035 保守 ordered LSU、forwarding 与 store visibility；启用可选 I-036 speculative load replay 后重跑 V-018 的 late alias/kill 并与 V-029 核对。C/A/S/U/Sv39/F/D 各增加 V-044–V-051 对应任务；V 增加 V-052–V-060；多 hart memory 加 V-064–V-067；LLB/MEF 加 V-061–V-063；cohort 加 V-068–V-071；pod 加 V-072。Linux 单 hart p1 必须 V-073 全部单 hart boot/userspace/MMU/timer/context 义务，不能只靠 I-048 启动烟测；SMP Linux 另需 V-064/V-065/V-067 与 V-073 多 hart 分支。每个所声明 FPGA 目标各需对应 V-077 板证据和 V-079 wrapper 证据；ASIC 目标需 V-079 及独立 ASIC 证据，不因 V-079 执行而要求任何 FPGA 板卡。可选 p0 DCLS 先需 V-081–V-083，可选目标物理 DCLS 再需 V-084 和目标 FPGA/ASIC 证据（V-084 的 V-079 仅物理目标触发）。

RVA23U64/S64 mandatory conformance 增加 V-085–V-087，S64 必须包含 U64 全部义务及 Sha 八成员；V-086 单 hart 用独立 coherent agent 验证区域 PMA，**仅**广告多 hart/shared memory 才追加 V-065/V-067。所有 ratified profile 选项走 V-090 的逐项 deferred/selected 分类，仅声明 Zicfilp/Zicfiss 才执行 V-088、声明 Zvkng/Zvksg/Zvbc 才执行 V-089；项目自选 `RVA23 Secure` 不是 ratified profile，Linux/Server Platform 另有独立规范/设备/软件 gate。不得将这些可选项目、SMP、密码或 FPGA 板卡强加给 mandatory Core。任务 Depends 仅表达无条件构建前提，不代替 claim 验收范围；所有扩展再次运行受影响的 baseline、negative-control、ACT、fuzz 与 replay 套件。

| 场景 | 不能采用的方法 | 裁决与证据 |
|---|---|---|
| 不同 branch/cache/route 延迟 | 对齐 cycle 后强行比较 PC | 按每 hart architectural boundary 对齐；cycle 仅作为性能/重放时间轴 |
| 非同步 multi-hart 数据竞争 | 按主机 callback 顺序强制参考 SC | 保留实际 memory observation，查 RVWMO 合法见证；功能参考只校验指令计算 |
| IRQ 到达与接受之间存在合法延迟 | 每看到 mismatch 就调注入 cycle | 事件文件固定 assert；DUT 接受必须满足 enable/priority/bounded-response 合同，再在记录边界让参考处理 |
| LR/SC 合法失败 | 总是给参考复制 DUT rd | 独立检查预约/冲突与允许结果，再选择同一个合法 nondeterministic branch；进展另检 |
| RVV FOF 可缩短 vl | 固定期待永远最大 vl | 在规范区间检验合法值、最小进展与后续程序结果；记录每次选择 |
| FP unordered reduction / agnostic tail | 全部 vector bits 屏蔽 | 指令专用集合/关系判断、相邻非法值负控制，明确不保证相同 bit pattern |
| lockstep 模式 | 把两个architectural hart直接组成“lockstep” | 只有I-087声明的main/shadow pair有效；异步输入必须同步，mismatch fail-closed，DCLS不声称纠错 |
| CSR WARL/FS/VS、计数器 | 整个 CSR 不比较 | 逐字段规则；性能计数值分离；所有 waiver 逐次列账 |
| 未支持模型功能 | `skip` 后写 DUT state 到 REF | 阻断该功能差分 gate，选择真正支持的第二模型或独立形式/定向 oracle；无验证不得宣称支持 |

尚待实施决策的 gate 不是缺失范围：具体 image digest/JDK/firtool/工具链、RV64 baseline CSR/PMP/PMA、VLEN/ELEN、NEMU 新核精确配置、SMP golden-memory 支持上限、宽 VLEN adapter、各平台 trace 带宽/存储容量、ASIC formal 工具覆盖，都由对应任务输出明确决议。若某参考不能支持必须功能，工作是实现/选择可审计适配器并校准，不是删除该功能目标。

## 7. SRC-03 完整阅读记录与逐标题处置图

本轮已**完整阅读** `deep-research-report(2).md` 第 1–2528 行，使用显式 `:raw` 范围：**1–350、351–700、701–1050、1051–1400、1401–1750、1751–2100、2101–2528**。连续无缺口；另外用 heading grep 核对全部 45 个标题。原文不修改。原报告内 `cite…` 是不可追溯会话 token，不是外部主源；下表保留思想但不把 token 转换成伪参考文献。

处置术语：**接受**是有条件工程原则，非已证明实现；**修正**指出不可照搬的语义；**实验**是待比较的假设；**后置**保留完整研究义务但不放入 p0；**拒绝**拒绝原始无限化/错误解释，仍记录原因及替代。不把“后置”当永久删除。

| 源范围 | 原标题（按原层级顺序，含父标题自己的引言） | 处置与验证落点 |
|---|---|---|
| 1–2 | 面向 RISC-V / RVV 的动态聚合处理器架构研究：从高 IPC CPU 到 GPU-like SIMT 的弹性执行 Fabric | 接受研究问题；整体 V-001–V-080，不承诺 GPU 神奇加速 |
| 3–54 | 研究结论与架构定位 | 接受 architectural state 固定/分层弹性；研究数值均未复现；V-002/V-076/V-078 |
| 55–56 | 从 XiangShan 与现有 RISC-V 设计推导新的 Pipeline | 接受设计对照但修正 XiangShan 不是 semantic oracle；V-005/V-075 |
| 57–129 | 不要把 Frontend 也完全 Fabric 化 | 接受 ordered frontend/commit；不排除常规预测优化；V-013–V-016 |
| 130–229 | uOP 不应该只是传统 CPU 的 micro-op | 接受 tag/capability/epoch 观念；字段压缩须保留可观测不变量；V-008/V-027/V-029 |
| 230–281 | 关键不是一个 Global Scheduler，而是 Scheduler Hierarchy | 接受局部 scheduling/全局慢分配；拒绝无界 flat select；V-028/V-032/V-033 |
| 282–323 | Dynamic scaling 必须有两个时间尺度 | 接受但补 drain、generation、取消/异常协议；V-031/V-034/V-035 |
| 324–369 | Pipeline depth 因此自然变成 Variable，而不是硬做 Variable Pipeline | 接受 route latency 可变；不让 latency 改变 ISA 顺序；V-010/V-026/V-028 |
| 370–395 | RVV 与 CPU—GPU 连续体 | 接受 ILP/MLP/DLP/TLP 区分；测 useful work 而非单指标；V-002/V-074/V-076 |
| 396–448 | RVV 应该成为 Fabric 的第一等公民 | 后置完整 V，接受物理 lane 分区但 architectural VLEN 固定；V-052/V-060 |
| 449–539 | 不要把一个 RVV instruction 完全展开成数百个 ROB uOP | 接受 macro descriptor，不删除 element fault/progress state；V-054–V-059 |
| 540–576 | RVV 和 SIMT 不能简单画等号 | 接受；不同 hart 仍有独立 PC/CSR/trap；V-064/V-068/V-069 |
| 577–663 | Dynamic Cohort Fusion | 后置并修正仅 same PC 不足；全相容条件与独立 fault；V-068–V-071 |
| 664–726 | 但是这里存在一个不可消除的极限 | 拒绝任意单线程自动产生 TLP；保留标准多线程/RVV 路线；V-073/V-074/V-076 |
| 727–789 | 我建议定义五个 Execution Personality | CPU latency/throughput/RVV/cohort 分阶段；Extreme SIMT/optional ISA 是后置探索，不宣称现成 GPU runtime；V-034/V-035/V-068–V-072 |
| 790–807 | Memory Fabric 与 Per-Lane Cache 的重新设计 | 接受 LLB+shared-L1+coalescer 研究目标；V-061–V-063/V-067 |
| 808–888 | 为什么完整 per-lane cache 容易失败 | 实验对照而非无测量断言；不要求先造复杂 coherent cache 才能完成基础验证；V-061/V-067/V-076 |
| 889–952 | Lane Locality Buffer | 接受 reuse buffer 但必须证明 load 新鲜性与地址权限；V-061 |
| 953–985 | LLB 最适合作为非 coherent microarchitectural structure | 修正“非参与者”≠“无需一致性”；失效或版本机制必要；V-061/V-067 |
| 986–1041 | 不应该简单做“预测 LLB 或 L1 二选一” | 接受预测只影响延迟、不改语义，投机并行访问不能重复副作用；V-019/V-061 |
| 1042–1123 | Memory Operation 也应该成为 Packet，而不只是 uOP | 接受 packetization；必须可逆映射、逐 byte/element fault；V-018/V-056/V-062 |
| 1124–1160 | 更进一步：Cross-Hart Coalescing | 后置并加物理地址/权限/PMA/order 相容；V-062/V-070 |
| 1161–1199 | Memory Fabric 可以做三层调度 | 接受 MEF 区分 address/transaction/criticality；V-032/V-063 |
| 1200–1256 | Critical Load 不应该排在 100 个 GPU-like Load 后面 | 接受 QoS，但不能永久饿死 bulk 流；V-033/V-063 |
| 1257–1272 | Ara-Opt 给这个方向提供了非常强的证据 | 数值/归因待外部主源与本项目实测，不能当性能保证；V-063/V-076 |
| 1273–1274 | Dynamic Execution、Unified Writeback 与 Core Aggregation | 接受逻辑统一/物理分布边界；V-027–V-035/V-072 |
| 1275–1342 | Unified Writeback 的概念是对的，但不能是一根 Bus | 拒绝无界单 bus，接受 hierarchy/credit；V-008/V-024/V-032 |
| 1343–1424 | Result Locality 应成为 Scheduling 的第一等指标 | 实验 oldest/locality/congestion heuristic；不预设 neural predictor；V-028/V-033/V-076 |
| 1425–1488 | 从 Giant ROB 转向 Per-Hart Commit Domain | 接受 independent retirement；共享 FU 不共享错误 architectural 顺序；V-013/V-064 |
| 1489–1536 | Core 也不应该消失，而应该变成 Commit / Control Domain | 接受 control/execution 分离；core fusion 不可混同 architectural hart fusion；V-064/V-072 |
| 1537–1595 | 真正可扩展的单位应当叫 Slice 或 Tile | 接受 pod 内细粒度、pod 间粗粒度；跨 pod 任意 ALU 借用不作为基线；V-072 |
| 1596–1669 | IPC、Throughput 与这类架构真正能赢在哪里 | 接受 useful work、MLP、bytes/op 等多指标；cycle 模拟与频率/功耗区分；V-074/V-076 |
| 1670–1734 | 传统 High-IPC CPU 的根本问题之一是资源被静态 provision | 实验动态 ownership 收益；必须固定资源对照；V-028/V-034/V-076 |
| 1735–1818 | 但是 Dynamic Fabric 本身也可能让 IPC 更低 | 接受 fast local island，必须保留负收益结果；V-028/V-076 |
| 1819–1877 | 可以做 Execution Locality Predictor | 后置性能实验；预测错误不影响正确性；V-028/V-076 |
| 1878–1928 | Branch Prediction 不应该被过度“Fabric 化” | 接受传统 predictor + 分布 kill，补 ABA/late response；V-016/V-029–V-031 |
| 1929–1996 | 这里最大的创新点其实可能是 “Elastic Window” | 接受 waiting state 分散；in-flight 工作量不等于无限 ROB；V-027/V-032/V-040 |
| 1997–2016 | FPGA 原型与验证路线 | 接受分期；不是性能 gate 失败就删除用户后续目标；V-078 |
| 2017–2077 | 最小原型应该是 Scalar Elastic Backend | 接受先 scalar、明确 RV64IM_Zicsr_Zifencei/M-mode；原 2–4 wide 是假设；V-025–V-043 |
| 2078–2128 | 下一版加入 RVV Lane Fabric | 后置完整 V、partition A/B；V-052–V-060 |
| 2129–2171 | 然后验证 Lane Locality Buffer | 实验 L1-only/LLB/coalescer；private coherent cache 是有预算条件的对照，不默认为最佳；V-061–V-063/V-076 |
| 2172–2234 | 再加入多 Hart 与 Cohort Fusion | 后置并分别验收多 hart memory 与 cohort；V-064–V-071 |
| 2235–2270 | 最后才做 Pod Aggregation | 后置 coarse task/vector sharing；保留真正跨 pod 验证；V-072 |
| 2271–2346 | XiangShan 的工程方法很适合借鉴 | 接受 commit-based difftest；修正 vector atomic completion 与 oracle 身份；V-001–V-024/V-056–V-058/V-075 |
| 2347–2528 | 推荐的最终 EAF-V 架构 | 接受四原则为MosaicRV研究约束，Linux/GPU/能耗等不提前宣称；完整分期保留；V-073/V-076–V-080 |

## 8. 主源参考本地账本

以下 ID 专属本文，可由 [参考总账](references.md) 汇总。**全部检索日期为 2026-09-29**。URL 是已读取的主源；不使用原报告会话引用 token。`current/main` 页面用于发现，实施 pin 与实际构建/运行证据仍必须通过 V-001。没有声称本文已经执行其中任何命令。

| ID | 主源 URL、版本/日期 | 本文支撑断言 | 不能据此推断 |
|---|---|---|---|
| VR-001 | [XiangShan 官方仓库 README](https://github.com/OpenXiangShan/XiangShan)，branch maintenance 表 2026-06-30 | 当前研究优先 kunminghu-v2，V3 快速演化；XiangShan 是处理器设计 | V2 无 bug、任意 config 稳定、其结果可替代 ISA |
| VR-002 | [V2 revision API](https://api.github.com/repos/OpenXiangShan/XiangShan/commits/kunminghu-v2)、[固定 tree](https://api.github.com/repos/OpenXiangShan/XiangShan/git/trees/1dc8c8bc37ef309af9c9ff12f711f47207bb4800)、[固定 README](https://raw.githubusercontent.com/OpenXiangShan/XiangShan/e7bab53e66dfb3c4a1d11cf9519b0396f8576cae/README.md)、[Dockerfile](https://raw.githubusercontent.com/OpenXiangShan/XiangShan/e7bab53e66dfb3c4a1d11cf9519b0396f8576cae/Dockerfile)、[Mill version](https://raw.githubusercontent.com/OpenXiangShan/XiangShan/e7bab53e66dfb3c4a1d11cf9519b0396f8576cae/.mill-version)；commit 2026-09-24 | SHA/gitlinks、Mill 0.12.3、`make init/verilog/emu` 与 sample emu 命令；容器 `latest` 未固定 | 实测能构建、具体硬件需求、JDK/firtool 自动正确 |
| VR-003 | [Difftest 固定 README](https://raw.githubusercontent.com/OpenXiangShan/difftest/3729300ae233816d332d057f170472ebe35147b0/README.md)、[Bundles.scala](https://raw.githubusercontent.com/OpenXiangShan/difftest/3729300ae233816d332d057f170472ebe35147b0/src/main/scala/Bundles.scala)、[refproxy.h](https://raw.githubusercontent.com/OpenXiangShan/difftest/3729300ae233816d332d057f170472ebe35147b0/src/test/csrc/difftest/refproxy.h)、[refproxy.cpp](https://raw.githubusercontent.com/OpenXiangShan/difftest/3729300ae233816d332d057f170472ebe35147b0/src/test/csrc/difftest/refproxy.cpp)；gitlink pin | 非 Chisel 生成接口、mandatory probes、reference ABI、固定 vector 形状、skip/waive 风险、Linux multi-instance 路径、LightSSS 参数限制 | 任意 SV 自动接入、任意 VLEN/hart 数完整支持、golden memory patch 等于内存模型证明 |
| VR-004 | [ready-to-run 固定目录/README](https://github.com/OpenXiangShan/ready-to-run/tree/c4114ce3fffcd5c147c525014b40f1c841347238)、[commit provenance](https://api.github.com/repos/OpenXiangShan/ready-to-run/commits/c4114ce3fffcd5c147c525014b40f1c841347238)；2026-09-24 | 预编译 NEMU 来源 f39e307、single/dual 库、Spike fork 来源、样例镜像说明 | 编译器/所有 build flags 全可追溯、样例适合 p0、新核 Linux 已可运行 |
| VR-005 | [NEMU 官方 README](https://github.com/OpenXiangShan/NEMU)、[固定 configs 目录](https://github.com/OpenXiangShan/NEMU/tree/f39e3077d7bac3cd9a3a853a9300a5f8f0293a2c/configs)、[固定 xs-ref config](https://raw.githubusercontent.com/OpenXiangShan/NEMU/f39e3077d7bac3cd9a3a853a9300a5f8f0293a2c/configs/riscv64-xs-ref_defconfig) | RV64 保证范围、reference `.so` 构建模式、full-system/bare-metal 路径、standalone ELF 限制、RVV/H/Sv48 等 config 事实 | README 旧的 RVV 概述优于实际 config；reference configuration 与 p0 天然相同 |
| VR-006 | [Spike 主线 README](https://github.com/riscv-software-src/riscv-isa-sim)、[revision API](https://api.github.com/repos/riscv-software-src/riscv-isa-sim/commits/master)；commit 2026-09-28 | ISA 模型、RVV v1、SC 内存执行子集、C++ 非稳定 API、`spike pk hello` 前提 | SC 执行覆盖所有 RVWMO 行为、pk 支持 p0、主线直接导出 NEMU ABI |
| VR-007 | [Sail 当前官方 README](https://github.com/riscv/sail-riscv)、[0.14.1 tag API](https://api.github.com/repos/riscv/sail-riscv/git/ref/tags/0.14.1)、[固定 README](https://raw.githubusercontent.com/riscv/sail-riscv/e4b243f4eb5d1ed05bbbc030ad338c2a32c45d72/README.md) | RISC-V International 采用的 Sail 语义、配置 schema、ELF CLI、F/D/V/Sv39 支持列表、source-build compiler 要求 | 执行 Sail 就形式证明 DUT；所有非确定性策略与 DUT 相同；必然支持所有实验扩展 |
| VR-008 | [Verilator arguments](https://verilator.org/guide/latest/exe_verilator.html)、[C++ example](https://verilator.org/guide/latest/example_cc.html)、[connecting](https://verilator.org/guide/latest/connecting.html)、[installation](https://verilator.org/guide/latest/install.html)、[v5.052 tag](https://api.github.com/repos/verilator/verilator/git/ref/tags/v5.052)、[tag object](https://api.github.com/repos/verilator/verilator/git/tags/efa4927be48e75c3cd08fc848b198d1d9d237f00)；5.052/tag 2026-09-05 | `--cc --exe --build`、eval/final/DPI/timing、源码依赖与确切 tag commit | 仿真等于四态/CDC/板级时序验证、与全部上游组合已兼容 |
| VR-009 | [ACT4/riscv-arch-test 官方 README](https://github.com/riscv/riscv-arch-test)、[revision API](https://api.github.com/repos/riscv/riscv-arch-test/commits/main)；commit 2026-09-08 | ACT4 替代 RISCOF、UDB/rvmodel/linker/Sail 0.14.1、全部适用自检 ELF、默认排除项、非充分验证 | 通过 ACT 即完整 CPU 正确或已取得认证；主源默认排除可静默继承 |
| VR-010 | [RISCV-DV 官方 README](https://github.com/chipsalliance/riscv-dv)；检索时主线 | RV32/64 IMAFDC、privilege/MMU/随机指令覆盖；UVM simulator 前提 | 已支持完整 V、现成流程必然在指定 Verilator 版本运行；随机数多等于正确 |
| VR-011 | [riscv-formal 官方 README](https://github.com/YosysHQ/riscv-formal)；检索时主线 | RVFI/processor wrapper、RV32I/RV64I 主要关注、proof/assumption/cover 思路 | 已有完整 OoO/RVV/multi-hart 证明；bounded verification 就全状态证明 |
| VR-012 | [RISC-V ISA Manual 官方仓库](https://github.com/riscv/riscv-isa-manual)、[正式规范入口](https://riscv.org/specifications/) | 正式/草稿版本边界与整数、扩展、privilege 语义的规范来源；实施时逐条 pin | 主线重构文件路径/最新草稿等于项目批准的 ISA 版本；未读条款可据 README 凭空补充 |
| VR-013 | [vector chapter source](https://raw.githubusercontent.com/riscv/riscv-isa-manual/main/src/unpriv/zv.adoc)、[vector-common normative source](https://raw.githubusercontent.com/riscv/riscv-isa-manual/main/src/unpriv/vector-common.adoc)、[Zve32x source](https://raw.githubusercontent.com/riscv/riscv-isa-manual/main/src/unpriv/zve32x.adoc)；检索时 main，正在重构 | VLEN/ELEN、完整 V 的要求、agnostic 合法结果、masked/tail/prestart、FOF、vstart/precise partial traps；common 已读开头及 agnostic/vstart/FOF/segment/precise-trap 定位全文段落 | 完整向量指令全原子、agnostic 任意值、所有 tail 都不比较、动态 lane 等于动态 VLEN |
| VR-014 | [RVWMO normative source](https://raw.githubusercontent.com/riscv/riscv-isa-manual/main/src/unpriv/rvwmo.adoc)；检索时 main | po/global-memory-order/atomic primitives、multi-operation、依赖/同步规则、I/O/PTW/vector 形式化范围限制 | 一份普通 memory `.cat` 自动验证全部 I/O/VM/vector coherence、同退休顺序等于唯一全局 memory order |
| VR-015 | [herdtools7 官方 README](https://github.com/herd/herdtools7)；检索时主线 | herd7 是弱内存模型模拟工具、litmus/diy 工具族 | 当前 README 列出的 litmus7 native 架构当然含 RISC-V；具体 RISC-V `.cat`/runner 命令尚需 pin 和小例校准，本文未伪造命令 |

## 9. 本轮交付证据与实施前未决事项

- 原报告 SRC-03 全文连续 2528 行已阅读，45 个标题逐项处置；保留所有架构思想的接受/修正/实验/后置/拒绝关系。
- 主源确证了 XiangShan V2→Difftest/ready-to-run→NEMU 的来源链，发现固定 128-bit vector 参考布局、vector skip 限制、主线 Spike SC 范围、Sail/ACT4 版本组合及 ACT4 默认排除风险。
- 90 个未来验证任务覆盖 scalar→A/C/privilege→F/D/V→LLB/MEF→多 hart/cohort→pod→程序/Linux→完整 RVA23U64/S64 mandatory /逐项 ratified options /项目自选 Secure→独立平台回放；每项含依赖、输入、动作、输出与 pass/fail。它们不是已完成的实现或测试。
- 可运行性、参考 ABI 兼容性、工具/镜像 digest、准确 profile 参数、宽 VLEN adapter、SMP memory checker、板卡 trace 及 ASIC 工具全部是后续任务的明确验收产物；本轮没有用安装/构建/模拟结果冒充文献核实。
