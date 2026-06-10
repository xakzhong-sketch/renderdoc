# RenderDoc CPD Summary Analyzer 任务拆解

来源方案：`AIDoc/CPDSummaryAnalyzerImplementationPlan.md`

## 总体目标

新增一个独立的 CPD Summary Analyzer，用于消费 `Export Current Drawcall Data` 生成的导出目录，并结合 UE 解包数据和 UE layout 配置，生成面向 Unity Shader 还原的 Custom Primitive Data / PrimitiveSceneData / InstanceData 证据摘要。

核心产物：

- `Analysis/CPD/custom_primitive_data_summary.md`
- `Analysis/CPD/custom_primitive_data_summary.json`
- `Analysis/CPD/resource_candidates.json`
- `Analysis/CPD/shader_loads.json`
- `Analysis/CPD/buffer_windows.json`
- `Analysis/CPD/unresolved_loads.json`

## 完成标准

- 可以通过命令行对一个已有 drawcall export 目录运行分析。
- 不修改原始 export 文件，只在 `Analysis/CPD` 下写入分析结果。
- 能读取 `manifest.json`、`drawcall.json`、`pipeline_state.json`、`shader_reconstruction_index.json`、`ConstantBuffers/constant_buffers.json`、`ResourceBuffers/resource_buffers.json` 和 `Shaders/<stage>` disassembly。
- 能定位 Primitive/GPUScene/Instance/CustomPrimitiveData 相关候选资源。
- 能解析 D3D11 DXBC disassembly 中的 cbuffer、structured buffer、raw buffer load 证据。
- 能将 shader load 映射到导出的 cbuffer/resource buffer raw 文件。
- 能截取小窗口并输出 float/uint/int/hex 解码结果。
- 能生成 Markdown 报告，明确区分事实证据、推断和未解问题。
- 缺少 optional 输入时仍可运行；缺少必要输入时给出明确错误。

## 任务依赖图

```text
M1 最小可用链路

T01 Tool Scaffolding
  -> T02 Shared Schemas
    -> T03 Export Index Loader
      -> T04 Resource Candidate Locator
      -> T05 Disassembly Discovery
        -> T06 DXBC CBuffer Load Parser
        -> T07 DXBC Structured/Raw Load Parser
          -> T08 Shader Load Normalizer
            -> T09 Register/Resource Resolver
              -> T10 Buffer Window Decoder
                -> T11 Buffer Window Extractor
                  -> T12 Report JSON Writer
                  -> T13 Report Markdown Writer
                    -> T14 CLI
                      -> T15 M1 Tests
                      -> T16 M1 Sample Validation

M2 UE layout + bundle hint

T17 UE Layout Parser
  -> T18 Layout Matcher
T19 Bundle Hint Loader
  -> T20 Bundle Hint Matcher
T04/T09/T11/T18/T20
  -> T21 Finding Scorer
    -> T22 M2 Report Upgrade
      -> T23 M2 Tests + Sample Validation

M3 Workflow 集成

T24 Update Goal Template
  -> T25 Update Export Workflow Docs
    -> T26 Workflow Validation

M4 可选 UI 集成

T27 Analyzer Process Wrapper
  -> T28 qrenderdoc UI Entry
    -> T29 UI Smoke Validation
```

## M1. 最小可用 CPD Summary

M1 目标是先打通证据链，不引入 UE layout 和 bundle hint。只要能从 shader disassembly 追到资源、raw 文件和小窗口数据，就可以开始验证这个方向是否有效。

## T01. 建立工具目录和 CLI 骨架

优先级：P0

目标：

- 新增独立 Python 工具目录。
- 提供可运行但暂不完整分析的 CLI 入口。

涉及文件：

- `tools/shader_reconstruction/cpd_summary.py`
- `tools/shader_reconstruction/cpd/__init__.py`
- `tools/shader_reconstruction/cpd/errors.py`
- `tools/shader_reconstruction/README.md`

实现要点：

- 使用标准库优先：`argparse`、`json`、`pathlib`、`dataclasses`、`struct`、`re`。
- CLI 参数先占位：
  - `--dc-export`
  - `--bundle`
  - `--ue-layout`
  - `--out`
  - `--max-window-bytes`
  - `--max-report-findings`
  - `--include-low-priority`
  - `--stage`
  - `--verbose`
