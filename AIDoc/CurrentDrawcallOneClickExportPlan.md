# RenderDoc 选中 Drawcall 一键导出方案

## 目标

在 qrenderdoc 界面增加一个菜单入口。用户在 Event Browser 中选中某个 drawcall 后，点击菜单即可一次性导出该事件相关的数据，用于后续 AI Agent 进行 shader 还原、资源关系分析和离线复现。

导出内容：

- 所有 input 贴图，并在 manifest 中保留 RenderDoc 原始贴图名。
- 所有 output 贴图。
- 当前 drawcall 使用的 mesh，OBJ 格式。
- 当前 Pipeline State，复用原界面 “Export the current pipeline state to an HTML file” 的 HTML 内容。
- VS 和 Pixel Shader 的 Disassembly 代码。
- VS 和 Pixel Shader 阶段的 Constant Buffer 数据。
- 面向 AI Agent 的结构化元数据：drawcall 参数、shader reflection、descriptor/sampler 绑定、vertex input layout、render target 格式、viewport/scissor、blend/depth/rasterizer 状态等。

参考手工导出目录：`D:\Tmp\Ref\Data`。当前可见样本包含 `Pipeline.html`、`vs.hlsl`、`fs.hlsl`、`vs_uniformBuffer*.csv`、`fs_uniformBuffer*.csv`，其中 shader 文件实际是 SPIR-V disassembly 文本，CSV 是 uniform buffer 数据。正式实现不需要沿用参考目录的文件名，应该使用更适合程序解析的稳定命名，并通过 manifest 记录原始 RenderDoc 名称。

## 现有代码入口

已定位到可复用的现有逻辑：

| 功能 | 现有代码 |
| --- | --- |
| 主菜单结构 | `qrenderdoc/Windows/MainWindow.ui`，`File`、`Tools`、`Export As...` 菜单 |
| 当前事件/当前 drawcall | `CaptureContext::CurEvent()`、`CurSelectedEvent()`、`CurAction()`、`CurSelectedAction()` |
| Pipeline HTML 导出 | `qrenderdoc/Windows/PipelineState/PipelineStateViewer.cpp` 的 `beginHTMLExport()` / `endHTMLExport()`；各 API viewer 的 `on_exportHTML_clicked()` |
| 贴图保存 | `IReplayController::SaveTexture(const TextureSave&, const rdcstr&)`；UI 入口在 `TextureViewer::on_saveTex_clicked()` |
| input/output 贴图枚举 | `PipeState::GetReadOnlyResources()`、`GetReadWriteResources()`、`GetOutputTargets()`、`GetDepthTarget()`；Texture Viewer 的 `Following::*` helper 已处理 copy/clear/compute 特例 |
| Shader disassembly | `IReplayController::DisassembleShader(pipeline, reflection, target)`；Shader Viewer 默认传空 target 获取默认 disassembly |
| Shader reflection | `PipeState::GetShaderReflection(stage)`、`GetShader(stage)`、`GetShaderEntryPoint(stage)` |
| Constant Buffer | `PipeState::GetConstantBlocks(stage)`；`IReplayController::GetCBufferVariableContents(...)` |
| Mesh 数据 | `IReplayController::GetPostVSData(instance, view, MeshDataStage)`、`GetBufferData(...)`；Mesh Viewer 目前只提供 CSV/raw bytes 导出 |

## 用户入口

建议做两个入口，第一阶段至少实现主菜单：

- 主菜单：`File -> Export Current Drawcall Data...`
- 可选：Event Browser 右键菜单增加同名项，直接作用于右键所在事件。

主菜单 action 的启用条件：

- capture 已加载：`m_Ctx.IsCaptureLoaded()`
- 当前事件有 action：`m_Ctx.CurAction() != nullptr`
- 当前 action 是 graphics drawcall。copy/clear/compute 事件可允许导出部分数据，但 UI 文案和 manifest 里要标记跳过项。

点击后弹出目录选择框，导出器在该目录下创建事件子目录：

```text
<chosen_dir>/
  EID_<eventId>_<safe_action_name>/
```

