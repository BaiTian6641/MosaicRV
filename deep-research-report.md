# 面向动态资源织构的高性能 RISC-V 微架构研究：从 XiangShan、BOOM、OpenC910 到 SIMT、Core Fusion 与 FPGA 原型

## 结论与总体判断

你的设想在方向上是成立的，而且**真正有研究价值的部分并不是“RISC-V → uOP → Rename/ROB/Issue”本身，而是把传统 OoO CPU 中相对固定的执行端口、寄存器端口、写回路径、Memory Pipeline 进一步抽象成一个可以运行时分配、路由和聚合的“Execution Resource Fabric”**。

现有高性能 RISC-V 设计已经证明了前半部分：XiangShan、BOOM、OpenC910 都采用或体现了宽发射、寄存器重命名、乱序调度、ROB、LSQ、多个执行流水线这条路线。XiangShan Kunminghu 的公开设计尤其有参考价值：其 Rename 维护独立的整数、浮点、向量物理寄存器资源；ROB 为 160 entries；Issue Queue 按类型组织并进行 ready/age 调度；LSU 具有多个独立 Load/Store pipeline；而 Bypass 和 Writeback 已经明显呈现“网络化、仲裁化”的趋势。citeturn19view0turn20view0turn20view1turn20view3turn20view4

但 XiangShan 仍然基本属于：

> **固定拓扑、固定 FU 类型/连接关系之上的动态 instruction scheduling。**

你想进一步做的是：

> **instruction scheduling + resource scheduling + route scheduling + thread/lane scheduling。**

这是一个重要区别。

我建议把你的架构从：

```text
Fetch
→ PreDecode
→ Decode
→ Rename/ReOrder/Queue/Dispatch
→ Functional Array
→ Unified Memory
→ Unified Write Back
```

进一步明确成：

```text
                       ┌──────── Architectural Context ────────┐
 Fetch → PreDecode → Decode / uOP Expansion → Rename / ROB
                       └────────────────┬──────────────────────┘
                                        │
                                Resource Scheduler
                                        │
                 ┌──────────────────────┼──────────────────────┐
                 ▼                      ▼                      ▼
            Exec Cluster 0         Exec Cluster 1         Exec Cluster N
         ┌───────────────┐      ┌───────────────┐      ┌───────────────┐
         │ Local IQ      │      │ Local IQ      │      │ Local IQ      │
         │ Operand Bank  │      │ Operand Bank  │      │ Operand Bank  │
         │ ALU/MUL/BR    │      │ ALU/FPU/VEC   │      │ AGU/ALU/...   │
         │ Local Bypass  │      │ Local Bypass  │      │ Local Bypass  │
         └──────┬────────┘      └──────┬────────┘      └──────┬────────┘
                └──────────────────────┼───────────────────────┘
                                       ▼
                            Completion / Result Fabric
                         ↙              ↓              ↘
                       PRF          ROB Complete       Wakeup
                                       │
                               Unified Memory Fabric
                                       │
                          L1 / Shared Cache / Coherence
```

这里最关键的一点是：

**不要真的把所有东西做成一个“大统一 Scheduler、大统一 PRF、大统一 Bypass、大统一 LSQ”。**

那样逻辑上很优雅，但物理实现会迅速变成 wakeup/select、CAM、寄存器端口、crossbar 和长线延迟问题。XiangShan 当前 27 个 functional units 已经对应总计 71 个 source operands，并且其 bypass connectivity 是显式配置的；这正说明高性能核心扩宽以后，数据移动和连接复杂度会成为第一等问题，而不是 ALU 数量。citeturn20view3

更可行的方向是：

**逻辑统一、物理分布；全局分配、局部调度；动态路由、静态约束。**

这也是我认为你的设计最有机会形成独立架构贡献的地方。

## 从 XiangShan、BOOM 与 OpenC910 能学到什么

### XiangShan 是最值得作为第一参考对象的设计

截至 2026 年，XiangShan 项目仍在积极开发第三代 Kunminghu；项目同时保留较稳定的 Kunminghu V2R2 设计文档用于研究、验证与下游开发，并提供 Chisel RTL、Verilator 仿真以及 difftest 环境。换言之，它不仅适合研究“高性能 RISC-V 怎么设计”，也很适合研究“怎么把这样一个设计做成可以实际验证的工程”。citeturn15view3

XiangShan 的后端结构与你的起点非常接近：

```text
Decode
   ↓
Rename
   ↓
Dispatch
   ├── Scalar Integer IQ
   ├── Scalar Memory IQ
   ├── Vector/FP IQ
   └── Vector Memory IQ
            ↓
         DataPath
            ↓
       Bypass Network
            ↓
          EXUs
            ↓
       WbDataPath
        ↙      ↘
      PRF       ROB
```

其 Rename 阶段已经明确是 uOP-centric：Rename 接收 Decode 信息、分配 `robIdx` 和物理寄存器、查询 operand physical register，然后将 renamed uOP 送给 Dispatch。它分别维护 224 个整数、192 个浮点和 128 个向量物理寄存器状态，并通过 speculative/committed mapping 以及 snapshot/re-rename 支持错误路径恢复。citeturn19view0

更值得注意的是它的端口数量。Kunminghu V2R2 文档描述的 RenameTableWrapper 有：