- 默认输出目录为 `<dc-export>/Analysis/CPD`。
- CLI 可以创建输出目录，但不写分析结果以外的文件。

验收标准：

- `python tools/shader_reconstruction/cpd_summary.py --help` 正常输出。
- 不传 `--dc-export` 返回非零 exit code。
- 传入不存在的 export 目录时输出明确错误。
- 不依赖 qrenderdoc 编译。

依赖：无。

## T02. 定义共享数据结构和 JSON schema 约定

优先级：P0

目标：

- 用统一的数据结构贯穿 loader、parser、resolver、window extractor 和 report writer。

涉及文件：

- `tools/shader_reconstruction/cpd/schemas.py`

建议数据结构：

- `ExportContext`
- `ShaderStageInfo`
- `ConstantBufferRecord`
- `ResourceBufferRecord`
- `ShaderDisassemblyFile`
- `ResourceCandidate`
- `ShaderLoad`
- `ResolvedShaderLoad`
- `BufferWindow`
- `AnalyzerWarning`
- `AnalyzerResult`

实现要点：

- 使用 `dataclasses.dataclass`。
- 所有输出 JSON 都经过 `to_dict()`，避免直接 dump Python 对象。
- 路径字段统一存 export root 相对路径。
- stage 使用小写短名：`vs`、`ps`、`cs`、`hs`、`ds`、`gs`、`ms`、`as`。
- confidence 使用 `0.0 - 1.0`。
- parse/resolution 状态使用稳定字符串枚举。

验收标准：

- schema 文件不依赖其它模块，避免循环引用。
- 每个 schema 都有最小字段和 optional 字段区分。
- JSON 输出字段名稳定，后续任务不能随意重命名。

依赖：T01。

## T03. 实现 Export Index Loader

优先级：P0

目标：

- 从 drawcall export 目录读取基础索引，建立统一的资源视图。

涉及文件：

- `tools/shader_reconstruction/cpd/export_index.py`

输入文件：

- `manifest.json`
- `drawcall.json`
- `pipeline_state.json`
- `shader_reconstruction_index.json`
- `ConstantBuffers/constant_buffers.json`
- `ResourceBuffers/resource_buffers.json`

实现要点：

- 对必需文件和可选文件分级：
  - 必需：export root 存在。
  - 强建议：`manifest.json`、`drawcall.json`。
  - 可缺失但警告：`shader_reconstruction_index.json`、`constant_buffers.json`、`resource_buffers.json`。
- 读取 JSON 时保留原始顶层对象，供后续规则扩展。
- 提取 capture context：
  - event id
  - API
  - draw/dispatch name
  - active stages
- 加载 constant buffer index：
  - stage
  - binding / slot
  - display name
  - resource id
  - json/csv/raw path
  - byte size
- 加载 resource buffer index：
  - stage
  - binding / slot
  - descriptor type
  - shader resource name
  - RenderDoc resource name
  - raw file path
  - byte offset / byte size
  - stride / format if present
  - priority / guidance if present
- 将不存在的 raw 文件记录为 warning，不直接失败。

验收标准：

- 能输出 export 统计：
  - event id
  - API
  - stages count
  - cbuffer count
  - resource buffer count
  - warnings count
- 对空或旧版本 export 目录能降级运行并输出 warning。
- 所有路径在内存中统一为 `Path`，JSON 输出统一为相对路径字符串。

依赖：T02。

## T04. 实现 Resource Candidate Locator

优先级：P0

目标：

- 从 cbuffer/resource buffer 中筛出可能承载 CPD、PrimitiveSceneData、GPUScene、InstanceData 的资源。

涉及文件：

- `tools/shader_reconstruction/cpd/resource_candidates.py`

高优先级关键词：

- `PrimitiveUniformBuffer`
- `PrimitiveSceneData`
- `GPUScene`
- `InstanceSceneData`
- `InstancePayloadData`
- `CustomPrimitiveData`
- `PerInstanceCustomData`
- `PrimitiveData`
- `InstanceData`

低优先级关键词：

- `ZBin`
- `Z-Bin`
- `Tile`
- `Cluster`
- `LightList`
- `Culling`
- `HZB`
- `DepthPyramid`
- `Shadow`

实现要点：

