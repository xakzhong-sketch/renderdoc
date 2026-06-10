from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional

from .schemas import (
    AnalyzerWarning,
    BufferWindow,
    BundleHint,
    ExportContext,
    Finding,
    LayoutHit,
    ResourceCandidate,
    ResolvedShaderLoad,
    ShaderLoad,
    to_jsonable,
)


def write_reports(
    ctx: ExportContext,
    out_dir: Path,
    candidates: List[ResourceCandidate],
    shader_loads: List[ShaderLoad],
    unresolved_loads: List[ShaderLoad],
    resolved_loads: List[ResolvedShaderLoad],
    windows: List[BufferWindow],
    findings: Optional[List[Finding]] = None,
    layout_hits: Optional[List[LayoutHit]] = None,
    bundle_hints: Optional[List[BundleHint]] = None,
    input_args: Optional[Dict[str, Any]] = None,
    max_report_findings: int = 10,
) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    findings = findings or []
    layout_hits = layout_hits or []
    bundle_hints = bundle_hints or []
    input_args = input_args or {}

    _write_json(out_dir / "resource_candidates.json", {"version": 1, "resources": [c.to_dict() for c in candidates]})
    _write_json(out_dir / "shader_loads.json", {"version": 1, "loads": [x.to_dict() for x in shader_loads], "resolved_loads": [x.to_dict() for x in resolved_loads]})
    _write_json(out_dir / "buffer_windows.json", {"version": 1, "windows": [x.to_dict() for x in windows]})
    _write_json(out_dir / "unresolved_loads.json", {"version": 1, "loads": [x.to_dict() for x in unresolved_loads]})
    if layout_hits:
        _write_json(out_dir / "layout_hits.json", {"version": 1, "layout_hits": [x.to_dict() for x in layout_hits]})
    if bundle_hints:
        _write_json(out_dir / "bundle_hints.json", {"version": 1, "bundle_hints": [x.to_dict() for x in bundle_hints]})

    summary = {
        "version": 1,
        "input": input_args,
        "capture": ctx.capture,
        "counts": {
            "constant_buffers": len(ctx.constant_buffers),
            "resource_buffers": len(ctx.resource_buffers),
            "disassembly_files": len(ctx.disassembly_files),
            "resource_candidates": len(candidates),
            "shader_loads": len(shader_loads),
            "resolved_loads": len([x for x in resolved_loads if x.resolution_status == "resolved"]),
            "buffer_windows": len([x for x in windows if x.status == "decoded"]),
            "findings": len(findings),
            "warnings": len(ctx.warnings),
        },
        "warnings": [w.to_dict() for w in ctx.warnings],
        "findings": [f.to_dict() for f in findings],
        "resource_candidates": [c.to_dict() for c in candidates[:25]],
        "resolved_loads": [x.to_dict() for x in resolved_loads[:50]],
        "buffer_windows": [x.to_dict() for x in windows[:50]],
        "layout_hits": [x.to_dict() for x in layout_hits[:50]],
        "bundle_hints": [x.to_dict() for x in bundle_hints[:50]],
    }
    _write_json(out_dir / "custom_primitive_data_summary.json", summary)
    (out_dir / "custom_primitive_data_summary.md").write_text(
        _render_markdown(ctx, candidates, resolved_loads, windows, unresolved_loads, findings, layout_hits, bundle_hints, max_report_findings),
        encoding="utf-8",
    )


