# RenderDoc CPD Summary Analyzer 实现计划

## 背景

当前 `Export Current Drawcall Data` 已经能导出 shader、pipeline、textures、constant buffers、resource buffers、mesh 和面向 Unity Shader 还原的工作流文档。实际用于 UE5 项目还原 Unity Shader 时，新的主要问题不是“缺少更多 raw dump”，而是缺少一条可追踪的证据链：

```text
shader load instruction
  -> stage/register/slot
  -> RenderDoc descriptor/resource
  -> exported raw buffer byte range
  -> decoded float/uint/int window
  -> UE Primitive/Instance/CustomPrimitiveData candidate
  -> Unity shader parameter hypothesis
```

因此本计划建议新增一个独立的 CPD Summary Analyzer。它消费当前 drawcall export 目录、UE 解包数据和 UE layout 配置，生成小而强的 CPD 分析摘要，供 AI Agent 直接用于 Unity Shader 还原。

## 目标

- 从一个已导出的 drawcall 目录中定位可能承载 UE Custom Primitive Data、PrimitiveSceneData、InstanceSceneData、GPUScene 相关数据的资源。
- 解析 VS/PS/CS shader disassembly 中对 constant buffer、structured buffer、raw buffer、byte address buffer、UAV/SRV buffer 的 load 访问。
- 将 shader load 访问映射回 `ConstantBuffers/constant_buffers.json` 和 `ResourceBuffers/resource_buffers.json` 中的导出文件。
- 对候选 offset 周边做小窗口解码，输出 float/uint/int/hex 多视图数据。
- 结合 UE 解包出的 material/shader bundle 参数名，生成 `_Blend4_CustomPrim_Mask`、`CustomPrimitiveData`、`PerInstanceCustomData` 等候选值和置信度。
- 输出 Markdown 和 JSON，让后续 AI Agent 可以在 Codex 中直接阅读并继续还原 Unity Shader。

## 非目标

- 不在基础 drawcall export 中继续无差别扩大 raw buffer dump。
- 不承诺自动还原 UE 的完整 Nanite/GPUScene/RenderGraph 管线。
- 不承诺从一个 drawcall 中恢复 material 默认参数。drawcall 里看到的是该次绘制实际使用的数据。
- 不要求第一版完整求解所有动态索引表达式。MVP 先输出可验证证据和候选窗口。
- 不把 CPD 判断逻辑硬编码在 qrenderdoc GUI 导出流程里。

## 总体架构

第一阶段实现为独立离线分析工具：

```text
qrenderdoc one-click export
  -> EID_xxxx_Draw...
       manifest.json
       pipeline_state.json
       shader_reconstruction_index.json
       Shaders/
       ConstantBuffers/
       ResourceBuffers/
       Textures/

cpd-summary analyzer
  --dc-export <EID_xxxx_Draw...>
  --bundle <UE unpacked analysis dir, optional>
  --ue-layout <UE layout json, optional>
  --out <output dir, optional>

outputs
  Analysis/CPD/custom_primitive_data_summary.json
  Analysis/CPD/custom_primitive_data_summary.md
  Analysis/CPD/shader_loads.json
  Analysis/CPD/buffer_windows.json
  Analysis/CPD/resource_candidates.json
```

建议先用 repo 内工具实现，不直接依赖 qrenderdoc UI 生命周期。原因：

- 分析输入已经是文件，独立工具更容易迭代、测试和对比。
- UE layout 和 bundle hint 会频繁变化，独立工具更适合外部 JSON 配置。
- 后续如果需要 UI 入口，只需要在 qrenderdoc 里增加“Run CPD Summary Analyzer”动作，调用同一套 analyzer。

## 推荐落点

优先使用 Python 实现第一版 analyzer：

```text
tools/
  shader_reconstruction/
    cpd_summary.py
    cpd/
      __init__.py
      export_index.py
      resource_candidates.py
      disassembly_loads.py
      buffer_windows.py
      bundle_hints.py
      ue_layout.py
      report_writer.py
      schemas.py
    layouts/
      UE5_5_GPUScene.example.json
      UE5_6_GPUScene.example.json
    README.md
```

选择 Python 的理由：

- 主要工作是 JSON、文本 disassembly 和 binary window 解码，Python 实现成本低。
- 方便后续 AI Agent 调试和扩展规则。
- 不影响 qrenderdoc 编译链，也不把 UE 特定逻辑塞进 RenderDoc GUI。