## 输出目录结构

建议 V1 使用如下结构，目录和文件名优先服务机器解析和后续 Agent 自动消费：

```text
EID_1234_Draw/
  manifest.json
  export_summary.md
  drawcall.json
  pipeline_state.html
  pipeline_state.json
  Textures/
    Inputs/
      tex_in__ps__t000__albedo_basecolor__A1B2C3D4.png
      tex_in__vs__t001__height_field__B2C3D4E5.dds
    Outputs/
      tex_out__rt0__scene_color__C3D4E5F6.png
      tex_out__depth__main_depth__D4E5F607.dds
    Metadata/
      textures.json
      samplers.json
  Mesh/
    mesh_postvs.obj
    mesh_postvs.json
    vertex_input_layout.json
    index_buffer.bin
    vertex_buffer_slot0.bin
  Shaders/
    vs/
      shader.json
      disassembly_default.txt
      raw_shader.bin
      source_0.txt
      reflection.json
    ps/
      shader.json
      disassembly_default.txt
      raw_shader.bin
      source_0.txt
      reflection.json
  ConstantBuffers/
    vs/
      cb__vs__b000__SceneGlobals__AABBCCDD.variables.csv
      cb__vs__b000__SceneGlobals__AABBCCDD.raw.bin
      cb__vs__b000__SceneGlobals__AABBCCDD.json
    ps/
      cb__ps__b000__MaterialParams__BBCCDDEE.variables.csv
      cb__ps__b000__MaterialParams__BBCCDDEE.raw.bin
      cb__ps__b000__MaterialParams__BBCCDDEE.json
  ResourceBuffers/
    srv__ps__t004__Lights__11223344.raw.bin
    uav__ps__u000__OutputList__22334455.raw.bin
```

文件命名规则：

- 文件名格式采用 `category__stage_or_role__binding__safe_resource_name__resourceId.ext`。
- stage 使用固定短名：`vs`、`hs`、`ds`、`gs`、`ps`、`cs`、`task`、`mesh`。
- binding 使用固定宽度数字，例如 `t000`、`s003`、`b012`、`u001`、`rt0`、`depth`。
- `safe_resource_name` 来自 `m_Ctx.GetResourceName(resourceId)`，只作为可读补充，不作为唯一 key。
- Windows 非法字符 `<>:"/\|?*` 替换为 `_`，连续空白压缩为 `_`，长度建议限制到 64 字符。
- 唯一性由 `resourceId + stage + binding + subresource` 保证；同一资源被多个 stage 访问时可以硬链接/复制一份，也可以只导出一次并在 manifest 记录多处引用。
- RenderDoc 原始名称、完整 resourceId、descriptor 信息、subresource 信息必须写入 manifest，不依赖文件名反推。

`manifest.json` 用于记录完整映射和失败项，建议包含：

- capture 文件、eventId、action name、API 类型、导出时间。
- 每个导出文件的类别、stage、slot、resourceId、原始 RenderDoc 名称、最终文件名。
- drawcall 到资源的引用图：shader stage -> descriptor -> resource file。
- shader stage 到 constant buffer、texture、sampler、render target、mesh buffer 的映射。
- 每个失败项的原因，例如 unsupported topology、invalid texture descriptor、SaveTexture 失败。

`drawcall.json` 用于给 AI Agent 快速理解当前事件，不需要解析 HTML：

- eventId、action name、draw/dispatch/copy 类型、draw 参数、instance/view、index count、vertex count。
- API 类型、frame number、capture filename。
- 当前 pipeline object、VS/PS shader resourceId、entry point、shader encoding。
- viewport、scissor、primitive topology、front face/cull mode、depth/stencil/blend 状态摘要。
- render target/depth target 的尺寸、格式、sample count。

`pipeline_state.json` 是 AI Agent 还原 shader 的关键结构化数据，建议和 `pipeline_state.html` 同时导出。HTML 保留给人看，JSON 给 Agent 用。

## 新增导出器设计

新增一个 UI 无关的导出器类，避免把批量逻辑塞进 `MainWindow` 或各 viewer：