def _write_json(path: Path, data: Dict[str, Any]) -> None:
    path.write_text(json.dumps(to_jsonable(data), ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def _render_markdown(
    ctx: ExportContext,
    candidates: List[ResourceCandidate],
    resolved_loads: List[ResolvedShaderLoad],
    windows: List[BufferWindow],
    unresolved_loads: List[ShaderLoad],
    findings: List[Finding],
    layout_hits: List[LayoutHit],
    bundle_hints: List[BundleHint],
    max_items: int,
) -> str:
    lines: List[str] = []
    lines.append("# Custom Primitive Data Summary")
    lines.append("")
    lines.append("## Capture Context")
    lines.append("")
    lines.append(f"- Export directory: `{ctx.export_root}`")
    for key in ("capture_file", "event_id", "selected_event_id", "api", "draw_name", "active_stages"):
        if key in ctx.capture:
            lines.append(f"- {key}: `{ctx.capture[key]}`")
    lines.append("")

    if findings:
        lines.append("## High Confidence Findings")
        lines.append("")
        high = [f for f in findings if f.confidence_label == "high"]
        if not high:
            lines.append("- None.")
        for finding in high[:max_items]:
            _append_finding(lines, finding)
        lines.append("")
        lines.append("## Medium Confidence Findings")
        lines.append("")
        medium = [f for f in findings if f.confidence_label == "medium"]
        if not medium:
            lines.append("- None.")
        for finding in medium[:max_items]:
            _append_finding(lines, finding)
        lines.append("")

    lines.append("## Top Resource Candidates")
    lines.append("")
    if not candidates:
        lines.append("- No cbuffer/resource buffer candidates were found.")
    for cand in candidates[:max_items]:
        reason = "; ".join(cand.reasons[:3])
        caveat = f" Caveat: {'; '.join(cand.caveats[:2])}." if cand.caveats else ""
        lines.append(f"- `{cand.priority}` score `{cand.score}` `{cand.stage}` `{cand.binding}` `{cand.display_name}`: {reason}.{caveat}")
    lines.append("")

    lines.append("## Resolved Shader Load Evidence")
    lines.append("")
    resolved_subset = [x for x in resolved_loads if x.resolution_status == "resolved"][: max_items * 2]
    if not resolved_subset:
        lines.append("- No shader load was resolved to exported raw data.")
    for item in resolved_subset:
        load = item.load
        lines.append(
            f"- `{load.stage}` `{load.register}` offset `{load.byte_offset}` -> `{item.record_display_name}` raw `{item.raw_path}` "
            f"from `{load.disassembly_file}:{load.line_number}`"
        )
    lines.append("")

    lines.append("## Buffer Windows")
    lines.append("")
    decoded = [x for x in windows if x.status == "decoded"][:max_items]
    if not decoded:
        lines.append("- No deterministic buffer windows were decoded.")
    for window in decoded:
        lines.append(f"- `{window.id}` load `{window.load_id}` raw `{window.raw_path}` start `{window.start_offset}` bytes `{window.actual_size}`")
        for row in window.rows[:2]:
            lines.append(f"  - `{row['offset']}` float4 `{row['float4']}` uint4 `{row['uint4']}`")
    lines.append("")

    if layout_hits:
        lines.append("## UE Layout Matches")
        lines.append("")
        for hit in layout_hits[:max_items]:
            lines.append(
                f"- `{hit.layout_name}.{hit.field_name}` load `{hit.load_id}` window `{hit.window_id}` "
                f"match `{hit.match_kind}` confidence `{hit.confidence}` relative offset `{hit.field_relative_offset}`"
            )
        lines.append("")

    if bundle_hints:
        lines.append("## Bundle Hints")
        lines.append("")
        for hint in bundle_hints[:max_items]:
            state = "matched" if hint.matched else "unmatched"
            lines.append(f"- `{state}` `{hint.name}` category `{hint.category}` from `{hint.source_path}` keywords `{', '.join(hint.keywords)}`")
        lines.append("")

    lines.append("## Unresolved Loads")
    lines.append("")
    unresolved_subset = [x for x in unresolved_loads if x.parse_status != "declaration"][: max_items * 2]
    if not unresolved_subset:
        lines.append("- None.")
    for load in unresolved_subset:
        lines.append(f"- `{load.stage}` `{load.register}` `{load.parse_status}` at `{load.disassembly_file}:{load.line_number}`: `{load.instruction_text}`")
    lines.append("")

    lines.append("## Warnings")
    lines.append("")
    if not ctx.warnings:
        lines.append("- None.")
    for warning in ctx.warnings[: max_items * 2]:
        lines.append(f"- `{warning.code}` {warning.message} `{warning.path or ''}`")
    lines.append("")

    lines.append("## Next Agent Steps")
    lines.append("")
    lines.append("- Treat resolved shader loads and decoded windows as observed runtime evidence.")
    lines.append("- Treat UE layout matches and bundle parameter names as inference signals, not final proof by themselves.")
    lines.append("- For Nanite or compute-written GBuffer paths, keep unresolved dynamic indexing visible instead of assuming a simple primitive index.")
    lines.append("- Do not inspect full raw buffers unless a shader load, layout match, or finding points to a narrow byte range.")
    lines.append("")
    return "\n".join(lines)


def _append_finding(lines: List[str], finding: Finding) -> None:
    reasons = "; ".join(finding.reasons[:4])
    caveats = f" Caveats: {'; '.join(finding.caveats[:3])}." if finding.caveats else ""
    lines.append(f"- `{finding.confidence_label}` `{finding.confidence}` `{finding.kind}` `{finding.name}`: {reasons}.{caveats}")

