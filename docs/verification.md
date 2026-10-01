# 本轮验证、重跑方法与交付边界
原始报告含既有行尾空格，但其字节完整性必须保持；whitespace审计范围因此限定为新增/修订的规划文档，原报告由hash与Git index跟踪。当前工作树相对index无未暂存变更时，新文档的unstaged whitespace检查会输出空结果；这不是跳过检查。


本轮验证对象是规划文档，不是处理器实现。下列可执行检查器直接读取当前仓库，验证原始hash、全部标题覆盖、任务字段、合并依赖图、citation定义、local links、Git跟踪和whitespace。它不能自动证明架构主张；语义复核另见§2，一手来源读取与访问失败见references.md。

## 1. 精确重跑命令

在仓库根目录运行；仅依赖Python标准库与Git，不安装工具、不创建脚本或处理器文件。先确保有意交付的文档已经git add，否则跟踪检查故意失败。

```sh
python3 -c 'from pathlib import Path; p=Path("docs/verification.md"); s=p.read_text(); c=s.split("<!-- DOC-CHECK-BEGIN -->\n```python\n",1)[1].split("\n```\n<!-- DOC-CHECK-END -->",1)[0]; exec(compile(c,str(p),"exec"))'
```

外部URL检查是格式/来源账本完整性，不在每次重跑时联网；HTTP403/动态门户/版本未锁的真实限制已逐项登记，不能把此检查解释成所有链接200或所有工具已执行。

