# 面向 RISC-V / RVV 的动态聚合处理器架构研究：从高 IPC CPU 到 GPU-like SIMT 的弹性执行 Fabric

## 研究结论与架构定位

你的想法是有实际研究价值的，但我认为需要先做一个很重要的“重新定义”：

> **最值得做的并不是让每一条 uOP 在一个完全任意、完全可重构的流水线图中自由游走，而是把传统固定 OoO Backend 重构成一个“分层、弹性、可聚合的执行资源 Fabric”。**

也就是说，仍然保留 RISC-V 软件所需要的严格 architectural semantics、精确异常、每个 hart 的 architectural state 和 commit order，但在 **Rename 之后、Commit 之前**，将传统的固定 Issue Queue → Fixed Execution Ports → Fixed LSU → Fixed Writeback 拆成大量可以动态租用、组合和重新分区的执行组件。

我建议把这个体系暂时命名为：

> **EAF-V — Elastic Aggregate Fabric for RISC-V Vector**

它不是传统意义上的“大核”，也不是简单的 RISC-V GPU，而是一个具有连续工作点的架构：

**Latency-oriented CPU ←→ Wide OoO CPU ←→ RVV Vector Machine ←→ Throughput Machine ←→ SIMT-like Engine**

其核心不是动态改变 ISA，而是动态改变**同一套物理资源如何被 architectural threads / uOP / vector elements 使用**。

这个方向和现有几个研究结果非常吻合，但又并没有被它们直接做过：

- XiangShan 代表的是非常重要的高性能 RISC-V OoO 基线，而且当前官方仓库仍将其定位为开源高性能 RISC-V processor；截至 2026 年 6 月，Kunminghu V2 是官方建议用于研究和下游工作的稳定基线，而 V3 仍快速演进。XiangShan 已有 Chisel、RTL 生成、Verilator、NEMU/difftest 等成熟开发验证路径，非常适合借鉴其“高性能 CPU 的正确性边界”和工程方法。citeturn16view0
- Ara/Ara2 已证明“lane 化 vector backend”具有很高的可扩展性；Ara2 的实现覆盖 2–16 lanes，并在计算密集型 kernel 上报告约 95% functional-unit utilization。更关键的是，Ara2 发现，**相同总 FPU 数下，多组较小 vector core 可以明显优于一个非常宽的 vector core**：8 个 2-lane Ara2 对 32×32×32 matrix multiplication 比一个 16-lane Ara2 超过 3× 性能，同时能效提高约 1.5×。这正好支持“资源应该能够拆分与重新组合，而不应该永远锁死成一个超宽向量单元”的思路。citeturn16view1
- 2026 年的 SEAM-V 更直接证明，RVV backend 不应该机械地依赖 scalar core 一条条喂 vector instruction。它通过 task-level decoupling、local instruction supply、execute packet 和更动态的 backend scheduling，在 Ara-based baseline 上报告了 1.38× geometric-mean speedup。citeturn17view0
- Ara-Opt 的最新研究则发现，即使**完全不增加 raw memory bandwidth，也不改变主要计算资源数量**，仅改善 memory-side transaction progress、dependence/issue control、operand delivery 和 result propagation，就能得到 1.33× geometric-mean speedup。这意味着你所说的“pipeline organization 本身是否足够高效”确实是一个很大的性能空间，而不只是“加 ALU、加 cache”。citeturn17view1
- MemPool 和最新的 TeraPool 则说明 tightly coupled shared-memory execution fabric 确实可以扩展得很大，但同时非常清楚地说明了一个边界：**不能用一个巨型 flat crossbar 去连接所有计算组件和所有 memory banks**。MemPool 将共享 L1 扩展到 256 个 RISC-V PEs；TeraPool 进一步做到超过 1000 个 PE、超过 4000 个 shared-L1 banks，但依靠的是 hierarchical interconnect，因为 fully-connected PE-to-L1 crossbar 的复杂度会随规模迅速失控。citeturn17view3turn17view2

所以我对这个项目的总体判断是：

| 设计想法 | 潜力 | 我的判断 |
|---|---:|---|
| Decode 后统一转 uOP | 很高 | 应作为基本抽象 |
| 完全任意动态 pipeline routing | 较低 | 太动态会伤害 latency / frequency |
| Cluster / slice 级动态聚合 | **非常高** | 架构核心 |
| Dynamic execution-resource leasing | **非常高** | 值得重点研究 |
| RVV lane 动态扩缩 | **非常高** | 与 RVV 天然契合 |
| CPU 与 GPU-like backend 共用 datapath | 高 | 可行 |
| arbitrary single-thread 自动变成 GPU | 很低 | 不存在足够 parallelism 时不可能 |
| 多 hart 自动 cohort / warp fusion | **高，但困难** | 很有研究价值 |
| 每 lane 完整 coherent L1 | 较低 | coherence / duplication 成本太高 |
| 每 lane L0 / Locality Buffer | **非常高** | 强烈建议 |
| Shared banked L1 + lane-local buffer + coalescer | **非常高** | 推荐 memory architecture |
| 全局 unified writeback bus | 低 | 应该是逻辑统一、物理分布 |
| Hierarchical completion fabric | **高** | 推荐 |
| 跨多个 core 聚合一个巨大 ROB | 低 | 线延迟和依赖网络会吞掉收益 |
| pod 内执行资源聚合 | **非常高** | 推荐 |

这实际上把你的问题从“如何造一条更动态的 pipeline”提升成：

> **能否做一台 architectural state 是传统 RISC-V，但 microarchitectural execution substrate 是 elastic dataflow / vector / SIMT fabric 的机器？**

我的结论是：**可以，而且比把 CPU 和 GPU 做成两个割裂的 accelerator 更值得探索；但成功的关键是 hierarchical elasticity，而不是 absolute dynamism。**

## 从 XiangShan 与现有 RISC-V 设计推导新的 Pipeline

### 不要把 Frontend 也完全 Fabric 化

传统高性能 CPU 的：

```text
Fetch
 ↓
Branch Prediction
 ↓
Predecode
 ↓
Decode
 ↓
Rename
 ↓
Dispatch
 ↓
Issue
 ↓
Execute
 ↓
Writeback
 ↓
Commit
```

并不是所有部分都值得动态化。

例如 Fetch → Predict → Decode → Rename 的主要问题是 **latency**，而不是资源利用率。

因此你的架构中，我建议明确划出两个世界：

