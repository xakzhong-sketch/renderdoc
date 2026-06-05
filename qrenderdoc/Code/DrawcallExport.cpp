/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2019-2025 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "DrawcallExport.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTextStream>
#include <QVector>
#include <string.h>
#include "Code/QRDUtils.h"

static QString ToQStr(const rdcstr &str)
{
  return QString(str);
}

DrawcallExporter::DrawcallExporter(ICaptureContext &ctx, QWidget *parent) : m_Ctx(ctx), m_Parent(parent)
{
}

ResultDetails DrawcallExporter::ExportCurrentDrawcall(const QString &rootDir,
                                                      const DrawcallExportOptions &opts)
{
  m_Options = opts;
  m_Files.clear();
  m_Failures.clear();
  m_ConstantBuffers.clear();
  m_UsedFilenames.clear();
  m_ExportDir.clear();

  if(!m_Ctx.IsCaptureLoaded())
    return {ResultCode::InternalError};

  const ActionDescription *action = m_Ctx.CurAction();
  if(action == NULL)
    return {ResultCode::InternalError};

  QDir root(rootDir);
  if(!root.exists())
    return {ResultCode::FileIOFailed};

  QString actionName = ToQStr(action->GetName(m_Ctx.GetStructuredFile()));
  if(actionName.isEmpty())
    actionName = QFormatStr("Action_%1").arg(action->eventId);

  const QString dirName =
      QFormatStr("EID_%1_%2").arg(action->eventId).arg(SafeName(actionName, lit("Drawcall")));

  m_ExportDir = root.filePath(dirName);

  QDir exportDir(m_ExportDir);
  if(exportDir.exists())
  {
    QString timestamp = QDateTime::currentDateTime().toString(lit("yyyyMMdd_hhmmss"));
    m_ExportDir = root.filePath(dirName + lit("_") + timestamp);
  }

  if(!root.mkpath(QFileInfo(m_ExportDir).fileName()))
    return {ResultCode::FileIOFailed};

  ResultDetails ret = ExportPackage();
  WriteManifestAndSummary();
  return ret;
}

QString DrawcallExporter::SafeName(const QString &name, const QString &fallback) const
{
  QString ret = name.trimmed();
  if(ret.isEmpty())
    ret = fallback.isEmpty() ? lit("unnamed") : fallback;

  QString safe;
  safe.reserve(ret.count());

  bool lastUnderscore = false;
  for(QChar ch : ret)
  {
    const bool invalid = ch.unicode() < 32 || QStringLiteral("<>:\"/\\|?*()").contains(ch);
    const bool whitespace = ch.isSpace();

    if(invalid || whitespace)
    {
      if(!lastUnderscore)
        safe += lit("_");
      lastUnderscore = true;
    }
    else
    {
      safe += ch;
      lastUnderscore = false;
    }

    if(safe.count() >= 64)
      break;
  }

  safe = safe.trimmed();
  while(safe.startsWith(lit("_")))
    safe.remove(0, 1);
  while(safe.endsWith(lit("_")))
    safe.chop(1);

  return safe.isEmpty() ? (fallback.isEmpty() ? lit("unnamed") : fallback) : safe;
}

QString DrawcallExporter::UniqueRelativePath(const QString &relativeDir, const QString &baseName,
                                             const QString &extension)
{
  EnsureDir(relativeDir);

  QString cleanBase = SafeName(baseName);
  QString cleanExt = extension;
  if(!cleanExt.startsWith(lit(".")))
    cleanExt = lit(".") + cleanExt;

  QString keyBase = relativeDir + lit("/") + cleanBase;
  int count = m_UsedFilenames.value(keyBase + cleanExt, 0);

  QString filename = cleanBase + cleanExt;
  if(count > 0)
    filename = QFormatStr("%1__%2%3").arg(cleanBase).arg(count + 1).arg(cleanExt);

  m_UsedFilenames[keyBase + cleanExt] = count + 1;

  return relativeDir + lit("/") + filename;
}

QString DrawcallExporter::MakeResourceBaseName(const QString &category, const QString &stage,
                                               const QString &binding, ResourceId id,
                                               const QString &resourceName) const
{
  return QFormatStr("%1__%2__%3__%4__%5")
      .arg(category)
      .arg(stage)
      .arg(binding)
      .arg(SafeName(resourceName, lit("resource")))
      .arg(SafeName(ToQStr(id), lit("ResourceId")));
}

rdcarray<ShaderStage> DrawcallExporter::ActiveShaderStages() const
{
  const PipeState &pipe = m_Ctx.CurPipelineState();
  rdcarray<ShaderStage> ret;

  for(ShaderStage stage :
      {ShaderStage::Vertex, ShaderStage::Hull, ShaderStage::Domain, ShaderStage::Geometry,
       ShaderStage::Pixel, ShaderStage::Compute, ShaderStage::Task, ShaderStage::Mesh})
  {
    if(pipe.GetShader(stage) != ResourceId() || pipe.GetShaderReflection(stage) != NULL)
      ret.push_back(stage);
  }

  return ret;
}

ResourceId DrawcallExporter::PipelineObjectForStage(ShaderStage stage) const
{
  const PipeState &pipe = m_Ctx.CurPipelineState();
  return stage == ShaderStage::Compute ? pipe.GetComputePipelineObject()
                                       : pipe.GetGraphicsPipelineObject();
}

bool DrawcallExporter::EnsureDir(const QString &relativeDir)
{
  QDir dir(m_ExportDir);
  if(relativeDir.isEmpty())
    return dir.exists();

  return dir.mkpath(relativeDir);
}