<!-- DOC-CHECK-BEGIN -->
```python
from pathlib import Path
from urllib.parse import unquote, urlsplit
from collections import Counter
import hashlib
import json
import re
import subprocess

root = Path.cwd().resolve()
originals = {
    "deep-research-report.md": "4de278a43e8cbbf0bcc4ca06841a71a946224fa75c595f9fa4cf462dd6cc0364",
    "deep-research-report(1).md": "ff7a2a005a91d2d9b59602479cadcdc4962ad75b68afac0ee7d627028319c6fc",
    "deep-research-report(2).md": "faf4317a72ba9fbd812eeeea7c1bb09e2f0a1519ce21622799cad608137e74fb",
}
plans = ["docs/implementation-plan.md", "docs/validation-plan.md", "docs/platform-plan.md"]
stage_docs = [
    "docs/stage-0-contracts-bringup.md", "docs/stage-1-scalar-control.md",
    "docs/stage-2-execution-fabric.md", "docs/stage-3-memory-system.md",
    "docs/stage-4-vector-locality.md", "docs/stage-5-multihart-aggregation.md",
    "docs/stage-6-verification-quality.md", "docs/stage-7-fpga-hardware.md",
    "docs/stage-8-asic-release.md", "docs/stage-9-lockstep-safety.md",
    "docs/stage-10-rva23-security.md",
]
new_docs = ["README.md", "docs/architecture-review.md", *plans,
            "docs/source-inventory.md", "docs/references.md", "docs/verification.md",
            *stage_docs]
expected_files = set(originals) | set(new_docs)
actual_md = {str(p.relative_to(root)) for p in root.rglob("*.md") if ".git" not in p.parts}
assert actual_md == expected_files, ("document inventory mismatch", sorted(actual_md ^ expected_files))
texts = {name: (root / name).read_text(encoding="utf-8") for name in expected_files}
for name, digest in originals.items():
    assert hashlib.sha256((root/name).read_bytes()).hexdigest() == digest, ("original modified", name)

inventory = texts["docs/source-inventory.md"]
expected_coverage = {}
for index, name in enumerate(originals, 1):
    lines = texts[name].splitlines()
    headings = [(n, s) for n, s in enumerate(lines, 1) if re.match(r"^#{1,3} ", s)]
    for pos, (start, title) in enumerate(headings):
        end = headings[pos+1][0]-1 if pos+1 < len(headings) else len(lines)
        expected_coverage[f"SRC-{index:02}:{start}-{end}"] = title.lstrip("# ").replace("|", "／")
rows = re.findall(r"^\| (SRC-\d{2}:\d+-\d+) \| ([^|]+) \| ([^|]+) \| ([^|]+) \|$", inventory, re.M)
assert len(rows) == len(expected_coverage) == 102, ("coverage count", len(rows))
assert {key: title.strip() for key, title, _, _ in rows} == expected_coverage, "source heading coverage mismatch"
assert len({key for key, *_ in rows}) == len(rows), "duplicate source coverage"

fields = ["Depends", "Inputs", "Action", "Outputs", "Pass", "Fail", "Covers", "Sources"]
tasks = {}
for name in plans:
    text = texts[name]
    heads = list(re.finditer(r"^### ([IVH]-\d{3}) — (.+)$", text, re.M))
    for n, head in enumerate(heads):
        task_id = head[1]
        assert task_id not in tasks, ("duplicate task", task_id)
        block = text[head.end():heads[n+1].start() if n+1 < len(heads) else len(text)]
        values = {}
        for field in fields:
            entries = re.findall(r"^- " + field + r": (.+)$", block, re.M)
            assert len(entries) == 1 and entries[0].strip(), ("task field", task_id, field)
            values[field] = entries[0]
        tasks[task_id] = values
assert Counter(task_id[0] for task_id in tasks) == {"I":98, "V":90, "H":51}, "task count changed"
for prefix, count in [("I",98),("V",90),("H",51)]:
    assert {key for key in tasks if key.startswith(prefix)} == {f"{prefix}-{n:03}" for n in range(1,count+1)}, "task ID gap"
deps = {key: set(re.findall(r"\b[IVH]-\d{3}\b", value["Depends"])) for key,value in tasks.items()}
for key, value in tasks.items():
    assert deps[key] or value["Depends"] == "none", ("unparseable dependency", key)
extra = re.findall(r"^\| ([IVH]-\d{3}) \| ([IVH]-\d{3}(?:, [IVH]-\d{3})*) \|", texts[plans[0]], re.M)
assert len(extra) == len({key for key,_ in extra}), "duplicate integration row"
for key, predecessors in extra:
    assert key in tasks, ("unknown integration task", key)
    deps[key].update(predecessors.split(", "))
for key, predecessors in deps.items():
    assert predecessors <= tasks.keys(), ("unknown dependency", key, predecessors-tasks.keys())
visiting, visited, order = set(), set(), []
def visit(key):
    assert key not in visiting, ("dependency cycle", key, sorted(visiting))
    if key in visited:
        return
    visiting.add(key)
    for predecessor in sorted(deps[key]):
        visit(predecessor)
    visiting.remove(key)
    visited.add(key)
    order.append(key)
for key in sorted(tasks):
    visit(key)
assert len(order) == len(tasks)
def ancestors(key):
    result, pending = set(), list(deps[key])
    while pending:
        predecessor = pending.pop()
        if predecessor not in result:
            result.add(predecessor)
            pending.extend(deps[predecessor])
    return result

# Acyclicity alone does not prove that an optional future profile is absent
# from the unconditional p0/Core/FPGA publication path.
claim_guards = {
    "I-080": {"I-036","I-042","I-043","I-047","I-076","I-083","I-091","I-095","I-096","I-097","I-098","V-065","V-067","V-076"},
    "I-086": {"I-083","I-085","I-091","I-095","I-096","I-097","I-098","H-032","H-035","H-045","H-050","H-051"},
    "I-098": {"I-083","I-091","I-095","I-096","I-097","V-065","V-067","V-088","V-089","V-090","H-050","H-051"},
    "H-035": {"I-036","I-042","I-043","I-076","I-077","H-017","H-025","H-031","H-033"},
    "I-091": {"I-083","V-067","V-084","H-048","H-049"},
    "H-047": {"H-032","H-035","I-081","I-084"},
    "I-085": {"H-032","H-035","I-081","I-084","I-083"},
}
for gate, optional in claim_guards.items():
    assert not (blocked := ancestors(gate) & optional), ("optional task blocks baseline gate", gate, sorted(blocked))
assert {"I-080","V-080"} <= (ancestors("I-086") | deps["I-086"]), "baseline release lost functional/advertisement gate"
assert {"I-098","H-032"} <= deps["H-050"], "RVA23 board proof must follow ISA acceptance"
assert {"H-043","H-044","H-045"} <= deps["H-049"], "ASIC DCLS proof lacks physical signoff"
assert {"I-085","H-045","I-097","V-090","I-098"} <= deps["H-051"], "ASIC Secure proof lacks candidate/evidence"
assert "H-049" not in deps["H-051"], "ASIC Secure must not require optional DCLS"

for _,_,disposition,task_refs in rows:
    ids = set(re.findall(r"\b[IVH]-\d{3}\b", task_refs))
    assert ids and ids <= tasks.keys() and disposition.strip(), ("unmapped source", task_refs)

expected_refs = {f"{prefix}-{n:03}" for prefix,count in [("AR",27),("IR",4),("VR",15),("HR",16)] for n in range(1,count+1)}
definitions = []
for name in ["docs/architecture-review.md",*plans]:
    definitions += re.findall(r"^\| ((?:AR|VR|HR)-\d{3}) \|", texts[name], re.M)
    definitions += re.findall(r"^- \*\*(AR-\d{3}|IR-\d{3})\*\*", texts[name], re.M)
assert len(definitions) == len(set(definitions)) == 62 and set(definitions) == expected_refs, "reference definitions mismatch"
master_refs = re.findall(r"^\| ((?:AR|IR|VR|HR)-\d{3}) \|", texts["docs/references.md"], re.M)
assert len(master_refs) == 62 and set(master_refs) == expected_refs, "master ledger mismatch"
local_links, external_urls = 0, set()
for name in new_docs:
    text = re.sub(r"```[^\n]*\n.*?\n```", "", texts[name], flags=re.S)
    assert set(re.findall(r"\b(?:AR|IR|VR|HR)-\d{3}\b", text)) <= expected_refs, ("unknown citation", name)
    assert set(re.findall(r"\b[IVH]-\d{3}\b", text)) <= tasks.keys(), ("unknown task reference", name)
    assert "\ue200cite\ue202turn" not in text, ("new opaque citation", name)
    for target in re.findall(r"\[[^\]\n]+\]\(([^)\s]+)\)", text):
        parts = urlsplit(target)
        if parts.scheme:
            assert parts.scheme == "https" and parts.netloc, ("invalid external source", target)
            external_urls.add(target)
        else:
            path = unquote(parts.path)
            dest = (root/path) if path.startswith("docs/") else (root/name).parent/path if path else root/name
            assert dest.resolve().is_relative_to(root) and dest.is_file(), ("broken local link", name, target)
            assert not parts.fragment, ("anchor needs explicit validation", name, target)
            local_links += 1

tracked = set(subprocess.check_output(["git", "ls-files", "-z"], cwd=root).decode().split("\0")) - {""}
assert expected_files <= tracked, ("untracked documents", sorted(expected_files-tracked))
for stage in stage_docs:
    text = texts[stage]
    for required in ["## 1. 这个子系统是做什么的", "## 2. 为什么存在 / 上游输入",
        "## 3. 总体结构", "```mermaid", "## 4. 团队分工与进度追踪",
        "## 5. 可跟踪任务卡", "## 6. 阶段级阻塞清单", "## 7. 阶段 Track Log（持续追加）"]:
        assert required in text, ("stage guide missing", stage, required)
    assert text.count("## 1. 这个子系统是做什么的") == 1, ("ambiguous stage purpose heading", stage)
    assert text.count("### ") >= 2, ("stage guide has no task cards", stage)
    assert text.count("```mermaid") >= 1 and text.count("```") % 2 == 0, ("unbalanced code fence", stage)
