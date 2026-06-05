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

#pragma once

#include <QMap>
#include <QString>
#include <QVariant>
#include "Code/Interface/QRDInterface.h"

class QTextStream;

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

  QString ExportDirectory() const { return m_ExportDir; }
  uint32_t FailureCount() const { return (uint32_t)m_Failures.count(); }

private:
  ICaptureContext &m_Ctx;
  QWidget *m_Parent = NULL;
  QString m_ExportDir;
  DrawcallExportOptions m_Options;
  QVariantList m_Files;
  QVariantList m_Failures;
  QVariantList m_ConstantBuffers;
  QMap<QString, int> m_UsedFilenames;

  QString SafeName(const QString &name, const QString &fallback = QString()) const;
  QString UniqueRelativePath(const QString &relativeDir, const QString &baseName,
                             const QString &extension);
  QString MakeResourceBaseName(const QString &category, const QString &stage, const QString &binding,
                               ResourceId id, const QString &resourceName) const;
  rdcarray<ShaderStage> ActiveShaderStages() const;
  ResourceId PipelineObjectForStage(ShaderStage stage) const;

  bool EnsureDir(const QString &relativeDir);
  bool WriteTextFile(const QString &relativePath, const QString &contents);
  bool WriteBytesFile(const QString &relativePath, const bytebuf &contents);
  bool WriteJSONFile(const QString &relativePath, const QVariantMap &contents);

  void RecordFile(const QString &category, const QString &relativePath,
                  const QVariantMap &metadata = QVariantMap());
  void RecordFailure(const QString &task, const QString &message,
                     const QVariantMap &metadata = QVariantMap());
  QString ShaderReconstructionPriority(const QVariantMap &file) const;
  QString ShaderReconstructionRole(const QVariantMap &file) const;
  QString ShaderReconstructionPhase(const QVariantMap &file) const;
  QVariantMap MakeShaderReconstructionIndex() const;
  QString LoadShaderReconstructionGoalTemplate(QString *sourcePath) const;
  QString DefaultShaderReconstructionGoalTemplate() const;
  QString RenderShaderReconstructionGoalTemplate(const QString &templateText,
                                                 const QString &sourcePath) const;
  void WriteShaderReconstructionDocs();

  ResultDetails ExportPackage();
  void ExportDrawcallJSON();
  void ExportPipelineHTML();
  void ExportPipelineJSON();
  void ExportTexturesAndSamplers();
  void ExportShaderTextures(ShaderStage stage, const QString &stageShort,
                            const QString &role, const rdcarray<UsedDescriptor> &descriptors,
                            QVariantList &textureMetadata);
  void ExportOutputTextures(QVariantList &textureMetadata);
  void ExportShaders();
  void ExportShaderStage(ShaderStage stage, const QString &stageShort);
  void ExportConstantBuffers();
  void ExportConstantBuffersForStage(ShaderStage stage, const QString &stageShort);
  void ExportMesh();
  void ExportResourceBuffers();
  void WriteManifestAndSummary();

  QVariantMap MakeDrawcallJSON() const;
  QVariantMap MakePipelineStateJSON() const;
  QVariantMap MakeShaderJSON(const ShaderReflection *refl, ShaderStage stage) const;
  QVariantMap MakeShaderReflectionJSON(const ShaderReflection *refl) const;
  QVariantMap MakeConstantBlockJSON(const ConstantBlock &block, const UsedDescriptor &used,
                                    ShaderStage stage, uint32_t slot, uint32_t arrayIdx,
                                    const rdcarray<ShaderVariable> &variables) const;
  QVariantMap MakeResourceFormatJSON(const ResourceFormat &format) const;
  QVariantMap MakeDescriptorJSON(const Descriptor &descriptor) const;
  QVariantMap MakeSamplerJSON(const SamplerDescriptor &sampler) const;
  QVariantMap MakeDescriptorAccessJSON(const DescriptorAccess &access) const;
  QVariantMap MakeUsedDescriptorJSON(const UsedDescriptor &used) const;
  QVariantMap MakeTextureJSON(const TextureDescription *tex) const;
  QVariantMap MakeBufferJSON(const BufferDescription *buf) const;
  QVariantMap MakeBoundVBufferJSON(const BoundVBuffer &buffer) const;
  QVariantMap MakeVertexInputJSON(const VertexInputAttribute &input) const;
  QVariantMap MakeViewportJSON(const Viewport &viewport) const;
  QVariantMap MakeScissorJSON(const Scissor &scissor) const;
  QVariantMap MakeMeshFormatJSON(const MeshFormat &mesh) const;
  QVariantMap MakeShaderVariableJSON(const ShaderVariable &var) const;
  QVariantList MakeShaderVariableList(const rdcarray<ShaderVariable> &vars) const;
  QVariantList MakeSignatureList(const rdcarray<SigParameter> &sig) const;
  QVariantList MakeResourceList(const rdcarray<ShaderResource> &resources) const;
  QVariantList MakeConstantBlockList(const rdcarray<ConstantBlock> &blocks) const;
  QVariantList MakeUsedDescriptorList(const rdcarray<UsedDescriptor> &descriptors) const;
  QVariantList MakeVBufferList(const rdcarray<BoundVBuffer> &buffers) const;
  QVariantList MakeVertexInputList(const rdcarray<VertexInputAttribute> &inputs) const;
  QVariantList MakeOutputTargetList() const;
  QVariantList MakeViewportList() const;
  QVariantList MakeScissorList() const;

  QString StageShortName(ShaderStage stage) const;
  QString BindingName(const QString &prefix, uint32_t slot) const;
  QString DescriptorBindingName(const UsedDescriptor &used, const QString &defaultPrefix) const;
  QString ConstantBufferDisplayName(const ConstantBlock &block, const UsedDescriptor &used) const;
  QVariantMap ConstantBufferGuidance(const QString &displayName, uint64_t byteSize) const;
  QString ShaderProcessorTargetName(const ShaderProcessingTool &processor) const;
  QString DisassemblyFileBase(const QString &kind, const QString &targetName) const;
  QString ShaderResourceReflectionName(ShaderStage stage, const UsedDescriptor &used,
                                       bool readWrite) const;
  QString ResourceBufferDisplayName(ShaderStage stage, const UsedDescriptor &used,
                                    bool readWrite) const;
  QVariantMap ResourceBufferGuidance(const QString &displayName, uint64_t byteSize,
                                     bool truncated) const;
  QString ActionFlagsString(ActionFlags flags) const;
  QString ShaderVariableValueString(const ShaderVariable &var, uint32_t idx) const;
  QString FileTypeExtension(FileType type) const;
  FileType PreferredTextureFileType(const TextureDescription *tex, const Descriptor &descriptor) const;
  bool SaveTextureDescriptor(const QString &category, const QString &role, const QString &stage,
                             const QString &binding, const Descriptor &descriptor,
                             QVariantList &textureMetadata);
  bool IsTextureResource(ResourceId id) const;
  bool IsBufferResource(ResourceId id) const;
  bool ReadPostVSPosition(const MeshFormat &mesh, uint32_t vert, const bytebuf &vertexData,
                          FloatVector &pos) const;
  bool WritePostVSOBJ(const MeshFormat &mesh, const bytebuf &vertexData, const bytebuf &indexData,
                      const QString &relativePath);
  uint32_t ReadIndex(const MeshFormat &mesh, const bytebuf &indexData, uint32_t idx) const;
  void WriteShaderVariablesCSV(QTextStream &stream, const rdcarray<ShaderVariable> &vars,
                               const QString &prefix = QString()) const;
};
