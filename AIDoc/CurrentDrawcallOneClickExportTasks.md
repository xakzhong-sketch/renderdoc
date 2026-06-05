# RenderDoc 选中 Drawcall 一键导出任务拆解

来源方案：`AIDoc/CurrentDrawcallOneClickExportPlan.md`

## 总体目标

在 qrenderdoc 中增加“Export Current Drawcall Data”能力。用户选中一个 drawcall 后，一次性导出 AI Agent 还原 shader 所需的数据包，包括纹理、mesh、shader、constant buffer、pipeline/drawcall 状态和资源绑定关系。

最终导出包必须机器可读，不能依赖 AI Agent 解析 HTML 或从文件名猜测资源语义。文件名只作为稳定可读标识，完整语义以 `manifest.json`、`drawcall.json`、`pipeline_state.json` 为准。

## 完成标准

- 主菜单可以对当前选中 drawcall 导出数据包。
- 每次导出创建独立目录，不覆盖其它事件的数据。
- 导出目录包含 `manifest.json`、`drawcall.json`、`pipeline_state.html`、`pipeline_state.json`。
- VS/PS 的 disassembly、raw shader bytes、reflection metadata 能导出。
- VS/PS constant buffers 能导出 CSV、JSON、raw bytes。
- VS/PS 使用的 input textures、samplers、output render targets/depth 能导出。
- Mesh 至少包含 post-transform OBJ、vertex input layout、相关 VB/IB raw bytes。
- 失败项不会中断整个导出，必须写入 manifest。
- 对 D3D11、D3D12、Vulkan、GL 至少完成 smoke validation。

## 任务依赖图

```text
T01 UI Entry
  -> T02 Exporter Skeleton
    -> T03 File Naming + Manifest Registry
      -> T04 Drawcall JSON
      -> T05 Pipeline HTML Refactor
      -> T06 Pipeline JSON
      -> T07 Texture + Sampler Export
      -> T08 Shader Export
      -> T09 Constant Buffer Export
      -> T10 Mesh Export
      -> T11 Resource Buffer Export
        -> T12 Final Manifest + Summary
          -> T13 Event Browser Context Menu
          -> T14 Validation
```

## T01. 增加主菜单入口

优先级：P0

目标：

- 在主菜单加入 `File -> Export Current Drawcall Data...`。
- 在无 capture、无当前 action、或当前事件不适用时禁用该 action。

涉及文件：

- `qrenderdoc/Windows/MainWindow.ui`
- `qrenderdoc/Windows/MainWindow.h`
- `qrenderdoc/Windows/MainWindow.cpp`

实现要点：

- 新增 `action_Export_Current_Drawcall_Data`。
- 新增 slot：`on_action_Export_Current_Drawcall_Data_triggered()`。
- slot 中选择导出根目录，并调用后续 `DrawcallExporter`。
- 菜单启用状态随 capture loaded/closed 和 event changed 更新。

验收标准：

- 未加载 capture 时菜单不可用。
- 加载 capture 并选中 drawcall 后菜单可用。
- 点击菜单能弹出目录选择框。
- 取消选择目录不会报错。

依赖：无。

## T02. 新增 DrawcallExporter 骨架

优先级：P0

目标：

- 新增 UI 无关的批量导出调度类。
- 实现目录创建、选中 event/action 检查、分步骤执行和错误收集。

涉及文件：

- `qrenderdoc/Code/DrawcallExport.h`
- `qrenderdoc/Code/DrawcallExport.cpp`
- 构建文件，如 qmake/cmake/vcxproj 中 qrenderdoc 源文件列表

建议接口：

```cpp
struct DrawcallExportOptions
{
  bool exportInputs = true;
  bool exportOutputs = true;
  bool exportMeshOBJ = true;
  bool exportMeshRawBuffers = true;
  bool exportPipelineHTML = true;
  bool exportPipelineJSON = true;
  bool exportShaders = true;
  bool exportShaderRawBytes = true;
  bool exportShaderSourceFiles = true;
  bool exportShaderReflectionJSON = true;
  bool exportConstantBuffers = true;
  bool exportResourceBuffers = true;
  bool exportSamplerState = true;
  bool onlyUsedDescriptors = true;
};

class DrawcallExporter
{
public:
  DrawcallExporter(ICaptureContext &ctx, QWidget *parent);
  ResultDetails ExportCurrentDrawcall(const QString &rootDir, const DrawcallExportOptions &opts);
};
```