如果后续确认要产品化，可以再把稳定规则迁移成 C++，或保持 Python 工具作为 repo 自带分析器。

## 输入约定

### 必需输入

`--dc-export`

指向一次 drawcall export 目录，例如：

```text
K:/WorkSpace/Profiler/Test003/Ref/EID_3483_ID3D11DeviceContext_DrawIndexedInstanced
```

工具需要读取：

- `manifest.json`
- `drawcall.json`
- `pipeline_state.json`
- `shader_reconstruction_index.json`
- `Shaders/<stage>/disassembly_default.txt`
- `Shaders/<stage>/disassembly_native_*.txt`
- `ConstantBuffers/constant_buffers.json`
- `ResourceBuffers/resource_buffers.json`

### 可选输入

`--bundle`

指向 UE 解包或上游 AI Agent 生成的分析目录。第一版优先识别：

- `analysis/material_layer_parameter_bindings.json`
- `parameters/material_parameters.json`
- `parameters/textures.json`
- `*.usf` / `*.ush` / material dump 文本中的参数名

`--ue-layout`

指向 UE GPUScene/PrimitiveSceneData layout 配置。配置必须外置，不能硬编码在 analyzer 里。

`--out`

默认输出到：

```text
<dc-export>/Analysis/CPD/
```

## 输出约定

### `custom_primitive_data_summary.md`

面向人和 AI Agent 的首要入口，建议结构：

```text
# Custom Primitive Data Summary

## Capture Context
- Event ID
- API
- Draw/Dispatch name
- Active shader stages
- Export directory

## High Confidence Findings
- candidate parameter
- likely source buffer
- stage/register/instruction evidence
- decoded value window
- confidence

## Resource Candidates
- PrimitiveUniformBuffer
- PrimitiveSceneData / GPUScene
- InstanceSceneData
- InstancePayloadData
- CustomPrimitiveData / PerInstanceCustomData
- low priority unrelated buffers

## Shader Load Evidence
- stage
- disassembly file
- instruction line
- register / slot
- offset expression
- channels

## Buffer Windows
- raw file
- byte offset
- float4 / uint4 / int4 / hex

## Agent Guidance
- parameters to try first in Unity shader
- data that should be treated as low priority
- unresolved dynamic indexing that needs manual review
```

### `custom_primitive_data_summary.json`

机器可读总索引，建议 schema：

```json
{
  "version": 1,
  "input": {
    "dc_export": "...",
    "bundle": "...",
    "ue_layout": "..."
  },
  "capture": {
    "event_id": 3483,
    "api": "D3D11",
    "draw_name": "ID3D11DeviceContext_DrawIndexedInstanced",
    "active_stages": ["vs", "ps", "cs"]
  },
  "findings": [
    {
      "name": "_Blend4_CustomPrim_Mask",
      "kind": "custom_primitive_data",
      "confidence": 0.72,
      "source": {
        "stage": "ps",
        "register": "t12",
        "resource_name": "PrimitiveSceneData",
        "raw_file": "ResourceBuffers/..."
      },
      "evidence": {
        "instruction_ids": ["ps:123"],
        "byte_offsets": [320],
        "decoded_windows": ["win_001"]
      },
      "notes": [
        "Matched bundle parameter name containing CustomPrim",
        "Buffer name and stride match UE layout"
      ]
    }
  ]
}
```

### 其它中间文件

- `resource_candidates.json`：所有候选 buffer、命中原因、优先级。
- `shader_loads.json`：解析出的 shader load 指令。
- `buffer_windows.json`：所有截取窗口和解码结果。
- `unresolved_loads.json`：动态索引暂时无法确定 offset 的访问。

## 实现阶段

## P0. Export Index Loader

目标：

- 读取 drawcall export 目录中的 JSON 和 shader 文件。
- 建立 stage、register、slot、resource、raw file 之间的统一索引。

任务：

- 实现 `export_index.py`。
- 解析 `manifest.json`、`drawcall.json`、`pipeline_state.json`。
- 解析 `ConstantBuffers/constant_buffers.json`：
  - stage
  - binding / slot
  - display name
  - resource id
  - json/csv/raw path
  - byte size
- 解析 `ResourceBuffers/resource_buffers.json`：
  - stage
  - binding / slot
  - shader resource name
  - RenderDoc resource name
  - descriptor byte offset / byte size
  - raw file path
  - stride / format / view type if present
  - priority / guidance