```text
          Ordered / latency-sensitive world
 ┌────────────────────────────────────────────┐
 │ Fetch → Predict → Decode → Rename/Allocate │
 └───────────────────┬────────────────────────┘
                     │
                     │ Tagged uOP Packet
                     ▼
          Elastic / dynamic execution world
 ┌────────────────────────────────────────────┐
 │                                            │
 │         Aggregate Execution Fabric         │
 │                                            │
 │ Scalar    FP     AGU      RVV     SIMT     │
 │ Slices   Slices  Slices   Lanes   Lanes    │
 │                                            │
 │             Memory Fabric                  │
 │                                            │
 └───────────────────┬────────────────────────┘
                     │
                  completion
                     ▼
 ┌────────────────────────────────────────────┐
 │      Ordered Commit / Precise State        │
 └────────────────────────────────────────────┘
```

这里有一个重要原则：

> **architectural order 固定，execution topology 动态。**

这是我认为你的架构能够兼容 Linux、现有 compiler、普通 RISC-V binary，同时又能够做得非常 unconventional 的关键。

XiangShan 非常适合作为前半部分和 architectural correctness 的参考，因为它本身就是一个持续开发中的高性能 RISC-V 项目，并且已有独立 cache subsystem、difftest infrastructure、Verilog 生成和 Verilator simulation flow。citeturn16view0

但我不建议直接把 XiangShan backend 做“小改”。

你的研究价值恰恰来自：

> **在 Rename/Allocate 之后，彻底更换 execution model。**

### uOP 不应该只是传统 CPU 的 micro-op

我建议 uOP packet 比传统 OoO CPU 的内部 uOP 更“自描述”。

概念上可以是：

```text
Dynamic-uOP
{
    Context:
        HartID
        ThreadID
        SequenceID
        Epoch
        BranchMask

    Dependency:
        SrcPhysTag[]
        DstPhysTag
        ReadyMask

    Operation:
        OpClass
        SubOp
        RequiredCapability

    Memory:
        IsLoad/Store
        MemOrderClass
        LocalityHint
        PredictedRegion
        SpeculationState

    Vector:
        IsVector
        VL
        SEW
        LMUL
        MaskState
        ElementGroup

    Routing:
        PreferredCluster
        LocalityAffinity
        LatencyClass

    Commit:
        ROBTag
        ExceptionState
}
```

这并不意味着全部字段都真的需要物理存储；大量 metadata 可以 implicit encoding 或存在 descriptor table 里。

重点是：

> uOP 不再代表“去 ALU2 port 执行”，而代表“我需要满足这些 capabilities 和 dependency”。

传统设计：

```text
uOP → predetermined port set → FU
```

你的设计：

```text
uOP
 ↓
Capability + dependency + locality matching
 ↓
currently available execution slice
```

因此：

```text
ADD
```

不是：

```text
IQ0 → ALU0/ALU1
```

而是：

```text
ADD token
   ↓
Resource Broker
   ├─ Scalar Slice 0
   ├─ Scalar Slice 2
   ├─ Vector Lane 1 configured scalar
   └─ SIMT ALU Lane Group 3
```

当然，最后一个选择是否允许，取决于 mode、operand locality 和 predicted execution cost。

### 关键不是一个 Global Scheduler，而是 Scheduler Hierarchy

这里是整个架构能否成功的一个关键点。

一个直觉实现可能是：

```text
              Giant Global Queue
                 / / | \ \
              every execution unit
```

我强烈不建议。

原因和 TeraPool 遇到的 shared-memory interconnect scaling problem 是同一种物理现实：随着 endpoint 数量增加，flat connectivity 很快会导致 wiring、arbitration 和 timing 失控；TeraPool 因此采用 hierarchical interconnect，而不是完全连接所有 PE 与 L1 bank。citeturn17view2

更合理的是：

```text
                  Global Resource Manager
                           │
           ┌───────────────┼────────────────┐
           ▼               ▼                ▼
        Cluster A        Cluster B        Cluster C
       Local IQ          Local IQ         Local IQ
      /  /   \  \       /  /  \  \       /  |  \
    ALU FP AGU VEC     ALU FP AGU VEC    VEC MEM ALU
```

Global 层不做 cycle-by-cycle wakeup/select。

它主要决定：

```text
Hart 0 → Cluster A+B
Hart 1 → Cluster C
Vector job → lanes 0...7
Throughput job → four independent lane groups
```

Local scheduler 才做真正的：

```text
ready?
dependency satisfied?
FU free?
operand nearby?
issue now?
```

这样才有机会保持高 clock。

### Dynamic scaling 必须有两个时间尺度

这一点非常重要。

不能每个 cycle 都问：

> “现在要不要把 ALU3 从 CPU0 转给 CPU1？”

那会造成巨大的 scheduling overhead。

我建议：

**Fine-grain dynamic scheduling**

发生在每个 uOP：

```text
uOP → one available resource inside allocated cluster
```

**Coarse-grain resource reconfiguration**

发生在 execution epoch：

```text
Cluster lease:
CPU0 : 4 scalar slices + 2 AGU + 8 vector lanes

                 ↓ workload changes

CPU0 : 2 scalar slices + 1 AGU
CPU1 : 2 scalar slices + 1 AGU
RVV  : 4 lanes
SIMT : 4 lanes
```

也就是说：

> **resource partition 是慢动态；uOP routing 是快动态。**

这可以极大降低控制复杂度。

### Pipeline depth 因此自然变成 Variable，而不是硬做 Variable Pipeline

你的“variable depth pipeline”不应该通过：

```text
Stage1 → optionally Stage2 → optionally Stage3...
```

来实现。

而应该自然产生于 route：

```text
Local ALU result:
Dispatch → ALU → Local Forward
≈ short route

Remote FP:
Dispatch → Router → FP Cluster → Return Router
≈ longer route

Cache hit:
AGU → LLB
≈ short route

Shared L1:
AGU → Memory Fabric → L1 Bank → Return
≈ medium route

L2:
AGU → Memory Fabric → L1 → L2 → Return
≈ long route
```

于是**物理 pipeline latency 本身就是 task-dependent 和 route-dependent 的**。

这比人为设计一个“长度可变 pipeline”更自然。

我会称它为：

> **Elastic Latency Execution**

每个 dependency 由 tags + completion events 驱动，而不是依赖“第 7 stage 必定出现结果”。

这已经很接近 dataflow machine 的思想，但又保留传统 OoO CPU 的 Rename/ROB/precise exception，所以不会承担传统 dataflow architecture 那么高的软件和 state-management 复杂度。

## RVV 与 CPU—GPU 连续体

这是你的架构最有意思的一部分。

关键是不要把：

- OoO
- SMT
- SIMD/vector
- SIMT

混成同一件事情。

它们利用的是不同 parallelism。

| Parallelism | 你的机器如何获得 |
|---|---|
| ILP | 一个 hart 内多个独立 uOP |
| MLP | 多个 outstanding memory operations |
| DLP | RVV |
| TLP | SMT / 多 hart |
| SIMT-like | 多 thread cohort fusion |
| Core-level parallelism | 多 execution pods |

你真正希望构建的是一台可以动态改变这些 parallelism 比例的机器。