| 类型 | Rename map read ports | Rename write ports |
|---|---:|---:|
| Integer | 12 | 6 |
| FP | 18 | 6 |
| Vector | 30 | 6 |

这些数量并非偶然；宽 superscalar backend 很快就会把“端口数量”变成面积和 timing 问题。citeturn19view0

ROB 则是 160 entries，并支持最高每周期 8 个 ROB entry 的 commit/walk。ROB 不只是一个 reorder queue，它同时承担 precise state、exception、redirect、snapshot、vector completion 等责任。citeturn20view0

这对于你的设计有一个非常重要的启示：

> **Dynamic resource scaling 不能取代 architectural ordering layer。**

也就是说，下面这一层可以非常自由：

```text
uOP A → Cluster 0
uOP B → Cluster 3
uOP C → Cluster 1
uOP D → Memory Pipe 2
```

但是上层仍需要一个稳定的 architectural identity：

```text
Thread ID
Architectural Instruction ID
ROB ID
uOP ID
Epoch / Speculation ID
Destination Tag
Exception State
```

否则 branch recovery、exception、interrupt、fence、memory ordering 和 precise retirement 会变得极其困难。

### XiangShan 的 Issue Queue 已经接近你的“资源感知调度”入口

XiangShan 将 Issue Queue 分成 scalar integer、vector/FP、scalar memory、vector memory 等类型；每个 IQ 可接受最多两个 dispatch，并持续监视 wakeup，随后最多选择两个 ready instruction，采用 oldest-ready 优先，同时还支持 speculative wakeup 和提前检测 writeback conflict。citeturn20view1

“提前检测 writeback conflict”这一点尤其值得注意，因为它证明：

> 执行资源不仅仅是 FU。

真正的资源集合至少是：

```text
FU slot
Register Read Port
Operand Network Slot
Bypass Slot
Execution Pipeline Slot
Memory Port
Translation Port
Cache Port
Writeback Port
PRF Bank Write Port
ROB Completion Port
Inter-cluster Link
```

因此，你所说的“pipeline dynamically scale the resource”最好不要定义成：

> 多几个 ALU 就动态使用几个 ALU。

而应该定义成：

> **一个 uOP 在 issue 前获取一组 execution-resource contracts。**

例如：

```text
uOP {
    src0 = p42
    src1 = p81
    dst  = p103

    required_class = INT_ALU
    earliest_issue = cycle N

    allocated:
        exec_cluster = C2
        alu_slot     = A1
        src_bank0    = RF3
        src_bank1    = RF0
        wb_bank      = RF5
        result_route = R7
}
```

这才是真正意义上的 resource-aware OoO。

### BOOM 恰好指出了你的核心研究问题

BOOM 同样采用显式物理寄存器重命名。它把 logical register 映射到 physical register，以消除 WAR/WAW，仅留下真正的 RAW dependence；它的 Issue Queue 保存尚未执行的 uOP，并根据 operand readiness 选择执行。citeturn20view5turn20view6

但是 BOOM 的 register-file 文档有一段非常有价值的设计讨论：当前 BOOM 静态配置足够的 read ports 来满足所有 issue ports；文档明确指出，未来设计可以**减少物理 read ports，再动态调度/仲裁这些端口以提高面积效率**，代价是额外仲裁 pipeline、structural hazard detection，以及当资源冲突发生时 kill/reissue uOP 的能力。citeturn15view2

这实际上与您的想法几乎直接相交。

区别在于 BOOM 的设想主要还是：

```text
Instruction Scheduler
       ↓
dynamic RF-port arbitration
       ↓
fixed FU topology
```

而你可以把它扩展为：

```text
            uOP Scheduler
                  ↓
        Resource Allocation Layer
       ↙       ↓        ↓       ↘
 RF ports     FU      Route    WB ports
       \       ↓        ↓       /
        Dynamic Execution Fabric
```

这比“dynamic RF scheduling”更一般化。

### OpenC910 提供另一个实际 OoO RISC-V 参考点

OpenC910 是公开 RTL 的 RISC-V 核心；PULP 团队的 PULP-C910 项目明确将它描述为 superscalar out-of-order RISC-V core，并已经完成面向 PULP/Cheshire 的标准接口适配以及 FPGA Linux 启动，包括 Xilinx VCU128。citeturn15view0turn15view1

这对你的项目有两个意义。

第一，**高性能 OoO RISC-V 放到 FPGA 并不是一个纯理论目标**；PULP-C910 已经证明能够围绕完整 OoO core 做 FPGA/Linux 系统集成。citeturn15view1

第二，在你的研究阶段不必直接把整个 SoC、cache coherence 和 backend novelty 同时创新。可以像 PULP-C910 那样，把核心 backend novelty 和 SoC glue/interface 分离，否则验证空间会爆炸。citeturn15view1

## 建议的动态执行织构

我会把你的架构暂时称为 **Dynamic Execution Fabric OoO，简称 DEF-OoO**。名字不重要，重要的是定义几个明确的 abstraction。

### uOP 应成为真正的内部 ISA

RISC-V architectural instruction 不应该直接穿过整个后端。Decode 后建议产生一个内部 uOP format：

```text
uOP {
    ContextID
    WarpID / ThreadGroupID
    MacroInstID
    UopID
    ROBTag

    OpcodeClass
    FuCapabilityMask

    SrcTag[0:N]
    SrcReady[0:N]
    DstTag

    Immediate

    BranchMask / SpecEpoch

    MemTag
    LoadQueueTag
    StoreQueueTag

    LaneMask

    ExceptionMeta

    FirstUop
    LastUop
}
```