- 输入为 `ExportContext`。
- 对 constant buffer 和 resource buffer 都生成 `ResourceCandidate`。
- score 来源：
  - resource name / display name / shader resource name 命中。
  - stage 是否为 active stage。
  - resource 类型是否为 structured/raw/byte address。
  - stride 是否接近常见结构化场景数据。
  - 文件是否存在且未明显截断。
  - 现有 export 中的 priority/guidance。
- 低优先级资源不删除，只标记 `priority: low` 和 `reason`。

输出：

- `resource_candidates.json`

验收标准：

- `PrimitiveSceneData`、`GPUScene`、`InstanceSceneData` 命中后能排到前面。
- `URP Z-Bin Buffer`、`URP Tile Buffer`、light-list 类资源会被标为 low priority。
- 无名 buffer 也可以基于类型和访问信息进入 medium/low candidate。
- 每个 candidate 都有 reasons，不能只有一个分数。

依赖：T03。

## T05. 实现 Disassembly 文件发现

优先级：P0

目标：

- 找到每个 stage 可用于解析 shader load 的 disassembly 文件。

涉及文件：

- `tools/shader_reconstruction/cpd/disassembly_loads.py`

实现要点：

- 扫描：
  - `Shaders/<stage>/disassembly_native_*.txt`
  - `Shaders/<stage>/disassembly_default.txt`
  - `Shaders/<stage>/disassembly_hlsl_*.txt`
- MVP 优先解析 native/default，不把 HLSL decompiler 输出作为主要证据。
- 为每个文件判断 backend：
  - `dxbc`
  - `dxil`
  - `spirv`
  - `glsl`
  - `hlsl_decompiled`
  - `unknown`
- backend 判断基于文件名和文本头部关键字。
- 记录 line count、stage、relative path。

验收标准：

- 能找到 VS/PS/CS 的 disassembly 文件。
- 同一 stage 有多个文件时，优先顺序稳定。
- 未识别 backend 时仍保留文件记录，但 parser 可跳过并输出 warning。

依赖：T03。

## T06. 实现 DXBC Constant Buffer Load Parser

优先级：P0

目标：

- 解析 DXBC disassembly 中 constant buffer 访问。

涉及文件：

- `tools/shader_reconstruction/cpd/disassembly_loads.py`
- `tools/shader_reconstruction/tests/fixtures/dxbc_cbuffer_loads.txt`

需要识别的典型模式：

```text
cb0[12].xyzw
cb1[r0.x + 3].z
dcl_constantbuffer cb0[...]
```

实现要点：

- 输出 `ShaderLoad`：
  - `resource_class = "constant_buffer"`
  - `register = "cb0"`
  - `slot = 0`
  - `element_index_expr`
  - `member_offset_expr`
  - `channels`
  - `line_number`
  - `instruction_text`
- 对 `cb0[12]` 计算 `byte_offset = 12 * 16`。
- 对 `cb0[12].z` 记录 component offset，但 window 仍按 float4 register 展示。
- 对动态索引保留表达式，`byte_offset = null`，`parse_status = unresolved_dynamic_index`。

验收标准：

- 静态 cbuffer offset 可以解析出 slot 和 byte offset。
- 动态 cbuffer offset 不丢弃，能进入 unresolved 输出。
- 每条记录能回到原 disassembly 文件和行号。

依赖：T05。

## T07. 实现 DXBC Structured/Raw Buffer Load Parser

优先级：P0

目标：

- 解析 DXBC disassembly 中 SRV/UAV structured/raw/byte address buffer load。

涉及文件：

- `tools/shader_reconstruction/cpd/disassembly_loads.py`
- `tools/shader_reconstruction/tests/fixtures/dxbc_buffer_loads.txt`

需要识别的典型模式：

```text
ld_structured
ld_raw
ld
t12
u3
```

实现要点：

- 解析 instruction op：
  - `ld_structured`
  - `ld_raw`
  - `ld`
  - 其它 load 类 op 先保守记录。
- 提取：
  - SRV register：`tN`
  - UAV register：`uN`
  - element index expression
  - byte/member offset expression
  - channels / mask
- 静态 offset 可解析时计算 byte offset。
- 动态 offset 不强行猜测，记录 expression。

验收标准：

- 能识别 structured/raw buffer 的 register slot。
- 无法解析 offset 的指令进入 `unresolved_loads.json`。
- 不把 texture sample 误认为 CPD buffer load。

依赖：T05。

## T08. Shader Load Normalizer

优先级：P0

目标：

- 将不同 parser 输出的 load 统一成稳定 schema，方便 resolver 消费。

