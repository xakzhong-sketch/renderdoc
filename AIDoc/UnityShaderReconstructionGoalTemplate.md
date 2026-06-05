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
3. Read `drawcall.json`, `pipeline_state.html`, and `pipeline_state.json`.
4. Read all active `Shaders/*` stages. If the event is a compute dispatch or compute-written GBuffer path, start with `Shaders/cs`. Prefer `disassembly_hlsl_*.txt` when present, then cross-check native DXBC/DXIL in `disassembly_native_*.txt` or `disassembly_default.txt`.
5. Read `ConstantBuffers/constant_buffers.json`, then high-priority decoded cbuffer `.json` or `.csv` files, including `ConstantBuffers/cs` for compute dispatches.
6. Read `Textures/Metadata/textures.json`, exported input/output textures, compute `RWTexture2D`/UAV GBuffer outputs under `Textures/Outputs`, and `Textures/Metadata/samplers.json`.
7. Read `Mesh/vertex_input_layout.json`, `Mesh/mesh_postvs.json`, and `Mesh/mesh_postvs.obj`.
8. Read `ResourceBuffers/resource_buffers.json` only when shader code directly indexes a specific SRV/UAV buffer.

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
- The final answer cites the export files used for major reconstruction decisions.