这里应明确区分：

```text
Architectural Instruction
        ↓ decode/expand
    1..N uOPs
```

和：

```text
1 ROB architectural entry
        ↕
N internal uOP completions
```

这对于 vector、复杂 memory op、atomic、future custom instruction 尤其重要。XiangShan 的 vector memory instruction 本身就会在 decode 后拆成多个 uOP，而且每个 uOP 还可能对应多个 element，并在 dispatch 时分配多个 LSQ entries。citeturn20view4

因此，不建议简单采用：

```text
ROB entry == uOP
```

更好的关系是：

```text
MacroInst / Architectural ROB Entry
                  │
       ┌──────────┼──────────┐
       ▼          ▼          ▼
      uOP0       uOP1       uOP2
       │          │          │
     done0      done1      done2
       └──────────┼──────────┘
                  ▼
         instruction complete
                  ▼
               retire
```

这为以后 SIMT/vector/microcoded instructions 都保留了扩展余量。

### Resource Scheduler 应与传统 Issue Scheduler 分离

这是我最推荐的结构创新。

传统 OoO：

```text
Ready uOP
   ↓
Issue Select
   ↓
Fixed execution port
```

你的架构可以改为：

```text
Dependency-ready
      ↓
Logical Issue
      ↓
Resource Match
      ↓
Route Reservation
      ↓
Operand Collection
      ↓
Physical Issue
```

也就是说，将“ready”和“可以真实执行”拆成两个状态。

例如：

```text
WAIT_OPERAND
    ↓
OPERAND_READY
    ↓
WAIT_RESOURCE
    ↓
RESOURCE_RESERVED
    ↓
IN_FLIGHT
    ↓
RESULT_PENDING
    ↓
COMPLETE
```

这样一个 ALU operation 即使 operand 已经 ready，也不意味着立即执行。Scheduler 还需要看到：

```text
ALU availability
source bank availability
route availability
writeback availability
destination bank availability
cluster occupancy
expected latency
```

这种设计特别适合 FPGA，因为你可以故意减少昂贵资源：

```text
8 logical issue slots
4 logical ALU-capable routes
2 physical ALUs
2 PRF read banks / cycle
```

然后通过 queue/time multiplexing 获取吞吐，而不需要把所有 logical issue width 映射成对应数量的硬件端口。BOOM 文档明确指出这种动态 register-port arbitration 可以节省资源，但也会引入 structural hazard 和重发机制；你的架构可将这一思想提升为系统性的 resource scheduler。citeturn15view2

### Functional Array 最好是 Clustered，而不是完全统一

我强烈不建议做：

```text
                     ┌ ALU0
                     ├ ALU1
Global IQ → crossbar ├ MUL0
                     ├ FPU0
                     ├ LSU0
                     ├ ALU2
                     ...
```

随着规模增加：

```text
Scheduler size × wakeup sources × operand ports × FU ports
```

很容易形成近似全互连的物理问题。

XiangShan 的公开 bypass 数据已经很好地展示这种压力：27 个 functional units、71 个 source operand，并需要显式规定哪些 producer 可以 bypass 到哪些 consumer。citeturn20view3

建议采用：

```text
                    Global Resource Manager
                            │
          ┌─────────────────┼─────────────────┐
          ▼                 ▼                 ▼
      Cluster A         Cluster B         Cluster C
     ┌──────────┐      ┌──────────┐      ┌──────────┐
     │ Local IQ │      │ Local IQ │      │ Local IQ │
     │ ALU × 2  │      │ ALU × 2  │      │ FPU/VEC  │
     │ MUL      │      │ Branch   │      │ MUL      │
     │ PRF Bank │      │ PRF Bank │      │ PRF Bank │
     │ Bypass   │      │ Bypass   │      │ Bypass   │
     └────┬─────┘      └────┬─────┘      └────┬─────┘
          └─────────────────┼─────────────────┘
                       Result Network
```

核心原则：

**same-cluster dependency = fast path**

```text
producer C0 → local bypass → consumer C0
```

**cross-cluster dependency = slower path**

```text
producer C0
    ↓
result router
    ↓ (+1/+2 cycles)
consumer C2
```

Scheduler 应该具有 affinity：

```text
if dependency producer is in Cluster 1:
    prefer Cluster 1
else:
    choose least-loaded compatible cluster
```

这就把传统 CPU 的“execution port selection”升级成一个小型 placement/routing problem。

这方面并不是完全没有历史先例。TRIPS 的研究明确探索过由 replicated execution tiles、分布式控制和 switched operand network 构成的 distributed microarchitecture，并让动态 OoO execution 跨多个 tile 工作；其研究动机之一正是大型集中式结构面临的 wire-delay/scalability 问题。citeturn7search3turn7search7turn7search12

因此，**你的创新点不能简单表述为“distributed functional units”**。更有说服力的研究问题是：

> 如何在保持标准 RISC-V、precise exception、现代 OoO speculation、RVWMO 和 FPGA-friendly implementation 的同时，实现可动态分配的 execution/resource fabric？

这比单纯“做一个 distributed CPU”更有研究辨识度。

