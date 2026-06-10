from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional

from .schemas import AnalyzerWarning, BufferWindow, LayoutField, LayoutHit, ResolvedShaderLoad, parse_int


def _field_size(field_type: str, count: Optional[int]) -> int:
    text = field_type.lower()
    scalar_size = 4
    if "float4x4" in text:
        scalar_size = 64
    elif "float4x3" in text or "float3x4" in text:
        scalar_size = 48
    elif "float4" in text or "uint4" in text or "int4" in text:
        scalar_size = 16
    elif "float3" in text or "uint3" in text or "int3" in text:
        scalar_size = 12
    elif "float2" in text or "uint2" in text or "int2" in text:
        scalar_size = 8
    return scalar_size * max(1, count or 1)


def load_layout_fields(path: Optional[Path], warnings: List[AnalyzerWarning]) -> tuple[Dict[str, Any], List[LayoutField]]:
    if path is None:
        return {}, []
    if not path.exists():
        warnings.append(AnalyzerWarning("missing_ue_layout", "UE layout file does not exist", path.as_posix()))
        return {}, []
    try:
        root = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        warnings.append(AnalyzerWarning("invalid_ue_layout", f"Could not parse UE layout: {exc}", path.as_posix()))
        return {}, []

    fields: List[LayoutField] = []
    layouts = root.get("layouts") if isinstance(root, dict) else None
    if not isinstance(layouts, dict):
        warnings.append(AnalyzerWarning("invalid_ue_layout", "UE layout JSON has no layouts object", path.as_posix()))
        return root if isinstance(root, dict) else {}, []

    for layout_name, layout in layouts.items():
        if not isinstance(layout, dict):
            continue
        for field in layout.get("fields", []):
            if not isinstance(field, dict):
                continue
            offset = parse_int(field.get("offset"))
            if offset is None:
                continue
            count = parse_int(field.get("count"))
            field_type = str(field.get("type") or "unknown")
            fields.append(
                LayoutField(
                    layout_name=str(layout_name),
                    name=str(field.get("name") or f"field_{offset}"),
                    offset=offset,
                    field_type=field_type,
                    size=_field_size(field_type, count),
                    count=count,
                )
            )
    return root, fields


def match_layouts(loads: Iterable[ResolvedShaderLoad], windows: Iterable[BufferWindow], fields: List[LayoutField]) -> List[LayoutHit]:
    hits: List[LayoutHit] = []
    if not fields:
        return hits
    for resolved in loads:
        if resolved.file_byte_offset is None:
            continue
        for field in fields:
            hit = _match_offset(resolved.file_byte_offset, field)
            if hit:
                hits.append(
                    LayoutHit(
                        load_id=resolved.load.id,
                        window_id=None,
                        layout_name=field.layout_name,
                        field_name=field.name,
                        field_relative_offset=resolved.file_byte_offset - field.offset,
                        match_kind=hit[0],
                        confidence=hit[1],
                    )
                )
    for window in windows:
        if window.start_offset is None:
            continue
        for field in fields:
            hit = _match_offset(window.start_offset, field)
            if hit:
                hits.append(
                    LayoutHit(
                        load_id=window.load_id,
                        window_id=window.id,
                        layout_name=field.layout_name,
                        field_name=field.name,
                        field_relative_offset=window.start_offset - field.offset,
                        match_kind=hit[0],
                        confidence=hit[1],
                    )
                )
    return hits


def _match_offset(offset: int, field: LayoutField) -> Optional[tuple[str, float]]:
    if offset == field.offset:
        return "exact", 0.9
    if field.offset <= offset < field.offset + field.size:
        return "inside", 0.75
    distance = abs(offset - field.offset)
    if distance <= 16:
        return "nearest_16b", 0.45
    return None