bool DrawcallExporter::WriteTextFile(const QString &relativePath, const QString &contents)
{
  QFileInfo info(QDir(m_ExportDir).filePath(relativePath));
  QDir().mkpath(info.dir().absolutePath());

  QFile f(info.absoluteFilePath());
  if(!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
  {
    RecordFailure(lit("write_file"), QFormatStr("Couldn't write %1: %2").arg(relativePath).arg(f.errorString()));
    return false;
  }

  QTextStream stream(&f);
  stream.setCodec("UTF-8");
  stream << contents;
  return true;
}

bool DrawcallExporter::WriteBytesFile(const QString &relativePath, const bytebuf &contents)
{
  QFileInfo info(QDir(m_ExportDir).filePath(relativePath));
  QDir().mkpath(info.dir().absolutePath());

  QFile f(info.absoluteFilePath());
  if(!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    RecordFailure(lit("write_file"), QFormatStr("Couldn't write %1: %2").arg(relativePath).arg(f.errorString()));
    return false;
  }

  if(!contents.empty())
    f.write((const char *)contents.data(), (qint64)contents.size());

  return true;
}

bool DrawcallExporter::WriteJSONFile(const QString &relativePath, const QVariantMap &contents)
{
  return WriteTextFile(relativePath, VariantToJSON(contents));
}

void DrawcallExporter::RecordFile(const QString &category, const QString &relativePath,
                                  const QVariantMap &metadata)
{
  QVariantMap entry = metadata;
  entry[lit("category")] = category;
  entry[lit("path")] = relativePath;
  entry[lit("status")] = lit("exported");
  if(!entry.contains(lit("shader_reconstruction_priority")))
    entry[lit("shader_reconstruction_priority")] = ShaderReconstructionPriority(entry);
  if(!entry.contains(lit("shader_reconstruction_role")))
    entry[lit("shader_reconstruction_role")] = ShaderReconstructionRole(entry);
  if(!entry.contains(lit("shader_reconstruction_phase")))
    entry[lit("shader_reconstruction_phase")] = ShaderReconstructionPhase(entry);
  m_Files.push_back(entry);
}

void DrawcallExporter::RecordFailure(const QString &task, const QString &message,
                                     const QVariantMap &metadata)
{
  QVariantMap entry = metadata;
  entry[lit("task")] = task;
  entry[lit("message")] = message;
  entry[lit("status")] = lit("failed");
  m_Failures.push_back(entry);
}

QString DrawcallExporter::ShaderReconstructionPriority(const QVariantMap &file) const
{
  if(file.contains(lit("shader_reconstruction_priority")))
    return file[lit("shader_reconstruction_priority")].toString();

  const QString category = file[lit("category")].toString();
  const QString path = file[lit("path")].toString().toLower();

  if(category == lit("drawcall") || category == lit("pipeline_html") ||
     category == lit("pipeline_json") || category == lit("shader_reconstruction_index") ||
     category == lit("shader_reconstruction_goal") ||
     category == lit("shader_reconstruction_agent_guide") ||
     category == lit("shader_reconstruction_workflow") || category == lit("texture_metadata") ||
     category == lit("sampler_metadata") || category == lit("constant_buffer_index") ||
     category == lit("constant_buffer_notes"))
    return lit("high");

  if(category == lit("shader_metadata") || category == lit("shader_reflection") ||
     category == lit("shader_disassembly") || category == lit("shader_disassembly_native") ||
     category == lit("shader_disassembly_hlsl") ||
     category == lit("shader_disassembly_metadata"))
    return lit("high");

  if(category == lit("input_texture") || category == lit("output_texture"))
    return lit("high");

  if(category == lit("constant_buffer_json") || category == lit("constant_buffer_csv"))
    return lit("high");

  if(category == lit("mesh_postvs_obj") || category == lit("mesh_postvs_metadata") ||
     category == lit("mesh_vertex_input_layout"))
    return lit("high");

  if(category == lit("resource_buffer_raw"))
    return lit("medium");

  if(category == lit("resource_buffer_metadata") || category == lit("resource_buffer_notes"))
    return lit("medium");

  if(category == lit("shader_raw") || category == lit("shader_source") ||
     category == lit("constant_buffer_raw") || category.startsWith(lit("mesh_")) ||
     path.endsWith(lit(".bin")))
    return lit("medium");

  if(category == lit("shader_disassembly_hlsl_log"))
    return lit("low");

  return lit("medium");
}

QString DrawcallExporter::ShaderReconstructionRole(const QVariantMap &file) const
{
  const QString category = file[lit("category")].toString();

  if(category == lit("shader_reconstruction_agent_guide"))
    return lit("Entry point for AI agents; read this first.");
  if(category == lit("shader_reconstruction_workflow"))
    return lit("Step-by-step Unity shader reconstruction workflow.");
  if(category == lit("shader_reconstruction_index"))
    return lit("Machine-readable priority index for all exported files.");
  if(category == lit("shader_reconstruction_goal"))
    return lit("Codex /goal objective for reconstructing the Unity 6 URP mobile shader.");
  if(category == lit("drawcall"))
    return lit("Selected event identity, draw dimensions, topology, and shader IDs.");
  if(category == lit("pipeline_html"))
    return lit("Full RenderDoc pipeline state export; best human-readable state reference.");
  if(category == lit("pipeline_json"))
    return lit("Structured pipeline subset for tools and agents.");
  if(category == lit("shader_metadata") || category == lit("shader_reflection"))
    return lit("Shader entry point, signatures, reflection resources, and binding layout.");
  if(category == lit("shader_disassembly") || category == lit("shader_disassembly_native"))
    return lit("Native DXBC/DXIL disassembly for ground-truth control/data flow.");
  if(category == lit("shader_disassembly_hlsl"))
    return lit("HLSL decompiler output for faster Unity shader reconstruction; cross-check with native disassembly.");
  if(category == lit("shader_raw"))
    return lit("Original shader bytecode for tools or later reprocessing.");
  if(category == lit("input_texture"))
    return lit("Bound shader input texture; core visual/material evidence.");
  if(category == lit("output_texture"))
    return lit("Render target/depth/UAV output; target visual result for validation, including compute-written GBuffer textures.");
  if(category == lit("texture_metadata"))
    return lit("Texture path, binding, descriptor, dimensions, and format index.");
  if(category == lit("sampler_metadata"))
    return lit("Sampler state for UV/filter/address-mode reconstruction.");
  if(category == lit("constant_buffer_index"))
    return lit("Slot-to-file map for active-stage constant buffers, using RenderDoc buffer display names.");
  if(category == lit("constant_buffer_json") || category == lit("constant_buffer_csv"))
    return lit("Decoded constant buffer values and layout; primary numeric shader inputs.");
  if(category == lit("constant_buffer_raw"))
    return lit("Raw constant buffer bytes; secondary when JSON/CSV decoded values are sufficient.");
  if(category == lit("mesh_postvs_obj") || category == lit("mesh_postvs_metadata") ||
     category == lit("mesh_vertex_input_layout"))
    return lit("Geometry context for vertex inputs and post-transform validation.");
  if(category.startsWith(lit("mesh_")))
    return lit("Raw mesh/index/vertex buffer data; supporting evidence.");
  if(category == lit("resource_buffer_raw"))
    return lit("Raw SRV/UAV buffer; optional unless shader code directly indexes it.");
  if(category == lit("resource_buffer_metadata"))
    return lit("Resource buffer index with names, byte ranges, and priority guidance.");
  if(category == lit("resource_buffer_notes"))
    return lit("Notes explaining how to treat large SRV/UAV buffers.");

  return lit("Supporting export file.");
}

QString DrawcallExporter::ShaderReconstructionPhase(const QVariantMap &file) const
{
  const QString category = file[lit("category")].toString();

  if(category == lit("shader_reconstruction_agent_guide") ||
     category == lit("shader_reconstruction_workflow") ||
     category == lit("shader_reconstruction_index") ||
     category == lit("shader_reconstruction_goal") || category == lit("drawcall") ||
     category == lit("pipeline_html") || category == lit("pipeline_json"))
    return lit("orientation");

  if(category.startsWith(lit("shader_")))
    return lit("shader_code");

  if(category == lit("constant_buffer_index") || category.startsWith(lit("constant_buffer")))
    return lit("parameters");

  if(category == lit("input_texture") || category == lit("output_texture") ||
     category == lit("texture_metadata") || category == lit("sampler_metadata"))
    return lit("textures");

  if(category.startsWith(lit("mesh_")))
    return lit("geometry");

  if(category.startsWith(lit("resource_buffer")))
    return lit("optional_buffers");

  return lit("supporting");
}

ResultDetails DrawcallExporter::ExportPackage()
{
  ExportDrawcallJSON();

  if(m_Options.exportPipelineHTML)
    ExportPipelineHTML();
  if(m_Options.exportPipelineJSON)
    ExportPipelineJSON();
  if(m_Options.exportInputs || m_Options.exportOutputs || m_Options.exportSamplerState)
    ExportTexturesAndSamplers();
  if(m_Options.exportShaders)
    ExportShaders();
  if(m_Options.exportConstantBuffers)
    ExportConstantBuffers();
  const ActionDescription *action = m_Ctx.CurAction();
  if((m_Options.exportMeshOBJ || m_Options.exportMeshRawBuffers) && action &&
     (action->flags & (ActionFlags::Drawcall | ActionFlags::MeshDispatch)))
    ExportMesh();
  if(m_Options.exportResourceBuffers)
    ExportResourceBuffers();

  return {ResultCode::Succeeded};
}

void DrawcallExporter::ExportDrawcallJSON()
{
  QVariantMap root = MakeDrawcallJSON();
  const QString path = lit("drawcall.json");
  if(WriteJSONFile(path, root))
    RecordFile(lit("drawcall"), path);
}

void DrawcallExporter::ExportPipelineHTML()
{
  const QString path = lit("pipeline_state.html");
  IPipelineStateViewer *viewer = m_Ctx.GetPipelineViewer();
  if(viewer == NULL)
  {
    RecordFailure(lit("pipeline_html"), lit("Pipeline viewer is not available"));
    return;
  }

  if(viewer->ExportHTMLToFile(rdcstr(QDir(m_ExportDir).filePath(path))))
    RecordFile(lit("pipeline_html"), path);
  else
    RecordFailure(lit("pipeline_html"), lit("Pipeline viewer failed to export HTML"));
}

void DrawcallExporter::ExportPipelineJSON()
{
  const QString path = lit("pipeline_state.json");
  if(WriteJSONFile(path, MakePipelineStateJSON()))
    RecordFile(lit("pipeline_json"), path);
}

QVariantMap DrawcallExporter::MakeDrawcallJSON() const
{
  const ActionDescription *action = m_Ctx.CurAction();
  QVariantMap root;

  root[lit("schema")] = lit("renderdoc.current_drawcall.v1");
  root[lit("capture_file")] = ToQStr(m_Ctx.GetCaptureFilename());
  root[lit("api")] = ToQStr(m_Ctx.APIProps().pipelineType);
  root[lit("event_id")] = action ? action->eventId : 0;
  root[lit("selected_event_id")] = m_Ctx.CurSelectedEvent();
  root[lit("frame_number")] = m_Ctx.FrameInfo().frameNumber;
  root[lit("exported_at")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

  if(action)
  {
    root[lit("action_id")] = action->actionId;
    root[lit("action_name")] = ToQStr(action->GetName(m_Ctx.GetStructuredFile()));
    root[lit("custom_name")] = ToQStr(action->customName);
    root[lit("flags")] = ActionFlagsString(action->flags);
    root[lit("flags_raw")] = (uint32_t)action->flags;
    root[lit("num_indices")] = action->numIndices;
    root[lit("num_instances")] = action->numInstances;
    root[lit("base_vertex")] = action->baseVertex;
    root[lit("index_offset")] = action->indexOffset;
    root[lit("vertex_offset")] = action->vertexOffset;
    root[lit("instance_offset")] = action->instanceOffset;
    root[lit("draw_index")] = action->drawIndex;

    QVariantList dispatchDim;
    QVariantList dispatchThreads;
    QVariantList dispatchBase;
    for(int i = 0; i < 3; i++)
    {
      dispatchDim.push_back(action->dispatchDimension[i]);
      dispatchThreads.push_back(action->dispatchThreadsDimension[i]);
      dispatchBase.push_back(action->dispatchBase[i]);
    }
    root[lit("dispatch_dimension")] = dispatchDim;
    root[lit("dispatch_threads_dimension")] = dispatchThreads;
    root[lit("dispatch_base")] = dispatchBase;
  }

  const PipeState &pipe = m_Ctx.CurPipelineState();
  root[lit("graphics_pipeline")] = ToQStr(pipe.GetGraphicsPipelineObject());
  root[lit("compute_pipeline")] = ToQStr(pipe.GetComputePipelineObject());

  QVariantMap shaders;
  for(ShaderStage stage : ActiveShaderStages())
  {
    QVariantMap shader;
    shader[lit("stage")] = StageShortName(stage);
    shader[lit("resource_id")] = ToQStr(pipe.GetShader(stage));
    shader[lit("entry_point")] = ToQStr(pipe.GetShaderEntryPoint(stage));
    const ShaderReflection *refl = pipe.GetShaderReflection(stage);
    shader[lit("has_reflection")] = refl != NULL;
    if(refl)
      shader[lit("encoding")] = ToQStr(refl->encoding);
    shaders[StageShortName(stage)] = shader;
  }
  root[lit("shaders")] = shaders;

  root[lit("topology")] = ToQStr(pipe.GetPrimitiveTopology());

  return root;
}

QVariantMap DrawcallExporter::MakePipelineStateJSON() const
{
  const PipeState &pipe = m_Ctx.CurPipelineState();
  QVariantMap root;

  root[lit("schema")] = lit("renderdoc.pipeline_state.v1");
  root[lit("api")] = ToQStr(m_Ctx.APIProps().pipelineType);
  root[lit("event_id")] = m_Ctx.CurEvent();
  root[lit("graphics_pipeline")] = ToQStr(pipe.GetGraphicsPipelineObject());
  root[lit("compute_pipeline")] = ToQStr(pipe.GetComputePipelineObject());
  root[lit("topology")] = ToQStr(pipe.GetPrimitiveTopology());
  root[lit("restart_enabled")] = pipe.IsRestartEnabled();
  root[lit("restart_index")] = pipe.GetRestartIndex();

  QVariantMap shaders;
  for(ShaderStage stage : ActiveShaderStages())
  {
    QVariantMap sh;
    sh[lit("stage")] = StageShortName(stage);
    sh[lit("stage_name")] = ToQStr(stage, m_Ctx.APIProps().pipelineType);
    sh[lit("shader")] = ToQStr(pipe.GetShader(stage));
    sh[lit("entry_point")] = ToQStr(pipe.GetShaderEntryPoint(stage));

    const ShaderReflection *refl = pipe.GetShaderReflection(stage);
    sh[lit("has_reflection")] = refl != NULL;
    if(refl)
    {
      sh[lit("encoding")] = ToQStr(refl->encoding);
      sh[lit("constant_blocks")] = MakeConstantBlockList(refl->constantBlocks);
      sh[lit("read_only_reflection_resources")] = MakeResourceList(refl->readOnlyResources);
      sh[lit("read_write_reflection_resources")] = MakeResourceList(refl->readWriteResources);
    }

    sh[lit("read_only_descriptors")] =
        MakeUsedDescriptorList(pipe.GetReadOnlyResources(stage, m_Options.onlyUsedDescriptors));
    sh[lit("read_write_descriptors")] =
        MakeUsedDescriptorList(pipe.GetReadWriteResources(stage, m_Options.onlyUsedDescriptors));
    sh[lit("constant_buffer_descriptors")] =
        MakeUsedDescriptorList(pipe.GetConstantBlocks(stage, m_Options.onlyUsedDescriptors));
    sh[lit("samplers")] =
        MakeUsedDescriptorList(pipe.GetSamplers(stage, m_Options.onlyUsedDescriptors));

    shaders[StageShortName(stage)] = sh;
  }
  root[lit("shaders")] = shaders;

  QVariantMap vertexInput;
  vertexInput[lit("vertex_buffers")] = MakeVBufferList(pipe.GetVBuffers());
  vertexInput[lit("index_buffer")] = MakeBoundVBufferJSON(pipe.GetIBuffer());
  vertexInput[lit("attributes")] = MakeVertexInputList(pipe.GetVertexInputs());
  root[lit("vertex_input")] = vertexInput;

  QVariantMap outputs;
  outputs[lit("render_targets")] = MakeOutputTargetList();
  outputs[lit("depth_target")] = MakeDescriptorJSON(pipe.GetDepthTarget());
  outputs[lit("depth_resolve_target")] = MakeDescriptorJSON(pipe.GetDepthResolveTarget());
  root[lit("outputs")] = outputs;

  root[lit("viewports")] = MakeViewportList();
  root[lit("scissors")] = MakeScissorList();

  QVariantList descriptorGraph;
  for(const UsedDescriptor &used : pipe.GetAllUsedDescriptors(m_Options.onlyUsedDescriptors))
    descriptorGraph.push_back(MakeUsedDescriptorJSON(used));
  root[lit("descriptor_graph")] = descriptorGraph;

  QVariantMap unsupported;
  unsupported[lit("api_specific_state")] =
      lit("Use pipeline_state.html for full API-specific UI export. This JSON contains the "
          "API-agnostic PipeState subset plus descriptor graph.");
  root[lit("unsupported_or_deferred")] = unsupported;

  return root;
}

void DrawcallExporter::ExportTexturesAndSamplers()
{
  EnsureDir(lit("Textures/Inputs"));
  EnsureDir(lit("Textures/Outputs"));
  EnsureDir(lit("Textures/Metadata"));

  QVariantList textureMetadata;

  if(m_Options.exportInputs || m_Options.exportOutputs)
  {
    for(ShaderStage stage : ActiveShaderStages())
    {
      const QString stageShort = StageShortName(stage);
      if(m_Options.exportInputs)
        ExportShaderTextures(stage, stageShort, lit("read_only"),
                             m_Ctx.CurPipelineState().GetReadOnlyResources(
                                 stage, m_Options.onlyUsedDescriptors),
                             textureMetadata);
      if(m_Options.exportOutputs)
        ExportShaderTextures(stage, stageShort, lit("read_write"),
                             m_Ctx.CurPipelineState().GetReadWriteResources(
                                 stage, m_Options.onlyUsedDescriptors),
                             textureMetadata);
    }
  }

  if(m_Options.exportOutputs)
    ExportOutputTextures(textureMetadata);

  QVariantMap texturesRoot;
  texturesRoot[lit("textures")] = textureMetadata;
  if(WriteJSONFile(lit("Textures/Metadata/textures.json"), texturesRoot))
    RecordFile(lit("texture_metadata"), lit("Textures/Metadata/textures.json"));

  if(m_Options.exportSamplerState)
  {
    QVariantList samplers;
    for(ShaderStage stage : ActiveShaderStages())
    {
      const QString stageShort = StageShortName(stage);
      for(const UsedDescriptor &used :
          m_Ctx.CurPipelineState().GetSamplers(stage, m_Options.onlyUsedDescriptors))
      {
        QVariantMap sampler = MakeUsedDescriptorJSON(used);
        sampler[lit("stage")] = stageShort;
        sampler[lit("binding")] = DescriptorBindingName(used, lit("s"));
        samplers.push_back(sampler);
      }
    }

    QVariantMap samplersRoot;
    samplersRoot[lit("samplers")] = samplers;
    if(WriteJSONFile(lit("Textures/Metadata/samplers.json"), samplersRoot))
      RecordFile(lit("sampler_metadata"), lit("Textures/Metadata/samplers.json"));
  }
}

void DrawcallExporter::ExportShaderTextures(ShaderStage stage, const QString &stageShort,
                                            const QString &role,
                                            const rdcarray<UsedDescriptor> &descriptors,
                                            QVariantList &textureMetadata)
{
  for(const UsedDescriptor &used : descriptors)
  {
    if(!IsTextureResource(used.descriptor.resource))
      continue;

    const bool readWrite = role == lit("read_write");
    const QString binding = DescriptorBindingName(used, readWrite ? lit("u") : lit("t"));
    SaveTextureDescriptor(readWrite ? lit("output_texture") : lit("input_texture"), role,
                          stageShort, binding, used.descriptor,
                          textureMetadata);
  }
}

void DrawcallExporter::ExportOutputTextures(QVariantList &textureMetadata)
{
  const rdcarray<Descriptor> rts = m_Ctx.CurPipelineState().GetOutputTargets();
  for(uint32_t i = 0; i < rts.size(); i++)
  {
    if(IsTextureResource(rts[i].resource))
      SaveTextureDescriptor(lit("output_texture"), QFormatStr("rt%1").arg(i), lit("output"),
                            QFormatStr("rt%1").arg(i), rts[i], textureMetadata);
  }

  Descriptor depth = m_Ctx.CurPipelineState().GetDepthTarget();
  if(IsTextureResource(depth.resource))
    SaveTextureDescriptor(lit("output_texture"), lit("depth"), lit("output"), lit("depth"), depth,
                          textureMetadata);

  Descriptor depthResolve = m_Ctx.CurPipelineState().GetDepthResolveTarget();
  if(IsTextureResource(depthResolve.resource))
    SaveTextureDescriptor(lit("output_texture"), lit("depth_resolve"), lit("output"),
                          lit("depth_resolve"), depthResolve, textureMetadata);
}

void DrawcallExporter::ExportShaders()
{
  for(ShaderStage stage : ActiveShaderStages())
    ExportShaderStage(stage, StageShortName(stage));
}

void DrawcallExporter::ExportShaderStage(ShaderStage stage, const QString &stageShort)
{
  const PipeState &pipe = m_Ctx.CurPipelineState();
  const ShaderReflection *refl = pipe.GetShaderReflection(stage);
  const QString shaderDir = lit("Shaders/") + stageShort;
  EnsureDir(shaderDir);

  if(refl == NULL)
  {
    QVariantMap meta;
    meta[lit("stage")] = stageShort;
    RecordFailure(lit("shader"), lit("No shader reflection available"), meta);
    return;
  }

  QVariantMap shaderJSON = MakeShaderJSON(refl, stage);
  if(WriteJSONFile(shaderDir + lit("/shader.json"), shaderJSON))
  {
    QVariantMap meta;
    meta[lit("stage")] = stageShort;
    RecordFile(lit("shader_metadata"), shaderDir + lit("/shader.json"), meta);
  }

  if(m_Options.exportShaderReflectionJSON)
  {
    QVariantMap reflectionJSON = MakeShaderReflectionJSON(refl);
    if(WriteJSONFile(shaderDir + lit("/reflection.json"), reflectionJSON))
    {
      QVariantMap meta;
      meta[lit("stage")] = stageShort;
      meta[lit("shader")] = ToQStr(refl->resourceId);
      RecordFile(lit("shader_reflection"), shaderDir + lit("/reflection.json"), meta);
    }
  }

  const ResourceId pipeline = PipelineObjectForStage(stage);
  rdcarray<rdcstr> disasmTargets;
  rdcstr defaultDisasm;
  m_Ctx.Replay().BlockInvoke([&disasmTargets, &defaultDisasm, pipeline, refl](IReplayController *r) {
    disasmTargets = r->GetDisassemblyTargets(pipeline != ResourceId());
    defaultDisasm = r->DisassembleShader(pipeline, refl, "");
  });

  QVariantList disasmTargetJSON;
  QVariantList disasmExportJSON;
  bool hlslDecompilerExported = false;
  const QString defaultTarget =
      disasmTargets.empty() ? lit("native") : ToQStr(disasmTargets[0]);

  for(int i = 0; i < disasmTargets.count(); i++)
  {
    QVariantMap targetMeta;
    targetMeta[lit("name")] = ToQStr(disasmTargets[i]);
    targetMeta[lit("native_default")] = i == 0;
    targetMeta[lit("contains_hlsl")] =
        ToQStr(disasmTargets[i]).contains(lit("hlsl"), Qt::CaseInsensitive);
    disasmTargetJSON.push_back(targetMeta);
  }

  auto recordDisassembly = [&](const QString &category, const QString &path,
                               const QString &text, const QString &target,
                               const QString &source) {
    if(WriteTextFile(path, text))
    {
      QVariantMap meta;
      meta[lit("stage")] = stageShort;
      meta[lit("shader")] = ToQStr(refl->resourceId);
      meta[lit("target")] = target;
      meta[lit("source")] = source;
      RecordFile(category, path, meta);

      QVariantMap exported;
      exported[lit("path")] = path;
      exported[lit("category")] = category;
      exported[lit("target")] = target;
      exported[lit("source")] = source;
      disasmExportJSON.push_back(exported);

      if(category == lit("shader_disassembly_hlsl"))
        hlslDecompilerExported = true;
    }
  };

  if(defaultDisasm.empty())
  {
    QVariantMap meta;
    meta[lit("stage")] = stageShort;
    meta[lit("shader")] = ToQStr(refl->resourceId);
    RecordFailure(lit("shader_disassembly"), lit("Disassembly was empty"), meta);
  }
  else
  {
    const QString path = shaderDir + lit("/disassembly_default.txt");
    recordDisassembly(lit("shader_disassembly"), path, ToQStr(defaultDisasm), defaultTarget,
                      lit("renderdoc_default"));

    const QString nativePath =
        UniqueRelativePath(shaderDir, DisassemblyFileBase(lit("native"), defaultTarget), lit(".txt"));
    recordDisassembly(lit("shader_disassembly_native"), nativePath, ToQStr(defaultDisasm),
                      defaultTarget, lit("renderdoc"));
  }

  for(int i = 0; i < disasmTargets.count(); i++)
  {
    const rdcstr targetName = disasmTargets[i];
    const QString target = ToQStr(targetName);
    if(!target.contains(lit("hlsl"), Qt::CaseInsensitive))
      continue;

    rdcstr hlslDisasm;
    m_Ctx.Replay().BlockInvoke([pipeline, refl, targetName, &hlslDisasm](IReplayController *r) {
      hlslDisasm = r->DisassembleShader(pipeline, refl, targetName);
    });

    if(!hlslDisasm.empty())
    {
      const QString path =
          UniqueRelativePath(shaderDir, DisassemblyFileBase(lit("hlsl"), target), lit(".txt"));
      recordDisassembly(lit("shader_disassembly_hlsl"), path, ToQStr(hlslDisasm), target,
                        lit("renderdoc"));
    }
  }

  for(const ShaderProcessingTool &processor : m_Ctx.Config().ShaderProcessors)
  {
    if(processor.input != refl->encoding || processor.output != ShaderEncoding::HLSL)
      continue;

    const QString target = ShaderProcessorTargetName(processor);
    QVariantMap targetMeta;
    targetMeta[lit("name")] = target;
    targetMeta[lit("native_default")] = false;
    targetMeta[lit("contains_hlsl")] = true;
    targetMeta[lit("external_processor")] = true;
    targetMeta[lit("tool")] = ToQStr(processor.tool);
    targetMeta[lit("executable")] = ToQStr(processor.executable);
    disasmTargetJSON.push_back(targetMeta);

    ShaderToolOutput out = processor.DisassembleShader(m_Parent, refl, "");
    if(out.result.empty())
    {
      if(!out.log.empty())
      {
        const QString logPath =
            UniqueRelativePath(shaderDir, DisassemblyFileBase(lit("hlsl_log"), target), lit(".txt"));
        recordDisassembly(lit("shader_disassembly_hlsl_log"), logPath, ToQStr(out.log), target,
                          lit("shader_processor"));
      }
      continue;
    }

    const QString text = QString::fromUtf8((const char *)out.result.data(), (int)out.result.size());
    if(text.trimmed().isEmpty())
      continue;

    const QString path =
        UniqueRelativePath(shaderDir, DisassemblyFileBase(lit("hlsl"), target), lit(".txt"));
    recordDisassembly(lit("shader_disassembly_hlsl"), path, text, target,
                      lit("shader_processor"));
  }

  QVariantMap disasmRoot;
  disasmRoot[lit("native_default_target")] = defaultTarget;
  disasmRoot[lit("targets")] = disasmTargetJSON;
  disasmRoot[lit("exports")] = disasmExportJSON;
  disasmRoot[lit("hlsl_decompiler_exported")] = hlslDecompilerExported;
  if(WriteJSONFile(shaderDir + lit("/disassembly_targets.json"), disasmRoot))
  {
    QVariantMap meta;
    meta[lit("stage")] = stageShort;
    meta[lit("shader")] = ToQStr(refl->resourceId);
    RecordFile(lit("shader_disassembly_metadata"), shaderDir + lit("/disassembly_targets.json"),
               meta);
  }

  if(m_Options.exportShaderRawBytes)
  {
    if(refl->rawBytes.empty())
    {
      QVariantMap meta;
      meta[lit("stage")] = stageShort;
      meta[lit("shader")] = ToQStr(refl->resourceId);
      RecordFailure(lit("shader_raw"), lit("Shader raw bytes are empty"), meta);
    }
    else
    {
      const QString path = shaderDir + lit("/raw_shader.bin");
      if(WriteBytesFile(path, refl->rawBytes))
      {
        QVariantMap meta;
        meta[lit("stage")] = stageShort;
        meta[lit("shader")] = ToQStr(refl->resourceId);
        RecordFile(lit("shader_raw"), path, meta);
      }
    }
  }

  if(m_Options.exportShaderSourceFiles)
  {
    int fileIndex = 0;
    for(const ShaderSourceFile &source : refl->debugInfo.files)
    {
      QString sourceName = SafeName(QFileInfo(ToQStr(source.filename)).fileName(),
                                    QFormatStr("source_%1").arg(fileIndex));
      QString path = UniqueRelativePath(shaderDir, QFormatStr("source_%1__%2").arg(fileIndex).arg(sourceName),
                                        lit(".txt"));
      if(WriteTextFile(path, ToQStr(source.contents)))
      {
        QVariantMap meta;
        meta[lit("stage")] = stageShort;
        meta[lit("shader")] = ToQStr(refl->resourceId);
        meta[lit("source_index")] = fileIndex;
        meta[lit("original_filename")] = ToQStr(source.filename);
        RecordFile(lit("shader_source"), path, meta);
      }
      fileIndex++;
    }

    if(fileIndex == 0)
    {
      QVariantMap meta;
      meta[lit("stage")] = stageShort;
      meta[lit("shader")] = ToQStr(refl->resourceId);
      RecordFailure(lit("shader_source"), lit("No embedded shader source files"), meta);
    }
  }
}

QVariantMap DrawcallExporter::MakeShaderJSON(const ShaderReflection *refl, ShaderStage stage) const
{
  QVariantMap ret;
  ret[lit("stage")] = StageShortName(stage);
  ret[lit("stage_name")] = ToQStr(stage, m_Ctx.APIProps().pipelineType);
  ret[lit("resource_id")] = refl ? ToQStr(refl->resourceId) : ToQStr(ResourceId());
  ret[lit("pipeline_id")] = ToQStr(PipelineObjectForStage(stage));
  ret[lit("entry_point")] = refl ? ToQStr(refl->entryPoint) : QString();
  ret[lit("encoding")] = refl ? ToQStr(refl->encoding) : QString();
  ret[lit("raw_byte_size")] = refl ? (uint)refl->rawBytes.size() : 0U;
  ret[lit("source_file_count")] = refl ? (uint)refl->debugInfo.files.size() : 0U;
  ret[lit("entry_source_name")] = refl ? ToQStr(refl->debugInfo.entrySourceName) : QString();
  return ret;
}

QVariantMap DrawcallExporter::MakeShaderReflectionJSON(const ShaderReflection *refl) const
{
  QVariantMap ret;
  if(refl == NULL)
    return ret;

  ret[lit("resource_id")] = ToQStr(refl->resourceId);
  ret[lit("entry_point")] = ToQStr(refl->entryPoint);
  ret[lit("stage")] = StageShortName(refl->stage);
  ret[lit("encoding")] = ToQStr(refl->encoding);
  ret[lit("input_signature")] = MakeSignatureList(refl->inputSignature);
  ret[lit("output_signature")] = MakeSignatureList(refl->outputSignature);
  ret[lit("constant_blocks")] = MakeConstantBlockList(refl->constantBlocks);
  ret[lit("read_only_resources")] = MakeResourceList(refl->readOnlyResources);
  ret[lit("read_write_resources")] = MakeResourceList(refl->readWriteResources);
  return ret;
}

QVariantList DrawcallExporter::MakeSignatureList(const rdcarray<SigParameter> &sig) const
{
  QVariantList ret;
  for(const SigParameter &param : sig)
  {
    QVariantMap p;
    p[lit("var_name")] = ToQStr(param.varName);
    p[lit("semantic_name")] = ToQStr(param.semanticName);
    p[lit("semantic_index_name")] = ToQStr(param.semanticIdxName);
    p[lit("semantic_index")] = param.semanticIndex;
    p[lit("register_index")] = param.regIndex == SigParameter::NoIndex ? lit("NoIndex") : QString::number(param.regIndex);
    p[lit("system_value")] = ToQStr(param.systemValue);
    p[lit("var_type")] = ToQStr(param.varType);
    p[lit("component_count")] = param.compCount;
    p[lit("register_channel_mask")] = param.regChannelMask;
    p[lit("channel_used_mask")] = param.channelUsedMask;
    p[lit("stream")] = param.stream;
    p[lit("per_primitive_rate")] = param.perPrimitiveRate;
    ret.push_back(p);
  }
  return ret;
}

QVariantList DrawcallExporter::MakeResourceList(const rdcarray<ShaderResource> &resources) const
{
  QVariantList ret;
  for(const ShaderResource &res : resources)
  {
    QVariantMap r;
    r[lit("name")] = ToQStr(res.name);
    r[lit("descriptor_type")] = ToQStr(res.descriptorType);
    r[lit("texture_type")] = ToQStr(res.textureType);
    r[lit("fixed_bind_number")] = res.fixedBindNumber;
    r[lit("fixed_bind_set_or_space")] = res.fixedBindSetOrSpace;
    r[lit("bind_array_size")] = res.bindArraySize;
    r[lit("is_texture")] = res.isTexture;
    r[lit("has_sampler")] = res.hasSampler;
    r[lit("is_input_attachment")] = res.isInputAttachment;
    r[lit("is_read_only")] = res.isReadOnly;
    ret.push_back(r);
  }
  return ret;
}

QVariantList DrawcallExporter::MakeConstantBlockList(const rdcarray<ConstantBlock> &blocks) const
{
  QVariantList ret;
  for(const ConstantBlock &block : blocks)
  {
    QVariantMap b;
    b[lit("name")] = ToQStr(block.name);
    b[lit("fixed_bind_number")] = block.fixedBindNumber;
    b[lit("fixed_bind_set_or_space")] = block.fixedBindSetOrSpace;
    b[lit("bind_array_size")] = block.bindArraySize;
    b[lit("byte_size")] = block.byteSize;
    b[lit("buffer_backed")] = block.bufferBacked;
    b[lit("inline_data_bytes")] = block.inlineDataBytes;
    b[lit("compile_constants")] = block.compileConstants;
    ret.push_back(b);
  }
  return ret;
}

QVariantMap DrawcallExporter::MakeResourceFormatJSON(const ResourceFormat &format) const
{
  QVariantMap ret;
  ret[lit("name")] = ToQStr(format.Name());
  ret[lit("type")] = ToQStr(format.type);
  ret[lit("component_count")] = format.compCount;
  ret[lit("component_byte_width")] = format.compByteWidth;
  ret[lit("component_type")] = ToQStr(format.compType);
  ret[lit("element_size")] = format.ElementSize();
  ret[lit("bgra_order")] = format.BGRAOrder();
  ret[lit("srgb")] = format.SRGBCorrected();
  ret[lit("block_format")] = format.BlockFormat();
  ret[lit("yuv_subsampling")] = format.YUVSubsampling();
  ret[lit("yuv_plane_count")] = format.YUVPlaneCount();
  return ret;
}

QVariantMap DrawcallExporter::MakeDescriptorJSON(const Descriptor &descriptor) const
{
  QVariantMap ret;
  ret[lit("type")] = ToQStr(descriptor.type);
  ret[lit("flags")] = ToQStr(descriptor.flags);
  ret[lit("resource_id")] = ToQStr(descriptor.resource);
  ret[lit("resource_name")] = m_Ctx.GetResourceName(descriptor.resource);
  ret[lit("secondary_id")] = ToQStr(descriptor.secondary);
  ret[lit("view_id")] = ToQStr(descriptor.view);
  ret[lit("format")] = MakeResourceFormatJSON(descriptor.format);
  ret[lit("byte_offset")] = QString::number(descriptor.byteOffset);
  ret[lit("byte_size")] = QString::number(descriptor.byteSize);
  ret[lit("counter_byte_offset")] = descriptor.counterByteOffset;
  ret[lit("buffer_struct_count")] = descriptor.bufferStructCount;
  ret[lit("element_byte_size")] = descriptor.elementByteSize;
  ret[lit("first_slice")] = descriptor.firstSlice;
  ret[lit("num_slices")] = descriptor.numSlices;
  ret[lit("first_mip")] = descriptor.firstMip;
  ret[lit("num_mips")] = descriptor.numMips;
  ret[lit("texture_type")] = ToQStr(descriptor.textureType);
  ret[lit("is_texture")] = IsTextureResource(descriptor.resource);
  ret[lit("is_buffer")] = IsBufferResource(descriptor.resource);
  ret[lit("texture")] = MakeTextureJSON(m_Ctx.GetTexture(descriptor.resource));
  ret[lit("buffer")] = MakeBufferJSON(m_Ctx.GetBuffer(descriptor.resource));
  return ret;
}

QVariantMap DrawcallExporter::MakeSamplerJSON(const SamplerDescriptor &sampler) const
{
  QVariantMap ret;
  ret[lit("type")] = ToQStr(sampler.type);
  ret[lit("object_id")] = ToQStr(sampler.object);
  ret[lit("address_u")] = ToQStr(sampler.addressU);
  ret[lit("address_v")] = ToQStr(sampler.addressV);
  ret[lit("address_w")] = ToQStr(sampler.addressW);
  ret[lit("compare_function")] = ToQStr(sampler.compareFunction);
  ret[lit("filter")] = ToQStr(sampler.filter);
  ret[lit("max_anisotropy")] = sampler.maxAnisotropy;
  ret[lit("min_lod")] = sampler.minLOD;
  ret[lit("max_lod")] = sampler.maxLOD;
  ret[lit("mip_bias")] = sampler.mipBias;
  ret[lit("border_color_type")] = ToQStr(sampler.borderColorType);
  ret[lit("uses_border")] = sampler.UseBorder();
  ret[lit("srgb_border")] = sampler.srgbBorder;
  ret[lit("seamless_cubemaps")] = sampler.seamlessCubemaps;
  ret[lit("unnormalized")] = sampler.unnormalized;
  ret[lit("creation_time_constant")] = sampler.creationTimeConstant;
  ret[lit("ycbcr_sampler")] = ToQStr(sampler.ycbcrSampler);
  return ret;
}

QVariantMap DrawcallExporter::MakeDescriptorAccessJSON(const DescriptorAccess &access) const
{
  QVariantMap ret;
  ret[lit("stage")] = StageShortName(access.stage);
  ret[lit("stage_name")] = ToQStr(access.stage, m_Ctx.APIProps().pipelineType);
  ret[lit("descriptor_type")] = ToQStr(access.type);
  ret[lit("index")] =
      access.index == DescriptorAccess::NoShaderBinding ? lit("NoShaderBinding") : QString::number(access.index);
  ret[lit("array_element")] = access.arrayElement;
  ret[lit("descriptor_store")] = ToQStr(access.descriptorStore);
  ret[lit("byte_offset")] = access.byteOffset;
  ret[lit("byte_size")] = access.byteSize;
  ret[lit("statically_unused")] = access.staticallyUnused;
  return ret;
}

QVariantMap DrawcallExporter::MakeUsedDescriptorJSON(const UsedDescriptor &used) const
{
  QVariantMap ret;
  ret[lit("access")] = MakeDescriptorAccessJSON(used.access);
  ret[lit("descriptor")] = MakeDescriptorJSON(used.descriptor);
  ret[lit("sampler")] = MakeSamplerJSON(used.sampler);
  ret[lit("binding")] = DescriptorBindingName(used, lit("r"));
  return ret;
}

QVariantMap DrawcallExporter::MakeTextureJSON(const TextureDescription *tex) const
{
  QVariantMap ret;
  if(tex == NULL)
    return ret;

  ret[lit("resource_id")] = ToQStr(tex->resourceId);
  ret[lit("name")] = m_Ctx.GetResourceName(tex->resourceId);
  ret[lit("format")] = MakeResourceFormatJSON(tex->format);
  ret[lit("dimension")] = tex->dimension;
  ret[lit("texture_type")] = ToQStr(tex->type);
  ret[lit("width")] = tex->width;
  ret[lit("height")] = tex->height;
  ret[lit("depth")] = tex->depth;
  ret[lit("cubemap")] = tex->cubemap;
  ret[lit("mips")] = tex->mips;
  ret[lit("array_size")] = tex->arraysize;
  ret[lit("creation_flags")] = ToQStr(tex->creationFlags);
  ret[lit("ms_quality")] = tex->msQual;
  ret[lit("ms_samples")] = tex->msSamp;
  ret[lit("byte_size")] = QString::number(tex->byteSize);
  return ret;
}

QVariantMap DrawcallExporter::MakeBufferJSON(const BufferDescription *buf) const
{
  QVariantMap ret;
  if(buf == NULL)
    return ret;

  ret[lit("resource_id")] = ToQStr(buf->resourceId);
  ret[lit("name")] = m_Ctx.GetResourceName(buf->resourceId);
  ret[lit("creation_flags")] = ToQStr(buf->creationFlags);
  ret[lit("gpu_address")] = QString::number(buf->gpuAddress);
  ret[lit("length")] = QString::number(buf->length);
  return ret;
}

QVariantMap DrawcallExporter::MakeBoundVBufferJSON(const BoundVBuffer &buffer) const
{
  QVariantMap ret;
  ret[lit("resource_id")] = ToQStr(buffer.resourceId);
  ret[lit("resource_name")] = m_Ctx.GetResourceName(buffer.resourceId);
  ret[lit("byte_offset")] = QString::number(buffer.byteOffset);
  ret[lit("byte_stride")] = buffer.byteStride;
  ret[lit("byte_size")] = QString::number(buffer.byteSize);
  ret[lit("buffer")] = MakeBufferJSON(m_Ctx.GetBuffer(buffer.resourceId));
  return ret;
}

QVariantMap DrawcallExporter::MakeVertexInputJSON(const VertexInputAttribute &input) const
{
  QVariantMap ret;
  ret[lit("name")] = ToQStr(input.name);
  ret[lit("vertex_buffer")] = input.vertexBuffer;
  ret[lit("byte_offset")] = input.byteOffset;
  ret[lit("per_instance")] = input.perInstance;
  ret[lit("instance_rate")] = input.instanceRate;
  ret[lit("format")] = MakeResourceFormatJSON(input.format);
  ret[lit("generic_enabled")] = input.genericEnabled;
  ret[lit("used")] = input.used;
  return ret;
}

QVariantMap DrawcallExporter::MakeViewportJSON(const Viewport &viewport) const
{
  QVariantMap ret;
  ret[lit("enabled")] = viewport.enabled;
  ret[lit("x")] = viewport.x;
  ret[lit("y")] = viewport.y;
  ret[lit("width")] = viewport.width;
  ret[lit("height")] = viewport.height;
  ret[lit("min_depth")] = viewport.minDepth;
  ret[lit("max_depth")] = viewport.maxDepth;
  return ret;
}

QVariantMap DrawcallExporter::MakeScissorJSON(const Scissor &scissor) const
{
  QVariantMap ret;
  ret[lit("enabled")] = scissor.enabled;
  ret[lit("x")] = scissor.x;
  ret[lit("y")] = scissor.y;
  ret[lit("width")] = scissor.width;
  ret[lit("height")] = scissor.height;
  return ret;
}

QVariantMap DrawcallExporter::MakeMeshFormatJSON(const MeshFormat &mesh) const
{
  QVariantMap ret;
  ret[lit("index_resource_id")] = ToQStr(mesh.indexResourceId);
  ret[lit("index_byte_offset")] = QString::number(mesh.indexByteOffset);
  ret[lit("index_byte_stride")] = mesh.indexByteStride;
  ret[lit("index_byte_size")] = QString::number(mesh.indexByteSize);
  ret[lit("base_vertex")] = mesh.baseVertex;
  ret[lit("vertex_resource_id")] = ToQStr(mesh.vertexResourceId);
  ret[lit("vertex_byte_offset")] = QString::number(mesh.vertexByteOffset);
  ret[lit("vertex_byte_stride")] = mesh.vertexByteStride;
  ret[lit("vertex_byte_size")] = QString::number(mesh.vertexByteSize);
  ret[lit("format")] = MakeResourceFormatJSON(mesh.format);
  ret[lit("topology")] = ToQStr(mesh.topology);
  ret[lit("num_indices")] = mesh.numIndices;
  ret[lit("inst_step_rate")] = mesh.instStepRate;
  ret[lit("restart_index")] = mesh.restartIndex;
  ret[lit("unproject")] = mesh.unproject;
  ret[lit("flip_y")] = mesh.flipY;
  ret[lit("instanced")] = mesh.instanced;
  ret[lit("show_alpha")] = mesh.showAlpha;
  ret[lit("allow_restart")] = mesh.allowRestart;
  ret[lit("status")] = ToQStr(mesh.status);
  return ret;
}

QVariantList DrawcallExporter::MakeUsedDescriptorList(const rdcarray<UsedDescriptor> &descriptors) const
{
  QVariantList ret;
  for(const UsedDescriptor &used : descriptors)
    ret.push_back(MakeUsedDescriptorJSON(used));
  return ret;
}

QVariantList DrawcallExporter::MakeVBufferList(const rdcarray<BoundVBuffer> &buffers) const
{
  QVariantList ret;
  for(const BoundVBuffer &buffer : buffers)
    ret.push_back(MakeBoundVBufferJSON(buffer));
  return ret;
}

QVariantList DrawcallExporter::MakeVertexInputList(const rdcarray<VertexInputAttribute> &inputs) const
{
  QVariantList ret;
  for(const VertexInputAttribute &input : inputs)
    ret.push_back(MakeVertexInputJSON(input));
  return ret;
}

QVariantList DrawcallExporter::MakeOutputTargetList() const
{
  QVariantList ret;
  const rdcarray<Descriptor> rts = m_Ctx.CurPipelineState().GetOutputTargets();
  for(uint32_t i = 0; i < rts.size(); i++)
  {
    QVariantMap rt = MakeDescriptorJSON(rts[i]);
    rt[lit("slot")] = i;
    rt[lit("binding")] = QFormatStr("rt%1").arg(i);
    ret.push_back(rt);
  }
  return ret;
}

QVariantList DrawcallExporter::MakeViewportList() const
{
  QVariantList ret;
  const PipeState &pipe = m_Ctx.CurPipelineState();
  for(uint32_t i = 0; i < 16; i++)
  {
    Viewport vp = pipe.GetViewport(i);
    if(vp.enabled || vp.width != 0.0f || vp.height != 0.0f)
    {
      QVariantMap v = MakeViewportJSON(vp);
      v[lit("slot")] = i;
      ret.push_back(v);
    }
  }
  return ret;
}

QVariantList DrawcallExporter::MakeScissorList() const
{
  QVariantList ret;
  const PipeState &pipe = m_Ctx.CurPipelineState();
  for(uint32_t i = 0; i < 16; i++)
  {
    Scissor sc = pipe.GetScissor(i);
    if(sc.enabled || sc.width != 0 || sc.height != 0)
    {
      QVariantMap s = MakeScissorJSON(sc);
      s[lit("slot")] = i;
      ret.push_back(s);
    }
  }
  return ret;
}

void DrawcallExporter::ExportConstantBuffers()
{
  m_ConstantBuffers.clear();
  for(ShaderStage stage : ActiveShaderStages())
    ExportConstantBuffersForStage(stage, StageShortName(stage));

  QVariantMap root;
  root[lit("notes")] =
      lit("Constant buffer display_name prefers the RenderDoc Buffer column name when available. Unity/URP Z-bin and Tile buffers can appear here as constant buffers; they are exported for completeness but are usually low-priority renderer intermediate data for shader reconstruction.");
  root[lit("buffers")] = m_ConstantBuffers;
  if(WriteJSONFile(lit("ConstantBuffers/constant_buffers.json"), root))
    RecordFile(lit("constant_buffer_index"), lit("ConstantBuffers/constant_buffers.json"));

  QString readme;
  readme += lit("# ConstantBuffers\n\n");
  readme += lit("This directory contains active shader stage constant buffers for the selected event, including CS for compute dispatches.\n\n");
  readme += lit("File names and `constant_buffers.json` use `display_name`, which prefers the same buffer object name shown in RenderDoc's Constant Buffers table. For example, `URP Z-Bin Buffer` and `URP Tile Buffer` should appear as `URP_Z-Bin_Buffer` and `URP_Tile_Buffer` in exported file names and index metadata.\n\n");
  readme += lit("For shader reconstruction, most material/per-draw/per-camera constant buffers are high priority. Unity/URP z-bin, tile, cluster, light-list, and culling buffers are usually renderer intermediate data and should be treated as low priority unless the shader logic explicitly reads those values.\n");
  if(WriteTextFile(lit("ConstantBuffers/README.md"), readme))
    RecordFile(lit("constant_buffer_notes"), lit("ConstantBuffers/README.md"));
}

void DrawcallExporter::ExportConstantBuffersForStage(ShaderStage stage, const QString &stageShort)
{
  const PipeState &pipe = m_Ctx.CurPipelineState();
  const ShaderReflection *refl = pipe.GetShaderReflection(stage);
  if(refl == NULL)
    return;

  rdcarray<UsedDescriptor> cblocks = pipe.GetConstantBlocks(stage, m_Options.onlyUsedDescriptors);
  if(cblocks.empty())
    return;

  EnsureDir(lit("ConstantBuffers/") + stageShort);

  for(const UsedDescriptor &used : cblocks)
  {
    const uint32_t slot = used.access.index;
    const uint32_t arrayIdx = used.access.arrayElement;
    if(slot >= refl->constantBlocks.size())
    {
      QVariantMap meta;
      meta[lit("stage")] = stageShort;
      meta[lit("slot")] = slot;
      RecordFailure(lit("constant_buffer"), lit("Descriptor references an invalid constant block slot"), meta);
      continue;
    }

    const ConstantBlock &block = refl->constantBlocks[slot];
    const QString binding = BindingName(lit("b"), slot);
    const QString blockName = ToQStr(block.name);
    const QString renderdocBufferName = m_Ctx.GetResourceName(used.descriptor.resource);
    const QString displayName = ConstantBufferDisplayName(block, used);
    const uint64_t rawLength = used.descriptor.byteSize ? used.descriptor.byteSize : block.byteSize;
    const QVariantMap guidance = ConstantBufferGuidance(displayName, rawLength);
    const QString baseName =
        MakeResourceBaseName(lit("cb"), stageShort, binding, used.descriptor.resource, displayName);
    const QString cbufDir = lit("ConstantBuffers/") + stageShort;

    ResourceId shader = pipe.GetShader(stage);
    rdcstr entry = pipe.GetShaderEntryPoint(stage);
    ResourceId pipeline = PipelineObjectForStage(stage);
    rdcarray<ShaderVariable> variables;

    m_Ctx.Replay().BlockInvoke([&](IReplayController *r) {
      variables = r->GetCBufferVariableContents(pipeline, shader, stage, entry, slot,
                                                used.descriptor.resource,
                                                used.descriptor.byteOffset,
                                                used.descriptor.byteSize);
    });

    QVariantMap cbufJSON = MakeConstantBlockJSON(block, used, stage, slot, arrayIdx, variables);
    QVariantMap indexEntry;
    indexEntry[lit("stage")] = stageShort;
    indexEntry[lit("binding")] = binding;
    indexEntry[lit("slot")] = slot;
    indexEntry[lit("resource")] = ToQStr(used.descriptor.resource);
    indexEntry[lit("display_name")] = displayName;
    indexEntry[lit("block_name")] = blockName;
    indexEntry[lit("renderdoc_buffer_name")] = renderdocBufferName;
    indexEntry[lit("byte_offset")] = QString::number(used.descriptor.byteOffset);
    indexEntry[lit("byte_size")] = QString::number(rawLength);
    indexEntry[lit("shader_reconstruction_priority")] =
        guidance[lit("shader_reconstruction_priority")];
    indexEntry[lit("recommended_for_shader_reconstruction")] =
        guidance[lit("recommended_for_shader_reconstruction")];
    indexEntry[lit("constant_buffer_guidance")] = guidance;

    QString jsonPath = UniqueRelativePath(cbufDir, baseName, lit(".json"));
    if(WriteJSONFile(jsonPath, cbufJSON))
    {
      QVariantMap meta;
      meta[lit("stage")] = stageShort;
      meta[lit("binding")] = binding;
      meta[lit("resource")] = ToQStr(used.descriptor.resource);
      meta[lit("display_name")] = displayName;
      meta[lit("block_name")] = blockName;
      meta[lit("renderdoc_buffer_name")] = renderdocBufferName;
      meta[lit("shader_reconstruction_priority")] =
          guidance[lit("shader_reconstruction_priority")];
      meta[lit("recommended_for_shader_reconstruction")] =
          guidance[lit("recommended_for_shader_reconstruction")];
      RecordFile(lit("constant_buffer_json"), jsonPath, meta);
      indexEntry[lit("json_path")] = jsonPath;
    }

    QString csvPath = UniqueRelativePath(cbufDir, baseName + lit(".variables"), lit(".csv"));
    QFileInfo csvInfo(QDir(m_ExportDir).filePath(csvPath));
    QDir().mkpath(csvInfo.dir().absolutePath());
    QFile csv(csvInfo.absoluteFilePath());
    if(csv.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
    {
      QTextStream stream(&csv);
      stream.setCodec("UTF-8");
      stream << "path,name,type,rows,columns,value0,value1,value2,value3,value4,value5,value6,value7,value8,value9,value10,value11,value12,value13,value14,value15\n";
      WriteShaderVariablesCSV(stream, variables);
      QVariantMap meta;
      meta[lit("stage")] = stageShort;
      meta[lit("binding")] = binding;
      meta[lit("resource")] = ToQStr(used.descriptor.resource);
      meta[lit("display_name")] = displayName;
      meta[lit("block_name")] = blockName;
      meta[lit("renderdoc_buffer_name")] = renderdocBufferName;
      meta[lit("shader_reconstruction_priority")] =
          guidance[lit("shader_reconstruction_priority")];
      meta[lit("recommended_for_shader_reconstruction")] =
          guidance[lit("recommended_for_shader_reconstruction")];
      RecordFile(lit("constant_buffer_csv"), csvPath, meta);
      indexEntry[lit("csv_path")] = csvPath;
    }
    else
    {
      RecordFailure(lit("constant_buffer_csv"),
                    QFormatStr("Couldn't write %1: %2").arg(csvPath).arg(csv.errorString()));
    }

    if(used.descriptor.resource != ResourceId() && rawLength > 0)
    {
      bytebuf raw;
      const ResourceId buffer = used.descriptor.resource;
      const uint64_t byteOffset = used.descriptor.byteOffset;
      m_Ctx.Replay().BlockInvoke([&](IReplayController *r) {
        raw = r->GetBufferData(buffer, byteOffset, rawLength);
      });

      QString rawPath = UniqueRelativePath(cbufDir, baseName + lit(".raw"), lit(".bin"));
      if(WriteBytesFile(rawPath, raw))
      {
        QVariantMap meta;
        meta[lit("stage")] = stageShort;
        meta[lit("binding")] = binding;
        meta[lit("resource")] = ToQStr(used.descriptor.resource);
        meta[lit("display_name")] = displayName;
        meta[lit("block_name")] = blockName;
        meta[lit("renderdoc_buffer_name")] = renderdocBufferName;
        meta[lit("byte_offset")] = QString::number(used.descriptor.byteOffset);
        meta[lit("byte_size")] = QString::number(rawLength);
        meta[lit("shader_reconstruction_priority")] =
            guidance[lit("shader_reconstruction_priority")];
        meta[lit("recommended_for_shader_reconstruction")] =
            guidance[lit("recommended_for_shader_reconstruction")];
        RecordFile(lit("constant_buffer_raw"), rawPath, meta);
        indexEntry[lit("raw_path")] = rawPath;
      }
    }
    else
    {
      QVariantMap meta;
      meta[lit("stage")] = stageShort;
      meta[lit("binding")] = binding;
      meta[lit("resource")] = ToQStr(used.descriptor.resource);
      RecordFailure(lit("constant_buffer_raw"),
                    lit("Constant buffer has no buffer resource or explicit byte size"), meta);
    }

    m_ConstantBuffers.push_back(indexEntry);
  }
}

void DrawcallExporter::ExportMesh()
{
  EnsureDir(lit("Mesh"));

  const PipeState &pipe = m_Ctx.CurPipelineState();
  const ActionDescription *action = m_Ctx.CurAction();

  QVariantMap layout;
  layout[lit("topology")] = ToQStr(pipe.GetPrimitiveTopology());
  layout[lit("vertex_buffers")] = MakeVBufferList(pipe.GetVBuffers());
  layout[lit("index_buffer")] = MakeBoundVBufferJSON(pipe.GetIBuffer());
  layout[lit("attributes")] = MakeVertexInputList(pipe.GetVertexInputs());
  if(action)
  {
    layout[lit("num_indices")] = action->numIndices;
    layout[lit("num_instances")] = action->numInstances;
    layout[lit("vertex_offset")] = action->vertexOffset;
    layout[lit("index_offset")] = action->indexOffset;
    layout[lit("instance_offset")] = action->instanceOffset;
    layout[lit("base_vertex")] = action->baseVertex;
  }
  if(WriteJSONFile(lit("Mesh/vertex_input_layout.json"), layout))
    RecordFile(lit("mesh_vertex_input_layout"), lit("Mesh/vertex_input_layout.json"));

  MeshDataStage postStage = MeshDataStage::VSOut;
  MeshFormat postVS;
  m_Ctx.Replay().BlockInvoke([&](IReplayController *r) {
    if(pipe.GetShader(ShaderStage::Mesh) != ResourceId())
    {
      postStage = MeshDataStage::MeshOut;
      postVS = r->GetPostVSData(0, 0, postStage);
    }
    else
    {
      postStage = MeshDataStage::GSOut;
      postVS = r->GetPostVSData(0, 0, postStage);
      if(postVS.vertexResourceId == ResourceId() || postVS.numIndices == 0)
      {
        postStage = MeshDataStage::VSOut;
        postVS = r->GetPostVSData(0, 0, postStage);
      }
    }
  });

  QVariantMap postVSJSON;
  postVSJSON[lit("stage")] = ToQStr(postStage);
  postVSJSON[lit("mesh")] = MakeMeshFormatJSON(postVS);

  bytebuf postVertexData;
  bytebuf postIndexData;
  if(postVS.vertexResourceId != ResourceId())
  {
    m_Ctx.Replay().BlockInvoke([&](IReplayController *r) {
      postVertexData = r->GetBufferData(postVS.vertexResourceId, postVS.vertexByteOffset,
                                        postVS.vertexByteSize);
    });
    if(m_Options.exportMeshRawBuffers && WriteBytesFile(lit("Mesh/postvs_vertex_buffer.bin"), postVertexData))
      RecordFile(lit("mesh_postvs_vertex_raw"), lit("Mesh/postvs_vertex_buffer.bin"));
  }

  if(postVS.indexResourceId != ResourceId() && postVS.indexByteStride > 0)
  {
    const uint64_t indexBytes =
        postVS.indexByteSize ? postVS.indexByteSize : uint64_t(postVS.numIndices) * postVS.indexByteStride;
    m_Ctx.Replay().BlockInvoke([&](IReplayController *r) {
      postIndexData = r->GetBufferData(postVS.indexResourceId, postVS.indexByteOffset, indexBytes);
    });
    if(m_Options.exportMeshRawBuffers && WriteBytesFile(lit("Mesh/postvs_index_buffer.bin"), postIndexData))
      RecordFile(lit("mesh_postvs_index_raw"), lit("Mesh/postvs_index_buffer.bin"));
  }

  if(m_Options.exportMeshOBJ)
  {
    if(postVS.vertexResourceId == ResourceId() || postVS.numIndices == 0 || postVertexData.empty())
    {
      QVariantMap meta;
      meta[lit("stage")] = ToQStr(postStage);
      meta[lit("status")] = ToQStr(postVS.status);
      RecordFailure(lit("mesh_obj"), lit("No post-transform vertex data available"), meta);
    }
    else if(WritePostVSOBJ(postVS, postVertexData, postIndexData, lit("Mesh/mesh_postvs.obj")))
    {
      RecordFile(lit("mesh_postvs_obj"), lit("Mesh/mesh_postvs.obj"));
      postVSJSON[lit("obj_path")] = lit("Mesh/mesh_postvs.obj");
    }
  }

  if(WriteJSONFile(lit("Mesh/mesh_postvs.json"), postVSJSON))
    RecordFile(lit("mesh_postvs_metadata"), lit("Mesh/mesh_postvs.json"));

  if(!m_Options.exportMeshRawBuffers || action == NULL)
    return;

  const BoundVBuffer ib = pipe.GetIBuffer();
  if((action->flags & ActionFlags::Indexed) && ib.resourceId != ResourceId() && ib.byteStride > 0)
  {
    BufferDescription *buf = m_Ctx.GetBuffer(ib.resourceId);
    uint64_t offset = ib.byteOffset + uint64_t(action->indexOffset) * ib.byteStride;
    uint64_t length = uint64_t(action->numIndices) * ib.byteStride;
    if(buf && offset < buf->length)
      length = qMin(length, buf->length - offset);

    bytebuf raw;
    m_Ctx.Replay().BlockInvoke(
        [&](IReplayController *r) { raw = r->GetBufferData(ib.resourceId, offset, length); });
    if(WriteBytesFile(lit("Mesh/index_buffer.bin"), raw))
    {
      QVariantMap meta;
      meta[lit("resource")] = ToQStr(ib.resourceId);
      meta[lit("byte_offset")] = QString::number(offset);
      meta[lit("byte_size")] = QString::number(length);
      RecordFile(lit("mesh_index_buffer_raw"), lit("Mesh/index_buffer.bin"), meta);
    }
  }

  rdcarray<BoundVBuffer> vbs = pipe.GetVBuffers();
  const uint64_t rows = qMax(uint64_t(action->numIndices), uint64_t(action->numInstances));
  for(uint32_t slot = 0; slot < vbs.size(); slot++)
  {
    const BoundVBuffer &vb = vbs[slot];
    if(vb.resourceId == ResourceId() || vb.byteStride == 0)
      continue;

    BufferDescription *buf = m_Ctx.GetBuffer(vb.resourceId);
    uint64_t offset = vb.byteOffset + uint64_t(action->vertexOffset) * vb.byteStride;
    uint64_t length = rows * vb.byteStride;
    if(vb.byteSize != 0 && vb.byteSize != ~0ULL)
      length = qMin(length, vb.byteSize);
    if(buf && offset < buf->length)
      length = qMin(length, buf->length - offset);

    bytebuf raw;
    m_Ctx.Replay().BlockInvoke(
        [&](IReplayController *r) { raw = r->GetBufferData(vb.resourceId, offset, length); });

    const QString path = QFormatStr("Mesh/vertex_buffer_slot%1.bin").arg(slot, 2, 10, QLatin1Char('0'));
    if(WriteBytesFile(path, raw))
    {
      QVariantMap meta;
      meta[lit("slot")] = slot;
      meta[lit("resource")] = ToQStr(vb.resourceId);
      meta[lit("byte_offset")] = QString::number(offset);
      meta[lit("byte_size")] = QString::number(length);
      meta[lit("stride")] = vb.byteStride;
      RecordFile(lit("mesh_vertex_buffer_raw"), path, meta);
    }
  }
}

void DrawcallExporter::ExportResourceBuffers()
{
  EnsureDir(lit("ResourceBuffers"));

  const uint64_t maxExportBytes = 64ULL * 1024ULL * 1024ULL;
  QVariantList buffersJSON;

  for(ShaderStage stage : ActiveShaderStages())
  {
    const QString stageShort = StageShortName(stage);
    auto processDescriptors = [&](const rdcarray<UsedDescriptor> &descriptors, bool readWrite) {
      for(const UsedDescriptor &used : descriptors)
      {
        ResourceId id = used.descriptor.resource;
        BufferDescription *buf = m_Ctx.GetBuffer(id);
        if(buf == NULL)
          continue;

        const QString binding = DescriptorBindingName(used, lit("r"));
        uint64_t offset = used.descriptor.byteOffset;
        uint64_t length = used.descriptor.byteSize;
        if(length == 0 && offset < buf->length)
          length = buf->length - offset;

        const uint64_t requestedLength = length;
        if(length == 0)
        {
          QVariantMap meta;
          meta[lit("stage")] = stageShort;
          meta[lit("binding")] = binding;
          meta[lit("resource")] = ToQStr(id);
          RecordFailure(lit("resource_buffer"), lit("Descriptor has no byte range to export"), meta);
          continue;
        }

        bool truncated = false;
        if(length > maxExportBytes)
        {
          length = maxExportBytes;
          truncated = true;
        }

        bytebuf raw;
        m_Ctx.Replay().BlockInvoke([id, offset, length, &raw](IReplayController *r) {
          raw = r->GetBufferData(id, offset, length);
        });

        const QString shaderResourceName = ShaderResourceReflectionName(stage, used, readWrite);
        const QString renderdocResourceName = m_Ctx.GetResourceName(id);
        const QString displayName = ResourceBufferDisplayName(stage, used, readWrite);
        const QString baseName = MakeResourceBaseName(lit("buf"), stageShort, binding, id, displayName);
        const QString path = UniqueRelativePath(lit("ResourceBuffers"), baseName, lit(".raw.bin"));
        const QVariantMap guidance = ResourceBufferGuidance(displayName, requestedLength, truncated);

        QVariantMap meta;
        meta[lit("stage")] = stageShort;
        meta[lit("binding")] = binding;
        meta[lit("resource")] = ToQStr(id);
        meta[lit("display_name")] = displayName;
        meta[lit("shader_resource_name")] = shaderResourceName;
        meta[lit("renderdoc_resource_name")] = renderdocResourceName;
        meta[lit("original_name")] = renderdocResourceName;
        meta[lit("descriptor")] = MakeUsedDescriptorJSON(used);
        meta[lit("byte_offset")] = QString::number(offset);
        meta[lit("requested_byte_size")] = QString::number(requestedLength);
        meta[lit("exported_byte_size")] = QString::number(length);
        meta[lit("byte_size")] = QString::number(length);
        meta[lit("truncated")] = truncated;
        meta[lit("shader_reconstruction_priority")] =
            guidance[lit("shader_reconstruction_priority")];
        meta[lit("recommended_for_shader_reconstruction")] =
            guidance[lit("recommended_for_shader_reconstruction")];
        meta[lit("resource_buffer_guidance")] = guidance;

        if(WriteBytesFile(path, raw))
        {
          meta[lit("path")] = path;
          RecordFile(lit("resource_buffer_raw"), path, meta);
          buffersJSON.push_back(meta);
        }
      }
    };

    processDescriptors(
        m_Ctx.CurPipelineState().GetReadOnlyResources(stage, m_Options.onlyUsedDescriptors), false);
    processDescriptors(
        m_Ctx.CurPipelineState().GetReadWriteResources(stage, m_Options.onlyUsedDescriptors), true);
  }

  QVariantMap root;
  root[lit("max_export_bytes")] = QString::number(maxExportBytes);
  root[lit("priority_notes")] =
      lit("ResourceBuffers are SRV/UAV/raw buffers. For shader reconstruction, inspect ConstantBuffers, textures, shader code, and pipeline state first. Unity/URP z-bin, tile, cluster, light-list, and culling buffers are normally low-priority intermediate data unless the shader logic explicitly depends on them.");
  root[lit("buffers")] = buffersJSON;
  if(WriteJSONFile(lit("ResourceBuffers/resource_buffers.json"), root))
    RecordFile(lit("resource_buffer_metadata"), lit("ResourceBuffers/resource_buffers.json"));

  QString readme;
  readme += lit("# ResourceBuffers\n\n");
  readme += lit("These files are raw SRV/UAV buffer ranges used by the selected active shader stages, including CS for compute dispatches. They are kept for completeness, but they are usually secondary evidence for shader reconstruction.\n\n");
  readme += lit("Use priority:\n\n");
  readme += lit("- High: `ConstantBuffers/`, `Shaders/`, input/output textures, mesh OBJ/raw buffers, and `pipeline_state.html/json`.\n");
  readme += lit("- Medium: Resource buffers whose shader resource name is directly referenced by the HLSL/DXBC logic.\n");
  readme += lit("- Low: Unity/URP z-bin, tile, cluster, light-list, culling, and other large intermediate buffers. These often describe renderer-side light/binning data and usually do not need to be read unless the restored shader explicitly indexes them.\n\n");
  readme += lit("See `resource_buffers.json` for `display_name`, `shader_resource_name`, byte ranges, truncation status, and `shader_reconstruction_priority` for each exported buffer.\n");
  if(WriteTextFile(lit("ResourceBuffers/README.md"), readme))
    RecordFile(lit("resource_buffer_notes"), lit("ResourceBuffers/README.md"));
}

QVariantMap DrawcallExporter::MakeConstantBlockJSON(const ConstantBlock &block,
                                                    const UsedDescriptor &used, ShaderStage stage,
                                                    uint32_t slot, uint32_t arrayIdx,
                                                    const rdcarray<ShaderVariable> &variables) const
{
  QVariantMap ret;
  ret[lit("stage")] = StageShortName(stage);
  ret[lit("stage_name")] = ToQStr(stage, m_Ctx.APIProps().pipelineType);
  ret[lit("slot")] = slot;
  ret[lit("array_index")] = arrayIdx;
  ret[lit("name")] = ToQStr(block.name);
  ret[lit("display_name")] = ConstantBufferDisplayName(block, used);
  ret[lit("block_name")] = ToQStr(block.name);
  ret[lit("renderdoc_buffer_name")] = m_Ctx.GetResourceName(used.descriptor.resource);
  ret[lit("fixed_bind_number")] = block.fixedBindNumber;
  ret[lit("fixed_bind_set_or_space")] = block.fixedBindSetOrSpace;
  ret[lit("bind_array_size")] = block.bindArraySize;
  ret[lit("byte_size")] = block.byteSize;
  ret[lit("buffer_backed")] = block.bufferBacked;
  ret[lit("inline_data_bytes")] = block.inlineDataBytes;
  ret[lit("compile_constants")] = block.compileConstants;
  ret[lit("resource_id")] = ToQStr(used.descriptor.resource);
  ret[lit("descriptor_type")] = ToQStr(used.descriptor.type);
  ret[lit("descriptor_byte_offset")] = QString::number(used.descriptor.byteOffset);
  ret[lit("descriptor_byte_size")] = QString::number(used.descriptor.byteSize);
  ret[lit("constant_buffer_guidance")] =
      ConstantBufferGuidance(ret[lit("display_name")].toString(),
                             used.descriptor.byteSize ? used.descriptor.byteSize : block.byteSize);
  ret[lit("variables")] = MakeShaderVariableList(variables);
  return ret;
}

QVariantMap DrawcallExporter::MakeShaderVariableJSON(const ShaderVariable &var) const
{
  QVariantMap ret;
  ret[lit("name")] = ToQStr(var.name);
  ret[lit("type")] = ToQStr(var.type);
  ret[lit("rows")] = var.rows;
  ret[lit("columns")] = var.columns;
  ret[lit("flags")] = ToQStr(var.flags);

  QVariantList values;
  for(uint32_t i = 0; i < 16; i++)
    values.push_back(ShaderVariableValueString(var, i));
  ret[lit("values")] = values;
  ret[lit("members")] = MakeShaderVariableList(var.members);
  return ret;
}

QVariantList DrawcallExporter::MakeShaderVariableList(const rdcarray<ShaderVariable> &vars) const
{
  QVariantList ret;
  for(const ShaderVariable &var : vars)
    ret.push_back(MakeShaderVariableJSON(var));
  return ret;
}

QString DrawcallExporter::StageShortName(ShaderStage stage) const
{
  switch(stage)
  {
    case ShaderStage::Vertex: return lit("vs");
    case ShaderStage::Hull: return lit("hs");
    case ShaderStage::Domain: return lit("ds");
    case ShaderStage::Geometry: return lit("gs");
    case ShaderStage::Pixel: return lit("ps");
    case ShaderStage::Compute: return lit("cs");
    case ShaderStage::Task: return lit("task");
    case ShaderStage::Mesh: return lit("mesh");
    default: break;
  }
  return lit("unknown");
}

QString DrawcallExporter::BindingName(const QString &prefix, uint32_t slot) const
{
  return QFormatStr("%1%2").arg(prefix).arg(slot, 3, 10, QLatin1Char('0'));
}

QString DrawcallExporter::DescriptorBindingName(const UsedDescriptor &used,
                                                const QString &defaultPrefix) const
{
  QString prefix = defaultPrefix;
  switch(used.access.type)
  {
    case DescriptorType::ConstantBuffer: prefix = lit("b"); break;
    case DescriptorType::Sampler: prefix = lit("s"); break;
    case DescriptorType::ImageSampler:
    case DescriptorType::Image:
    case DescriptorType::Buffer:
    case DescriptorType::TypedBuffer:
    case DescriptorType::AccelerationStructure: prefix = lit("t"); break;
    case DescriptorType::ReadWriteImage:
    case DescriptorType::ReadWriteTypedBuffer:
    case DescriptorType::ReadWriteBuffer: prefix = lit("u"); break;
    default: break;
  }

  if(used.access.index == DescriptorAccess::NoShaderBinding)
    return QFormatStr("%1_direct_%2").arg(prefix).arg(used.access.byteOffset);

  QString binding = BindingName(prefix, used.access.index);
  if(used.access.arrayElement > 0)
    binding += QFormatStr("_a%1").arg(used.access.arrayElement);
  return binding;
}

QString DrawcallExporter::ConstantBufferDisplayName(const ConstantBlock &block,
                                                    const UsedDescriptor &used) const
{
  const QString blockName = ToQStr(block.name).trimmed();
  const QString bufferName = m_Ctx.GetResourceName(used.descriptor.resource).trimmed();

  if(bufferName.isEmpty())
    return blockName.isEmpty() ? DescriptorBindingName(used, lit("b")) : blockName;

  const QString lowerBlock = blockName.toLower();
  const bool genericBlockName =
      blockName.isEmpty() || lowerBlock.startsWith(lit("cbuffer")) ||
      lowerBlock.startsWith(lit("constantbuffer")) || lowerBlock == DescriptorBindingName(used, lit("b"));

  if(genericBlockName || blockName == bufferName)
    return bufferName;

  return blockName + lit("__") + bufferName;
}

QVariantMap DrawcallExporter::ConstantBufferGuidance(const QString &displayName,
                                                     uint64_t byteSize) const
{
  QVariantMap ret;
  QVariantList reasons;
  const QString lower = displayName.toLower();

  auto note = [&reasons](const QString &text) { reasons.push_back(text); };

  bool lowPriority = false;
  if(lower.contains(lit("z-bin")) || lower.contains(lit("z_bin")) ||
     lower.contains(lit("z bin")))
  {
    lowPriority = true;
    note(lit("Unity/URP z-bin constant buffer is usually renderer-side binning data."));
  }

  if(lower.contains(lit("tile")) || lower.contains(lit("cluster")) ||
     lower.contains(lit("light list")) || lower.contains(lit("lightlist")) ||
     lower.contains(lit("culling")))
  {
    lowPriority = true;
    note(lit("Tile/cluster/light-list/culling constant buffers are usually renderer intermediate data."));
  }

  if(byteSize >= 1024ULL * 1024ULL)
    note(lit("Large constant buffer; inspect after material/per-draw/per-camera buffers."));

  ret[lit("shader_reconstruction_priority")] = lowPriority ? lit("low") : lit("high");
  ret[lit("recommended_for_shader_reconstruction")] = !lowPriority;
  ret[lit("guidance")] =
      lowPriority ? lit("Usually not needed for Unity shader reconstruction unless shader code directly reads values from this buffer.")
                  : lit("Primary shader reconstruction input; inspect variable values and layout.");
  ret[lit("reasons")] = reasons;
  return ret;
}

QString DrawcallExporter::ShaderProcessorTargetName(const ShaderProcessingTool &processor) const
{
  return lit("%1 (%2)").arg(ToQStr(processor.output)).arg(ToQStr(processor.name));
}

QString DrawcallExporter::DisassemblyFileBase(const QString &kind, const QString &targetName) const
{
  QString base = lit("disassembly_") + SafeName(kind, lit("target"));
  if(!targetName.isEmpty())
    base += lit("_") + SafeName(targetName, lit("target"));
  return SafeName(base, lit("disassembly"));
}

QString DrawcallExporter::ShaderResourceReflectionName(ShaderStage stage, const UsedDescriptor &used,
                                                       bool readWrite) const
{
  if(used.access.index == DescriptorAccess::NoShaderBinding)
    return QString();

  const ShaderReflection *refl = m_Ctx.CurPipelineState().GetShaderReflection(stage);
  if(refl == NULL)
    return QString();

  const rdcarray<ShaderResource> &resources =
      readWrite ? refl->readWriteResources : refl->readOnlyResources;
  if(used.access.index >= resources.size())
    return QString();

  const ShaderResource &resource = resources[used.access.index];
  QString name = ToQStr(resource.name);
  if(name.isEmpty())
    return QString();

  if(resource.bindArraySize > 1 && used.access.arrayElement > 0)
    name += QFormatStr("[%1]").arg(used.access.arrayElement);

  return name;
}

QString DrawcallExporter::ResourceBufferDisplayName(ShaderStage stage, const UsedDescriptor &used,
                                                    bool readWrite) const
{
  const QString reflectionName = ShaderResourceReflectionName(stage, used, readWrite);
  const QString resourceName = m_Ctx.GetResourceName(used.descriptor.resource).trimmed();

  if(!reflectionName.isEmpty() && !resourceName.isEmpty() && reflectionName != resourceName)
    return reflectionName + lit("__") + resourceName;
  if(!reflectionName.isEmpty())
    return reflectionName;
  if(!resourceName.isEmpty())
    return resourceName;

  return DescriptorBindingName(used, readWrite ? lit("u") : lit("r"));
}

QVariantMap DrawcallExporter::ResourceBufferGuidance(const QString &displayName, uint64_t byteSize,
                                                     bool truncated) const
{
  QVariantMap ret;
  QVariantList reasons;
  const QString lower = displayName.toLower();

  auto note = [&reasons](const QString &text) { reasons.push_back(text); };

  bool lowPriority = false;
  if(lower.contains(lit("z-bin")) || lower.contains(lit("z_bin")) ||
     lower.contains(lit("z bin")))
  {
    lowPriority = true;
    note(lit("Unity/URP z-bin buffer is normally a lighting/culling intermediate buffer."));
  }

  if(lower.contains(lit("tile")) || lower.contains(lit("cluster")) ||
     lower.contains(lit("light list")) || lower.contains(lit("lightlist")) ||
     lower.contains(lit("culling")))
  {
    lowPriority = true;
    note(lit("Tile/cluster/light-list style buffers are usually engine-side culling data, not material inputs."));
  }

  const bool largeFile = byteSize >= 8ULL * 1024ULL * 1024ULL || truncated;
  if(largeFile)
    note(lit("Large raw buffer; inspect only if the shader code directly indexes this resource."));

  ret[lit("shader_reconstruction_priority")] = lowPriority ? lit("low") : lit("medium");
  ret[lit("recommended_for_shader_reconstruction")] = !lowPriority;
  ret[lit("large_file")] = largeFile;
  ret[lit("guidance")] =
      lowPriority
          ? lit("Usually not needed for Unity shader reconstruction unless the shader logic explicitly depends on this buffer.")
          : lit("Optional context. Prefer constant buffers, textures, shader code, and pipeline state first.");
  ret[lit("reasons")] = reasons;
  return ret;
}

QString DrawcallExporter::FileTypeExtension(FileType type) const
{
  switch(type)
  {
    case FileType::PNG: return lit(".png");
    case FileType::JPG: return lit(".jpg");
    case FileType::BMP: return lit(".bmp");
    case FileType::TGA: return lit(".tga");
    case FileType::HDR: return lit(".hdr");
    case FileType::EXR: return lit(".exr");
    case FileType::Raw: return lit(".raw");
    case FileType::DDS:
    default: break;
  }
  return lit(".dds");
}

FileType DrawcallExporter::PreferredTextureFileType(const TextureDescription *tex,
                                                    const Descriptor &descriptor) const
{
  if(tex == NULL)
    return FileType::DDS;

  const bool simpleView = descriptor.numMips <= 1 && descriptor.numSlices <= 1;
  const bool simpleTexture =
      tex->dimension == 2 && tex->arraysize == 1 && tex->mips == 1 && tex->msSamp <= 1 &&
      !tex->cubemap && !tex->format.BlockFormat();

  return simpleView && simpleTexture ? FileType::PNG : FileType::DDS;
}

bool DrawcallExporter::IsTextureResource(ResourceId id) const
{
  return id != ResourceId() && m_Ctx.GetTexture(id) != NULL;
}

bool DrawcallExporter::IsBufferResource(ResourceId id) const
{
  return id != ResourceId() && m_Ctx.GetBuffer(id) != NULL;
}

bool DrawcallExporter::SaveTextureDescriptor(const QString &category, const QString &role,
                                             const QString &stage, const QString &binding,
                                             const Descriptor &descriptor,
                                             QVariantList &textureMetadata)
{
  TextureDescription *tex = m_Ctx.GetTexture(descriptor.resource);
  if(tex == NULL)
  {
    QVariantMap meta;
    meta[lit("stage")] = stage;
    meta[lit("binding")] = binding;
    meta[lit("resource")] = ToQStr(descriptor.resource);
    RecordFailure(category, lit("Descriptor does not reference a texture resource"), meta);
    return false;
  }

  const QString dir = category == lit("output_texture") ? lit("Textures/Outputs") : lit("Textures/Inputs");
  const QString filePrefix = category == lit("output_texture") ? lit("tex_out") : lit("tex_in");
  const QString baseName =
      MakeResourceBaseName(filePrefix, stage, binding, descriptor.resource,
                           m_Ctx.GetResourceName(descriptor.resource));

  FileType attempts[2] = {PreferredTextureFileType(tex, descriptor), FileType::DDS};
  for(int i = 0; i < 2; i++)
  {
    FileType destType = attempts[i];
    if(i == 1 && attempts[0] == FileType::DDS)
      break;

    TextureSave save;
    save.resourceId = descriptor.resource;
    save.destType = destType;
    save.typeCast = descriptor.format.compType == CompType::Typeless ? CompType::Typeless
                                                                     : descriptor.format.compType;
    save.mip = destType == FileType::DDS ? -1 : descriptor.firstMip;
    save.slice.sliceIndex = destType == FileType::DDS ? -1 : descriptor.firstSlice;
    save.sample.sampleIndex = TextureSampleMapping::ResolveSamples;
    save.alpha = AlphaMapping::Preserve;

    const QString path = UniqueRelativePath(dir, baseName, FileTypeExtension(destType));
    ResultDetails result = {ResultCode::Succeeded};
    const QString absolutePath = QDir(m_ExportDir).filePath(path);
    m_Ctx.Replay().BlockInvoke(
        [&result, save, absolutePath](IReplayController *r) { result = r->SaveTexture(save, absolutePath); });

    if(result.OK())
    {
      QVariantMap meta;
      meta[lit("stage")] = stage;
      meta[lit("role")] = role;
      meta[lit("binding")] = binding;
      meta[lit("resource")] = ToQStr(descriptor.resource);
      meta[lit("original_name")] = m_Ctx.GetResourceName(descriptor.resource);
      meta[lit("file_type")] = ToQStr(destType);
      meta[lit("descriptor")] = MakeDescriptorJSON(descriptor);
      meta[lit("texture")] = MakeTextureJSON(tex);
      RecordFile(category, path, meta);

      QVariantMap texMeta = meta;
      texMeta[lit("path")] = path;
      textureMetadata.push_back(texMeta);
      return true;
    }

    if(destType == FileType::DDS)
    {
      QVariantMap meta;
      meta[lit("stage")] = stage;
      meta[lit("role")] = role;
      meta[lit("binding")] = binding;
      meta[lit("resource")] = ToQStr(descriptor.resource);
      meta[lit("file_type")] = ToQStr(destType);
      RecordFailure(category, QFormatStr("SaveTexture failed for %1: %2").arg(path).arg(result.Message()),
                    meta);
    }
  }

  return false;
}

QString DrawcallExporter::ActionFlagsString(ActionFlags flags) const
{
  QStringList names;
  auto add = [&names, flags](ActionFlags flag, const QString &name) {
    if(flags & flag)
      names << name;
  };

  add(ActionFlags::Clear, lit("Clear"));
  add(ActionFlags::Drawcall, lit("Drawcall"));
  add(ActionFlags::Dispatch, lit("Dispatch"));
  add(ActionFlags::MeshDispatch, lit("MeshDispatch"));
  add(ActionFlags::CmdList, lit("CmdList"));
  add(ActionFlags::SetMarker, lit("SetMarker"));
  add(ActionFlags::PushMarker, lit("PushMarker"));
  add(ActionFlags::PopMarker, lit("PopMarker"));
  add(ActionFlags::Present, lit("Present"));
  add(ActionFlags::MultiAction, lit("MultiAction"));
  add(ActionFlags::Copy, lit("Copy"));
  add(ActionFlags::Resolve, lit("Resolve"));
  add(ActionFlags::GenMips, lit("GenMips"));
  add(ActionFlags::PassBoundary, lit("PassBoundary"));
  add(ActionFlags::DispatchRay, lit("DispatchRay"));
  add(ActionFlags::BuildAccStruct, lit("BuildAccStruct"));
  add(ActionFlags::Indexed, lit("Indexed"));
  add(ActionFlags::Instanced, lit("Instanced"));
  add(ActionFlags::Auto, lit("Auto"));
  add(ActionFlags::Indirect, lit("Indirect"));
  add(ActionFlags::ClearColor, lit("ClearColor"));
  add(ActionFlags::ClearDepthStencil, lit("ClearDepthStencil"));
  add(ActionFlags::BeginPass, lit("BeginPass"));
  add(ActionFlags::EndPass, lit("EndPass"));
  add(ActionFlags::CommandBufferBoundary, lit("CommandBufferBoundary"));

  return names.isEmpty() ? lit("NoFlags") : names.join(lit("|"));
}

QString DrawcallExporter::ShaderVariableValueString(const ShaderVariable &var, uint32_t idx) const
{
  if(idx >= 16)
    return QString();

  switch(var.type)
  {
    case VarType::Float: return QString::number(var.value.f32v[idx], 'g', 9);
    case VarType::Double: return QString::number(var.value.f64v[idx], 'g', 17);
    case VarType::SInt:
    case VarType::SShort:
    case VarType::SByte:
    case VarType::Enum: return QString::number(var.value.s32v[idx]);
    case VarType::UInt:
    case VarType::UShort:
    case VarType::UByte:
    case VarType::Bool: return QString::number(var.value.u32v[idx]);
    case VarType::SLong: return QString::number((qlonglong)var.value.s64v[idx]);
    case VarType::ULong:
    case VarType::GPUPointer: return QString::number((qulonglong)var.value.u64v[idx]);
    case VarType::Half: return QFormatStr("0x%1").arg(var.value.u16v[idx], 4, 16, QLatin1Char('0'));
    default: break;
  }

  return QFormatStr("0x%1").arg(var.value.u32v[idx], 8, 16, QLatin1Char('0'));
}

bool DrawcallExporter::ReadPostVSPosition(const MeshFormat &mesh, uint32_t vert,
                                          const bytebuf &vertexData, FloatVector &pos) const
{
  if(mesh.vertexByteStride == 0)
    return false;

  const uint64_t byteOffset = uint64_t(vert) * mesh.vertexByteStride;
  const ResourceFormat &fmt = mesh.format;
  const uint32_t elemSize = fmt.ElementSize();
  if(elemSize == 0 || byteOffset + elemSize > vertexData.size())
    return false;

  const byte *data = vertexData.data() + byteOffset;
  float values[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  const uint32_t comps = qMin<uint32_t>(fmt.compCount, 4);

  if(fmt.type != ResourceFormatType::Regular)
    return false;

  for(uint32_t c = 0; c < comps; c++)
  {
    const byte *src = data + c * fmt.compByteWidth;
    switch(fmt.compType)
    {
      case CompType::Float:
        if(fmt.compByteWidth == 4)
        {
          float v = 0.0f;
          memcpy(&v, src, sizeof(float));
          values[c] = v;
        }
        else if(fmt.compByteWidth == 8)
        {
          double v = 0.0;
          memcpy(&v, src, sizeof(double));
          values[c] = (float)v;
        }
        else
        {
          return false;
        }
        break;
      case CompType::SInt:
      {
        int32_t v = 0;
        if(fmt.compByteWidth == 1)
        {
          int8_t s = 0;
          memcpy(&s, src, sizeof(s));
          v = s;
        }
        else if(fmt.compByteWidth == 2)
        {
          int16_t s = 0;
          memcpy(&s, src, sizeof(s));
          v = s;
        }
        else if(fmt.compByteWidth == 4)
        {
          memcpy(&v, src, sizeof(v));
        }
        else
        {
          return false;
        }
        values[c] = (float)v;
        break;
      }
      case CompType::UInt:
      {
        uint32_t v = 0;
        if(fmt.compByteWidth == 1)
        {
          uint8_t u = 0;
          memcpy(&u, src, sizeof(u));
          v = u;
        }
        else if(fmt.compByteWidth == 2)
        {
          uint16_t u = 0;
          memcpy(&u, src, sizeof(u));
          v = u;
        }
        else if(fmt.compByteWidth == 4)
        {
          memcpy(&v, src, sizeof(v));
        }
        else
        {
          return false;
        }
        values[c] = (float)v;
        break;
      }
      default: return false;
    }
  }

  pos = FloatVector(values[0], values[1], values[2], values[3]);
  return true;
}

uint32_t DrawcallExporter::ReadIndex(const MeshFormat &mesh, const bytebuf &indexData,
                                     uint32_t idx) const
{
  if(mesh.indexResourceId == ResourceId() || mesh.indexByteStride == 0 || indexData.empty())
    return idx;

  const uint64_t offset = uint64_t(idx) * mesh.indexByteStride;
  if(offset + mesh.indexByteStride > indexData.size())
    return idx;

  const byte *src = indexData.data() + offset;
  if(mesh.indexByteStride == 1)
    return *src;
  if(mesh.indexByteStride == 2)
  {
    uint16_t v = 0;
    memcpy(&v, src, sizeof(v));
    return v;
  }
  if(mesh.indexByteStride == 4)
  {
    uint32_t v = 0;
    memcpy(&v, src, sizeof(v));
    return v;
  }

  return idx;
}

bool DrawcallExporter::WritePostVSOBJ(const MeshFormat &mesh, const bytebuf &vertexData,
                                      const bytebuf &indexData, const QString &relativePath)
{
  QFileInfo info(QDir(m_ExportDir).filePath(relativePath));
  QDir().mkpath(info.dir().absolutePath());

  QFile f(info.absoluteFilePath());
  if(!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
  {
    RecordFailure(lit("mesh_obj"), QFormatStr("Couldn't write %1: %2").arg(relativePath).arg(f.errorString()));
    return false;
  }

  QTextStream stream(&f);
  stream.setCodec("UTF-8");
  stream << "# RenderDoc post-transform mesh export\n";
  stream << "# Topology: " << ToQStr(mesh.topology) << "\n";

  QVector<uint32_t> objIndices;
  objIndices.reserve((int)mesh.numIndices);
  for(uint32_t i = 0; i < mesh.numIndices; i++)
  {
    int64_t adjustedIndex = int64_t(ReadIndex(mesh, indexData, i)) + int64_t(mesh.baseVertex);
    if(adjustedIndex < 0)
    {
      QVariantMap meta;
      meta[lit("index")] = i;
      meta[lit("base_vertex")] = mesh.baseVertex;
      RecordFailure(lit("mesh_obj"), lit("Adjusted vertex index is negative"), meta);
      return false;
    }
    uint32_t vertexIndex = (uint32_t)adjustedIndex;

    FloatVector pos;
    if(!ReadPostVSPosition(mesh, vertexIndex, vertexData, pos))
    {
      QVariantMap meta;
      meta[lit("topology")] = ToQStr(mesh.topology);
      meta[lit("format")] = MakeResourceFormatJSON(mesh.format);
      meta[lit("vertex_index")] = vertexIndex;
      RecordFailure(lit("mesh_obj"), lit("Post-transform position format is unsupported or out of range"),
                    meta);
      return false;
    }

    objIndices.push_back(i + 1);
    stream << "v " << QString::number(pos.x, 'g', 9) << " " << QString::number(pos.y, 'g', 9)
           << " " << QString::number(pos.z, 'g', 9) << "\n";
  }

  auto face = [&stream, &objIndices](uint32_t a, uint32_t b, uint32_t c) {
    if(a < (uint32_t)objIndices.size() && b < (uint32_t)objIndices.size() &&
       c < (uint32_t)objIndices.size())
      stream << "f " << objIndices[(int)a] << " " << objIndices[(int)b] << " "
             << objIndices[(int)c] << "\n";
  };

  auto line = [&stream, &objIndices](uint32_t a, uint32_t b) {
    if(a < (uint32_t)objIndices.size() && b < (uint32_t)objIndices.size())
      stream << "l " << objIndices[(int)a] << " " << objIndices[(int)b] << "\n";
  };

  switch(mesh.topology)
  {
    case Topology::PointList:
      for(uint32_t i = 0; i < mesh.numIndices; i++)
        stream << "p " << objIndices[(int)i] << "\n";
      break;
    case Topology::LineList:
      for(uint32_t i = 0; i + 1 < mesh.numIndices; i += 2)
        line(i, i + 1);
      break;
    case Topology::LineStrip:
      for(uint32_t i = 0; i + 1 < mesh.numIndices; i++)
        line(i, i + 1);
      break;
    case Topology::TriangleList:
      for(uint32_t i = 0; i + 2 < mesh.numIndices; i += 3)
        face(i, i + 1, i + 2);
      break;
    case Topology::TriangleStrip:
      for(uint32_t i = 0; i + 2 < mesh.numIndices; i++)
      {
        if(i & 1)
          face(i + 1, i, i + 2);
        else
          face(i, i + 1, i + 2);
      }
      break;
    case Topology::TriangleFan:
      for(uint32_t i = 1; i + 1 < mesh.numIndices; i++)
        face(0, i, i + 1);
      break;
    default:
    {
      QVariantMap meta;
      meta[lit("topology")] = ToQStr(mesh.topology);
      RecordFailure(lit("mesh_obj"), lit("Topology is not supported by OBJ exporter"), meta);
      return false;
    }
  }

  return true;
}

void DrawcallExporter::WriteShaderVariablesCSV(QTextStream &stream,
                                               const rdcarray<ShaderVariable> &vars,
                                               const QString &prefix) const
{
  for(const ShaderVariable &var : vars)
  {
    QString path = prefix.isEmpty() ? ToQStr(var.name) : prefix + lit(".") + ToQStr(var.name);
    stream << "\"" << path << "\",\"" << ToQStr(var.name) << "\",\"" << ToQStr(var.type) << "\","
           << (uint)var.rows << "," << (uint)var.columns;

    for(uint32_t i = 0; i < 16; i++)
      stream << ",\"" << ShaderVariableValueString(var, i) << "\"";
    stream << "\n";

    WriteShaderVariablesCSV(stream, var.members, path);
  }
}

QVariantMap DrawcallExporter::MakeShaderReconstructionIndex() const
{
  QVariantMap root;
  root[lit("schema")] = lit("renderdoc.unity_shader_reconstruction_index.v1");
  root[lit("capture_file")] = ToQStr(m_Ctx.GetCaptureFilename());
  root[lit("event_id")] = m_Ctx.CurEvent();
  root[lit("api")] = ToQStr(m_Ctx.APIProps().pipelineType);

  QVariantMap targetEnvironment;
  targetEnvironment[lit("unity_version")] = lit("Unity 6.0 (6000.0.62f1)");
  targetEnvironment[lit("urp_version")] = lit("URP 17.0.4");
  targetEnvironment[lit("platform")] = lit("Mobile game");
  targetEnvironment[lit("primary_graphics_api")] = lit("Vulkan");
  targetEnvironment[lit("compatibility_graphics_api")] = lit("OpenGLES");
  targetEnvironment[lit("rendering_path")] = lit("Deferred");
  targetEnvironment[lit("render_graph")] = lit("Supported");
  targetEnvironment[lit("gpu_resident_drawer")] =
      lit("Supported; preserve instancing/instance ID assumptions.");
  root[lit("target_environment")] = targetEnvironment;

  QVariantMap priorityMeaning;
  priorityMeaning[lit("high")] =
      lit("Core evidence for reconstructing Unity/URP shader code and validating output.");
  priorityMeaning[lit("medium")] =
      lit("Supporting evidence; inspect after high-priority files or when code references it.");
  priorityMeaning[lit("low")] =
      lit("Usually renderer intermediate data or logs; keep available but do not start here.");
  root[lit("priority_meaning")] = priorityMeaning;

  QVariantList high;
  QVariantList medium;
  QVariantList low;
  QVariantMap phases;

  auto compactFile = [](const QVariantMap &file) {
    QVariantMap item;
    const QStringList keys = {
        lit("category"),
        lit("path"),
        lit("shader_reconstruction_priority"),
        lit("shader_reconstruction_phase"),
        lit("shader_reconstruction_role"),
        lit("stage"),
        lit("binding"),
        lit("display_name"),
        lit("block_name"),
        lit("renderdoc_buffer_name"),
        lit("shader_resource_name"),
        lit("resource"),
        lit("byte_size"),
        lit("requested_byte_size"),
        lit("exported_byte_size"),
        lit("truncated"),
        lit("recommended_for_shader_reconstruction"),
    };

    for(const QString &key : keys)
      if(file.contains(key))
        item[key] = file[key];

    return item;
  };

  for(const QVariant &fileVar : m_Files)
  {
    const QVariantMap file = fileVar.toMap();
    const QVariantMap item = compactFile(file);
    const QString priority = file[lit("shader_reconstruction_priority")].toString();
    const QString phase = file[lit("shader_reconstruction_phase")].toString();

    if(priority == lit("high"))
      high.push_back(item);
    else if(priority == lit("low"))
      low.push_back(item);
    else
      medium.push_back(item);

    QVariantList phaseFiles = phases[phase].toList();
    phaseFiles.push_back(item);
    phases[phase] = phaseFiles;
  }

  root[lit("high_priority")] = high;
  root[lit("medium_priority")] = medium;
  root[lit("low_priority")] = low;
  root[lit("by_phase")] = phases;

  QVariantList lowPriorityPatterns;
  lowPriorityPatterns.push_back(lit("Unity/URP Z-bin buffers"));
  lowPriorityPatterns.push_back(lit("Unity/URP Tile buffers"));
  lowPriorityPatterns.push_back(lit("Cluster/light-list/culling buffers"));
  lowPriorityPatterns.push_back(lit("Large SRV/UAV raw buffers not directly indexed by shader code"));
  root[lit("low_priority_patterns")] = lowPriorityPatterns;

  return root;
}

QString DrawcallExporter::LoadShaderReconstructionGoalTemplate(QString *sourcePath) const
{
  QStringList candidateRoots;
  candidateRoots << QDir::currentPath();

  QDir appDir(QCoreApplication::applicationDirPath());
  for(int i = 0; i < 8; i++)
  {
    candidateRoots << appDir.absolutePath();
    if(!appDir.cdUp())
      break;
  }

  for(const QString &root : candidateRoots)
  {
    const QString path =
        QDir(root).filePath(lit("AIDoc/UnityShaderReconstructionGoalTemplate.md"));
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly | QIODevice::Text))
      continue;

    QTextStream stream(&file);
    stream.setCodec("UTF-8");
    if(sourcePath)
      *sourcePath = QFileInfo(path).absoluteFilePath();
    return stream.readAll();
  }

  if(sourcePath)
    *sourcePath = lit("built-in fallback");
  return DefaultShaderReconstructionGoalTemplate();
}

QString DrawcallExporter::DefaultShaderReconstructionGoalTemplate() const
{
  QString text;
  text += lit("# Goal: Reconstruct Unity URP Shader From RenderDoc Drawcall Export\n\n");
  text += lit("Objective: Reconstruct a Unity 6 URP shader/HLSL implementation for the selected RenderDoc drawcall export in `{{EXPORT_DIR}}`.\n\n");
  text += lit("Target environment:\n\n");
  text += lit("- Unity: {{UNITY_VERSION}}\n");
  text += lit("- URP: {{URP_VERSION}}\n");
  text += lit("- Platform: {{PLATFORM}}\n");
  text += lit("- Primary graphics API: {{PRIMARY_GRAPHICS_API}}\n");
  text += lit("- Compatibility graphics API: {{COMPATIBILITY_GRAPHICS_API}}\n");
  text += lit("- Rendering path: {{RENDERING_PATH}}\n");
  text += lit("- Render Graph: {{RENDER_GRAPH}}\n");
  text += lit("- GPU Resident Drawer: {{GPU_RESIDENT_DRAWER}}\n\n");
  text += lit("Start by reading `shader_reconstruction_index.json`, `AGENTS.md`, `drawcall.json`, `pipeline_state.html`, shader disassembly files, `ConstantBuffers/constant_buffers.json`, texture metadata, sampler metadata, and mesh metadata.\n\n");
  text += lit("Deliverables:\n\n");
  text += lit("- Reconstructed Unity shader source suitable for URP 17.0.4.\n");
  text += lit("- Mapping notes from exported RenderDoc bindings to Unity shader properties, cbuffers, textures, samplers, varyings, and passes.\n");
  text += lit("- A short uncertainty list for any inferred behavior.\n\n");
  text += lit("Completion criteria:\n\n");
  text += lit("- Shader logic is cross-checked against native DXBC/DXIL disassembly.\n");
  text += lit("- HLSL decompiler output is treated as a draft, not ground truth.\n");
  text += lit("- Low-priority URP Z-bin, Tile, cluster, light-list, culling, and large intermediate buffers are ignored unless shader code directly reads them.\n");
  return text;
}

QString DrawcallExporter::RenderShaderReconstructionGoalTemplate(const QString &templateText,
                                                                 const QString &sourcePath) const
{
  QString rendered = templateText;
  QMap<QString, QString> values;
  values[lit("EXPORT_DIR")] = QDir::toNativeSeparators(m_ExportDir);
  values[lit("CAPTURE_FILE")] = QDir::toNativeSeparators(ToQStr(m_Ctx.GetCaptureFilename()));
  values[lit("EVENT_ID")] = QString::number(m_Ctx.CurEvent());
  values[lit("SELECTED_EVENT_ID")] = QString::number(m_Ctx.CurSelectedEvent());
  values[lit("API")] = ToQStr(m_Ctx.APIProps().pipelineType);
  values[lit("UNITY_VERSION")] = lit("Unity 6.0 (6000.0.62f1)");
  values[lit("URP_VERSION")] = lit("URP 17.0.4");
  values[lit("PLATFORM")] = lit("Mobile game");
  values[lit("PRIMARY_GRAPHICS_API")] = lit("Vulkan");
  values[lit("COMPATIBILITY_GRAPHICS_API")] = lit("OpenGLES");
  values[lit("RENDERING_PATH")] = lit("Deferred");
  values[lit("RENDER_GRAPH")] = lit("Supported");
  values[lit("GPU_RESIDENT_DRAWER")] =
      lit("Supported; preserve instancing and instance ID assumptions.");
  values[lit("TEMPLATE_SOURCE")] = QDir::toNativeSeparators(sourcePath);

  for(auto it = values.begin(); it != values.end(); ++it)
    rendered.replace(lit("{{") + it.key() + lit("}}"), it.value());

  return rendered;
}

void DrawcallExporter::WriteShaderReconstructionDocs()
{
  QString templateSource;
  const QString goalTemplate = LoadShaderReconstructionGoalTemplate(&templateSource);
  const QString goal = RenderShaderReconstructionGoalTemplate(goalTemplate, templateSource);
  QVariantMap goalMeta;
  goalMeta[lit("template_source")] = templateSource;
  if(WriteTextFile(lit("UnityShaderReconstructionGoal.md"), goal))
    RecordFile(lit("shader_reconstruction_goal"), lit("UnityShaderReconstructionGoal.md"),
               goalMeta);

  QString agents;
  agents += lit("# AGENTS.md\n\n");
  agents += lit("This directory is a RenderDoc current-drawcall export prepared for Unity/URP shader reconstruction.\n\n");
  agents += QFormatStr("- Capture: `%1`\n").arg(ToQStr(m_Ctx.GetCaptureFilename()));
  agents += QFormatStr("- Event ID: `%1`\n").arg(m_Ctx.CurEvent());
  agents += QFormatStr("- API: `%1`\n\n").arg(ToQStr(m_Ctx.APIProps().pipelineType));
  agents += lit("## Target Environment\n\n");
  agents += lit("- Unity: Unity 6.0 (6000.0.62f1)\n");
  agents += lit("- URP: 17.0.4\n");
  agents += lit("- Platform: mobile game\n");
  agents += lit("- Graphics API: Vulkan primary, OpenGLES compatible\n");
  agents += lit("- Rendering path: Deferred\n");
  agents += lit("- Render Graph: supported\n");
  agents += lit("- GPU Resident Drawer: supported; preserve instanced drawing and instance ID assumptions.\n\n");
  agents += lit("## Codex Goal Entry\n\n");
  agents += lit("From this export directory, start Codex with `/goal UnityShaderReconstructionGoal.md`. That goal file is generated from the project template `AIDoc/UnityShaderReconstructionGoalTemplate.md` when available.\n\n");
  agents += lit("## Read Order\n\n");
  agents += lit("1. `UnityShaderReconstructionGoal.md` for the concrete Codex objective and deliverables.\n");
  agents += lit("2. `shader_reconstruction_index.json` for the priority-grouped file map.\n");
  agents += lit("3. `drawcall.json`, `pipeline_state.html`, and `pipeline_state.json` for event, topology, bindings, render targets, and fixed-function state.\n");
  agents += lit("4. `Shaders/*`: read all active stages. For compute GBuffer paths, `Shaders/cs` is primary. Prefer `disassembly_hlsl_*.txt` when present, then cross-check against `disassembly_native_*.txt` or `disassembly_default.txt`.\n");
  agents += lit("5. `ConstantBuffers/constant_buffers.json`, then high-priority `ConstantBuffers/*/*.json` or `.csv` for decoded parameters, including `ConstantBuffers/cs` for compute dispatches.\n");
  agents += lit("6. `Textures/Metadata/textures.json`, input textures, output textures, compute RWTexture/UAV GBuffer outputs, and `Textures/Metadata/samplers.json`.\n");
  agents += lit("7. `Mesh/mesh_postvs.obj`, `Mesh/mesh_postvs.json`, and `Mesh/vertex_input_layout.json` for geometry validation.\n");
  agents += lit("8. `ResourceBuffers/` only after shader code proves a specific SRV/UAV buffer is required.\n\n");
  agents += lit("## Priority Policy\n\n");
  agents += lit("- High priority: shader code/disassembly, pipeline state, decoded constant buffers, bound textures/samplers, render targets, and mesh layout/post-VS geometry.\n");
  agents += lit("- Medium priority: raw shader bytecode, raw cbuffer bytes, raw mesh buffers, and SRV/UAV resource buffers that may be directly referenced.\n");
  agents += lit("- Low priority: Unity/URP Z-bin, Tile, cluster, light-list, culling, and large intermediate buffers unless the shader directly reads them.\n\n");
  agents += lit("## Rules For Reconstruction\n\n");
  agents += lit("- Do not start from large raw buffers. Start from shader code, bindings, decoded cbuffers, textures, samplers, and outputs.\n");
  agents += lit("- Treat HLSL decompiler output as a readable draft, not ground truth. Resolve conflicts with native DXBC/DXIL disassembly and reflection metadata.\n");
  agents += lit("- Keep Unity/URP engine-side intermediate buffers documented but optional unless a binding name or instruction path proves they influence the material result.\n");
  agents += lit("- Reconstruct Unity properties and CBUFFER layouts from decoded constant buffers and reflection names before inferring artistic parameters.\n");
  agents += lit("- Use output textures and post-transform mesh data as validation targets for the reconstructed shader.\n");
  if(WriteTextFile(lit("AGENTS.md"), agents))
    RecordFile(lit("shader_reconstruction_agent_guide"), lit("AGENTS.md"));

  QString workflow;
  workflow += lit("# Unity Shader Reconstruction Workflow\n\n");
  workflow += lit("Target Unity stack: Unity 6.0 (6000.0.62f1), URP 17.0.4, mobile, Vulkan primary with OpenGLES compatibility, Deferred, Render Graph supported, GPU Resident Drawer/instanced drawing supported.\n\n");
  workflow += lit("Codex entry: run `/goal UnityShaderReconstructionGoal.md` from this export directory.\n\n");
  workflow += lit("## 1. Orient The Drawcall\n\n");
  workflow += lit("Read `drawcall.json` and `pipeline_state.html/json`. Identify the selected event, draw/dispatch topology, render targets or UAV outputs, depth target if present, active shader IDs, and descriptor bindings.\n\n");
  workflow += lit("## 2. Recover Shader Logic\n\n");
  workflow += lit("Read `Shaders/*/disassembly_targets.json` for every active stage. If the event is a compute dispatch, start with `Shaders/cs`. If `disassembly_hlsl_*.txt` exists, use it as the first-pass HLSL draft. Always compare control flow, resource loads, UAV writes, texture samples, and arithmetic with `disassembly_native_*.txt` or `disassembly_default.txt`.\n\n");
  workflow += lit("## 3. Map Parameters\n\n");
  workflow += lit("Read `ConstantBuffers/constant_buffers.json`. Prioritize entries marked `high`. Use each cbuffer JSON/CSV to recover variable names, layout, numeric values, matrices, vectors, feature toggles, and Unity-style property candidates.\n\n");
  workflow += lit("## 4. Map Textures And Samplers\n\n");
  workflow += lit("Read `Textures/Metadata/textures.json` and `samplers.json`. Map SRV/UAV names and slots to Unity `TEXTURE2D`, `RWTexture2D`, `SAMPLER`, render-target, depth, GBuffer, and intermediate texture declarations. Compute `read_write` textures exported under `Textures/Outputs` are high-priority output evidence. Use exported images for material evidence.\n\n");
  workflow += lit("## 5. Map Geometry Inputs\n\n");
  workflow += lit("Read `Mesh/vertex_input_layout.json` and `Mesh/mesh_postvs.json`. Use OBJ output to validate clip/world/object-space assumptions and vertex-stage reconstruction.\n\n");
  workflow += lit("## 6. Handle Optional Buffers\n\n");
  workflow += lit("Read `ResourceBuffers/resource_buffers.json` only for buffers directly referenced by shader code. Treat URP Z-bin, Tile, cluster, light-list, and culling data as low priority unless the shader explicitly depends on their values.\n\n");
  workflow += lit("## 7. Produce The Reconstructed Shader\n\n");
  workflow += lit("Write a Unity-compatible shader/HLSL reconstruction with declarations for textures, samplers, cbuffers, vertex inputs, interpolators, and passes. Note any uncertain mapping and cite the source file paths used.\n");
  if(WriteTextFile(lit("UnityShaderReconstructionWorkflow.md"), workflow))
    RecordFile(lit("shader_reconstruction_workflow"),
               lit("UnityShaderReconstructionWorkflow.md"));

  const QString indexPath = lit("shader_reconstruction_index.json");
  if(WriteJSONFile(indexPath, MakeShaderReconstructionIndex()))
  {
    RecordFile(lit("shader_reconstruction_index"), indexPath);
    WriteJSONFile(indexPath, MakeShaderReconstructionIndex());
  }
}

void DrawcallExporter::WriteManifestAndSummary()
{
  WriteShaderReconstructionDocs();

  QVariantMap root;
  root[lit("schema")] = lit("renderdoc.current_drawcall_export.v1");
  root[lit("capture_file")] = ToQStr(m_Ctx.GetCaptureFilename());
  root[lit("event_id")] = m_Ctx.CurEvent();
  root[lit("selected_event_id")] = m_Ctx.CurSelectedEvent();
  root[lit("api")] = ToQStr(m_Ctx.APIProps().pipelineType);
  root[lit("exported_at")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
  root[lit("files")] = m_Files;
  root[lit("failures")] = m_Failures;
  root[lit("file_count")] = m_Files.count();
  root[lit("failure_count")] = m_Failures.count();
  WriteJSONFile(lit("manifest.json"), root);

  QString summary;
  summary += lit("# Current Drawcall Export\n\n");
  summary += QFormatStr("- Capture: `%1`\n").arg(ToQStr(m_Ctx.GetCaptureFilename()));
  summary += QFormatStr("- Event ID: `%1`\n").arg(m_Ctx.CurEvent());
  summary += QFormatStr("- API: `%1`\n").arg(ToQStr(m_Ctx.APIProps().pipelineType));
  summary += QFormatStr("- Files exported: `%1`\n").arg(m_Files.count());
  summary += QFormatStr("- Failures: `%1`\n\n").arg(m_Failures.count());

  summary += lit("## Shader Reconstruction Notes\n\n");
  summary += lit("- Codex entry point: run `/goal UnityShaderReconstructionGoal.md` from this export directory.\n");
  summary += lit("- Target stack: Unity 6.0 (6000.0.62f1), URP 17.0.4, mobile, Vulkan primary with OpenGLES compatibility, Deferred, Render Graph, GPU Resident Drawer/instanced drawing.\n");
  summary += lit("- Active shader stages are exported, including `Shaders/cs` for compute dispatches. `Shaders/*/disassembly_default.txt` is kept for compatibility; `Shaders/*/disassembly_native_*.txt` is the native DXBC/DXIL disassembly for manual cross-checking.\n");
  summary += lit("- `Shaders/*/disassembly_hlsl_*.txt` is exported when a RenderDoc disassembly target or configured shader processor can produce HLSL.\n");
  summary += lit("- `ConstantBuffers/constant_buffers.json` maps each active-stage cbuffer slot to exported files, including CS. `display_name` prefers the same Buffer name shown in RenderDoc's Constant Buffers table.\n");
  summary += lit("- Compute RWTexture/UAV resources are exported as output textures under `Textures/Outputs` with role `read_write`; these are high-priority when GBuffer is written from CS.\n");
  summary += lit("- `ConstantBuffers/` data, shader code, input/output textures, mesh exports, and pipeline state are the primary references for shader reconstruction. Unity/URP z-bin, tile, cluster, light-list, and culling buffers can still appear under ConstantBuffers but are usually low-priority renderer intermediate data.\n");
  summary += lit("- `ResourceBuffers/` contains raw SRV/UAV buffers. Consult them only when shader code directly indexes those resources. See `ResourceBuffers/README.md` and `resource_buffers.json`.\n\n");

  if(!m_Failures.empty())
  {
    summary += lit("## Failures\n\n");
    for(const QVariant &failure : m_Failures)
    {
      QVariantMap f = failure.toMap();
      summary += QFormatStr("- `%1`: %2\n").arg(f[lit("task")].toString()).arg(f[lit("message")].toString());
    }
  }

  WriteTextFile(lit("export_summary.md"), summary);
}