## SIMT、ILP、SMT 与动态聚合应该如何组合

这里是你的描述中最需要在架构定义阶段澄清的一点。

你提到：

> Single instruction and multiple thread, plus multi-inst on concurrent run.

我建议把它正式拆成四种并行度。

| 并行形式 | 含义 | 你的架构是否需要 |
|---|---|---|
| ILP | 同一 instruction stream 中多个独立 uOP OoO 并发 | **需要** |
| Superscalar | 每周期 decode/rename/issue 多个 instruction/uOP | **需要** |
| SMT | 多个独立 architectural thread 共用 backend | 建议后期加入 |
| SIMT | 一条 instruction 驱动多个 thread/lane | **如果这是核心目标，需要明确设计** |
| RVV/SIMD | 一个线程的一条 vector instruction 操作多个 element | 可选但高度相关 |

这几个概念不能混在一起。

Vortex 是一个很好的 RISC-V SIMT 对照。它是完整的开源 RISC-V GPGPU，core、warp、thread、ALU/FPU/LSU/SFU 数量和 issue width 都可配置，并实现 FPGA backend；其 MICRO 工作展示了扩展 RISC-V 形成 GPU/SIMT 执行模型，并在 FPGA 上扩展到多 core。citeturn17view0

Ventus 则采用 RISC-V/RVV 方向构建 GPGPU，并专门针对 vector/GPGPU execution 做硬件和软件栈研究，说明 RISC-V 上 SIMT/vector execution 已经是一个活跃方向。citeturn17view1

因此，我不建议你的第一版采用：

```text
scalar OoO instruction
         ↓
突然广播成 N threads
```

而应该在 uOP 层面明确一个 **execution width abstraction**。

例如：

```text
uOP {
    ExecutionMode:
        SCALAR
        VECTOR
        SIMT

    ContextID
    GroupID
    ActiveMask
}
```

对于 SIMT：

```text
Warp / Thread Group
Thread 0  ─┐
Thread 1   │
Thread 2   ├── PC / Instruction
Thread 3   │
...        │
Thread N  ─┘

           ↓ Decode once

          warp-uOP
             │
       active lane mask
             │
       ┌─────┼─────┐
       ▼     ▼     ▼
      lane0 lane1 laneN
```

这样可以实现：

```text
one instruction → N lanes
```

同时 backend 允许：

```text
warp-uOP A
warp-uOP B
scalar-uOP C
scalar-uOP D
```

并发存在。

也就是你所说的：

> SIMT + multi-instruction concurrent execution

但这里有一个非常大的设计决定：

### 不要一开始让每个 SIMT lane 都成为独立 OoO thread

假设 8 lanes × 4 warps，每个 lane 都拥有：

```text
Rename Map
ROB
IQ state
Load Queue
Store Queue
speculation state
```

硬件复杂度会非常快失控。

更现实的第一版是：

```text
Warp = scheduling unit
Lane = execution unit
```

即：

```text
1 warp instruction
→ 1 warp-uOP
→ 1 ROB identity
→ N lane operations
→ lane completion mask
→ warp-uOP completion
```

然后不同 warp 之间可以：

```text
Warp 0 instruction A
Warp 1 instruction B
Warp 2 instruction C
```

动态 interleave。

也就是 GPU 风格的 TLP 与 CPU 风格的 resource scheduling 结合。

更高级版本才考虑：

```text
within-warp OoO
+
multiple warp OoO
```

否则 branch divergence、exception、memory ordering、lane completion tracking 与 replay 会同时涌入设计。

### 最好的统一抽象可能是 Context Group

我建议 architectural context 做三级：

```text
Core Fabric
   │
   ├── Context 0
   │      ├── Thread / Lane 0
   │      ├── Thread / Lane 1
   │      └── ...
   │
   ├── Context 1
   └── Context N
```

其中 Context 可以配置为：

```text
Scalar context:
    lanes = 1

SIMT context:
    lanes = 4 / 8 / 16

SMT:
    multiple scalar contexts simultaneously

Hybrid:
    one SIMT context + scalar contexts
```

那么物理 execution array 可以保持不变。

这实际上非常契合你的动态 scaling 思想：

```text
4 ALUs

single-thread mode:
Thread A gets 4 ALUs → maximize ILP

SMT mode:
Thread A gets 2
Thread B gets 2

SIMT mode:
Warp A gets 4 lanes

mixed:
Thread A gets 2
Warp B gets 2
```

这才是我认为“dynamic scaling”最有潜力的定义。

## Unified Memory 与 Unified Write Back 应该怎样实现

### Unified Memory 应统一“资源调度”，而不是统一“内存顺序状态”

这一点非常重要。

你可以有：

> Unified Memory Execution Fabric

但是不应该轻易做：

> 一个没有明确 load/store ordering structure 的万能 memory queue。

XiangShan 明确指出 Load/Store 因为存在 ordering、forwarding、violation detection 等复杂控制，必须保留 LoadQueue 和 StoreQueue；scalar memory instruction 分配相应 LSQ entry，而 vector memory instruction 会拆成多个 uOP/element，并携带对应的 LSQ identity。citeturn20view4

BOOM 也采取类似思路：Load Queue 和 Store Queue 保存 memory ordering information；store 在逻辑上可以分为 store-address 与 store-data 两部分，两者根据 operand readiness 分别推进，但仍属于同一 store-ordering state。citeturn20view7