### RVV 应该成为 Fabric 的第一等公民

Ara 的设计已经说明，把 vector architecture 分成相同的 lanes，每 lane 包含部分 vector register file 和 execution resources，是一种天然 scalable 的组织形式。citeturn18academia31

Ara2 又进一步说明“越宽越好”并不成立：短 vector 时，scalar instruction supply 和 vector startup 等问题可能使一个超宽 vector unit 利用率不足，而多个较窄 vector core 可以获得更高性能。citeturn16view1

这正好可以由 EAF-V 解决：

```text
Physical vector resources:

Lane0 Lane1 Lane2 Lane3 Lane4 Lane5 Lane6 Lane7
```

对于一个长 RVV workload：

```text
Hart0
  ↓
One RVV instruction
  ↓
[ Lane0 Lane1 Lane2 Lane3 Lane4 Lane5 Lane6 Lane7 ]
```

对于两个短 vector workload：

```text
Hart0 → [ Lane0 Lane1 Lane2 Lane3 ]

Hart1 → [ Lane4 Lane5 Lane6 Lane7 ]
```

对于四个更短任务：

```text
Hart0 → [ Lane0 Lane1 ]
Hart1 → [ Lane2 Lane3 ]
Hart2 → [ Lane4 Lane5 ]
Hart3 → [ Lane6 Lane7 ]
```

这正是 Ara2 多个小 vector engine 相较一个超宽 engine 的实验结果所提示的方向。citeturn16view1

而如果 vector workload 暂时没有：

```text
Lane ALUs
  ↓
temporarily usable by scalar/SIMT execution
```

这就进一步提高 utilization。

### 不要把一个 RVV instruction 完全展开成数百个 ROB uOP

这会破坏你想提高 in-flight work 的目标。

假设：

```text
vadd.vv
VL = 256
```

最糟的方式：

```text
ROB:
 element0 uOP
 element1 uOP
 element2 uOP
 ...
 element255 uOP
```

这样会：

- ROB 膨胀；
- rename state 膨胀；
- tag traffic 膨胀；
- wakeup/select 压力巨大。

更合理的是：

```text
ROB Entry
   │
   │ Vector Macro-uOP
   ▼
Vector Descriptor
   │
   ├── element packet 0
   ├── element packet 1
   ├── element packet 2
   └── ...
```

即：

```text
Architectural level:
    1 vector instruction

Execution level:
    N element groups / packets
```

例如：

```text
VectorDescriptor
{
    ROB tag
    source vector tags
    destination vector tag
    vl
    sew
    mask
    operation
    completed_segments bitmap
}
```

每个 lane 处理 segment：

```text
Lane0: element 0,8,16...
Lane1: element 1,9,17...
...
```

或者动态 block distribution。

ROB 只需要知道：

```text
Vector instruction complete?
Any exception?
```

而不是跟踪每个 element 为一个 architectural instruction。

SEAM-V 的 execute-packet 思路以及其 local instruction supply 正是非常值得参考的方向：它发现 tightly coupled scalar→vector instruction supply 会限制短 vector、loop tail 和 memory/control mixed workloads，而 packetization/decoupling 可以明显改善利用率。citeturn17view0

### RVV 和 SIMT 不能简单画等号

这是这里最大的 architectural distinction。

RVV：

```text
1 architectural thread
1 PC
1 vector instruction
N vector elements
```

SIMT：

```text
N architectural threads
N thread states
usually logically independent PCs
execute same instruction in a cohort/warp when converged
```

真正的 GPU SIMT 还需要：

- thread context；
- active mask；
- divergence；
- reconvergence；
- warp/cohort scheduling；
- per-thread memory state。

Vortex 是非常好的 RISC-V 参考：它本身就是 RISC-V GPGPU，但为了 OpenCL/SIMT execution，它引入了最小的 RISC-V ISA extensions，并修改了对应 runtime。换句话说，真正把 RISC-V 变成 conventional GPU programming model 并不是“仅靠更宽的 ALU”就能完成。citeturn17view4

因此我建议你的架构引入一个非常有意思的新层：

> **Opportunistic SIMT / Dynamic Cohort Fusion**

### Dynamic Cohort Fusion

假设 hardware 有多个标准 RISC-V harts：

```text
Hart0 PC=0x1000: add x5,x6,x7
Hart1 PC=0x1000: add x5,x6,x7
Hart2 PC=0x1000: add x5,x6,x7
Hart3 PC=0x1000: add x5,x6,x7
```

传统 CPU：

```text
4 independently decoded ADDs
→ four scalar uOPs
→ issue independently
```

你的机器可以检测：

```text
same PC / same instruction
same opcode
compatible exception state
compatible control state
```

然后内部融合：

```text
          ADD
           │
  ┌────────┼────────┐
Hart0    Hart1    Hart2    Hart3
src      src      src      src
  \        |       |       /
      SIMD/SIMT ALU group
```

于是：

```text
4 scalar uOPs

       ↓ cohort fusion

1 physical SIMD operation
4 architectural results
```

从 software 看：

```text
仍然只是 4 个普通 RISC-V threads
```

这是我认为**最接近你“软件什么都不用变，但硬件可以慢慢变成 GPU”目标的方法。**

我会把它称为：

> **Transparent Micro-SIMT**

当 threads divergence：

```text
Hart0 PC 0x1004
Hart1 PC 0x1004
Hart2 PC 0x1200
Hart3 PC 0x1004
```

cohort：

```text
Cohort A = Hart0,Hart1,Hart3
Cohort B = Hart2
```

之后如果重新 convergent：

```text
merge again
```

这是非常值得研究的方向。

### 但是这里存在一个不可消除的极限

硬件不能凭空创造 parallelism。

例如：

```c
a = load();
if (a)
    b = foo(a);
else
    b = bar(a);
return b;
```

只有一个 software thread 时，无论有：

```text
64 lanes
128 ALUs
1000 execution units
```

都不可能自动获得 GPU 的 1000-thread throughput。

所以：

> “同一个 arbitrary single-thread binary 在 extreme mode 自动变成 GPU workload”

是不现实的。

但以下情况可以：

```text
Linux/application
    │
    ├── multiple normal threads
    │
    ├── OpenMP threads
    │
    ├── pthread workers
    │
    ├── language runtime tasks
    │
    └── RVV
            ↓
   hardware dynamically finds parallelism
```

因此真正目标应该定义为：

> **同一个 ISA 和 execution substrate 可以连续覆盖 latency-oriented scalar、ILP、SMT、RVV、transparent cohort-SIMT，而不强制软件使用一种 GPU-specific ISA。**

对于真正 GPU kernel，如果未来愿意提供 optional extension：

```text
standard RISC-V
    +
optional task/spawn/cohort hints
```