worker_fields = ["执行者目标", "执行者须知", "建议工作顺序", "可接受完成", "何时停止求助", "交付说明", "参考资料"]
for stage in stage_docs:
    text = texts[stage]
    for match in re.finditer(r"^### ([IVH]-\d{3}) — ", text, re.M):
        nxt = re.search(r"^### [IVH]-\d{3} — ", text[match.end():], re.M)
        block = text[match.end():match.end() + (nxt.start() if nxt else len(text))]
        for field in worker_fields:
            assert f"- **{field}**" in block, ("missing worker field", stage, match[1], field)

combined_stage_text = "\n".join(texts[stage] for stage in stage_docs)
for task_id in tasks:
    assert re.search(r"^### " + task_id + r" — ", combined_stage_text, re.M), ("task missing team card", task_id)

for command in [["git","diff","--cached","--check","--",*new_docs],["git","diff","--check","--",*new_docs]]:
    subprocess.run(command, cwd=root, check=True)

print(json.dumps({"status":"PASS", "original_documents":3,
    "original_lines":sum(len(texts[name].splitlines()) for name in originals),
    "source_headings_covered":len(rows), "tasks":len(tasks),
    "tasks_by_kind":dict(Counter(key[0] for key in tasks)),
    "integration_rows":len(extra), "dependency_edges":sum(map(len,deps.values())),
    "dependency_graph":"acyclic", "reference_ids":len(definitions),
    "local_links_checked":local_links, "external_urls_catalogued":len(external_urls),
    "tracked_documents":len(expected_files), "stage_guides":len(stage_docs),
    "stage_task_cards":sum(1 for task_id in tasks if re.search(r"^### " + task_id + r" — ", combined_stage_text, re.M)),
    "claim_gate_checks":len(claim_guards)+5,
    "git_whitespace":"PASS",
    "verification_scope":"documentation/provenance/graph/claim boundaries; no processor simulation or hardware run"}, ensure_ascii=False, indent=2))