因此，我推荐：

```text
                   Unified Memory Scheduler
                           │
          ┌────────────────┼─────────────────┐
          │                │                 │
       Context 0        Context 1         Context N
        LSQ               LSQ               LSQ
          │                │                 │
          └───────────────┬┴─────────────────┘
                          ▼
                     AGU allocator
                    ↙     ↓      ↘
                  AGU0   AGU1    AGU2
                     \     |     /
                      Translation
                    TLB / PTW ports
                          │
                 Memory Dependency Engine
                          │
               Load / Store Port Scheduler
                     ↙          ↘
                  D$ Port0     D$ Port1
                        │
               Miss / Replay System
```

其中：

```text
LSQ = correctness/order domain
Memory Scheduler = physical-resource domain
```

这是一个很强的分层。

你可以动态 scale：

```text
Thread A:
    1 load pipe

Thread B idle:
    Thread A:
    3 load pipes
```

但是程序顺序仍由 Thread A 的 LSQ 管。

类似地：

```text
Warp 0:
    lane accesses
      ↓
coalescer
      ↓
memory uOPs
      ↓
shared AGUs/cache ports
```

SIMT memory 可以在 Unified Memory 层增加 coalescer，而无需改变整个 OoO architecture。

### XiangShan 很适合观察“为什么 memory 不容易完全统一”

Kunminghu V2R2 当前文档描述了：

```text
3 × Load pipelines
2 × Store-address pipelines
2 × Store-data pipelines
```

并具有 72-entry Load Queue、56-entry Store Queue、多种 replay/violation structures，以及独立 TLB/cache machinery。citeturn20view4

Load pipeline 本身还需要：

```text
address calculation
TLB lookup
DCache directory
arbitration
forwarding
RAW/RAR detection
MSHR interaction
replay
writeback
```

因此“Unified Memory”真正值得统一的不是这些全部变成一个 combinational scheduler，而是：

> **将所有 memory execution capacity 暴露为可分配 resource，并让 ordering/replay engine 与 physical pipeline 解耦。**

这是可扩展的。

### Unified Write Back 很有价值，但要把 Completion 与 Writeback 分开

这是我认为你的第二个很强的设计点。

传统直觉通常是：

```text
FU done
 ↓
write PRF
 ↓
wake dependent
 ↓
ROB complete
```

实际上这几个动作不必完全绑定。

建议定义一个 **Completion Packet**：

```text
CompletionPacket {
    ContextID
    ROBTag
    UopID

    DstTag
    DstType
    Value

    Exception
    Replay

    LaneMask

    BranchInfo
    MemoryInfo
}
```

所有 FU：

```text
ALU
MUL
DIV
FPU
Vector
LSU
Accelerator
```

统一输出 CompletionPacket。

然后：

```text
                           Completion Fabric
                                  │
        ┌─────────────────────────┼─────────────────────────┐
        ▼                         ▼                         ▼
   Wakeup Network            PRF Write             ROB Completion
        │                         │                         │
 dependent uOPs          bank/port arbitration       done/error bits
```

这比传统意义上的“Unified Write Back Bus”更好，因为你不必要求：

```text
completion == immediately write RF
```

可以变成：

```text
FU complete
   ↓
Result Queue
   ↓
WB credit available?
   ├─ no → hold
   └─ yes
       ↓
     route
       ↓
     PRF bank
```

XiangShan 目前已经有独立 `WbDataPath`，从 integer、FP、vector、memory execution unit 接收结果，并通过 `RealWBCollideChecker` 做 writeback arbitration；Issue Queue 还会提前检测某些 writeback conflicts。citeturn20view1turn20view2

这正说明 writeback 本身就是资源调度问题。

我的建议是比 XiangShan 再向前一步：

```text
Issue time:
reserve or predict WB resource

           ↓

Execute

           ↓

Result Queue

           ↓

Dynamic WB router
```

对于 fixed-latency operation：

```text
ADD latency = 1
MUL latency = 3
```

可以在 issue 时预订：

```text
WB_slot[cycle + latency]
```

对于 variable-latency：

```text
DIV
load miss
cache replay
accelerator
```

不要预订确定 cycle，而是进入：

```text
elastic completion queue
```

由 credit-based arbitration 排出。

这样才能避免一种致命错误：

```text
ALU0 result ─┐
ALU1 result ─┼→ one WB port
MUL result  ─┘

three complete simultaneously
             ↓
        two results lost
```

所有 producer 必须具备至少一种机制：

```text
backpressure
result buffering
reserved WB slot
or replay
```

不能默认“写回一定有空位”。

### Unified Writeback 不等于 Global CDB

我不推荐整个 core 采用：

```text
every FU
   ↓
giant crossbar
   ↓
every PRF bank + every IQ
```

规模大时它会非常昂贵。

建议层次化：

```text
Cluster 0 Completion
     ↓
local bypass
     ↓
local PRF
     │
     └── only non-local results
              ↓
        Global Result Fabric
              ↓
          other cluster
```

因此最常见路径是：

```text
producer C0
→ consumer C0
```

只有跨 cluster dependency 才是：

```text
C0 → global network → C2
```

这与 TRIPS 等 distributed execution 研究所暴露出的 wire/scalability 问题方向一致，同时也符合 XiangShan 现有 bypass connectivity 已经需要严格限制 producer/sink 关系的现实。citeturn7search3turn20view3