则可以进一步接近 Vortex/OpenCL 类型的 GPU，但这不应该是第一代必须做的。Vortex 本身也说明，完整 SIMT model 通常需要 ISA/runtime 支持。citeturn17view4

### 我建议定义五个 Execution Personality

同一块 silicon：

```text
                 Same Physical Fabric

   ┌───────── CPU-Latency ─────────┐
   │ few harts + big OoO windows   │
   │ aggressive speculation       │
   │ low routing distance         │
   └───────────────────────────────┘

                  ↕ dynamic

   ┌───────── CPU-Throughput ──────┐
   │ many harts                    │
   │ smaller per-hart resources    │
   │ SMT / cohort fusion           │
   └───────────────────────────────┘

                  ↕ dynamic

   ┌──────────── RVV ──────────────┐
   │ vector macro-uOP              │
   │ variable lane allocation      │
   │ chaining                      │
   └───────────────────────────────┘

                  ↕ dynamic

   ┌──────── Micro-SIMT ───────────┐
   │ converged standard harts      │
   │ cohort formation              │
   │ mask/divergence tracking      │
   └───────────────────────────────┘

                  ↕ optional

   ┌──────── Extreme SIMT ─────────┐
   │ massive contexts             │
   │ warp scheduling              │
   │ coalesced memory             │
   └───────────────────────────────┘
```

最关键的是：

**这些不是五个 processor。**

而是：

```text
same ALUs
same FPUs
same vector lanes
same AGUs
same memory banks
same result network
```

重新分配。

## Memory Fabric 与 Per-Lane Cache 的重新设计

我认为你提出的：

> “不仅访问 L1，还给每 lane 一个 tightly coupled memory/cache”

是整个方案里**非常值得发展**的一点。

但我的建议是：

> **不要直接实现成 N 个 full coherent per-lane L1。**

应该把它发展成：

> **Lane Locality Buffer + Shared Banked L1 + Memory Coalescing Fabric**

原因非常重要。

### 为什么完整 per-lane cache 容易失败

假设：

```text
16 lanes
```

每 lane：

```text
L1D0
L1D1
...
L1D15
```

然后：

```text
Lane0 accesses address A
Lane5 accesses address A
```

如果都是 coherent cache：

```text
Who owns A?
Shared?
Modified?
Dirty?
Need invalidate?
```

于是原本只是简单的 lane-local memory optimization，最后引入：

- N 份 tag array；
- coherence state；
- snoop/directory traffic；
- duplicate cache lines；
- migration cost；
- lane reassignment cost；
- same-line accesses 不容易自然合并；
- vector gather/scatter 可能制造极大量 coherence traffic。

这很容易把收益吃掉。

相反，Spatz、MemPool 和 TeraPool 提供了很好的启发。

Spatz 专门研究了 vector PE + shared-L1 cluster，并将 compact RISC-V vector processing units 集成到 MemPool；其结果显示 vector PEs 在共享 L1 cluster 上具有实际性能和能效优势。citeturn18academia32

MemPool 则把 256 个 RISC-V cores 放在一个 multi-banked shared L1 scratchpad domain 中，在无冲突时最多约五个周期访问，从而证明 tightly coupled memory domain 可以远远超过传统几个 core 的规模。citeturn17view3

TeraPool 更进一步证明了上千 PE、数千 bank 的 shared L1 是可以物理实现的，但必须使用 hierarchical network；它明确指出 fully-connected crossbar complexity 会随着 PE 数快速恶化。citeturn17view2

因此你的 memory hierarchy 最适合长这样：

```text
Lane 0 ─ LLB0 ─┐
Lane 1 ─ LLB1 ─┤
Lane 2 ─ LLB2 ─┤
Lane 3 ─ LLB3 ─┤
               │
          Memory Packetizer
               │
          Line Coalescer
               │
        Bank / Address Router
               │
  ┌────────────┼────────────┐
  ▼            ▼            ▼
 L1 B0        L1 B1        L1 Bn
  └────────────┬────────────┘
               │
              L2
               │
              LLC
               │
             DRAM
```

### Lane Locality Buffer

我建议不要叫 per-lane L1，而叫：

> **LLB — Lane Locality Buffer**

它更像：

```text
register cache
+
tiny L0 cache
+
stream buffer
+
load-reuse buffer
```

它的目标不是成为新的 architectural cache level。

它的目标只有一个：

> **让高概率会被同 lane 很快再次使用的数据不需要重新进入共享 memory hierarchy。**

例如：

```c
for (...)
    sum += A[i] * B[i];
```

lane 处理：

```text
Lane0:
 A[0]
 B[0]
 A[8]
 B[8]
 ...
```

如果某些 operand/metadata 有局部 reuse：

```text
LLB hit
 ↓
direct lane operand
```

而不是：

```text
lane
 ↓
network
 ↓
L1
 ↓
network
 ↓
lane
```

### LLB 最适合作为非 coherent microarchitectural structure

一种非常漂亮的实现是：

```text
Architectural coherence point = shared L1
```

LLB：

```text
not independently coherent
```

其内容可以是：

```text
clean speculative copies
stream fragments
forwarded store data
prefetch data
```

如果 coherence event 或 ownership uncertainty：

```text
invalidate local entries
```

而不是让每个 lane 变成一个完整 coherence participant。

这会把复杂度低很多。

### 不应该简单做“预测 LLB 或 L1 二选一”

你的原始想法大致是：

```text
memory op
  ↓ predict
LLB OR L1
```

我会稍微改一下：

```text
                 Address
                    │
       ┌────────────┴────────────┐
       ▼                         ▼
   LLB lookup              L1 bank route prep
       │                         │
       └──────── locality predictor
```

对 latency-critical scalar load：

```text
LLB lookup + L1 preparation overlap
```

避免 predictor 错了以后再花一轮 latency。

对于 streaming RVV/SIMT load：

```text
predict no reuse
 ↓
LLB bypass
 ↓
coalescer → L1
```

对于 high-locality load：

```text
LLB
 ↓ hit
lane immediately consumes
```

所以不是：

> “所有访问都先走 lane cache”

而是：

> **reuse-directed memory routing。**

### Memory Operation 也应该成为 Packet，而不只是 uOP

vector memory：

```text
vle64.v
```

Decode：

```text
1 vector macro-uOP
```

AGU：

```text
address element packets
```

例如：

```text
A+0
A+8
A+16
A+24
...
```

不要让 8 lanes 各自独立向 L1 发 8 个请求。

先做：

```text
             element addresses
       / / / / / / / /
              ↓
       Memory Packetizer
              ↓
      cache-line grouping
```

例如：

```text
A+0
A+8
A+16
A+24
A+32
A+40
A+48
A+56
```

实际上：

```text
one 64-byte cache line
```

因此只产生：

```text
one memory transaction
```

返回：

