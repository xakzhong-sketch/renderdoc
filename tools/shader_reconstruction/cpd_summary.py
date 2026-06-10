from __future__ import annotations

import argparse
import sys
from pathlib import Path

from cpd.bundle_hints import load_bundle_hints, match_bundle_hints
from cpd.buffer_windows import extract_buffer_windows
from cpd.disassembly_loads import parse_disassembly_loads
from cpd.errors import AnalyzerError
from cpd.export_index import load_export_context
from cpd.finding_scorer import apply_bundle_hint_scores, build_findings
from cpd.report_writer import write_reports
from cpd.resolver import resolve_shader_loads
from cpd.resource_candidates import find_resource_candidates
from cpd.ue_layout import load_layout_fields, match_layouts


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Generate a CPD/PrimitiveData evidence summary from a RenderDoc drawcall export.")
    parser.add_argument("--dc-export", required=True, help="RenderDoc current drawcall export directory.")
    parser.add_argument("--bundle", help="Optional UE unpacked analysis directory for parameter name hints.")
    parser.add_argument("--ue-layout", help="Optional UE GPUScene/PrimitiveSceneData layout JSON.")
    parser.add_argument("--out", help="Output directory. Defaults to <dc-export>/Analysis/CPD.")
    parser.add_argument("--max-window-bytes", type=int, default=256, help="Maximum bytes decoded per buffer window.")
    parser.add_argument("--max-report-findings", type=int, default=10, help="Maximum items per Markdown report section.")
    parser.add_argument("--include-low-priority", action="store_true", help="Decode windows for low-priority resources too.")
    parser.add_argument("--stage", action="append", help="Limit analysis to a shader stage. Can be repeated.")
    parser.add_argument("--verbose", action="store_true", help="Print per-stage counts.")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)

    try:
        export_root = Path(args.dc_export)
        out_dir = Path(args.out) if args.out else export_root / "Analysis" / "CPD"
        ctx = load_export_context(export_root, args.stage)
        candidates = find_resource_candidates(ctx)
        shader_loads, unresolved_loads = parse_disassembly_loads(ctx)
        resolved_loads = resolve_shader_loads(ctx, shader_loads)
        windows = extract_buffer_windows(ctx, resolved_loads, args.max_window_bytes, args.include_low_priority)
        layout_root, layout_fields = load_layout_fields(Path(args.ue_layout) if args.ue_layout else None, ctx.warnings)
        layout_hits = match_layouts(resolved_loads, windows, layout_fields)
        findings = build_findings(candidates, resolved_loads, windows, layout_hits)
        bundle_hints = load_bundle_hints(Path(args.bundle) if args.bundle else None, ctx.warnings)
        match_bundle_hints(bundle_hints, findings)
        findings = apply_bundle_hint_scores(findings, bundle_hints)
        write_reports(
            ctx=ctx,
            out_dir=out_dir,
            candidates=candidates,
            shader_loads=shader_loads,
            unresolved_loads=unresolved_loads,
            resolved_loads=resolved_loads,
            windows=windows,
            findings=findings,
            layout_hits=layout_hits,
            bundle_hints=bundle_hints,
            input_args={
                "dc_export": str(export_root),
                "bundle": args.bundle,
                "ue_layout": args.ue_layout,
                "out": str(out_dir),
                "max_window_bytes": args.max_window_bytes,
                "include_low_priority": args.include_low_priority,
                "stage": args.stage or [],
            },
            max_report_findings=args.max_report_findings,
        )
        if args.verbose:
            print(f"Export: {ctx.export_root}")
            print(f"Constant buffers: {len(ctx.constant_buffers)}")
            print(f"Resource buffers: {len(ctx.resource_buffers)}")
            print(f"Disassembly files: {len(ctx.disassembly_files)}")
            print(f"Shader loads: {len(shader_loads)}")
            print(f"Resolved loads: {len([x for x in resolved_loads if x.resolution_status == 'resolved'])}")
            print(f"Decoded windows: {len([x for x in windows if x.status == 'decoded'])}")
            print(f"Findings: {len(findings)}")
            print(f"Warnings: {len(ctx.warnings)}")
            print(f"Report: {out_dir / 'custom_primitive_data_summary.md'}")
        return 0
    except AnalyzerError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())