## Core Aggregation 最好理解为资源聚合，而不是完整 Core Fusion

你的另一个目标是：

> let core aggregate together, scalable.

这一部分有一个历史上非常直接的相关研究：**Core Fusion**。其基本思想就是让多个较小 core 在需要时组合资源，为单线程提供更大的执行能力，而在其他情况下保持并行多线程吞吐；TRIPS 也研究过 replicated tiles 上的聚合式、分布式执行。citeturn8search0turn7search7turn7search12

但我不建议第一版实现真正的：

```text
Core 0 full OoO state
+
Core 1 full OoO state
        ↓
 runtime merge
        ↓
one giant OoO core
```

因为真正困难的并不是：

```text
2 ALU + 2 ALU = 4 ALU
```

而是：

```text
Rename map?
Free list?
ROB?
Branch snapshots?
LSQ?
TLB?
PRF ownership?
Physical tags?
Interrupt state?
precise exception?
commit?
```

要怎么合并。

一个更漂亮的方案是从一开始就不存在那么坚硬的“backend core boundary”。

### 把 Core 拆成 Context Frontend 与 Backend Resource Tiles

例如：

```text
Frontend / Context A ─┐
Frontend / Context B ─┼── Global / Hierarchical Allocator
Frontend / Context C ─┘                │
                                      │
                   ┌──────────────────┼──────────────────┐
                   ▼                  ▼                  ▼
                 Tile 0             Tile 1             Tile 2
              ALU/BR/MUL         ALU/LSU           ALU/FPU/VEC
```

那么：

```text
Mode: throughput

Context A → Tile 0
Context B → Tile 1
Context C → Tile 2
```

可以变成：

```text
Mode: single-thread performance

Context A → Tile 0 + Tile 1 + Tile 2
```

或者：

```text
Mode: mixed

Context A → Tile 0 + Tile 1
Context B → Tile 2
```

这里你不是“融合两个完整 core”，而是：

> **动态改变 context 对 backend execution tiles 的 ownership。**

这个设计在实现上容易很多。

可以定义：

```text
Resource Allocation Table

Context   ClusterMask   LSUQuota   WBQuota   IQQuota
-----------------------------------------------------
C0        0011          2          4         24
C1        0100          1          2         12
C2        1000          1          2         12
```

然后每隔一个较长 epoch：

```text
100 cycles
1000 cycles
branch phase boundary
OS hint
hardware utilization threshold
```

重新配置资源。

### 不应该每周期“无限动态缩放”

这里有一个重要的 engineering distinction。

你可以让：

```text
instruction → FU selection
```

每周期动态。

但是：

```text
Context A owns Cluster 2?
```

不一定需要每周期变化。

建议分为三种时间尺度：

| 时间尺度 | 动态项目 |
|---|---|
| cycle-level | uOP issue、FU、read/write port、route |
| tens/hundreds cycles | IQ quota、execution cluster allocation |
| phase-level | core/context fusion、SIMT width、power gating |

这样可以大幅降低 arbitration complexity。

如果所有东西每周期都自由重构：

```text
thread
IQ
PRF
FU
LSU
cache
writeback
route
```

那么 scheduler 自己会变成最复杂、最慢、最耗电的单元。

真正可扩展的设计通常应该是：

> **fast local decisions + slow global decisions。**

### Resource Fabric 需要 credit-based flow control

我认为这是你的 architecture specification 中应该非常明确的一点。

每个物理 resource 暴露 credits：

```text
ClusterCredit
IQCredit
OperandReadCredit
ExecutionCredit
MemCredit
WBCredit
RouteCredit
```

例如：

```text
uOP candidate
     │
     ├─ operands ready?
     │
     ├─ compatible FU?
     │
     ├─ input route credit?
     │
     ├─ output credit?
     │
     └─ result buffer credit?
             │
             ▼
            issue
```

这使得整个 backend 可以用 ready/valid 风格构成 elastic pipeline，而不是隐含假设：

```text
next stage must accept
```

对 FPGA 尤其有帮助。

## FPGA 验证路线与建议的第一版参数

我认为**FPGA-friendly 应该成为架构约束，而不是 RTL 做完以后才考虑的验证方式**。

Vortex 是很好的例子：其项目同时维护软件 simulator、RTL simulator 和实际 Xilinx/Altera FPGA backend，硬件的 core/warp/thread/FU/issue width 都参数化，并已经在 FPGA 上实现过多 core RISC-V GPU。citeturn17view0 PULP-C910 也已经展示 superscalar OoO RISC-V 在 FPGA 上启动 Linux 的完整系统路径。citeturn15view1 FireSim 则允许 Chisel/Verilog RTL 通过 FPGA-accelerated simulation 以远高于传统 RTL 仿真的速度进行大规模 workload 验证。citeturn13search3turn13search15 XiangShan 自身也提供 Verilator/difftest 工作流。citeturn15view0

但 FPGA 会特别惩罚四种结构：

```text
large CAM
many-port RAM
wide global crossbar
long combinational wakeup/select
```

而恰恰这四个结构都是现代宽 OoO CPU 最喜欢产生的东西。

所以不要从“XiangShan-sized core”开始。

我建议第一版大约是：

