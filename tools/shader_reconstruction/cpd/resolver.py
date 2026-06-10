from __future__ import annotations

from pathlib import Path
from typing import Dict, List, Tuple

from .schemas import ConstantBufferRecord, ExportContext, ResourceBufferRecord, ResolvedShaderLoad, ShaderLoad


def resolve_shader_loads(ctx: ExportContext, loads: List[ShaderLoad]) -> List[ResolvedShaderLoad]:
    cb_lookup: Dict[Tuple[str, int], ConstantBufferRecord] = {}
    for record in ctx.constant_buffers:
        if record.slot is not None:
            cb_lookup[(record.stage, record.slot)] = record

    res_lookup: Dict[Tuple[str, str, int], ResourceBufferRecord] = {}
    for record in ctx.resource_buffers:
        if record.slot is not None:
            res_lookup[(record.stage, record.register_class, record.slot)] = record

    resolved: List[ResolvedShaderLoad] = []
    for load in loads:
        if load.parse_status == "declaration":
            continue
        if load.resource_class == "constant_buffer":
            resolved.append(_resolve_cbuffer(ctx, cb_lookup, load))
        elif load.register_class in ("srv", "uav"):
            resolved.append(_resolve_resource(ctx, res_lookup, load))
        else:
            resolved.append(
                ResolvedShaderLoad(
                    load=load,
                    resolution_status="unsupported_resource_class",
                    confidence=0.0,
                    warnings=[f"unsupported resource class {load.resource_class}"],
                )
            )
    return resolved


def _raw_info(ctx: ExportContext, raw_path: str | None) -> tuple[bool, int | None]:
    path = ctx.abs_path(raw_path)
    if path and path.exists():
        return True, path.stat().st_size
    return False, None


def _resolve_cbuffer(ctx: ExportContext, lookup: Dict[Tuple[str, int], ConstantBufferRecord], load: ShaderLoad) -> ResolvedShaderLoad:
    record = lookup.get((load.stage, load.slot if load.slot is not None else -1))
    reasons: List[str] = []
    warnings: List[str] = []
    confidence = 0.0
    if record is None and load.slot is not None:
        same_slot = [r for r in ctx.constant_buffers if r.slot == load.slot]
        if len(same_slot) == 1:
            record = same_slot[0]
            reasons.append("resolved by slot fallback across active stages")
            confidence = 0.55
        elif len(same_slot) > 1:
            return ResolvedShaderLoad(load=load, resolution_status="ambiguous_slot", confidence=0.0, warnings=["multiple cbuffers share slot across stages"])
    if record is None:
        return ResolvedShaderLoad(load=load, resolution_status="missing_index", confidence=0.0, warnings=["no matching cbuffer slot in export index"])
    raw_exists, raw_size = _raw_info(ctx, record.raw_path)
    if not raw_exists:
        warnings.append("matching cbuffer has no exported raw file")
    else:
        reasons.append("resolved cbuffer register to exported raw file")
        confidence = max(confidence, 0.9)
    return ResolvedShaderLoad(
        load=load,
        resolution_status="resolved" if raw_exists else "missing_raw_file",
        resource_kind="constant_buffer",
        resource_id=record.resource_id,
        record_id=record.id,
        record_display_name=record.display_name,
        raw_path=record.raw_path,
        raw_exists=raw_exists,
        raw_file_size=raw_size,
        file_byte_offset=load.byte_offset,
        confidence=confidence,
        reasons=reasons,
        warnings=warnings,
        record=record.to_dict(),
    )


def _resolve_resource(ctx: ExportContext, lookup: Dict[Tuple[str, str, int], ResourceBufferRecord], load: ShaderLoad) -> ResolvedShaderLoad:
    key = (load.stage, load.register_class, load.slot if load.slot is not None else -1)
    record = lookup.get(key)
    reasons: List[str] = []
    warnings: List[str] = []
    confidence = 0.0
    if record is None and load.slot is not None:
        same_slot = [r for r in ctx.resource_buffers if r.slot == load.slot and r.register_class == load.register_class]
        if len(same_slot) == 1:
            record = same_slot[0]
            reasons.append("resolved by SRV/UAV slot fallback across active stages")
            confidence = 0.5
        elif len(same_slot) > 1:
            return ResolvedShaderLoad(load=load, resolution_status="ambiguous_slot", confidence=0.0, warnings=["multiple resource buffers share register slot"])
    if record is None:
        return ResolvedShaderLoad(load=load, resolution_status="missing_index", confidence=0.0, warnings=["no matching resource buffer slot in export index"])
    raw_exists, raw_size = _raw_info(ctx, record.raw_path)
    if not raw_exists:
        warnings.append("matching resource buffer has no exported raw file")
    else:
        reasons.append("resolved SRV/UAV register to exported raw file")
        confidence = max(confidence, 0.85)
    return ResolvedShaderLoad(
        load=load,
        resolution_status="resolved" if raw_exists else "missing_raw_file",
        resource_kind="resource_buffer",
        resource_id=record.resource_id,
        record_id=record.id,
        record_display_name=record.display_name,
        raw_path=record.raw_path,
        raw_exists=raw_exists,
        raw_file_size=raw_size,
        file_byte_offset=load.byte_offset,
        confidence=confidence,
        reasons=reasons,
        warnings=warnings,
        record=record.to_dict(),
    )

