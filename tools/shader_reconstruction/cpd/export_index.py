from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple

from .errors import AnalyzerError
from .schemas import (
    AnalyzerWarning,
    ConstantBufferRecord,
    ExportContext,
    ResourceBufferRecord,
    ShaderDisassemblyFile,
    normalize_stage,
    parse_int,
)


def _load_json(path: Path, warnings: List[AnalyzerWarning], required: bool = False) -> Dict[str, Any]:
    if not path.exists():
        warning = AnalyzerWarning(
            "missing_json",
            f"Missing {'required' if required else 'optional'} JSON file: {path.name}",
            path.as_posix(),
        )
        warnings.append(warning)
        if required:
            raise AnalyzerError(warning.message)
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        warning = AnalyzerWarning("invalid_json", f"Could not parse JSON: {exc}", path.as_posix())
        warnings.append(warning)
        if required:
            raise AnalyzerError(warning.message)
        return {}


def _slot_from_binding(binding: Any, fallback: Any = None) -> Optional[int]:
    value = parse_int(fallback)
    if value is not None:
        return value
    text = str(binding or "")
    match = re.search(r"([btuars])0*([0-9]+)", text, re.IGNORECASE)
    if match:
        return int(match.group(2))
    match = re.search(r"0*([0-9]+)", text)
    if match:
        return int(match.group(1))
    return None


def _resource_register_class(entry: Dict[str, Any]) -> str:
    descriptor = entry.get("descriptor") or {}
    access = descriptor.get("access") or {}
    dtype = str(access.get("descriptor_type") or entry.get("descriptor_type") or "").lower()
    binding = str(entry.get("binding") or "").lower()
    if "readwrite" in dtype or "uav" in dtype or binding.startswith("u"):
        return "uav"
    return "srv"


def _path_exists(ctx: ExportContext, value: Optional[str]) -> bool:
    path = ctx.abs_path(value)
    return bool(path and path.exists())


def _file_size(ctx: ExportContext, value: Optional[str]) -> Optional[int]:
    path = ctx.abs_path(value)
    if path and path.exists():
        return path.stat().st_size
    return None


def _capture_context(manifest: Dict[str, Any], drawcall: Dict[str, Any], index: Dict[str, Any]) -> Dict[str, Any]:
    capture: Dict[str, Any] = {}
    for src in (index, drawcall, manifest):
        for key in ("capture_file", "event_id", "selected_event_id", "api", "draw_name", "name"):
            if key in src and key not in capture:
                capture[key] = src[key]
    if "draw_name" not in capture and "name" in capture:
        capture["draw_name"] = capture["name"]
    if "active_stages" not in capture:
        stages = []
        shaders = drawcall.get("shaders") or (index.get("by_phase") or {}).get("shaders") or {}
        if isinstance(shaders, dict):
            stages.extend(normalize_stage(k) for k in shaders.keys())
        for item in index.get("high_priority", []) + index.get("medium_priority", []):
            stage = item.get("stage") if isinstance(item, dict) else None
            if stage:
                stages.append(normalize_stage(stage))
        capture["active_stages"] = sorted(set(s for s in stages if s))
    return capture


def load_export_context(export_root: Path, stage_filter: Optional[Iterable[str]] = None) -> ExportContext:
    export_root = export_root.resolve()
    if not export_root.exists() or not export_root.is_dir():
        raise AnalyzerError(f"Drawcall export directory does not exist: {export_root}")

    warnings: List[AnalyzerWarning] = []
    manifest = _load_json(export_root / "manifest.json", warnings)
    drawcall = _load_json(export_root / "drawcall.json", warnings)
    pipeline_state = _load_json(export_root / "pipeline_state.json", warnings)
    shader_index = _load_json(export_root / "shader_reconstruction_index.json", warnings)

    ctx = ExportContext(
        export_root=export_root,
        manifest=manifest,
        drawcall=drawcall,
        pipeline_state=pipeline_state,
        shader_index=shader_index,
        warnings=warnings,
    )
    ctx.capture = _capture_context(manifest, drawcall, shader_index)

    allowed_stages = {normalize_stage(x) for x in stage_filter or [] if x}
    _load_constant_buffers(ctx, allowed_stages)
    _load_resource_buffers(ctx, allowed_stages)
    discover_disassembly_files(ctx, allowed_stages)
    return ctx