- 解析 `Shaders/<stage>/disassembly_*.txt`。
- 所有路径统一转成相对 export root 的路径，JSON 输出中避免绝对路径污染。

验收：

- 对现有导出目录运行后能打印资源统计：
  - shader stages count
  - constant buffer count
  - resource buffer count
  - disassembly file count
- 缺少某类文件时不中断，写 warning。

## P1. Resource Candidate Locator

目标：

- 找出可能与 UE Primitive/Instance/CustomPrimitiveData 相关的资源。
- 将 URP tile/z-bin、light list、cluster、depth pyramid 等无关或低价值资源继续弱化。

候选关键词：

高优先级：

- `PrimitiveUniformBuffer`
- `PrimitiveSceneData`
- `GPUScene`
- `InstanceSceneData`
- `InstancePayloadData`
- `CustomPrimitiveData`
- `PerInstanceCustomData`
- `PrimitiveData`
- `InstanceData`

中优先级：

- `Scene`
- `View`
- `Material`
- `Object`
- `Transform`
- `Packed`

低优先级：

- `ZBin`
- `Z-Bin`
- `Tile`
- `Cluster`
- `LightList`
- `Culling`
- `HZB`
- `DepthPyramid`
- `Shadow`

任务：

- 实现 `resource_candidates.py`。
- 对 constant buffer 和 resource buffer 分别计算 candidate score。
- score 不只看名字，也要考虑：
  - buffer view type
  - stride
  - byte size
  - shader stage
  - read-only / read-write
  - descriptor access
  - 是否出现在 shader load 中
- 输出 `resource_candidates.json`。

验收：

- 能把明显的 `PrimitiveSceneData` / `GPUScene` 相关资源排到前面。
- 能把 `URP Z-Bin Buffer`、`URP Tile Buffer`、light list 类资源标记为 low priority。
- 无名 buffer 也能基于 stride、访问模式和 shader load 进入候选列表。

## P2. Shader Load Analyzer

目标：

- 从 DXBC/DXIL/SPIR-V/GLSL disassembly 中提取 buffer load 访问。
- MVP 先重点支持 D3D11 DXBC，因为当前样本多来自 D3D11，且 RenderDoc 对 DXBC 的反编译和 disassembly 信息更稳定。

MVP 识别类型：

- constant buffer load：
  - `cbN[...]`
  - `cbuffer`
  - immediate offset / register component
- structured buffer load：
  - `ld_structured`
  - `ld_structured_indexable`
- raw / byte address buffer load：
  - `ld_raw`
  - `ld`
  - `Load`
- texture/sample 指令只记录资源引用，不进入 CPD buffer window。

输出字段：

- `id`
- `stage`
- `disassembly_file`
- `line_number`
- `instruction_text`
- `resource_class`
- `register`
- `slot`
- `uav_or_srv`
- `byte_offset`
- `element_index_expr`
- `member_offset_expr`
- `channels`
- `confidence`
- `parse_status`

任务：

- 实现 `disassembly_loads.py`。
- 采用多套正则 pattern，而不是尝试完整编译 shader IR。
- 第一版只解析确定性 offset；动态表达式保留表达式字符串。
- 建立解析失败样例库，输出到 `unresolved_loads.json`。

验收：

- 能从 VS/PS/CS disassembly 中提取 load 指令。
- 对无法确定 offset 的指令不丢弃，标记 `unresolved_dynamic_index`。
- 每条 load 都能回链到原始文件和行号，方便人工核对。

## P3. Register/Resource Resolver

目标：

- 把 shader load 里的 register/slot 映射到导出的 cbuffer/resource buffer。

任务：

- 在 `export_index.py` 中建立 lookup：
  - `(stage, resource_class, register)`
  - `(stage, descriptor_type, slot)`
  - `(stage, shader_resource_name)`
- 对 DXBC register 形式统一：
  - `cb0` -> constant buffer slot 0
  - `t12` -> SRV slot 12
  - `u3` -> UAV slot 3
- 处理 stage fallback：
  - 如果 PS 中找不到，查 active pipeline resources。
  - 如果 compute-only dispatch，优先 CS。
- 输出每条 load 的 `resolved_resource_id`、`resolved_export_path`、`resolution_confidence`。

验收：

- shader load 能映射到 `ConstantBuffers/constant_buffers.json` 或 `ResourceBuffers/resource_buffers.json` 中的记录。
- 不能映射时输出具体原因：
  - missing resource index
  - ambiguous slot
  - no raw file exported
  - raw file truncated