涉及文件：

- `tools/shader_reconstruction/cpd/disassembly_loads.py`

实现要点：

- 为每条 load 分配稳定 id：`<stage>:<file_index>:<line_number>:<ordinal>`。
- 统一 register 表示：
  - `cb0`
  - `t12`
  - `u3`
- 统一 resource class：
  - `constant_buffer`
  - `structured_buffer`
  - `raw_buffer`
  - `byte_address_buffer`
  - `uav_buffer`
  - `texture`
  - `unknown`
- 分离 `parse_status`：
  - `parsed_static_offset`
  - `parsed_dynamic_offset`
  - `partial`
  - `unsupported_backend`
  - `unrecognized`

输出：

- `shader_loads.json`
- `unresolved_loads.json`

验收标准：

- 所有 parser 输出都能 JSON 序列化。
- `shader_loads.json` 中包括 parsed 和 partial 记录。
- `unresolved_loads.json` 只包含需要人工复核或后续 parser 增强的记录。

依赖：T06、T07。

## T09. Register/Resource Resolver

优先级：P0

目标：

- 将 shader load 中的 register/slot 映射到 `constant_buffers.json` 或 `resource_buffers.json` 中的导出记录。

涉及文件：

- `tools/shader_reconstruction/cpd/resolver.py`

实现要点：

- 建立 lookup：
  - `(stage, "constant_buffer", slot)`
  - `(stage, "srv", slot)`
  - `(stage, "uav", slot)`
  - `(stage, shader_resource_name)`
- DXBC 映射：
  - `cb0` -> cbuffer slot 0
  - `t12` -> SRV slot 12
  - `u3` -> UAV slot 3
- 如果当前 stage 找不到，允许 fallback 到同名 active stage resource，但 confidence 降低。
- 记录 resolution 状态：
  - `resolved`
  - `missing_index`
  - `missing_raw_file`
  - `ambiguous_slot`
  - `unsupported_resource_class`
  - `stage_mismatch`
- 不修改原 `ShaderLoad`，输出 `ResolvedShaderLoad`。

验收标准：

- cbuffer load 能映射到对应 cbuffer raw/json/csv。
- SRV/UAV load 能映射到对应 resource buffer raw。
- 不能映射的 load 有清晰失败原因。
- 解析结果能回链到原 resource candidate。

依赖：T03、T08。

## T10. Binary Window Decoder

优先级：P0

目标：

- 实现小窗口二进制解码能力。

涉及文件：

- `tools/shader_reconstruction/cpd/buffer_windows.py`
- `tools/shader_reconstruction/tests/test_buffer_windows.py`

实现要点：

- 支持 little-endian：
  - float32
  - uint32
  - int32
  - hex bytes
- 输出按 16-byte 行组织：
  - offset
  - float4
  - uint4
  - int4
  - hex16
- 对不足 16 bytes 的尾部安全处理。
- 对 NaN/Inf 做 JSON 安全输出，例如字符串或 null + raw。

验收标准：

- 给定固定 binary fixture，解码结果稳定。
- 不因窗口越界崩溃。
- 输出 JSON 不包含 Python 特有类型。

依赖：T02。

## T11. Buffer Window Extractor

优先级：P0

目标：

- 根据 resolved shader load 截取对应 raw 文件的小窗口。

涉及文件：

- `tools/shader_reconstruction/cpd/buffer_windows.py`

实现要点：

- 默认窗口大小 `256` bytes。
- cbuffer：
  - offset 按 16-byte register 对齐。
  - 覆盖命中 register 前后若干 float4。
- structured buffer：
  - 如果有 stride，按 element 周边截取。
  - 如果无 stride，按 byte offset 截取。
- raw/byte address buffer：
  - 按 byte offset 截取，向下 16-byte 对齐。
- 对动态 offset：
  - 不截取或只截取资源头部小窗口，标记原因。
- 对 raw 文件缺失或 offset 超出范围：
  - 输出 warning 和 skipped window。
- 对已知低优先级资源，默认不生成大量窗口，除非 `--include-low-priority`。

输出：

- `buffer_windows.json`

验收标准：

- high priority candidate 的静态 offset load 能生成窗口。
- 窗口大小受 `--max-window-bytes` 控制。
- 不会把完整大 buffer 写进 summary。
- 截断 raw 文件会明确输出 `raw_truncated` 或 `offset_outside_exported_range`。