```text
64B line
 ↓
return distributor
 ├→ lane0
 ├→ lane1
 ├→ lane2
 ...
```

这就是 GPU memory coalescing 的核心思想之一，但完全可以放到 standard RVV memory backend 内部。

### 更进一步：Cross-Hart Coalescing

既然你希望做 Transparent Micro-SIMT，这一步非常自然。

假设：

```text
Hart0 → A[0]
Hart1 → A[1]
Hart2 → A[2]
Hart3 → A[3]
```

传统 multicore：

```text
four load requests
```

EAF-V：

```text
four memory uOPs
      ↓
address match/coalescer
      ↓
one cache-line request
      ↓
multicast result
```

于是不仅 vector loads 能 coalesce：

> **多个普通 RISC-V threads 也可以在 microarchitecture 内做 memory transaction fusion。**

这是我认为你这个架构非常有潜力形成独特贡献的地方。

### Memory Fabric 可以做三层调度

我建议 memory scheduler 不只是传统 LSU queue。

```text
              Memory uOP
                  │
        ┌─────────▼──────────┐
        │ Address Scheduler  │
        │ AGU availability   │
        └─────────┬──────────┘
                  │
        ┌─────────▼──────────┐
        │ Transaction Layer  │
        │ merge/coalesce     │
        │ bank scheduling    │
        │ prefetch/bypass    │
        └─────────┬──────────┘
                  │
        ┌─────────▼──────────┐
        │ Criticality Layer  │
        │ CPU latency        │
        │ RVV throughput     │
        │ SIMT bandwidth     │
        └────────────────────┘
```

这就回应了你原始的：

> Unified Memory 里也有一层 scheduling logic。

我认为这个判断是对的。

但建议名称不要叫 Unified Memory，因为容易和 unified virtual memory / unified cache 混淆。

我会叫：

> **Memory Execution Fabric — MEF**

### Critical Load 不应该排在 100 个 GPU-like Load 后面

CPU ↔ GPU dynamic architecture 最大的问题之一是：

```text
CPU wants:
  one 4-byte dependent load NOW

GPU-like cohort wants:
  hundreds of streaming loads
```

如果 memory fabric 完全公平：

```text
CPU latency collapses
```

因此 memory transactions 应该带：

```text
Criticality
Deadline class
Age
Miss status
Reuse class
Streaming class
Context QoS
```

memory scheduler：

```text
Score =
    critical_path_weight
  + age
  + row/bank efficiency
  + coalescing benefit
  + locality
  - congestion
```

特别是：

```text
dependent scalar load
```

应该能够抢占：

```text
non-critical prefetch
bulk SIMT streaming requests
```

这样 CPU personality 才不会被 GPU personality 摧毁。

### Ara-Opt 给这个方向提供了非常强的证据

Ara-Opt 报告 vector processor 的实际 throughput gap 不只来自 compute width，而来自：

- memory-side data supply；
- transaction progression；
- dependence / issue control；
- operand delivery；
- result propagation。

它通过 descriptor-driven memory frontend、next-VL prefetch、dynamic local issue、multi-source forwarding 等方式，在**没有增加 raw memory bandwidth**的情况下得到约 1.33× geometric-mean speedup。citeturn17view1

这说明：

> 对你的架构，Memory Fabric 甚至可能比“多加几个 ALU”更值得投入。

## Dynamic Execution、Unified Writeback 与 Core Aggregation

### Unified Writeback 的概念是对的，但不能是一根 Bus

例如：

```text
ALU0 \
ALU1  \
FPU0   \
FPU1    → UNIFIED WRITEBACK BUS → PRF
VEC0   /
VEC1  /
LOAD  /
```

随着 resources 增加，这几乎必然成为：

- fanout bottleneck；
- port bottleneck；
- arbitration bottleneck；
- physical wiring bottleneck。

因此“Unified Writeback”应该是：

> **逻辑统一，物理分布。**

建议改称：

> **Completion Fabric**

```text
        ALU Slice ─ Local Result ─┐
         FP Slice ─ Local Result ─┤
        MEM Slice ─ Load Result ──┤
      Vector Lane ─ Result ───────┤
                                  ▼
                         Completion Fabric
                            /    |    \
                           /     |     \
                       PRF B0  PRF B1  VRF
                          │      │
                          └── tags ready
                               ↓
                        dependent schedulers
                               ↓
                              ROB
```

ROB 不一定接收 data。

它只需要：

```text
Done
Exception
Replay
Branch status
```

data 进入：

```text
banked physical register file
local operand cache
forwarding network
```

这样才能 scale。

### Result Locality 应成为 Scheduling 的第一等指标

传统 scheduler 常问：

```text
FU free?
operand ready?
```

你的 scheduler 还应该问：

```text
operand在哪里？
```

例如：

```text
uOP A executes Cluster0
 ↓ produces p45

uOP B consumes p45
```

如果 B 可以：

```text
execute Cluster0
```

就应该偏向 Cluster0。

否则：

```text
p45
 ↓
global network
 ↓
Cluster3
```

会产生：

- 额外 latency；
- network energy；
- network contention。

因此 scheduler score 可以类似：

\[
Score(u,r)=
W_a A+
W_c C+
W_l L+
W_d D-
W_q Q-
W_r R
\]

其中：

- \(A\)：age；
- \(C\)：criticality；
- \(L\)：operand/data locality；
- \(D\)：desired capability match；
- \(Q\)：destination queue pressure；
- \(R\)：routing cost。

初期 FPGA 不建议搞 neural scheduler。

一个简单、可解释的 heuristic 就足够研究：

```text
ready
then oldest
then local operand
then least congested compatible slice
```

之后再看 performance counter。

### 从 Giant ROB 转向 Per-Hart Commit Domain

如果你未来有：

```text
4 cores
× 4 threads
× 8 vector lanes
```

绝对不要尝试：

```text
1 giant ROB
```

覆盖全部。

原因不是 ISA 做不到，而是物理上很难。

更好的：

```text
Hart0 Rename/ROB ─┐
Hart1 Rename/ROB ─┤
Hart2 Rename/ROB ─┤
Hart3 Rename/ROB ─┤
                  │
             Execution Fabric
                  │
Hart0 Commit ◄────┤
Hart1 Commit ◄────┤
Hart2 Commit ◄────┤
Hart3 Commit ◄────┘
```

也就是说：

> **Execution resources global-ish，architectural retirement local。**

每 hart 保留：

```text
rename map
ROB / sequence window
branch checkpoints
architectural exception order
store ordering state
```

而：

```text
ALU
FPU
vector lanes
AGU
memory ports
```

可以共享。

这是 aggregation architecture 中非常关键的分界。

### Core 也不应该消失，而应该变成 Commit / Control Domain

传统 core：

```text
Frontend
Rename
Scheduler
ALUs
FPU
LSU
L1
ROB
```