## P4. Buffer Window Extractor

目标：

- 对已解析的 candidate offset 附近截取小窗口，解码为多种类型，避免 AI Agent 直接读取大 raw buffer。

窗口策略：

- 默认窗口大小：`256` bytes。
- 对 constant buffer：
  - 以 16-byte register 为基本单位。
  - 对 `cbN[x].zw` 类访问，窗口覆盖 `x-2` 到 `x+5` 个 float4。
- 对 structured buffer：
  - 如果有 stride，按 element 切窗口。
  - 同时输出 element index 和 byte offset。
- 对 raw/byte address buffer：
  - 按 byte offset 截取。
  - 对 4-byte 对齐、16-byte 对齐分别解码。

解码视图：

- `float`
- `float2`
- `float3`
- `float4`
- `uint`
- `uint4`
- `int`
- `int4`
- `hex`

任务：

- 实现 `buffer_windows.py`。
- 支持 little-endian binary 解码。
- 输出 `buffer_windows.json`。
- 对超出 raw file 范围的 offset 输出 warning。
- 如果 resource buffer 在导出时被截断，明确标记 `truncated: true`。

验收：

- 每个 high priority candidate 至少输出一个可读 window，除非 offset 动态不可解。
- Markdown 中只展示最关键的窗口，完整数据放 JSON。
- raw file 很大时不会把完整内容写进 summary。

## P5. UE Layout Config

目标：

- 用外部 JSON 描述 UE 版本相关的 PrimitiveSceneData/GPUScene 布局，帮助 analyzer 给 offset 命名。

示例：

```json
{
  "version": 1,
  "engine": "UE5.5",
  "source": "Project-derived, verify before use",
  "layouts": {
    "PrimitiveSceneData": {
      "stride_bytes": 512,
      "fields": [
        {
          "name": "LocalToWorld",
          "offset": 0,
          "type": "float4x3"
        },
        {
          "name": "CustomPrimitiveData",
          "offset": 320,
          "type": "float4[]",
          "count": 8
        }
      ]
    }
  }
}
```

任务：

- 实现 `ue_layout.py`。
- 支持多个 layout block。
- 支持 offset 命名和范围匹配：
  - exact field offset
  - inside field range
  - nearest field
- 输出 layout hit：
  - `layout_name`
  - `field_name`
  - `field_relative_offset`
  - `confidence`

验收：

- 没有 layout 文件时工具仍可运行，只降低置信度。
- layout 文件字段不匹配时输出 warning，不崩溃。
- 每个 finding 记录使用了哪个 layout 文件和版本。

## P6. Bundle Parameter Hints

目标：

- 从 UE 解包数据中提取对 Unity Shader 还原有价值的参数名 hint。

优先提取：

- `_Blend4_CustomPrim_Mask`
- `CustomPrim`
- `CustomPrimitive`
- `PerInstance`
- `PrimitiveData`
- `Mask`
- `Blend`
- `Layer`
- `Variation`
- `Instance`

任务：

- 实现 `bundle_hints.py`。
- 解析已知 JSON：
  - material layer parameter bindings
  - material parameters
  - texture parameters
- 对未知文本文件做轻量参数名扫描。
- 为 hint 分类：
  - scalar
  - vector
  - texture
  - switch
  - unknown
- 输出 matched hints 和未匹配 hints。

验收：

- 对 bundle 中含 `CustomPrim` / `PerInstance` 的参数名能够提权。
- 不因为参数名相似就直接认定 offset，只作为 confidence signal。
- Markdown 中明确“hint 来源于解包数据，不等于 drawcall runtime value”。

## P7. Finding Scorer

目标：

- 将 resource candidate、shader load、buffer window、UE layout、bundle hint 合并成最终 findings。

建议评分：

```text
resource name hit                  +0.20
shader load references candidate   +0.25
resolved raw export path           +0.15
layout field match                 +0.20
bundle parameter hint match        +0.15
stable decoded numeric window      +0.05
dynamic unresolved index           -0.20
truncated raw file                 -0.20
low priority system buffer         -0.30
```

置信度等级：

- `high`: `>= 0.75`
- `medium`: `0.45 - 0.74`
- `low`: `< 0.45`

任务：

- 实现 `schemas.py` 中的 finding schema。
- 实现 scoring 合并逻辑。
- 每个 finding 必须输出 reasons，避免黑盒判断。