依赖：T09、T10。

## T12. Report JSON Writer

优先级：P0

目标：

- 写出完整机器可读结果。

涉及文件：

- `tools/shader_reconstruction/cpd/report_writer.py`

输出文件：

- `custom_primitive_data_summary.json`
- `resource_candidates.json`
- `shader_loads.json`
- `buffer_windows.json`
- `unresolved_loads.json`

实现要点：

- JSON 顶层带版本号：
  - `version: 1`
- summary JSON 包含：
  - input
  - capture
  - counts
  - warnings
  - resource candidates summary
  - resolved load summary
  - buffer window summary
- 中间文件保留完整记录。
- 所有路径相对 export root。

验收标准：

- 任何输出 JSON 都可被 `python -m json.tool` 解析。
- warnings 不只写到 console，也写入 JSON。
- 多次运行可覆盖 `Analysis/CPD` 下旧结果。

依赖：T04、T08、T09、T11。

## T13. Report Markdown Writer

优先级：P0

目标：

- 生成 Codex/AI Agent 可直接阅读的 CPD 摘要。

涉及文件：

- `tools/shader_reconstruction/cpd/report_writer.py`

文档结构：

- `Capture Context`
- `Top Resource Candidates`
- `Resolved Shader Load Evidence`
- `Buffer Windows`
- `Unresolved Loads`
- `Warnings`
- `Next Agent Steps`

实现要点：

- 默认只展示：
  - top 10 resource candidates
  - top 20 resolved loads
  - top 20 buffer windows
  - top 20 unresolved loads
- 大量明细只放 JSON。
- 每条 evidence 必须带：
  - stage
  - register
  - resource name
  - raw file
  - disassembly file + line
- 明确写出：
  - “observed runtime evidence”
  - “inference”
  - “unresolved”

验收标准：

- 打开 Markdown 后能直接判断下一步优先看哪些资源。
- 低优先级系统 buffer 不抢占报告主体。
- 不把 raw buffer 大内容贴进 Markdown。

依赖：T12。

## T14. CLI 串联分析流程

优先级：P0

目标：

- 将 loader、candidate locator、parser、resolver、window extractor、report writer 串成完整命令。

涉及文件：

- `tools/shader_reconstruction/cpd_summary.py`

实现流程：

```text
parse args
  -> validate paths
  -> load export index
  -> find resource candidates
  -> discover disassembly files
  -> parse shader loads
  -> resolve loads to resources
  -> extract buffer windows
  -> write JSON reports
  -> write Markdown report
```

验收标准：

- 命令能完整生成 `Analysis/CPD`。
- `--verbose` 输出各阶段 count。
- 阶段 warning 不导致整个流程失败。
- 只有必要输入缺失或不可读时返回非零 exit code。

依赖：T03、T04、T05、T08、T09、T11、T12、T13。

## T15. M1 单元测试

优先级：P0

目标：

- 覆盖 M1 的核心解析和解码逻辑。

涉及文件：

- `tools/shader_reconstruction/tests/test_export_index.py`
- `tools/shader_reconstruction/tests/test_disassembly_loads.py`
- `tools/shader_reconstruction/tests/test_resolver.py`
- `tools/shader_reconstruction/tests/test_buffer_windows.py`
- `tools/shader_reconstruction/tests/fixtures/`

测试项：

- export index 缺文件降级。
- DXBC cbuffer 静态 offset。
- DXBC cbuffer 动态 offset。
- DXBC structured/raw load register 提取。
- register 到 resource 映射。
- raw 文件不存在。
- offset 超出 raw 文件范围。
- float/uint/int/hex 解码。

验收标准：

- 使用标准 `unittest` 或 `pytest` 均可，但依赖要明确。
- 测试 fixture 小而稳定，不提交大 capture/raw 文件。
- 单测能在没有 RenderDoc GUI 的环境中运行。

依赖：T14。

## T16. M1 样本验证

优先级：P0

目标：

- 用真实导出目录验证 M1 是否能解决实际问题。

样本类型：

- D3D11 常规 VS/PS GBuffer drawcall。
- D3D11/UE5 Nanite 或 compute GBuffer dispatch。
- 一个含明显 CustomPrimitiveData 或 PerInstanceCustomData 迹象的 drawcall。

验证命令：