实现要点：

- 捕获 `m_Ctx.CurEvent()` 和 `m_Ctx.CurAction()`。
- 创建 `EID_<eventId>_<safe_action_name>` 子目录。
- 使用 `m_Ctx.Replay().BlockInvoke(...)` 包装 replay API 调用。
- 所有子任务返回统一 `ExportItemResult`，包含 success、path、category、error。

验收标准：

- 空实现也能创建导出目录和基础 `manifest.json`。
- 当前事件无 action 时返回明确错误，不崩溃。
- 一个子任务失败不会影响后续子任务执行。

依赖：T01。

## T03. 稳定文件命名和 Manifest Registry

优先级：P0

目标：

- 提供统一文件命名、去重、原始名称映射和 manifest 写入能力。

涉及文件：

- `qrenderdoc/Code/DrawcallExport.h`
- `qrenderdoc/Code/DrawcallExport.cpp`

实现要点：

- 文件名格式：`category__stage_or_role__binding__safe_resource_name__resourceId.ext`。
- stage 短名固定：`vs`、`hs`、`ds`、`gs`、`ps`、`cs`、`task`、`mesh`。
- binding 固定宽度：`t000`、`s003`、`b012`、`u001`、`rt0`、`depth`。
- Windows 非法字符替换为 `_`，连续 `_` 压缩，资源名长度限制到 64 字符。
- manifest entry 必须记录：category、stage、binding、resourceId、originalName、path、descriptor/subresource metadata。

验收标准：

- 同一资源在不同 stage/binding 下不会覆盖。
- 文件名中没有非法字符。
- manifest 可以从文件反查资源，也可以从资源反查文件。

依赖：T02。

## T04. 导出 drawcall.json

优先级：P0

目标：

- 输出当前 drawcall 的结构化上下文，供 AI Agent 快速理解事件。

输出文件：

- `drawcall.json`

数据内容：

- capture filename、frame number、API、eventId、action name。
- draw/dispatch/copy 类型和 action flags。
- draw 参数：numIndices、numInstances、baseVertex、vertexOffset、instanceOffset 等可用字段。
- 当前 graphics/compute pipeline object。
- VS/PS shader resourceId、entry point、encoding。
- topology、viewport、scissor。
- render target/depth target 尺寸、格式、sample count。
- blend/depth/stencil/rasterizer 状态摘要。

实现要点：

- 优先从 `ActionDescription` 和 `PipeState` 取通用字段。
- 后端特有字段可以先输出摘要，详细字段由 T06 负责。

验收标准：

- 不解析其它文件，仅看 `drawcall.json` 可以知道当前事件是什么 drawcall。
- VS/PS shader resourceId 与后续 shader 文件 manifest entry 对得上。

依赖：T03。

## T05. 重构 Pipeline HTML 为路径驱动导出

优先级：P0

目标：

- 批量导出时无需弹出 HTML 保存对话框。
- 保持原按钮导出的 HTML 内容不变。

涉及文件：

- `qrenderdoc/Windows/PipelineState/PipelineStateViewer.h`
- `qrenderdoc/Windows/PipelineState/PipelineStateViewer.cpp`
- `qrenderdoc/Windows/PipelineState/D3D11PipelineStateViewer.*`
- `qrenderdoc/Windows/PipelineState/D3D12PipelineStateViewer.*`
- `qrenderdoc/Windows/PipelineState/GLPipelineStateViewer.*`
- `qrenderdoc/Windows/PipelineState/VulkanPipelineStateViewer.*`
- `qrenderdoc/Code/Interface/QRDInterface.h`，如选择扩展 `IPipelineStateViewer`

实现要点：

- 增加 `beginHTMLExport(const QString &filename)`。
- 现有 `beginHTMLExport()` 保留，只负责弹框后调用新 overload。
- 各后端 viewer 增加 `ExportHTMLToFile(const QString &filename)`。
- 现有 `on_exportHTML_clicked()` 改成调用 `ExportHTMLToFile(...)`。

输出文件：

- `pipeline_state.html`

验收标准：