验收：

- 同一个参数/offset 的重复证据会合并，不刷屏。
- high confidence finding 必须至少有 shader load evidence 和 resource evidence。
- low confidence finding 不隐藏，但放在 Markdown 后面。

## P8. Report Writer

目标：

- 写出面向 AI Agent 的 Markdown 和机器可读 JSON。

任务：

- 实现 `report_writer.py`。
- 生成：
  - `custom_primitive_data_summary.md`
  - `custom_primitive_data_summary.json`
  - `resource_candidates.json`
  - `shader_loads.json`
  - `buffer_windows.json`
  - `unresolved_loads.json`
- Markdown 控制长度，默认只展示：
  - top 10 findings
  - top 20 shader load evidence
  - top 20 buffer windows
  - unresolved summary
- 完整数据放 JSON。

验收：

- Codex/AI Agent 只读 Markdown 就能知道优先分析哪些数据。
- JSON 保留完整证据链，方便自动化二次处理。
- 报告中明确哪些是事实、哪些是推断。

## P9. CLI 包装

目标：

- 提供稳定命令行入口。

建议命令：

```powershell
python tools/shader_reconstruction/cpd_summary.py `
  --dc-export "K:/WorkSpace/Profiler/Test003/Ref/EID_3483_ID3D11DeviceContext_DrawIndexedInstanced" `
  --bundle "K:/WorkSpace/UEUnpacked/Subnautica2" `
  --ue-layout "tools/shader_reconstruction/layouts/UE5_5_GPUScene.example.json"
```

参数：

- `--dc-export`: 必需。
- `--bundle`: 可选。
- `--ue-layout`: 可选。
- `--out`: 可选。
- `--max-window-bytes`: 默认 `256`。
- `--max-report-findings`: 默认 `10`。
- `--include-low-priority`: 默认 false。
- `--stage`: 可重复，默认全部 active stage。
- `--verbose`: 输出解析细节。

验收：

- 参数错误时返回非零 exit code，并输出可读错误。
- 缺少 optional 输入时仍可生成 report。
- 工具不修改原 export 文件，只写 `Analysis/CPD`。

## P10. Export Workflow 集成

目标：

- 让 drawcall export 后生成的 workflow 文档知道 CPD analyzer 的存在，AI Agent 可以按需运行。

改动点：

- 更新 `AIDoc/UnityShaderReconstructionGoalTemplate.md`：
  - 增加 CPD Summary 步骤。
  - 在 UE5/Nanite/CS GBuffer 场景中提示先查看 `Analysis/CPD/custom_primitive_data_summary.md`。
- 更新 qrenderdoc 生成的 `AGENTS.md` / `UnityShaderReconstructionWorkflow.md`：
  - 如果 `Analysis/CPD` 已存在，优先引用 summary。
  - 如果不存在，提示运行 CLI。

验收：

- 用户导出后可以在 export 目录中运行：

```text
/goal UnityShaderReconstructionGoal.md
```

- Agent 在遇到 UE Custom Primitive Data 问题时知道先跑或先读 CPD summary。

## P11. 可选 qrenderdoc UI 入口

目标：

- 当 CLI 稳定后，再考虑在 qrenderdoc 中增加菜单或 export option。

建议入口：

- `Tools -> Run CPD Summary Analyzer for Current Export...`
- 或在导出完成 dialog 中增加按钮：
  - `Run CPD Summary Analyzer`

实现原则：

- qrenderdoc 只负责传入 export dir 和可选 bundle/layout 路径。
- analyzer 仍然是独立进程。
- GUI 不承载 UE layout 规则和 bundle 解析规则。

验收：

- UI 入口失败不会影响基础 drawcall export。
- analyzer stdout/stderr 写入 `Analysis/CPD/cpd_summary.log`。

## 测试计划

### 单元测试

位置建议：

```text
tools/shader_reconstruction/tests/
```

覆盖：

- DXBC cbuffer load pattern。
- DXBC structured buffer load pattern。
- 动态索引 unresolved case。
- register/slot resolver。
- binary window float/uint/int 解码。
- UE layout offset 匹配。
- bundle hint 提取。

### 样本回归

至少准备三类样本：

- D3D11 常规 VS/PS GBuffer drawcall。
- D3D11/UE5 Nanite 或 compute GBuffer dispatch。
- 一个包含明显 `CustomPrimitiveData` 或 `PerInstanceCustomData` 的材质样本。

每个样本保留最小 export fixture，不提交大 raw 文件。大文件使用本地验证路径记录在测试说明中。

### 人工验收

对用户当前样本目录运行：

```powershell
python tools/shader_reconstruction/cpd_summary.py `
  --dc-export "K:/WorkSpace/Profiler/Test003/Ref/<event_dir>" `
  --bundle "K:/WorkSpace/<unpacked_ue_project_analysis>" `
  --ue-layout "tools/shader_reconstruction/layouts/UE5_5_GPUScene.example.json"
```