```text
qrenderdoc/Code/DrawcallExport.h
qrenderdoc/Code/DrawcallExport.cpp
```

核心接口建议：

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

导出器职责：

- 获取当前 event/action 和 pipeline state。
- 创建目录、生成安全文件名、维护去重。
- 串行执行每类导出，并汇总结果。
- 使用 `m_Ctx.Replay().BlockInvoke(...)` 执行 replay 线程相关 API。
- 写 `manifest.json` 和错误日志。

UI 层只负责：

- 菜单 action。
- 选择导出目录。
- 显示进度和最终成功/失败摘要。

## 各数据项实现

### 1. Pipeline.html

当前 `beginHTMLExport()` 内部弹保存文件对话框，不能直接用于批量导出。建议重构为路径驱动：

- 在 `PipelineStateViewer` 增加 `beginHTMLExport(const QString &filename)` overload。
- 现有 `beginHTMLExport()` 保留，负责弹框后调用新 overload。
- 各 API viewer 增加无 UI 方法，例如 `ExportHTMLToFile(const QString &filename)`，现有按钮 slot 调用该方法。
- `IPipelineStateViewer` 可增加 `bool ExportHTML(const rdcstr &filename)`，便于导出器只依赖接口。

导出器调用：

```cpp
m_Ctx.ShowPipelineViewer();
m_Ctx.GetPipelineViewer()->ExportHTML(ToRDCStr(outputPath));
```

这样 HTML 内容与原按钮导出保持一致。

同时新增 `pipeline_state.json`，不建议让 Agent 解析 HTML。JSON 可从 `PipeState` 和后端 pipeline state 对象导出：

- common state：`PipeState` 的 shader、descriptor、constant block、output target、vertex input、topology。
- backend state：D3D11/D3D12/GL/Vulkan 特有字段，如 root signature、descriptor set、specialization constants、push constants、rasterizer、blend、depth stencil。
- 所有 resourceId 保持原始字符串，并通过 manifest 关联到已导出的文件。

### 2. Input 贴图

输入贴图按 Texture Viewer “Inputs” 的语义收集，默认只导出当前 drawcall 实际使用的 descriptor。文件名不用照 RenderDoc 显示名，显示名写入 manifest：

- graphics drawcall：遍历 `ShaderStage::Vertex`、`Hull`、`Domain`、`Geometry`、`Pixel`，V1 至少保证 `Vertex` 和 `Pixel`。
- 调用 `m_Ctx.CurPipelineState().GetReadOnlyResources(stage, true)`。
- 对 `UsedDescriptor.descriptor.resource` 过滤出 texture 资源：`m_Ctx.GetTexture(resourceId) != nullptr`。
- 可选补充 `GetReadWriteResources(stage, true)` 中的 texture UAV，放在 `Textures/Inputs_RW` 或 manifest 中标注为 RW resource。

保存方式：

- 构造 `TextureSave`：
  - `resourceId = descriptor.resource`
  - `typeCast = CompType::Typeless`
  - `mip = descriptor.firstMip`，默认 0
  - `slice.sliceIndex = descriptor.firstSlice`，默认 0
  - `channelExtract = -1`
- 普通 2D 单 mip 单 slice：默认 PNG。
- 数组、3D、多 mip、多 sample 或格式不适合 PNG：使用 DDS，保留更多原始信息。
- 调用 `IReplayController::SaveTexture(saveData, path)`。
- 每张贴图额外写入 metadata：resourceId、原始名称、width/height/depth、array size、mips、sample count、resource format、view format、firstMip、firstSlice、descriptor type、stage、binding、sampler 关联。

AI Agent shader 还原时，sampler 状态经常和 texture 一样重要。因此 input 贴图导出时必须同步导出 sampler：

- `PipeState::GetSamplers(stage, true)` 或 `UsedDescriptor.sampler`。
- 写入 `Textures/Metadata/samplers.json`。
- 记录 filter、address mode、lod bias、min/max lod、comparison func、border color、anisotropy。

### 3. Output 贴图