```powershell
python tools/shader_reconstruction/cpd_summary.py `
  --dc-export "K:/WorkSpace/Profiler/Test003/Ref/<event_dir>" `
  --verbose
```

验收标准：

- 生成 `Analysis/CPD/custom_primitive_data_summary.md`。
- Top candidates 中能看到 Primitive/GPUScene/Instance 相关资源。
- shader load evidence 能回到 disassembly 行号。
- buffer window 输出不超过配置大小。
- Nanite/CS 场景中未解索引链被明确列出，而不是误判为确定 CPD。

依赖：T15。

## M2. UE Layout + Bundle Hint

M2 在 M1 证据链稳定后再做。目标是把 “看到的 runtime buffer evidence” 和 “UE layout/解包参数名” 结合起来，形成更强的 CPD finding。

## T17. UE Layout Parser

优先级：P1

目标：

- 读取外部 UE layout JSON，不把 UE 版本布局硬编码进 analyzer。

涉及文件：

- `tools/shader_reconstruction/cpd/ue_layout.py`
- `tools/shader_reconstruction/layouts/UE5_5_GPUScene.example.json`
- `tools/shader_reconstruction/layouts/UE5_6_GPUScene.example.json`

实现要点：

- 支持 layout block：
  - `PrimitiveSceneData`
  - `InstanceSceneData`
  - `InstancePayloadData`
  - `CustomPrimitiveData`
- 支持字段：
  - name
  - offset
  - type
  - count
  - stride_bytes
- 读取失败时降级为 no-layout mode。
- layout 文件必须带 `engine`、`source`、`version`。

验收标准：

- 没有 `--ue-layout` 时 M1 仍可运行。
- layout JSON 格式错误时输出 warning 或 error，行为明确。
- example layout 文件不宣称绝对准确，必须标注需要项目验证。

依赖：T14。

## T18. Layout Matcher

优先级：P1

目标：

- 将 resolved load 或 buffer window offset 匹配到 layout 字段。

涉及文件：

- `tools/shader_reconstruction/cpd/ue_layout.py`

匹配方式：

- exact field offset。
- inside field range。
- nearest field。

输出字段：

- `layout_name`
- `field_name`
- `field_relative_offset`
- `match_kind`
- `confidence`

验收标准：

- offset 落在 `CustomPrimitiveData` 范围内时能标记。
- offset 接近但不在范围内时只给 low/medium confidence。
- layout match 不会单独生成 high confidence finding，必须结合 shader load/resource evidence。

依赖：T17、T11。

## T19. Bundle Hint Loader

优先级：P1

目标：

- 从 UE 解包目录或上游 Agent 结果中提取参数名 hint。

涉及文件：

- `tools/shader_reconstruction/cpd/bundle_hints.py`

优先读取：

- `analysis/material_layer_parameter_bindings.json`
- `parameters/material_parameters.json`
- `parameters/textures.json`

实现要点：

- 递归扫描可配置最大深度，避免扫完整工程导致过慢。
- 对 JSON 提取 key/value 中看起来像参数名的字符串。
- 对文本文件做轻量正则扫描。
- 重点关键词：
  - `CustomPrim`
  - `CustomPrimitive`
  - `PerInstance`
  - `PrimitiveData`
  - `Blend`
  - `Mask`
  - `Layer`
  - `Variation`
- 分类：
  - scalar
  - vector
  - texture
  - switch
  - unknown

输出：

- `bundle_hints.json`

验收标准：

- 找到 `_Blend4_CustomPrim_Mask` 这类参数名。
- 不因为 bundle hint 存在就直接认定 runtime value。
- 没有 `--bundle` 时不影响主流程。

依赖：T14。

## T20. Bundle Hint Matcher

优先级：P1

目标：

- 将 bundle 参数名 hint 和资源/load/window 证据建立弱关联。

涉及文件：

- `tools/shader_reconstruction/cpd/bundle_hints.py`

实现要点：

- 基于关键词和语义类别做加分，不做强结论。
- 将 hint 关联到可能的 finding：
  - `CustomPrim` -> CPD candidate。
  - `PerInstance` -> InstanceData candidate。
  - `Blend/Mask/Layer` -> material parameter candidate。
- 输出 matched/unmatched hints。

验收标准：

- 同名或高相关参数能提升 finding confidence。
- 只有 bundle hint、没有 runtime shader/resource evidence 的项不能进入 high confidence。
- Markdown 中明确 hint 来源。