- 原 UI 的 pipeline HTML 导出行为不变。
- 一键导出能无对话框生成 `pipeline_state.html`。
- D3D11、D3D12、GL、Vulkan 均能走同一批量导出入口。

依赖：T02。

## T06. 导出 pipeline_state.json

优先级：P0

目标：

- 输出给 AI Agent 使用的结构化 pipeline state。
- 避免让 Agent 解析 `pipeline_state.html`。

输出文件：

- `pipeline_state.json`

数据内容：

- shader stage 列表、shader resourceId、entry point、encoding。
- descriptor binding graph：stage、binding、descriptor type、resourceId、samplerId、view format。
- constant blocks：stage、binding、name、byte size、bind set/space。
- vertex input layout、vertex buffers、index buffer。
- output targets、depth target、formats、sample count。
- viewport/scissor。
- rasterizer、blend、depth/stencil。
- 后端特有字段：root signature、descriptor set、push constants、specialization constants 等，先覆盖可直接读取字段。

验收标准：

- `pipeline_state.json` 能独立表达 stage -> binding -> resource 的关系。
- 所有已导出的资源文件都能从 JSON 或 manifest 找到引用关系。
- 缺失或暂未支持的后端字段以 `unsupported` 或 `not_exported` 明确标记。

依赖：T03、T05 可并行。

## T07. 导出 Input/Output Textures 和 Samplers

优先级：P0

目标：

- 导出 VS/PS 实际使用的 input textures。
- 导出当前 output render targets 和 depth target。
- 同步导出 texture metadata 和 sampler metadata。

输出文件：

- `Textures/Inputs/tex_in__*.png|dds`
- `Textures/Outputs/tex_out__*.png|dds`
- `Textures/Metadata/textures.json`
- `Textures/Metadata/samplers.json`

API 来源：

- inputs：`PipeState::GetReadOnlyResources(stage, true)`
- optional RW inputs：`PipeState::GetReadWriteResources(stage, true)`
- outputs：`PipeState::GetOutputTargets()`、`GetDepthTarget()`、`GetDepthResolveTarget()`
- save：`IReplayController::SaveTexture(...)`

实现要点：

- 普通 2D 单 mip 单 slice 默认 PNG。
- arrays、3D、mips、MSAA、depth 或 SaveTexture 失败时 fallback 到 DDS。
- metadata 记录 resourceId、original name、dimension、format、view format、mip/slice/sample、stage、binding。
- sampler metadata 记录 filter、address mode、comparison、LOD、border color、anisotropy。

验收标准：

- VS/PS 使用的 texture SRV 至少能导出。
- RT0 和 depth 至少能导出或有明确失败原因。
- `samplers.json` 能关联到 texture binding。
- 重名贴图不会覆盖。

依赖：T03。

## T08. 导出 VS/PS Shader 数据

优先级：P0

目标：

- 导出 VS/PS 的 disassembly、raw bytes、source/debug files 和 reflection metadata。

输出文件：

```text
Shaders/vs/shader.json
Shaders/vs/disassembly_default.txt
Shaders/vs/raw_shader.bin
Shaders/vs/source_*.txt
Shaders/vs/reflection.json
Shaders/ps/...
```

API 来源：

- reflection：`PipeState::GetShaderReflection(stage)`
- shader id：`PipeState::GetShader(stage)`
- entry point：`PipeState::GetShaderEntryPoint(stage)`
- pipeline：`PipeState::GetGraphicsPipelineObject()`
- disassembly：`IReplayController::DisassembleShader(pipeline, reflection, "")`
- targets：`IReplayController::GetDisassemblyTargets(true)`

实现要点：

- 不使用 `.hlsl` 作为默认扩展，默认用 `disassembly_default.txt`。
- `raw_shader.bin` 写 `ShaderReflection::rawBytes`。
- `source_*.txt` 写 `ShaderReflection::debugInfo.files`，没有源码时记录到 `shader.json`。
- `reflection.json` 输出 input/output signature、constantBlocks、resources、samplers、read/write resources。
- 如果有多个 disassembly target，额外导出 `disassembly_<safe_target>.txt`。

验收标准：

- VS/PS shader resourceId 与 `drawcall.json`、`pipeline_state.json` 一致。
- disassembly 文件非空，或 manifest 中有明确失败原因。
- raw bytes 可选但若存在 reflection rawBytes 必须写出。