新的“core”：

```text
Hart Control Domain
{
    Frontend
    Rename
    Architectural State
    ROB
    Branch Recovery
    Commit
}
```

而执行资源：

```text
Execution Fabric
{
    ALU slices
    FP slices
    AGU slices
    Vector/SIMT lanes
    Memory slices
}
```

于是：

> **core 不再等于固定数量的 execution units。**

这是最重要的 conceptual break。

### 真正可扩展的单位应当叫 Slice 或 Tile

例如一个 Pod：

```text
                     EAF Pod
 ┌─────────────────────────────────────────────┐
 │                                             │
 │  Hart Domain 0      Hart Domain 1           │
 │  Hart Domain 2      Hart Domain 3           │
 │          \            /                     │
 │           Dynamic Dispatch                  │
 │                 │                           │
 │   ┌────────┬────┼──────┬────────┐           │
 │   ▼        ▼    ▼      ▼        ▼           │
 │ Slice0   Slice1 Slice2 Slice3  Slice4        │
 │ ALU      ALU    FP     AGU     ALU+AGU       │
 │                                             │
 │   Vector/SIMT Lane Group                    │
 │ [L0][L1][L2][L3][L4][L5][L6][L7]          │
 │                                             │
 │   Memory Execution Fabric                   │
 │ [LLB][Coalescer][Banked Shared L1]          │
 │                                             │
 └─────────────────────────────────────────────┘
```

多个 Pod：

```text
Pod0 ── Fabric ── Pod1
 │                 │
Pod2 ── Fabric ── Pod3
```

但这里应有一个非常明确的 scaling boundary：

```text
within pod:
fine-grain uOP/resource sharing

between pods:
coarse-grain task/vector/memory sharing
```

不要：

```text
uOP from Pod0 every cycle freely selects ALU in Pod7
```

否则 routing latency 会直接破坏 CPU IPC。

AraXL 最新工作同样从物理实现角度得到类似结论：它为了把 RISC-V vector processor 扩展到最多 64 parallel lanes，采用的是 modular、distributed、hierarchical interconnect，而不是一个扁平的无限宽 vector backend。citeturn18academia30

所以：

> **hierarchy 不是妥协，而是 dynamic architecture 能扩展的前提。**

## IPC、Throughput 与这类架构真正能赢在哪里

“现代 CPU 就是追求 IPC”这个说法只对了一半。

你设计的 machine 应该同时优化：

\[
Performance \neq IPC_{scalar}
\]

而是至少观察：

\[
\text{Useful Work/sec}
\]

以及：

\[
\text{Ops/cycle}
\]

\[
\text{IPC/hart}
\]

\[
\text{MLP}
\]

\[
\text{Lane Utilization}
\]

\[
\text{Execution Slice Utilization}
\]

\[
\text{Data Movement per Useful Op}
\]

\[
\text{Energy per Op}
\]

例如：

一个 RVV instruction：

```text
1 instruction retired
```

但可能执行：

```text
64 arithmetic operations
```

所以：

```text
IPC = 1
```

并不意味着：

```text
performance low
```

同样 GPU 的 scalar-equivalent IPC 也不是非常有意义的主指标。

### 传统 High-IPC CPU 的根本问题之一是资源被静态 provision

例如一个大 CPU：

```text
4 ALUs
2 AGUs
2 FP pipes
vector
```

当前 workload 如果：

```text
integer branch-heavy
```

FP/vector：

```text
mostly idle
```

如果：

```text
vector FP
```

大量 scalar ALU：

```text
underutilized
```

如果：

```text
memory-bound
```

compute：

```text
idle waiting
```

你的 architecture 真正可以尝试解决的是：

> **把“资源属于某个 core/port”改变成“资源属于当前需要它的 workload”。**

也就是从：

```text
static provisioning
```

变成：

```text
temporal resource ownership
```

这是比“增加 issue width”更根本的改变。

### 但是 Dynamic Fabric 本身也可能让 IPC 更低

这是必须正视的。

例如传统 CPU：

```text
Scheduler → ALU
1 hop
```

EAF-V：

```text
Scheduler → route arbitration → network → slice → result network
```

如果每次加：

```text
+1 / +2 cycles
```

dependent chain：

```text
ADD → ADD → ADD → BR
```

性能会非常差。

因此 CPU mode 必须提供：

> **Fast Local Execution Island**

例如：

```text
Hart0
 │
 ├─ local ALU0
 ├─ local ALU1
 ├─ local branch
 └─ local AGU
```

dependency-critical uOP：

```text
never leaves local island
```

只有需要额外 throughput：

```text
borrow remote resources
```

这相当于：

```text
local resources = latency
remote resources = bandwidth
```

可以类比 cache：

```text
L1 = fast/local
L2/L3 = larger/slower
```

你的 execution hierarchy 也应该：

```text
E0 = local execution
E1 = pod execution
E2 = remote aggregated execution
```

这是一个非常有潜力的 conceptual model：

> **Execution hierarchy analogous to memory hierarchy.**

### 可以做 Execution Locality Predictor

CPU 已经预测：

```text
branch
memory dependence
cache behavior
```

那么你的机器也可以预测：

```text
这个 uOP 值不值得发到 remote slice？
```

例如：

```text
critical dependency chain:
prefer local

independent multiply:
remote OK

long FP op:
remote OK

vector packet:
remote strongly OK

cache-miss dependent op:
wait near memory completion point
```

可以记录：

```text
PC → execution affinity
```

例如：

```text
PC 0x4000:
critical scalar chain
→ pin local

PC 0x5100:
independent multiply
→ load-balance

PC 0x6200:
streaming vector
→ vector cluster
```

这样 pipeline topology 事实上由 program behavior 动态塑形。

### Branch Prediction 不应该被过度“Fabric 化”

你的架构依然需要非常强的 branch prediction。

因为：

```text
large in-flight window
+
more execution resources
```

意味着 misprediction 时浪费的 speculative work 更多。

所以 Branch Predictor 可以仍然是 conventional high-performance design。

区别在于 recovery 使用：

```text
Epoch ID
Branch Mask
```

所有 uOP packet：

```text
Epoch = E42
```

一旦：

```text
branch mispredict
```

发布：

```text
Kill E42 younger-than Seq X
```

各 distributed queue：

```text
local invalidate
```

而不需要 central scheduler 一条条 reclaim。

这样更适合 distributed execution fabric。

### 这里最大的创新点其实可能是 “Elastic Window”

传统 OoO：

```text
ROB capacity = N
Issue Queue capacity = M
```

限制 in-flight instructions。

EAF-V 可以把：

```text
architectural reorder metadata
```

与：

```text
execution waiting state
```

分离。

例如 ROB 只保存 compact descriptor：

```text
Seq
dest
exception
completion
branch
memory order metadata
```