输出贴图按 Texture Viewer “Outputs” 的语义收集：

- color targets：`m_Ctx.CurPipelineState().GetOutputTargets()`
- depth：`GetDepthTarget()`
- depth resolve：如需要，补充 `GetDepthResolveTarget()`
- 对 copy/present 等特殊 action，可参考 `TextureViewer::Following::GetOutputTargets()` 的逻辑处理 copy destination 和 swapbuffer。

保存方式与 input 贴图一致。Depth 默认也先尝试 PNG；若 SaveTexture 因格式失败则改 DDS，并在 manifest 里记录 fallback。

为了还原 pixel shader 行为，output metadata 需要记录：

- RT index、depth/stencil role。
- 输出格式、sRGB/typeless cast、sample count。
- blend enable、blend factors、color write mask。
- depth test/write、stencil state。

### 4. Mesh OBJ

现有 `BufferViewer::exportData()` 只支持 CSV 和 raw bytes，因此 OBJ 需要新增 exporter。

建议 V1 同时导出 post-transform OBJ 和原始 mesh 输入数据。OBJ 方便人和 Agent 看几何，原始 vertex/index buffer 更利于还原 VS 输入。

- OBJ 优先阶段：
  - mesh shader draw：`MeshDataStage::MeshOut`
  - 普通 graphics draw：若有 GS/DS 输出则 `MeshDataStage::GSOut`，否则 `MeshDataStage::VSOut`
- 调用 `IReplayController::GetPostVSData(instance, view, stage)` 获取 `MeshFormat`。
- 通过 `GetBufferData(vertexResourceId, vertexByteOffset, vertexByteSize)` 和 index buffer 字段读取数据。
- 解码 position 为 OBJ `v x y z`。
- 根据 topology 写 faces：
  - triangle list/strip：写 `f`
  - line/point topology：写 `l` 或 `p`
  - adjacency、patch、unknown：跳过 faces，并在 `mesh_export_notes.txt` 记录。

实现上建议抽公共 helper：

```text
qrenderdoc/Code/MeshOBJWriter.h
qrenderdoc/Code/MeshOBJWriter.cpp
```

不要直接依赖 `BufferViewer` UI model。可以复用或下沉 `BufferViewer.cpp` 中的格式解码 helper，例如 `CalcIndex`、`GetVariants`、format interpret 相关逻辑。若下沉成本过高，V1 先只支持常见 float position 格式，并对其它格式写明确 warning。

OBJ 坐标使用 RenderDoc mesh viewer 当前数据，不做额外轴映射。后续如需要和 Mesh Viewer 显示一致，可增加 axis mapping 配置。

另需导出原始输入：

- `vertex_input_layout.json`：`PipeState::GetVBuffers()`、`GetVertexInputs()`、attribute semantic/name、format、slot、offset、stride、per-instance、step rate。
- `index_buffer.bin`：当前 drawcall 使用的 index buffer bytes、offset、stride、baseVertex。
- `vertex_buffer_slot*.bin`：当前 drawcall 相关范围的 vertex buffer bytes。
- `mesh_postvs.json`：OBJ 对应的 topology、stage、vertex count、index count、position column、是否 unproject/flipY。

### 5. VS/Pixel Shader Disassembly

使用通用 `PipeState` API：

- VS：`ShaderStage::Vertex`
- PS：`ShaderStage::Pixel`
- reflection：`m_Ctx.CurPipelineState().GetShaderReflection(stage)`
- pipeline：graphics draw 使用 `m_Ctx.CurPipelineState().GetGraphicsPipelineObject()`
- disassembly：`r->DisassembleShader(pipeline, reflection, "")`

输出：

```text
Shaders/vs/disassembly_default.txt
Shaders/ps/disassembly_default.txt
```

不建议再命名为 `vs.hlsl` / `fs.hlsl`，因为内容不一定是 HLSL，可能是 DXBC/DXIL/SPIR-V/GLSL disassembly。

为了提高 shader 还原成功率，除 disassembly 外建议同步导出：