def _load_constant_buffers(ctx: ExportContext, allowed_stages: set[str]) -> None:
    path = ctx.export_root / "ConstantBuffers" / "constant_buffers.json"
    root = _load_json(path, ctx.warnings)
    entries = root.get("buffers") if isinstance(root, dict) else None
    if not isinstance(entries, list):
        if path.exists():
            ctx.warnings.append(AnalyzerWarning("invalid_constant_buffer_index", "constant_buffers.json has no buffers list", ctx.rel(path)))
        return

    for ordinal, entry in enumerate(entries):
        if not isinstance(entry, dict):
            continue
        stage = normalize_stage(entry.get("stage"))
        if allowed_stages and stage not in allowed_stages:
            continue
        slot = _slot_from_binding(entry.get("binding"), entry.get("slot"))
        binding = str(entry.get("binding") or (f"b{slot}" if slot is not None else ""))
        record = ConstantBufferRecord(
            id=f"cb:{stage}:{slot if slot is not None else ordinal}",
            stage=stage,
            slot=slot,
            binding=binding,
            display_name=str(entry.get("display_name") or entry.get("block_name") or binding),
            block_name=str(entry.get("block_name") or ""),
            renderdoc_buffer_name=str(entry.get("renderdoc_buffer_name") or ""),
            resource_id=str(entry.get("resource") or entry.get("resource_id") or ""),
            byte_offset=parse_int(entry.get("byte_offset"), 0) or 0,
            byte_size=parse_int(entry.get("byte_size"), 0) or 0,
            raw_path=entry.get("raw_path"),
            json_path=entry.get("json_path"),
            csv_path=entry.get("csv_path"),
            priority=str(entry.get("shader_reconstruction_priority") or "medium").lower(),
            recommended=entry.get("recommended_for_shader_reconstruction"),
            source=entry,
        )
        record.raw_exists = _path_exists(ctx, record.raw_path)
        if record.raw_path and not record.raw_exists:
            ctx.warnings.append(AnalyzerWarning("missing_raw_file", "Constant buffer raw file is missing", record.raw_path, {"record_id": record.id}))
        ctx.constant_buffers.append(record)