依赖：T03。

## T09. 导出 Constant Buffers

优先级：P0

目标：

- 导出 VS/PS constant buffers 的变量 CSV、结构 JSON、raw bytes。
- 支持 buffer-backed constant buffer，push/inline constants 至少有 fallback。

输出文件：

```text
ConstantBuffers/vs/cb__vs__b000__Name__ID.variables.csv
ConstantBuffers/vs/cb__vs__b000__Name__ID.json
ConstantBuffers/vs/cb__vs__b000__Name__ID.raw.bin
ConstantBuffers/ps/...
```

API 来源：

- `PipeState::GetConstantBlocks(stage, true)`
- `IReplayController::GetCBufferVariableContents(...)`
- `IReplayController::GetBufferData(...)`

实现要点：

- CSV 展开 `ShaderVariable` 变量树。
- JSON 记录 block name、stage、binding、bind set/space、byte size、offset、resourceId、变量 layout。
- raw bytes 优先来自 descriptor resource 和 byte range。
- push constants、inline constants、compile constants 不要静默跳过，至少记录 JSON，能读 raw 就写 raw。

验收标准：

- VS/PS constant buffer 数量与 Pipeline State 中 visible used constant blocks 对齐。
- 每个成功导出的 cbuffer 至少有 JSON。
- buffer-backed cbuffer 必须有 raw bytes。
- 失败项写入 manifest，不影响其它 cbuffer。

依赖：T03、T08。

## T10. 导出 Mesh OBJ 和原始 VB/IB

优先级：P1

目标：

- 导出当前 drawcall 的 post-transform mesh OBJ。
- 导出还原 VS 输入所需的 vertex input layout、vertex buffer、index buffer raw bytes。

涉及文件：

- `qrenderdoc/Code/MeshOBJWriter.h`
- `qrenderdoc/Code/MeshOBJWriter.cpp`
- `qrenderdoc/Code/DrawcallExport.*`
- 构建文件中的源文件列表

输出文件：

- `Mesh/mesh_postvs.obj`
- `Mesh/mesh_postvs.json`
- `Mesh/vertex_input_layout.json`
- `Mesh/index_buffer.bin`
- `Mesh/vertex_buffer_slot*.bin`

API 来源：

- post-transform：`IReplayController::GetPostVSData(instance, view, MeshDataStage)`
- raw buffer：`IReplayController::GetBufferData(...)`
- vertex layout：`PipeState::GetVBuffers()`、`PipeState::GetVertexInputs()`

实现要点：

- 普通 graphics draw 优先 `GSOut`，没有 GS/DS 输出时用 `VSOut`。
- mesh shader draw 使用 `MeshOut`。
- OBJ V1 支持 triangle list/strip、line、point。
- unsupported topology 写入 `mesh_postvs.json` 和 manifest。
- 原始 VB/IB 只导出当前 drawcall 需要范围，避免大文件。

验收标准：

- 常见 triangle draw 能生成可打开的 OBJ。
- `vertex_input_layout.json` 能描述每个 attribute 对应 slot、format、offset、stride。
- raw VB/IB 文件 byte range 与 JSON 记录一致。

依赖：T03。

## T11. 导出 Buffer SRV/UAV Raw Ranges

优先级：P1

目标：

- 导出 VS/PS 实际使用的 structured/storage/typed/raw buffer SRV/UAV 数据范围。
- 避免 shader 还原时缺少非贴图输入。

输出目录：

- `ResourceBuffers/*.raw.bin`
- `ResourceBuffers/resource_buffers.json`

API 来源：

- `PipeState::GetReadOnlyResources(stage, true)`
- `PipeState::GetReadWriteResources(stage, true)`
- `IReplayController::GetBufferData(...)`

实现要点：

- 只处理 descriptor resource 是 buffer 的条目。
- 默认导出 descriptor byte range，不导出完整 buffer。
- 设置最大导出大小阈值，例如 64 MB，超出时截断或跳过并记录。
- JSON 记录 stride、element size、byteOffset、byteSize、descriptor type、stage、binding。

验收标准：

- 使用 buffer SRV 的 shader 能在 `ResourceBuffers` 中找到对应 raw range。
- 超大 buffer 不导致 UI 长时间卡死或磁盘爆量。
- manifest 明确记录 skipped/truncated 状态。

