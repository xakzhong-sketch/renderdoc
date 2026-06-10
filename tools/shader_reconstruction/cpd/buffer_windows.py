from __future__ import annotations

import struct
from pathlib import Path
from typing import Any, Dict, Iterable, List

from .schemas import BufferWindow, ExportContext, ResolvedShaderLoad, stable_float


def decode_window(data: bytes, base_offset: int) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    for row_start in range(0, len(data), 16):
        chunk = data[row_start : row_start + 16]
        padded = chunk + b"\x00" * (16 - len(chunk))
        floats = [stable_float(x) for x in struct.unpack("<4f", padded)]
        uints = list(struct.unpack("<4I", padded))
        ints = list(struct.unpack("<4i", padded))
        rows.append(
            {
                "offset": base_offset + row_start,
                "float4": floats,
                "uint4": uints,
                "int4": ints,
                "hex16": chunk.hex(" "),
                "valid_bytes": len(chunk),
            }
        )
    return rows


def extract_buffer_windows(
    ctx: ExportContext,
    resolved_loads: Iterable[ResolvedShaderLoad],
    max_window_bytes: int = 256,
    include_low_priority: bool = False,
) -> List[BufferWindow]:
    windows: List[BufferWindow] = []
    max_window_bytes = max(16, min(max_window_bytes, 4096))
    for index, resolved in enumerate(resolved_loads):
        record_priority = str((resolved.record or {}).get("priority") or (resolved.record or {}).get("shader_reconstruction_priority") or "medium").lower()
        if record_priority == "low" and not include_low_priority:
            windows.append(
                BufferWindow(
                    id=f"win_{index:04d}",
                    load_id=resolved.load.id,
                    record_id=resolved.record_id,
                    raw_path=resolved.raw_path,
                    start_offset=None,
                    requested_size=0,
                    actual_size=0,
                    status="skipped_low_priority",
                    reason="low-priority resource; rerun with --include-low-priority to decode",
                    source=resolved.to_dict(),
                )
            )
            continue
        if resolved.resolution_status != "resolved" or not resolved.raw_path:
            windows.append(
                BufferWindow(
                    id=f"win_{index:04d}",
                    load_id=resolved.load.id,
                    record_id=resolved.record_id,
                    raw_path=resolved.raw_path,
                    start_offset=None,
                    requested_size=0,
                    actual_size=0,
                    status="skipped_unresolved",
                    reason="load was not resolved to an exported raw file",
                    source=resolved.to_dict(),
                )
            )
            continue
        if resolved.file_byte_offset is None:
            windows.append(
                BufferWindow(
                    id=f"win_{index:04d}",
                    load_id=resolved.load.id,
                    record_id=resolved.record_id,
                    raw_path=resolved.raw_path,
                    start_offset=0,
                    requested_size=0,
                    actual_size=0,
                    status="skipped_dynamic_offset",
                    reason="load offset is dynamic and cannot be mapped to a small deterministic window",
                    source=resolved.to_dict(),
                )
            )
            continue

        path = ctx.abs_path(resolved.raw_path)
        if path is None or not path.exists():
            windows.append(
                BufferWindow(
                    id=f"win_{index:04d}",
                    load_id=resolved.load.id,
                    record_id=resolved.record_id,
                    raw_path=resolved.raw_path,
                    start_offset=None,
                    requested_size=0,
                    actual_size=0,
                    status="missing_raw_file",
                    reason="raw file is missing",
                    source=resolved.to_dict(),
                )
            )
            continue

        file_size = path.stat().st_size
        if resolved.file_byte_offset >= file_size:
            windows.append(
                BufferWindow(
                    id=f"win_{index:04d}",
                    load_id=resolved.load.id,
                    record_id=resolved.record_id,
                    raw_path=resolved.raw_path,
                    start_offset=resolved.file_byte_offset,
                    requested_size=max_window_bytes,
                    actual_size=0,
                    status="offset_outside_exported_range",
                    reason="computed offset is outside the exported raw byte range",
                    truncated=bool((resolved.record or {}).get("truncated")),
                    source=resolved.to_dict(),
                )
            )
            continue

        if resolved.resource_kind == "constant_buffer":
            start = max(0, (resolved.file_byte_offset // 16) * 16 - 32)
        else:
            start = max(0, (resolved.file_byte_offset // 16) * 16)
        size = min(max_window_bytes, file_size - start)
        with path.open("rb") as handle:
            handle.seek(start)
            data = handle.read(size)
        windows.append(
            BufferWindow(
                id=f"win_{index:04d}",
                load_id=resolved.load.id,
                record_id=resolved.record_id,
                raw_path=resolved.raw_path,
                start_offset=start,
                requested_size=max_window_bytes,
                actual_size=len(data),
                status="decoded",
                rows=decode_window(data, start),
                truncated=bool((resolved.record or {}).get("truncated")),
                source=resolved.to_dict(),
            )
        )
    return windows