而 operand waiting：

```text
distributed reservation stations
```

vector element state：

```text
vector descriptor queues
```

memory state：

```text
memory fabric
```

这样可以提高：

> **有效 in-flight work，而不要求一个超大的 central CAM issue queue。**

这很符合你“increase amount of inst per flight”的目标。

但准确地说，应该追求：

> **Increase useful work in flight**

不只是：

> increase instruction count in ROB.

## FPGA 原型与验证路线

这类架构非常容易因为一次性目标太大而失败。

所以我建议不要第一版就做：

```text
RV64 + Linux
8-wide OoO
RVV
SIMT
multi-core aggregation
coherence
lane caches
```

第一代 FPGA 的目的应该是回答：

> **dynamic execution fabric 到底有没有性能价值？**

### 最小原型应该是 Scalar Elastic Backend

第一版：

```text
RV64I/M
    ↓
2–4 wide decode
    ↓
rename
    ↓
small ROB
    ↓
Dynamic Dispatch
   ┌────┼────┐
   ▼    ▼    ▼
Slice0 Slice1 Slice2
 ALU    ALU   AGU
   \     |    /
 Completion Fabric
       ↓
     commit
```

只做：

- uOP abstraction；
- distributed queue；
- dynamic FU routing；
- local/remote execution；
- tagged completion；
- precise retire。

对比一个：

```text
same resources
+
fixed pipeline backend
```

测：

```text
IPC
frequency
queue occupancy
remote execution %
bypass traffic
critical-chain latency
utilization
```

只有这一版证明：

```text
dynamic backend > fixed backend
```

才继续。

### 下一版加入 RVV Lane Fabric

第二版：

```text
Scalar OoO
    │
Vector Macro-uOP
    ▼
Vector Descriptor Queue
    │
  packetizer
    │
 ┌──┼──┬──┬──┐
 ▼  ▼  ▼  ▼  ▼
L0 L1 L2 L3 ...
```

重点不是一开始就最大 VLEN。

重点验证：

```text
2 lanes
4 lanes
8 lanes
```

以及：

```text
one workload × 8 lanes
vs
two workloads × 4 lanes
vs
four workloads × 2 lanes
```

Ara2 已经证明这种 partitioning tradeoff 是非常真实的研究空间，因此你的贡献可以是：

> **不需要不同 processor instances，而在 runtime 动态改变 lane aggregation。** citeturn16view1

同时实现：

```text
vector macro-uOP
element packet scheduler
vector completion bitmap
chaining/forwarding
```

### 然后验证 Lane Locality Buffer

第三版：

```text
AGUs
 ↓
Per-lane LLB
 ↓
coalescer
 ↓
banked shared L1
```

做四组配置：

```text
A. shared L1 only

B. private per-lane coherent caches

C. LLB + shared L1

D. LLB + coalescer + shared L1
```

我预期 **C/D 更有希望**，但这是应该由实验验证的设计推断，而不是预先当成结论。

特别观察：

```text
LLB hit rate
L1 transaction reduction
duplicate-line rate
network bytes/op
coalescing ratio
L1 bank conflicts
load-use latency
energy proxy
```

MemPool、Spatz 与 TeraPool 的结果说明 shared/tightly coupled L1 本身有良好的可扩展性，但非常依赖 bank organization 和 interconnect，因此这组实验非常值得做。citeturn18academia32turn17view3turn17view2

### 再加入多 Hart 与 Cohort Fusion

第四版：

```text
Hart0 ─┐
Hart1 ─┤
Hart2 ─┤ → Fusion Detector → SIMD/SIMT lane group
Hart3 ─┘
```

第一阶段甚至不用完整 branch divergence hardware。

只做：

```text
same decoded opcode
same execution class
same PC or compatible basic block
```

融合。

例如：

```text
4 ADD → 1 × 4-lane ADD
4 LOAD → one coalesced memory packet
```

然后研究：

```text
fusion rate
average cohort width
divergence frequency
memory coalescing ratio
extra scheduling overhead
energy saved
```

如果 effective：

再加入：

```text
active mask
reconvergence metadata
cohort splitting/merging
```

这一步实际上会把 architecture 从：

```text
CPU with vector
```

推进到真正很独特的：

```text
self-forming SIMT processor
```

### 最后才做 Pod Aggregation

```text
             System
       ┌──────┴──────┐
      Pod0          Pod1
   4 harts        4 harts
   8 lanes        8 lanes
      │              │
      └──── L2 ──────┘
```

先只允许：

```text
vector/task stealing
memory remote access
```

而不允许：

```text
arbitrary single-cycle remote ALU borrowing
```

之后测：

```text
remote execute latency
remote-data traffic
pod utilization
scaling efficiency
```

再决定 remote ALU sharing 是否值得。

### XiangShan 的工程方法很适合借鉴

XiangShan 官方仓库当前已经提供：

```text
Chisel source
   ↓
make verilog
   ↓
SystemVerilog

Verilator
NEMU
difftest
```

并将 `difftest` 作为项目的一部分；官方 README 也直接展示了生成 Verilog 与通过 Verilator + NEMU differential execution 的路径。citeturn16view0

对于 EAF-V，我强烈建议 architectural verification 也以：

```text
retirement boundary
```

作为 golden comparison。

即无论内部发生：

```text
fusion
reordering
vector splitting
lane migration
memory coalescing
remote execution
```

最终：

```text
Retire:
 PC
 instruction
 architectural destination/value
 CSR effects
 exception
 memory architectural effect
```

必须和 reference model 一致。

这正好符合：

> **software sees normal RISC-V; microarchitecture can be abnormal.**

建议 invariant 包括：

```text
No instruction retires before older unresolved exception

No wrong-epoch uOP can update architectural state

Every physical destination has exactly one valid producer

A fused cohort is architecturally identical to independent execution

A coalesced load returns exactly the value each original load would see

RVV packet completion is equivalent to atomic architectural completion
    subject to the ISA's defined exception/memory semantics

Store visibility obeys the selected RISC-V memory-model requirements
```

这种验证思路会比试图证明整个 distributed fabric 的 global timing 行为简单很多。

## 推荐的最终 EAF-V 架构

综合 XiangShan、Ara/Ara2、AraXL、SEAM-V、Ara-Opt、Spatz、MemPool、TeraPool、Vortex 的结果，我认为你这个想法最终最有潜力演化成下面这样的体系。现有研究分别证明了高性能 OoO RISC-V、lane-based scalable vector execution、packetized/decoupled vector backend、shared-L1 manycore、hierarchical thousand-PE memory fabric、以及 RISC-V SIMT GPU 的各部分可行性；真正新的部分，是把这些思想整合成**能够动态改变资源所有权的统一 backend**。citeturn16view0turn16view1turn17view0turn17view1turn18academia30turn17view3turn17view2turn17view4