- `raw_shader.bin`：`ShaderReflection::rawBytes`。
- `source_*.txt`：`ShaderReflection::debugInfo.files` 中可用的原始源码。
- `shader.json`：stage、shader resourceId、pipeline resourceId、entry point、encoding、compile flags、debug info 摘要。
- `reflection.json`：input/output signature、constant blocks、read-only/read-write resources、samplers、thread/group info。
- 如果 `GetDisassemblyTargets(true)` 返回多个目标，可导出 `disassembly_<target>.txt`，默认 target 仍作为主文件。

### 6. Constant Buffers

目标阶段：

- `ShaderStage::Vertex`
- `ShaderStage::Pixel`

收集：

- `m_Ctx.CurPipelineState().GetConstantBlocks(stage, true)`
- shader id：`GetShader(stage)`
- entry point：`GetShaderEntryPoint(stage)`
- pipeline：`GetGraphicsPipelineObject()`

变量解码：

```cpp
r->GetCBufferVariableContents(
    pipeline,
    shader,
    stage,
    entryPoint,
    cbufSlot,
    used.descriptor.resource,
    used.descriptor.byteOffset,
    used.descriptor.byteSize);
```

CSV/JSON/RAW 建议同时输出：

- `variables.csv` 风格：按 `ShaderVariable` 展开，包含变量名、类型、行列、数值。
- `*.json`：stage、binding、bind set/space、resourceId、offset、size、变量树、原始 block 名称。
- `*.raw.bin`：原始 bytes。AI Agent 需要在反推 packing、矩阵行列主序或类型不确定时读取原始数据。
- raw CSV fallback：当 reflection 不完整、inline constants、push constants 或解码失败时，使用 `GetBufferData(...)` 输出 32-bit hex rows，格式接近参考样本：

```text
Element, data.x, data.y, data.z, data.w
0, 3F800000, 00000000, 00000000, 00000000
```

对 Vulkan push constants：

- `ConstantBlock::inlineDataBytes` 场景不一定有 buffer resource。
- V1 不应只跳过，至少要单独导出为 `push_constants.json` / `push_constants.raw.bin`，因为 Vulkan shader 还原经常依赖 push constants。
- 若项目样本主要来自 uniform buffer，先保证 buffer-backed cbuffer。

### 7. 还原 Shader 建议额外导出的数据

原始需求中的数据已经覆盖主要观察对象，但如果目标是“给 AI Agent 还原 shader”，建议额外导出以下数据：

| 数据 | 原因 | 建议文件 |
| --- | --- | --- |
| Shader raw bytes | disassembly 不一定可重新编译，raw bytes 可用于重新反编译或验证 | `Shaders/<stage>/raw_shader.bin` |
| Shader source/debug files | 有源码时还原质量远高于 disassembly | `Shaders/<stage>/source_*.txt` |
| Shader reflection JSON | Agent 需要输入输出签名、资源绑定、cbuffer layout | `Shaders/<stage>/reflection.json` |
| Vertex input layout + raw VB/IB | 还原 VS 输入语义和 mesh 变换需要原始 attribute | `Mesh/vertex_input_layout.json`、`*.bin` |
| Sampler states | 纹理采样结果取决于 filter/address/comparison/lod | `Textures/Metadata/samplers.json` |
| Descriptor/resource binding graph | Agent 需要知道哪个 shader slot 对应哪个文件 | `manifest.json`、`pipeline_state.json` |
| Render state | PS 输出受 blend/depth/stencil/MSAA/format 影响 | `drawcall.json`、`pipeline_state.json` |
| Viewport/scissor | SV_Position、屏幕空间重建、depth 还原需要 | `drawcall.json` |
| Push constants / specialization constants | Vulkan/GL SPIR-V 常见，缺失会导致 shader 逻辑无法还原 | `ConstantBuffers/push_constants.*`、`Shaders/<stage>/specialization.json` |
| Structured/storage buffers | 很多 shader 输入不是贴图而是 buffer SRV/UAV | `ResourceBuffers/*.raw.bin` + metadata |
| Texture/resource metadata | 仅图片不够，需要格式、mip、array、view cast 信息 | `Textures/Metadata/textures.json` |