依赖：T03。

## T12. 完整 Manifest、Summary 和错误模型

优先级：P0

目标：

- 汇总所有导出项、资源关系和失败信息。
- 输出人可读摘要和机器可读 manifest。

输出文件：

- `manifest.json`
- `export_summary.md`

实现要点：

- manifest 包含 top-level export metadata。
- 每个文件 entry 包含 category、stage、binding、role、resourceId、originalName、path、status。
- 每个失败 entry 包含 task、reason、severity、recoverable。
- 资源引用图：shader stage -> descriptor -> resource file。
- summary 用于用户快速查看成功数量、失败数量和导出路径。

验收标准：

- 删除 HTML 后，仅靠 JSON 文件和 manifest 也能理解导出包。
- 所有导出文件都在 manifest 中有记录。
- 所有失败都能在 manifest 中找到，不只显示弹窗。

依赖：T04、T06、T07、T08、T09、T10、T11。

## T13. 增加 Event Browser 右键入口

优先级：P2

目标：

- 在 Event Browser 选中或右键某事件时可直接导出该事件。

涉及文件：

- `qrenderdoc/Windows/EventBrowser.cpp`
- `qrenderdoc/Windows/EventBrowser.h`

实现要点：

- 在 `events_contextMenu()` 中增加 `Export Current Drawcall Data...`。
- 右键某行时，必要时先切换当前 event，再调用 `DrawcallExporter`。
- 对不可导出 action 禁用该菜单项。

验收标准：

- 右键 drawcall 能直接导出对应 EID。
- 右键非 action 行不会误导出上一个 event。
- 主菜单和右键菜单导出结果结构一致。

依赖：T12。

## T14. 验证和回归测试

优先级：P0

目标：

- 确认功能在主要 API 和常见 drawcall 上稳定。

验证样例：

- D3D11 graphics draw。
- D3D12 graphics draw。
- Vulkan graphics draw，包含 descriptor set、push constants 或 specialization constants 的样例优先。
- GL graphics draw。
- 含 depth output 的 draw。
- 含 buffer SRV/UAV 的 draw。
- 含 instancing 或 index buffer 的 draw。

检查项：

- 菜单启用/禁用正确。
- 导出目录结构完整。
- `manifest.json` JSON 格式有效。
- `drawcall.json` 和 `pipeline_state.json` 中 resourceId 能关联到文件。
- texture 文件能打开，DDS fallback 有记录。
- VS/PS disassembly 非空。
- cbuffer CSV/JSON/raw 文件数量合理。
- OBJ 能被 MeshLab/Blender/Windows 3D Viewer 打开。
- 大资源不会导致长时间无响应。

自动化建议：

- 增加文件名 sanitize unit test。
- 增加 manifest registry 去重 test。
- 增加 JSON schema smoke test。
- 如现有测试框架不方便覆盖 UI，可先写纯 helper 单测，UI 走手工验证。

依赖：T12，T13 可选。

## 建议实施顺序

第一批可交付：

1. T01 主菜单入口。
2. T02 导出器骨架。
3. T03 文件命名和 manifest registry。
4. T04 `drawcall.json`。
5. T08 shader disassembly 和 raw bytes。
6. T09 constant buffer JSON/CSV/raw。

第二批补齐资源：

1. T05 pipeline HTML 路径重构。
2. T06 `pipeline_state.json`。
3. T07 texture/sampler。
4. T10 mesh OBJ/VB/IB。
5. T11 buffer SRV/UAV。

第三批产品化：

1. T12 manifest 和 summary 完整化。
2. T13 Event Browser 右键入口。
3. T14 多 API 验证和回归测试。

## 关键决策点

- `onlyUsedDescriptors` 默认设为 `true`。如需要“所有绑定资源”，后续增加 UI option。
- Texture 默认导出 PNG，复杂资源 fallback DDS。
- Shader 文件默认用 `.txt` 表示 disassembly，不使用 `.hlsl`。
- Constant buffer 必须保留 raw bytes，CSV 只作为可读视图。
- `pipeline_state.html` 给人看，`pipeline_state.json` 给 Agent 用。
- Resource buffer 默认只导 descriptor byte range，避免超大导出。