| 项目 | 建议 Prototype |
|---|---:|
| ISA | RV64IMAC，之后再加 F/D/V |
| Fetch width | 4 |
| Decode width | 2 |
| Rename width | 2 |
| Dispatch width | 2–4 |
| ROB | 48–64 |
| Architectural contexts | 1 |
| Physical execution clusters | 2 |
| Integer ALU | 每 cluster 1–2 |
| MUL/DIV | 1 shared |
| Branch unit | 1 |
| Load pipes | 1–2 |
| Store AGU | 1 |
| PRF | 2–4 banks |
| IQ | 每 cluster 8–16 |
| Result queue | 每 cluster 2–4 |
| Inter-cluster latency | 固定 1 cycle |
| L1 D$ | 简单 blocking/少量 MSHR 起步 |
| SIMT | 第一版关闭 |

最关键的是**先证明 Resource Fabric**，而不是先证明“大 CPU”。

一个合理的开发演化是：

| 原型 | 主要目标 | 应证明什么 |
|---|---|---|
| Scalar baseline | 普通 OoO | Rename/ROB/LSQ/precise state 正确 |
| Fabric prototype | 两个 execution clusters | uOP 可以任意 route 且结果正确 |
| Elastic WB | dynamic completion/writeback | 任何 WB contention 都不丢结果 |
| Dynamic ports | 减少 PRF ports | structural hazard 可 stall/replay |
| Multi-context | 两个 architectural contexts | backend resource partition/share |
| Resource aggregation | 一个 context 使用多个 clusters | 单线程可扩 execution width |
| SIMT mode | warp/lane execution | 同一 fabric 承载 thread group |
| Hybrid mode | ILP + TLP + SIMT | 动态策略优于固定 partition |

这个顺序非常重要。

**SIMT 不应该在最早阶段加入。**

否则当结果错误时，你无法快速判断是：

```text
rename bug
ROB bug
routing bug
lane-mask bug
reconvergence bug
memory-coalescing bug
writeback bug
```

### RTL 数据结构应该围绕 FPGA 做修改

尤其不要直接实现传统 fully-associative 巨型 IQ：

```text
64 entries
× N source tags
× every wakeup bus
```

更建议：

```text
small local IQ × clusters
```

例如：

```text
4 × 12-entry local IQ
```

通常会比：

```text
1 × 48-entry global IQ
```

更适合你的分布式思想，也更容易 place-and-route。

同样，PRF 不应该想成：

```text
one 256 × 64
12R + 8W memory
```

而应该想成：

```text
Bank 0
Bank 1
Bank 2
Bank 3
```

由 rename 时进行 bank-aware allocation：

```text
pdest allocation
      ↓
consider:
  occupancy
  expected producers
  expected writeback conflicts
      ↓
choose physical register bank
```

这样 Rename 本身就能参与后端资源优化。

这可能成为一个很有价值的研究点：

> **Resource-aware Physical Register Allocation**

而不是传统：

```text
choose any free physical register
```

例如：

```text
Instruction A likely executes Cluster 0
    ↓
allocate pdest from PRF bank near Cluster 0
```

如果其主要 consumer 也在 Cluster 0：

```text
local bypass / local RF
```

如果 consumer 被 route 到其他 cluster：

```text
result migration
```

你实际上就把 compiler-style register allocation、NoC placement 和 OoO rename 的思想结合起来了。

### 验证不能只靠程序跑通

你的架构高度动态，随机 workload 很容易“几亿周期没碰到那个 arbitration corner case”。

建议建立几个硬 invariant：

```text
Every allocated physical register:
    exactly one owner or free

No physical register:
    freed before its old architectural mapping retires

Every dispatched uOP:
    eventually completes OR is explicitly flushed

No completion packet:
    disappears under backpressure

No destination:
    receives two incompatible writes

ROB:
    commits architectural instructions strictly in order

Wrong-path uOP:
    never modifies committed architectural state

Memory:
    never violates required ordering without replay

Resource credits:
    never negative
    never spontaneously increase
```

对于你的 Unified Write Back，尤其应该做：

```text
assert(
    accepted_completion_count
    ==
    committed_or_flushed_completion_count
    + currently_buffered_completion_count
)
```

也就是一种 **packet conservation property**。

这会比单纯看 waveform 强很多。

XiangShan 的 difftest/co-simulation 工作流值得直接参考，因为对于 OoO 核心，最有效的体系之一就是将每个 architectural commit 与 reference model 对比，而不是要求内部执行顺序与 reference model 一致。citeturn15view3

## 我认为最值得做成论文或真实架构的版本

把所有调研放在一起后，我不会建议你把项目描述成：

> “A dynamically scalable RISC-V OoO CPU.”

这个范围太宽，而且很多单点思想都有历史先例：OoO/uOP/PRF 是成熟技术，BOOM 已经讨论动态 RF port scheduling；TRIPS 已经做过 distributed execution；Core Fusion 已经研究 core resource aggregation；Vortex/Ventus 已经研究 RISC-V SIMT/GPGPU。citeturn15view2turn7search3turn8search0turn17view0turn17view1

更强的定义是：

> **A hierarchical, dynamically allocatable execution fabric for RISC-V that unifies ILP, TLP and SIMT resource allocation while preserving conventional precise OoO architectural semantics.**

中文可以理解为：

