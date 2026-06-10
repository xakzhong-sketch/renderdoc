# Goal: Reconstruct Unity URP Shader From RenderDoc Drawcall Export

Objective: reconstruct a Unity 6 URP shader/HLSL implementation from the RenderDoc current-drawcall export at `{{EXPORT_DIR}}`.

Capture context:

- Capture: `{{CAPTURE_FILE}}`
- Event ID: `{{EVENT_ID}}`
- Selected Event ID: `{{SELECTED_EVENT_ID}}`
- Captured API: `{{API}}`
- Template source: `{{TEMPLATE_SOURCE}}`

Target environment:

- Unity: {{UNITY_VERSION}}
- URP: {{URP_VERSION}}
- Platform: {{PLATFORM}}
- Primary graphics API: {{PRIMARY_GRAPHICS_API}}
- Compatibility graphics API: {{COMPATIBILITY_GRAPHICS_API}}
- Rendering path: {{RENDERING_PATH}}
- Render Graph: {{RENDER_GRAPH}}
- GPU Resident Drawer: {{GPU_RESIDENT_DRAWER}}

Start here:

1. Read `shader_reconstruction_index.json`.
2. Read `AGENTS.md` and `UnityShaderReconstructionWorkflow.md`.
3. If `Analysis/CPD/custom_primitive_data_summary.md` exists, read it before opening large `ResourceBuffers/` raw files. If it does not exist and the shader appears to use UE Custom Primitive Data, PrimitiveSceneData, InstanceSceneData, GPUScene, Nanite, or compute-written GBuffer data, run the CPD Summary Analyzer from the RenderDoc source checkout:

   ```powershell
   python tools/shader_reconstruction/cpd_summary.py --dc-export "{{EXPORT_DIR}}" --verbose
   ```

4. Read `drawcall.json`, `pipeline_state.html`, and `pipeline_state.json`.
5. Read all active `Shaders/*` stages. If the event is a compute dispatch or compute-written GBuffer path, start with `Shaders/cs`. Prefer `disassembly_hlsl_*.txt` when present, then cross-check native DXBC/DXIL in `disassembly_native_*.txt` or `disassembly_default.txt`.
6. Read `ConstantBuffers/constant_buffers.json`, then high-priority decoded cbuffer `.json` or `.csv` files, including `ConstantBuffers/cs` for compute dispatches.
7. Read `Textures/Metadata/textures.json`, exported input/output textures, compute `RWTexture2D`/UAV GBuffer outputs under `Textures/Outputs`, and `Textures/Metadata/samplers.json`.
8. Read `Mesh/vertex_input_layout.json`, `Mesh/mesh_postvs.json`, and `Mesh/mesh_postvs.obj`.
9. Read `ResourceBuffers/resource_buffers.json` only when shader code or `Analysis/CPD/custom_primitive_data_summary.md` points to a specific SRV/UAV buffer and byte window.

Reconstruction rules:

- Produce Unity-compatible URP shader/HLSL for Unity 6.0 / URP 17.0.4.
- Preserve mobile constraints: avoid desktop-only shader assumptions, keep Vulkan as primary, and keep OpenGLES compatibility in mind.
- Preserve Deferred rendering assumptions and Render Graph compatibility.
- Preserve GPU Resident Drawer / instanced drawing assumptions, including instance ID, per-instance data, and SRP Batcher compatibility where applicable.
- Treat HLSL decompiler output as a readable draft, not ground truth.
- Resolve conflicts with native DXBC/DXIL disassembly, reflection metadata, descriptor bindings, and decoded constant buffers.
- Prioritize shader code, decoded cbuffers, textures, samplers, pipeline state, render targets, and mesh metadata.
- For compute-written GBuffer paths, prioritize `Shaders/cs`, `ConstantBuffers/cs`, CS SRV resources, and CS `read_write` textures exported under `Textures/Outputs`.
- Treat Unity/URP Z-bin, Tile, cluster, light-list, culling, and large intermediate buffers as low priority unless the shader directly reads them.
- Treat `Analysis/CPD/custom_primitive_data_summary.md` as a runtime evidence index for UE Custom Primitive Data, PrimitiveSceneData, InstanceSceneData, GPUScene, and Nanite/compute indirect data. Its layout and bundle matches are inference signals; shader load lines and decoded windows are the primary observed evidence.

Expected deliverables:

- Reconstructed Unity shader source or HLSL include/pass code.
- Binding map from RenderDoc resources to Unity properties, cbuffers, textures, samplers, varyings, and render targets.
- Notes for Unity 6 / URP 17.0.4 integration, including Deferred, Render Graph, GPU Resident Drawer, Vulkan, and OpenGLES compatibility.
- Validation notes comparing the reconstructed shader against exported output textures and mesh context.
- Uncertainty list for inferred behavior or missing data.

Completion criteria:

- Shader logic is traceable to exported disassembly and reflection.
- CBUFFER layouts and texture/sampler bindings are mapped to Unity declarations.
- Low-priority URP intermediate buffers are documented but not used as primary evidence unless required by shader instructions.
- CPD/Primitive/Instance data claims are backed by `Analysis/CPD` evidence when that report exists, or explicitly listed as unresolved when dynamic indexing prevents a deterministic value.
- The final answer cites the export files used for major reconstruction decisions.