```
<!-- DOC-CHECK-END -->

## 2. 语义与范围复核动作

- 将source-inventory的102行逐项与实际原文标题/行范围比较；审查接受/纠正/分期是否有对应I/V/H任务。三个源文件hash必须不变。
- 交叉审查ISA profile、XLEN/VLEN、RAM延迟、per-hart ownership、macro/uOP/attempt、RVV partial trap、LLB freshness、MMIO/AMO、cohort eligibility、credit/drain/ABA在四份主计划中的合同一致性。
- 十一个 stage 指南均含用途、上游输入、Mermaid 结构、团队进度表、任务卡（负责/输入/微步骤/风险/验收/回退）、阶段阻塞清单和 Track Log；239 个任务 ID 在主计划有定义且指南覆盖全部。Mermaid 仅检查存在与基本闭合，**不**证明架构正确。合并 DAG 之外的声明矩阵须人工核对能力与真实证据；新增的 p0/Core/ASIC 祖先检查只防止已知“可选项目无条件阻塞”的回归。
- 核对XiangShan是第二DUT，Difftest是框架，NEMU/Spike/Sail是各有能力边界的参考；候选SHA和upstream命令有primary来源，本轮未运行。多hart memory不由single-hart lockstep代替。
- 三family都有exact-part/工具/约束/route/编程/reset/program/signature/负控制/证据任务；Zynq分支不混用，ARM PS执行不能冒充PL RISC-V执行。
- ASIC的PDK/library/SRAM/DFT/ATPG/MBIST/STA/PDN/IR-EM/DRC-LVS/封装/首硅分开gate，FPGA验证不越权证明ASIC。
- 性能数字只作来源带限制的背景或未来实验假说；所有工作包声明输入/输出/动作/pass/fail，未来CLI与本轮实际执行清楚分离。

## 3. Git 与任务恢复记录

- Git在原始目录初始化，原始三报告已加入index。初始commit因缺少author identity失败；未改全局/本地身份配置，未编造作者，也未push。最终交付要求tracked/staged，不声称已commit。
- ArchitectureReview任务异常退出前保存了architecture-review.md；集成责任人阅读全文并纳入验证。PlatformPlan任务已全文读取SRC-02并保存可检查研究记录，但未写文件；集成责任人补完platform-plan.md。ValidationPlan正常完成V-001–V-080，并由集成责任人阅读全文与交叉对齐。
- 没有将失败任务状态当作完成，也没有只重新发起相同失败工作来取得表面成功；检查的是实际文件、来源与最后集成DAG。
- 2026-09-29：项目正式命名为 **MosaicRV**；`origin` 已配置为 `git@github.com:BaiTian6641/MosaicRV.git`（fetch/push同一SSH地址）。仅登记remote，不联网验证、不fetch、不push。


## 4. 验证记录

下方记录必须由实际检查结果更新。任何失败保留原因并修正规划，不减少检查范围使其通过。每条记录只对它列明的对象、命令和范围有效；ACT4/Sail 校准不得标成 MosaicRV DUT 通过，未实跑的 XiangShan/NEMU/FPGA/ASIC 项保持 BLOCKED。

## 5. 当前检查结果

2026-09-29 本轮执行 §1 嵌入式检查器：`PASS`；三份原文 hash 保持不变，102 个原文标题全覆盖，239 个任务（98 I/90 V/51 H）、90 行跨文档集成依赖、641 条依赖边无环，62 个引用 ID、110 个本地链接、22 份已跟踪文档和 11 份 stage 指南/239 张任务卡通过；12 项 claim gate 检查与 Git whitespace 检查通过。另以同一图实跑六组依赖闭包正反例：无 cache p0、有序三板 p0、独立 ASIC、RVA23 Core、ASIC Secure、ASIC DCLS 物理；所需祖先存在且各自可选项目未被无条件串入。此为文档/来源/图检查，不代表任何 RTL、CPU 仿真、实板或 ASIC 测试已运行。

2026-09-30 RVA23 细查后复核：§1 嵌入式检查器再次 `PASS`；仍是 239 个任务（98 I/90 V/51 H）、90 行集成依赖、642 条无环依赖边、62 个引用、111 个本地链接、22 份文档、11 份指南与 239 张任务卡，Git whitespace 为 PASS。另以针对脚本核对 RVA23U64/S64 的 59 个 mandatory/Sha 名称与 23 个 ratified option 在实施、验证、RVA23 阶段指南中均有归属，并在平台/README 中明确 Core、项目选项组合与 Server Platform 边界。此证据只覆盖规划文档完整性，不声称任何 CPU 功能、DIEL 实测、板测或 ASIC 结果。

## 6. 2026-09-30 外部 ISA/ACT 工具环境与校准结果

本记录针对当前 Darwin arm64 工作站，不是可移植 runner 镜像。当前无 Docker/Podman/Colima，也没有配置 SSH Linux 主机；计划要求的 Linux x86-64 XiangShan/NEMU 环境因此仍 BLOCKED。

- **Sail**：安装官方 [`0.14.1` Mac-arm64 release](https://github.com/riscv/sail-riscv/releases/tag/0.14.1)，资产 `sail-riscv-Mac-arm64.tar.gz` SHA-256 为 `bc35be7b45a21f60d32915ccd8f9f1746f5a342399e4d65a8fb2b7c1e81babdf`；`sail_riscv_sim --version` 实际输出 `0.14.1`。源码 commit `e4b243f4eb5d1ed05bbbc030ad338c2a32c45d72`；工具安装在 `$HOME/mosaic-ref/sail-riscv-0.14.1`。
- **ACT4**：[`riscv/riscv-arch-test`](https://github.com/riscv/riscv-arch-test/commit/96493a91448ca50780013fd892daec2c204487ba) checkout 固定在 `96493a91448ca50780013fd892daec2c204487ba`，source checkout clean。`mise 2026.9.15` 已安装；ACT4 `.mise.toml` 的 Ruby `3.4.11`、uv `0.11.33`、Bundler `4.0.21`、prek `0.5.3`、ShellCheck `0.11.0` 均已安装并实查版本。ACT Python CLI `uv run act --help` 正常。
- **UDB/Z3 适配**：ACT4 首次 UDB 检查失败，UDB `0.1.17` 试图在 Darwin 加载 Linux AArch64 `libz3.so`（Mach-O loader 拒绝 ELF）。已安装 Homebrew Z3 `5.1.0`，并在隔离 `$HOME/mosaic-ref/act4-native-cache` 中将 UDB 预期的 `libz3.so` 名称指向原生 `libz3.dylib`；再次配置检查通过，未修改上游 gem 或仓库代码。
- **ACT4/Sail/calibration smoke**：使用上游 `config/sail/sail-rv64-max` 样例参考配置、仅过滤 `EXTENSIONS=I`，生成 51 个 RV64I ELF；逐个通过上游 `run_tests.py` 在 Sail 0.14.1 上执行，`summary.log` 为 51/51 `RVCP-SUMMARY: TEST PASSED`。51 个 ELF 的 SHA-256 清单保存在 `$HOME/mosaic-ref/act4-calibration-work-nativez3/sail-rv64-max/elfs.sha256`。这只校准生成器、UDB、编译器、Sail 与 ACT runner 的连通性：测试基于 Sail max 配置并在同一 Sail 模型回放，不是独立 oracle，也不是 MosaicRV DUT 验收；ACT 官方明确其不是 verification tests。
- **交叉工具链边界**：当前 Homebrew `riscv64-elf-gcc` 为 GCC `16.2.0`、Binutils `2.47.20260726`。固定 ACT4 README 列 GCC `15`/Binutils `2.44` 或 LLVM `22` 为其当时受支持/CI 测试基线。上述 ACT smoke 实际使用 GCC 16，虽通过，不代表闭合精确工具链兼容性；交叉工具链 source/hash 仍需冻结。
- **Spike 与环境审计**：Spike commit `0bff12123b1fd510e19e19634dd997dbade70e54` 的本地 `spike --help` 输出 `1.1.1-dev`。`python3 tools/check_upstream.py --require riscv-arch-test --require sail_riscv_sim --require spike --require riscv64-elf-gcc --require riscv64-elf-objdump` 通过，且明确列出 XiangShan/NEMU 的 BLOCKED 原因。
- **未完成集成**：尚无 MosaicRV 专属 ACT4 UDB/Sail/linker/`rvmodel_macros.h` 配置或 DUT ELF runner；不得拿 `sail-rv64-max` 样例或上述 51/51 冒称 p0 通过。XiangShan `e7bab53e66dfb3c4a1d11cf9519b0396f8576cae` 递归 source closure/build，以及 NEMU `f39e3077d7bac3cd9a3a853a9300a5f8f0293a2c` reference build/ABI calibration，仍需 Linux x86-64 runner。

后续补齐 Linux runner 或 MosaicRV DUT ACT 配置时，另起带版本与哈希的 run record；本条不自动升级为 DUT PASS。