```text
                    EAF-V PROCESSOR

 ┌───────────────────────────────────────────────────────┐
 │                  FRONTEND DOMAIN                      │
 │                                                       │
 │  Fetch → Branch Prediction → Predecode → Decode       │
 │                                                       │
 │            multiple architectural harts               │
 └──────────────────────────┬────────────────────────────┘
                            │
                            ▼
 ┌───────────────────────────────────────────────────────┐
 │                RENAME / COMMIT DOMAINS                │
 │                                                       │
 │ Hart0: Rename + ROB + Branch Epoch + Commit           │
 │ Hart1: Rename + ROB + Branch Epoch + Commit           │
 │ Hart2: Rename + ROB + Branch Epoch + Commit           │
 │ Hart3: Rename + ROB + Branch Epoch + Commit           │
 └──────────────────────────┬────────────────────────────┘
                            │
                    tagged dynamic-uOPs
                            │
                            ▼
 ┌───────────────────────────────────────────────────────┐
 │              ELASTIC DISPATCH FABRIC                  │
 │                                                       │
 │     dependency + locality + criticality + load        │
 │                                                       │
 │              Dynamic Resource Broker                  │
 └────┬─────────────┬─────────────┬─────────────┬────────┘
      │             │             │             │
      ▼             ▼             ▼             ▼
 ┌────────┐    ┌────────┐    ┌────────┐   ┌──────────────┐
 │Scalar  │    │ FP/FMA │    │ AGU    │   │ Vector/SIMT  │
 │Slice   │    │ Slice  │    │ Slice  │   │ Lane Groups  │
 ├────────┤    ├────────┤    ├────────┤   ├──────────────┤
 │Local IQ│    │Local IQ│    │Local IQ│   │L0 L1 L2 L3..│
 │ALUs    │    │FPU     │    │Address │   │VRF fragments │
 │Branch  │    │Mul/Div │    │engine  │   │ALU/FMA       │
 └────┬───┘    └───┬────┘    └───┬────┘   └──────┬───────┘
      │            │             │               │
      └────────────┴──────┬──────┴───────────────┘
                          │
                          ▼
 ┌───────────────────────────────────────────────────────┐
 │               MEMORY EXECUTION FABRIC                 │
 │                                                       │
 │ Lane LLBs ──┐                                         │
 │ Load Queue ─┼→ Address Merge → Coalescer → Bank Route │
 │ Store Queue ┤                                         │
 │ Prefetch ───┘                                         │
 │                                                       │
 │                Banked Shared L1                       │
 │                       ↓                               │
 │                       L2                              │
 └──────────────────────────┬────────────────────────────┘
                            │
                            ▼
 ┌───────────────────────────────────────────────────────┐
 │                COMPLETION FABRIC                      │
 │                                                       │
 │ distributed writeback + forwarding + ready events     │
 │                                                       │
 │ PRF banks     VRF banks     Local Operand Buffers     │
 └──────────────────────────┬────────────────────────────┘
                            │
                            ▼
                    Per-Hart Commit
```

在我看来，这个设计最核心的四条 architectural principles 应该固定下来：

**第一，Architectural State 不动态。**  
RISC-V hart、register、CSR、exception、memory ordering 对软件依然是稳定、标准的。

**第二，Execution Ownership 动态。**  
ALU、FPU、AGU、RVV lanes、memory bandwidth 属于 Fabric，而不是永久属于某个 core。

**第三，Locality 比“全局共享”更重要。**  
能本地执行就本地执行；需要 bandwidth 才向外扩展。Dynamic 不等于所有东西都 global。

**第四，Hierarchy 是 scalability 的基础。**  
随着 lane/core 数增加，从 local slice → pod → inter-pod 分层，而不是用一张越来越大的 crossbar。AraXL 和 TeraPool 在 vector lanes 与 shared-memory PE scaling 上都从物理实现角度给出了很强的支持。citeturn18academia30turn17view2

最终，这个 architecture 最值得追求的并不是：

> “比传统 CPU 多一个奇怪的 scheduler。”

而是一个更大的概念：

\[
\boxed{
\text{RISC-V Architectural Machine}
\neq
\text{Fixed Physical Pipeline}
}
\]

软件看到：

```text
RV64 + RVV + normal harts + normal virtual memory + normal Linux
```

硬件内部则看到：

```text
instructions
   ↓
uOP/dataflow packets
   ↓
dependencies
   ↓
available capabilities
   ↓
locality
   ↓
dynamic execution topology
```

于是同一块 hardware 可以：

```text
single thread:
  concentrate resources
  → CPU-like

few independent threads:
  partition resources
  → SMT / throughput CPU

RVV workload:
  aggregate lanes
  → vector processor

many convergent threads:
  cohort fuse
  → SIMT-like

massively parallel workload:
  many contexts + coalesced memory + narrow per-thread control
  → GPU-like
```

但这里最重要的现实边界也必须明确：

\[
\boxed{
\text{GPU-like hardware cannot create parallelism that software does not contain}
}
\]

真正可行的目标不是“任意单线程程序神奇地获得 GPU speedup”，而是：

> **当 parallelism 存在时，硬件自动发现它、聚合资源、改变 execution personality；当 parallelism 不存在时，硬件重新聚合为 latency-oriented CPU。**

而你的 per-lane memory 思路也应该从“每 lane 再造一个 L1”进一步进化成：

\[
\boxed{
\text{LLB}
+
\text{Memory Coalescing}
+
\text{Banked Shared L1}
+
\text{Criticality-aware Memory Scheduling}
}
\]

这是我认为整个设计里除 dynamic execution fabric 以外，**最可能带来实际性能收益的第二核心创新**。2026 年 Ara-Opt 和 SEAM-V 的结果尤其强化了这一判断：在现代 vector architecture 中，性能瓶颈已经明显不只是“有多少 arithmetic lanes”，而是 instruction supply、dependency management、memory transaction progress、operand delivery 和 result propagation 是否能够持续喂满这些 lanes。citeturn17view0turn17view1

因此，如果把这个研究项目压缩成一句 architecture thesis，我会定义为：

> **EAF-V 是一种保持标准 RISC-V/RVV software model、但把传统固定 Core Backend 解构为 hierarchical elastic execution-and-memory fabric 的处理器；它通过动态 resource leasing、vector lane aggregation、transparent thread cohort fusion、lane-local locality buffering、memory coalescing 与 distributed completion，在单线程低延迟 CPU 和高线程吞吐 SIMT machine 之间连续重构，而不是把 CPU 与 GPU 当作两个独立处理器。**

从研究价值和可实现性来看，我认为这比“完全动态、任意拓扑的 pipeline”更强，也比单纯做一个“可以跑 RVV 的宽 OoO RISC-V core”更有原创空间。