def _load_resource_buffers(ctx: ExportContext, allowed_stages: set[str]) -> None:
    path = ctx.export_root / "ResourceBuffers" / "resource_buffers.json"
    root = _load_json(path, ctx.warnings)
    entries = root.get("buffers") if isinstance(root, dict) else None
    if not isinstance(entries, list):
        if path.exists():
            ctx.warnings.append(AnalyzerWarning("invalid_resource_buffer_index", "resource_buffers.json has no buffers list", ctx.rel(path)))
        return

    for ordinal, entry in enumerate(entries):
        if not isinstance(entry, dict):
            continue
        stage = normalize_stage(entry.get("stage"))
        if allowed_stages and stage not in allowed_stages:
            continue
        descriptor = entry.get("descriptor") or {}
        access = descriptor.get("access") if isinstance(descriptor, dict) else {}
        access = access or {}
        slot = _slot_from_binding(entry.get("binding"), access.get("index"))
        register_class = _resource_register_class(entry)
        binding = str(entry.get("binding") or (f"{'u' if register_class == 'uav' else 't'}{slot}" if slot is not None else ""))
        desc = descriptor.get("descriptor") if isinstance(descriptor, dict) else {}
        stride = parse_int((desc or {}).get("element_byte_size"))
        if stride in (0, None):
            stride = parse_int((desc or {}).get("buffer_struct_count"))
        raw_path = entry.get("path") or entry.get("raw_path")
        record = ResourceBufferRecord(
            id=f"res:{stage}:{register_class}:{slot if slot is not None else ordinal}",
            stage=stage,
            slot=slot,
            register_class=register_class,
            binding=binding,
            display_name=str(entry.get("display_name") or entry.get("shader_resource_name") or entry.get("renderdoc_resource_name") or binding),
            shader_resource_name=str(entry.get("shader_resource_name") or ""),
            renderdoc_resource_name=str(entry.get("renderdoc_resource_name") or entry.get("original_name") or ""),
            resource_id=str(entry.get("resource") or ""),
            descriptor_type=str(access.get("descriptor_type") or ""),
            byte_offset=parse_int(entry.get("byte_offset"), 0) or 0,
            requested_byte_size=parse_int(entry.get("requested_byte_size"), 0) or 0,
            exported_byte_size=parse_int(entry.get("exported_byte_size"), 0) or 0,
            byte_size=parse_int(entry.get("byte_size"), 0) or 0,
            stride=stride,
            raw_path=raw_path,
            priority=str(entry.get("shader_reconstruction_priority") or "medium").lower(),
            recommended=entry.get("recommended_for_shader_reconstruction"),
            truncated=bool(entry.get("truncated")),
            source=entry,
        )
        if not record.exported_byte_size:
            record.exported_byte_size = record.byte_size
        record.raw_exists = _path_exists(ctx, record.raw_path)
        size = _file_size(ctx, record.raw_path)
        if size is not None and not record.exported_byte_size:
            record.exported_byte_size = size
        if record.raw_path and not record.raw_exists:
            ctx.warnings.append(AnalyzerWarning("missing_raw_file", "Resource buffer raw file is missing", record.raw_path, {"record_id": record.id}))
        ctx.resource_buffers.append(record)


def discover_disassembly_files(ctx: ExportContext, allowed_stages: set[str] | None = None) -> List[ShaderDisassemblyFile]:
    shader_root = ctx.export_root / "Shaders"
    if not shader_root.exists():
        ctx.warnings.append(AnalyzerWarning("missing_shaders_dir", "Shaders directory is missing", "Shaders"))
        return []

    files: List[Tuple[int, Path]] = []
    patterns = [
        (0, "disassembly_native_*.txt"),
        (1, "disassembly_default.txt"),
        (2, "disassembly_hlsl_*.txt"),
        (3, "*.txt"),
    ]
    for stage_dir in shader_root.iterdir():
        if not stage_dir.is_dir():
            continue
        stage = normalize_stage(stage_dir.name)
        if allowed_stages and stage not in allowed_stages:
            continue
        seen = set()
        for priority, pattern in patterns:
            for path in sorted(stage_dir.glob(pattern)):
                if path in seen:
                    continue
                seen.add(path)
                files.append((priority, path))

    result: List[ShaderDisassemblyFile] = []
    for index, (priority, path) in enumerate(sorted(files, key=lambda item: (item[0], item[1].as_posix().lower()))):
        stage = normalize_stage(path.parent.name)
        text = path.read_text(encoding="utf-8", errors="replace")
        backend = detect_backend(path, text)
        rel = ctx.rel(path)
        result.append(
            ShaderDisassemblyFile(
                id=f"{stage}:{index}",
                stage=stage,
                path=rel,
                backend=backend,
                line_count=text.count("\n") + (1 if text else 0),
                preferred=priority <= 1,
            )
        )
    ctx.disassembly_files = result
    return result


def detect_backend(path: Path, text: str) -> str:
    name = path.name.lower()
    head = text[:4096].lower()
    if "hlsl" in name or "hlsldecompiler" in name:
        return "hlsl_decompiled"
    if "dxbc" in name or "dcl_constantbuffer" in head or "shader model" in head and "dcl_" in head:
        return "dxbc"
    if "dxil" in name or "dxil" in head or "llvm" in head or "_dx.types" in head or "initialisehandle" in head:
        return "dxil"
    if "spir-v" in head or "spirv" in name or "opcapability" in head:
        return "spirv"
    if "#version" in head or "layout(" in head:
        return "glsl"
    return "unknown"