`ResourceBuffers` 的 V1 范围建议只导出当前 VS/PS 实际使用的 buffer SRV/UAV，避免一次性导出过大的全局资源。对大 buffer 应记录原始总大小，并只导出 descriptor byte range；必要时提供 “export full resource buffers” 选项。

## 进度和错误处理

批量导出可能耗时，建议使用现有进度弹窗模式：

- 每个大类一个进度 step。
- SaveTexture / mesh buffer read / shader disassembly 都在 replay 线程调用。
- 单项失败不中断全局导出，记录到 manifest，最后弹摘要。
- 目录已存在时不清空，只覆盖同名文件前先确认，或创建带时间戳的新目录。

## 实施步骤

1. 增加导出器骨架和 manifest/file-name 工具函数。
2. 在 `MainWindow.ui` 增加 `action_Export_Current_Drawcall_Data`，在 `MainWindow.cpp/.h` 添加 slot。
3. 实现 `drawcall.json`、`manifest.json`、稳定文件命名和 resource binding graph。
4. 实现贴图枚举和保存，先覆盖 VS/PS read-only inputs、RT/depth outputs，同时导出 texture/sampler metadata。
5. 重构 Pipeline HTML 导出为路径驱动，并新增 `pipeline_state.json`。
6. 实现 shader disassembly、raw bytes、source files、reflection JSON 导出。
7. 实现 constant buffer CSV/JSON/raw 导出，包含 push constants fallback。
8. 实现 OBJ exporter，并导出 vertex input layout、VB/IB raw bytes。
9. 实现 VS/PS 使用到的 buffer SRV/UAV raw range 导出。
10. 增加 Event Browser 右键入口。
11. 增加手工验证用例：对同一 EID 对比 `D:\Tmp\Ref\Data` 的 Pipeline/shader/cbuffer 形态，并确认 input/output 贴图可以打开。

## 验证清单

- 未加载 capture 时菜单禁用。
- 选中非 drawcall 事件时菜单禁用或导出 manifest 标记无 drawcall。
- D3D11/D3D12/Vulkan/GL 至少各验证一次 pipeline HTML 和 shader disassembly。
- input/output 贴图文件名稳定可解析，manifest 中保留 RenderDoc 原始名称。
- 多个 descriptor 指向同一 texture 时不会重复覆盖。
- Constant buffer 数量与 Pipeline State 中 VS/PS Constant Buffers 表一致。
- `drawcall.json` / `pipeline_state.json` 能独立表达 stage、binding、resource file 的关系。
- shader raw bytes、disassembly、reflection 能按 VS/PS stage 对上同一个 shader resourceId。
- sampler JSON 能与 texture binding 对上。
- VB/IB raw bytes 和 `vertex_input_layout.json` 能还原 Mesh Viewer 的输入行。
- OBJ 能被 Blender、Windows 3D Viewer 或 MeshLab 打开。
- 对 unsupported topology，导出器不会崩溃，notes/manifest 有明确原因。

## 风险点

- OBJ 导出是新增能力，RenderDoc 当前 Mesh Viewer 没有 OBJ exporter，可复用的数据读取路径多，但需要谨慎抽离 UI 依赖。
- Texture 保存格式需要 fallback。PNG 便于查看，但不能完整表达数组、3D、mip、MSAA 和部分深度格式；DDS 更稳。
- Constant buffer 在 Vulkan push constants、inline constants、bindless descriptor 场景下不总是 buffer-backed，需要 JSON/raw fallback。
- Resource buffer 可能非常大，需要默认导出 descriptor byte range，并提供大小上限。
- 结构化 `pipeline_state.json` 初期会比 HTML 覆盖面少，建议先覆盖 AI 还原必要字段，再逐步补齐后端特有状态。
- “所有 Inputs” 的语义需要最终确认：V1 建议使用 `onlyUsedDescriptors=true`，匹配 Texture Viewer 当前 drawcall 真正访问的资源；如要导出所有绑定但未使用资源，可在 options 中开放开关。