> **“保持传统 RISC-V 精确乱序语义，但将后端执行单元、寄存器端口、memory pipelines 和 completion bandwidth 虚拟化为可动态分配资源的层次化执行织构。”**

这其中有几个非常明确的研究贡献候选。

### 最有潜力的是“资源虚拟化”

传统核心：

```text
Issue Port 0 → ALU 0
Issue Port 1 → ALU 1
Issue Port 2 → LSU
Issue Port 3 → FPU
```

你的模型：

```text
uOP
 ↓
capability requirement
 ↓
Resource Scheduler
 ↓
any compatible execution resource
```

换言之，从：

```text
port-bound backend
```

转向：

```text
capability-bound backend
```

uOP 不需要知道“我要 Port 2”，只需要声明：

```text
requires:
    integer-add
    2 operands
    1 result
    latency-class fast
```

scheduler 决定：

```text
where
when
route
writeback
```

这个 abstraction 非常好。

### 第二个强点是“Execution 与 Completion 解耦”

可以正式定义：

```text
Dispatch
  ↓
Dependency Scheduling
  ↓
Resource Reservation
  ↓
Operand Transport
  ↓
Execution
  ↓
Completion Queue
  ↓
Result Transport
  ↓
Architectural Completion
```

而不是传统：

```text
Issue → Execute → Writeback
```

Unified Writeback 因此不只是“多个 FU 共用 bus”，而是一个：

> **Completion Network / Completion Fabric**

它拥有：

```text
buffering
routing
credit
priority
QoS
bank matching
wakeup
replay
```

这很适合动态 backend。

### 第三个强点是“Core 只是逻辑边界”

最终可以得到这样的系统：

```text
                        RISC-V Fabric
                            │
          ┌─────────────────┼─────────────────┐
          │                 │                 │
       Context A         Context B         Context C
          │                 │                 │
          └──────┬──────────┴───────┬─────────┘
                 ▼                  ▼
           Scheduler Domain    Scheduler Domain
                 │                  │
       ┌─────────┼────────┐         │
       ▼         ▼        ▼         ▼
     Tile0     Tile1     Tile2     Tile3
```

当 workload 是：

```text
1 heavy scalar thread
```

变成：

```text
A → Tile0+1+2+3
```

当 workload 是：

```text
4 independent threads
```

变成：

```text
A → Tile0
B → Tile1
C → Tile2
D → Tile3
```

当 workload 是：

```text
SIMT
```

变成：

```text
Warp A → Tile0+1+2+3 as lanes
```

而当 workload 混合：

```text
latency thread + throughput kernel
```

可以变成：

```text
Scalar A → Tile0+1
SIMT B   → Tile2+3
```

这就把 ILP/TLP/SIMT 从三个几乎独立的 microarchitecture world，转化成同一组 backend resources 的不同 allocation policy。

我认为这比“动态增加 execution units”要深得多。

### 但第一代实现应刻意限制动态性

最终设计可以非常动态，但第一版硬件建议只有：

```text
2 clusters
1 scalar architectural context
dynamic per-uOP cluster routing
banked PRF
dynamic WB
unified memory-port scheduler
```

首先证明：

```text
Fixed Backend:
IPC = X
area = A

Dynamic Fabric:
IPC ≈ X or > X
area < A
or
same area → IPC > X
```

然后第二版增加：

```text
2 contexts
dynamic cluster allocation
```

证明：

```text
single thread:
both clusters aggregate

two threads:
one cluster each

adaptive:
better geometric mean / throughput / QoS
```

最后才加入 SIMT。

这是因为你的核心 hypothesis 应该先能在最小系统中被证伪：

> **通过将 backend 资源从固定 execution-port topology 解耦，并使用 hierarchy + credit + routing 进行动态配置，是否能够以可接受的调度开销换取更好的 utilization / scalability / FPGA efficiency？**

如果这个 hypothesis 成立，SIMT、vector、accelerator、core aggregation 都会自然成为这个 fabric 的扩展。

如果它不成立，你也能很早知道瓶颈到底在：

```text
scheduler delay
routing delay
PRF banking
writeback bandwidth
inter-cluster communication
```

而不是等完整 GPU/CPU hybrid 做完以后才发现问题。

综合 XiangShan、BOOM、OpenC910、TRIPS、Core Fusion 和 Vortex 的经验，我对这个方向的总体评价是：**有价值，而且非常适合作为研究型 RISC-V core；但要把创新核心放在“hierarchical resource virtualization / allocation”上，而不是“更宽的 OoO”或“所有东西统一”上。** XiangShan 已经很好地展示了大型集中式高性能后端随着规模扩大产生的 rename ports、IQ、LSU、bypass、writeback complexity；BOOM 明确指出动态端口调度的机会与代价；TRIPS/Core Fusion 给出了分布式执行与聚合资源的历史依据；Vortex 则证明了 RISC-V SIMT 与 FPGA scalability 可以真实落地。citeturn19view0turn20view1turn20view3turn15view2turn7search3turn8search0turn17view0

最值得坚持的设计原则可以压缩成一句话：

> **Architectural state remains ordered and precise; everything below it becomes elastic, packetized, routable, and dynamically allocatable.**

这会比单纯设计“另一个高性能 RISC-V OoO core”更有辨识度，也更符合你“core 可聚合、资源动态伸缩、SIMT + concurrent multi-inst、同时可以在 FPGA 上验证”的原始目标。