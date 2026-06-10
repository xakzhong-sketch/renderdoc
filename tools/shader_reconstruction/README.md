# Shader Reconstruction Tools

This directory contains offline helpers for RenderDoc current-drawcall exports.

## CPD Summary Analyzer

Run from the repository root:

```powershell
python tools/shader_reconstruction/cpd_summary.py `
  --dc-export "K:/WorkSpace/Profiler/Test003/Ref/EID_3483_ID3D11DeviceContext_DrawIndexedInstanced" `
  --ue-layout "tools/shader_reconstruction/layouts/UE5_5_GPUScene.example.json" `
  --verbose
```

The analyzer writes only under:

```text
<dc-export>/Analysis/CPD/
```

Primary report:

```text
Analysis/CPD/custom_primitive_data_summary.md
```

The Markdown report is intended for Codex/AI Agent shader reconstruction. JSON files keep the full evidence chain.