检查：

- Markdown top findings 是否把 Primitive/GPUScene/Instance 相关资源放在前面。
- URP/Tile/ZBin/light-list 等资源是否被弱化。
- shader load 行号是否能回到原 disassembly 文件人工核对。
- buffer window 解码是否足够小，且能给 Agent 提供可读数值。
- 对无法求解的动态索引是否明确写出下一步需要人工确认什么。

## 风险和处理

### DXIL/SPIR-V disassembly 格式差异

风险：

- 不同 API 的 disassembly 文本差异大，单套正则无法完全覆盖。

处理：

- MVP 优先 DXBC。
- Parser 分 backend，输出统一 load schema。
- 未识别指令进入 `unresolved_loads.json`，不要静默丢弃。

### Nanite/Compute GBuffer 索引链复杂

风险：

- CS 里可能通过 thread id、cluster id、page table、indirect args 多级间接定位 primitive/instance。

处理：

- 第一版只输出证据和候选 window，不承诺完全反推 primitive index。
- 对 `DispatchThreadId`、`GroupId`、indirect args、Nanite page/cluster buffer 记录 evidence。
- 报告中标注“geometry/visibility path unresolved”。

### Raw buffer 截断

风险：

- 当前 resource buffer export 可能为避免超大文件而截断，CPD offset 可能在截断范围外。

处理：

- Summary 明确输出 `raw_truncated` 和 `offset_outside_exported_range`。
- 后续可增加 range re-export 能力，但不是 MVP。

### Bundle hint 误导

风险：

- 解包参数名只是静态信息，不代表该 drawcall 运行时一定使用该值。

处理：

- Bundle hint 只加分，不单独形成 high confidence finding。
- Markdown 区分 `observed runtime evidence` 和 `bundle hint`。

## 里程碑

### M1. 最小可用 CPD Summary

范围：

- P0 Export Index Loader
- P1 Resource Candidate Locator
- P2 DXBC Shader Load Analyzer MVP
- P3 Register/Resource Resolver
- P4 Buffer Window Extractor
- P8 Report Writer 基础版
- P9 CLI

完成标准：

- 能对一个现有 drawcall export 目录生成 `Analysis/CPD/custom_primitive_data_summary.md/json`。
- 报告中能列出候选 buffer、load 指令、decoded window。
- 即使没有 bundle/layout，也能产生有用摘要。

### M2. UE Layout + Bundle Hint

范围：

- P5 UE Layout Config
- P6 Bundle Parameter Hints
- P7 Finding Scorer

完成标准：

- 能将 `_Blend4_CustomPrim_Mask` 等 bundle 参数名和 PrimitiveSceneData/CustomPrimitiveData offset 关联成中高置信度 finding。
- 报告中能明确每个 finding 的证据来源和推断强度。

### M3. Unity Shader Workflow 集成

范围：

- P10 Export Workflow 集成
- 更新 goal/template 文档

完成标准：

- 用户导出后直接 `/goal UnityShaderReconstructionGoal.md`，Agent 会按流程读取或生成 CPD summary。
- UE5/Nanite/CS GBuffer 场景有明确的 CPD/InstanceData 分析步骤。

### M4. qrenderdoc UI 可选集成

范围：

- P11 UI 入口

完成标准：

- 在 qrenderdoc 中可以从当前 export 目录触发 analyzer。
- CLI 仍然可以独立使用。

## 建议优先级

第一轮实现只做 M1。原因是 M1 能立刻验证最关键假设：shader load 是否能稳定映射到导出的 raw buffer，并产生小窗口证据。

M1 验证通过后，再做 M2。UE layout 和 bundle hint 的价值很高，但它们依赖 M1 的证据链质量。如果没有稳定的 load/resource/window 链路，过早引入 layout 和参数名只会制造看起来合理但不可核对的推断。