依赖：T19。

## T21. Finding Scorer

优先级：P1

目标：

- 合并 resource candidate、shader load、buffer window、layout match、bundle hint，生成最终 findings。

涉及文件：

- `tools/shader_reconstruction/cpd/finding_scorer.py`

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

实现要点：

- 输出 finding：
  - name
  - kind
  - confidence
  - confidence_label
  - source resource
  - shader evidence ids
  - window ids
  - layout hits
  - bundle hints
  - reasons
  - caveats
- confidence label：
  - high >= 0.75
  - medium >= 0.45
  - low < 0.45
- 合并重复 finding，避免同一个 offset 产生大量重复项。

验收标准：

- high confidence finding 必须至少包含 shader load evidence 和 resource evidence。
- 每个分数变化都有 reason。
- low confidence 不隐藏，但 Markdown 中排序靠后。

依赖：T04、T09、T11、T18、T20。

## T22. M2 Report Upgrade

优先级：P1

目标：

- 把 findings、layout hits、bundle hints 写入 summary JSON/Markdown。

涉及文件：

- `tools/shader_reconstruction/cpd/report_writer.py`

Markdown 新增章节：

- `High Confidence Findings`
- `Medium Confidence Findings`
- `Bundle Hints`
- `UE Layout Matches`
- `Caveats for Unity Shader Reconstruction`

验收标准：

- AI Agent 先看 findings，再看 raw evidence。
- 报告中事实和推断分离。
- `_Blend4_CustomPrim_Mask` 类参数若出现，能看到来源和证据强度。

依赖：T21。

## T23. M2 测试和样本验证

优先级：P1

目标：

- 验证 layout/hint/scoring 不制造不可核对的结论。

测试项：

- layout exact match。
- layout inside range。
- layout mismatch。
- bundle hint 提取。
- hint only 不生成 high confidence。
- hint + shader evidence + layout match 生成 medium/high。

样本验证：

```powershell
python tools/shader_reconstruction/cpd_summary.py `
  --dc-export "K:/WorkSpace/Profiler/Test003/Ref/<event_dir>" `
  --bundle "K:/WorkSpace/<unpacked_ue_project_analysis>" `
  --ue-layout "tools/shader_reconstruction/layouts/UE5_5_GPUScene.example.json" `
  --verbose
```

验收标准：

- findings 的 top 项能明显帮助 Unity Shader 参数还原。
- Nanite/CS 场景仍然保留不确定性说明。
- 没有 layout/bundle 时 M1 行为不退化。

依赖：T22。

## M3. Unity Shader 还原工作流集成

## T24. 更新 Goal 模板

优先级：P2

目标：

- 让导出的 `UnityShaderReconstructionGoal.md` 能主动使用 CPD Summary。

涉及文件：

- `AIDoc/UnityShaderReconstructionGoalTemplate.md`

实现要点：

- 增加步骤：
  - 检查 `Analysis/CPD/custom_primitive_data_summary.md` 是否存在。
  - 不存在时提示运行 `cpd_summary.py`。
  - UE5/Nanite/CS GBuffer 场景优先查看 CPD/InstanceData evidence。
- 强调：
  - CPD summary 是 runtime evidence。
  - 解包 material 参数是 static hint。

验收标准：

- 用户在 export 目录运行 `/goal UnityShaderReconstructionGoal.md` 后，Agent 会自然进入 CPD 分析流程。
- 文档不要求用户手动阅读大 raw buffer。

依赖：T16 或 T23。

## T25. 更新导出包工作流文档生成

优先级：P2

目标：

- 让 qrenderdoc 导出生成的 `AGENTS.md` / `UnityShaderReconstructionWorkflow.md` 知道 CPD analyzer。

涉及文件：

- `qrenderdoc/Code/DrawcallExport.cpp`
- `qrenderdoc/Code/DrawcallExport.h`

实现要点：

- 如果 `Analysis/CPD/custom_primitive_data_summary.md` 已存在，文档中优先引用它。
- 如果不存在，文档中给出 CLI 示例。
- 不在基础 export 流程中自动运行 analyzer。

验收标准：

- 新导出的目录中 workflow 文档包含 CPD Summary Analyzer 步骤。
- 不影响现有 one-click export 成功率。

依赖：T24。

## T26. Workflow 验证

优先级：P2

目标：

- 验证导出后直接 `/goal UnityShaderReconstructionGoal.md` 的流程可用。

验收步骤：

- 选择一个 UE5/Nanite/CS GBuffer drawcall 导出。
- 运行 CPD analyzer。
- 在 export 目录中启动 Codex goal。
- 检查 Agent 是否按顺序读取：
  - `UnityShaderReconstructionGoal.md`
  - `Analysis/CPD/custom_primitive_data_summary.md`
  - shader/resource/window evidence。

验收标准：

- Agent 不再优先读取大 raw buffer。
- Agent 能明确列出需要还原的 Unity shader 参数候选。
- Agent 能标注无法确定的 Nanite/Instance 索引链。

依赖：T25。

## M4. 可选 qrenderdoc UI 集成

M4 只有在 CLI 稳定后再做。UI 不应该承载 UE layout 和 bundle 规则，只负责调用 analyzer。

## T27. Analyzer Process Wrapper

优先级：P3

目标：

- 在 qrenderdoc 中封装外部 analyzer 进程调用。

涉及文件：

- `qrenderdoc/Code/DrawcallExport.cpp`
- `qrenderdoc/Code/DrawcallExport.h`
- 可能新增 `qrenderdoc/Code/ShaderReconstructionAnalyzer.*`

实现要点：

- 用户配置 analyzer script 路径或自动查找 repo 内 `tools/shader_reconstruction/cpd_summary.py`。
- 传入 export dir、bundle dir、layout path。
- stdout/stderr 写入：
  - `Analysis/CPD/cpd_summary.log`
- 外部进程失败时不影响基础 export。

验收标准：

- qrenderdoc 能启动 analyzer。
- analyzer 失败时 UI 显示错误摘要。
- log 文件可用于排查。

依赖：T23。

## T28. qrenderdoc UI 入口

优先级：P3

目标：

- 给用户提供可选 GUI 操作入口。

建议入口：

- `Tools -> Run CPD Summary Analyzer...`
- 或导出完成 dialog 上增加 `Run CPD Summary Analyzer`。

实现要点：

- 选择 export dir。
- 可选选择 bundle dir。
- 可选选择 UE layout JSON。
- 调用 T27 wrapper。

验收标准：

- 未选择 bundle/layout 时也能运行。
- UI 不阻塞主线程。
- 运行完成后提示 report 路径。

依赖：T27。

## T29. UI Smoke Validation

优先级：P3

目标：

- 验证 qrenderdoc UI 调用不会破坏现有导出流程。

测试项：

- 不加载 capture 时菜单状态。
- 已有 export 目录运行 analyzer。
- 缺少 Python 时错误提示。
- analyzer 返回非零 exit code。
- 成功生成 report 后打开目录。

验收标准：

- 基础 drawcall export 不受影响。
- analyzer 错误可恢复，不导致 qrenderdoc 崩溃。

依赖：T28。

## 建议实施顺序

第一轮只做 M1：

```text
T01 -> T02 -> T03 -> T04 -> T05 -> T06 -> T07 -> T08 -> T09 -> T10 -> T11 -> T12 -> T13 -> T14 -> T15 -> T16
```

M1 通过真实样本验证后，再做 M2：

```text
T17 -> T18 -> T19 -> T20 -> T21 -> T22 -> T23
```

M3 可以在 M2 前后穿插，但建议至少等 M1 有稳定输出后再更新 goal/template，避免文档承诺超过工具能力。

M4 暂缓。只有当 CLI 对多个真实样本稳定后，再把入口挂进 qrenderdoc。

## 当前最小开工集合

如果下一步要直接实现，建议先创建以下文件：

```text
tools/shader_reconstruction/cpd_summary.py
tools/shader_reconstruction/cpd/__init__.py
tools/shader_reconstruction/cpd/schemas.py
tools/shader_reconstruction/cpd/export_index.py
tools/shader_reconstruction/cpd/resource_candidates.py
tools/shader_reconstruction/cpd/disassembly_loads.py
tools/shader_reconstruction/cpd/resolver.py
tools/shader_reconstruction/cpd/buffer_windows.py
tools/shader_reconstruction/cpd/report_writer.py
tools/shader_reconstruction/tests/
```

第一版不用立刻接 qrenderdoc UI，也不用立刻解析 bundle/layout。先让 `--dc-export` 单输入能产出一份可靠的 CPD evidence report。

