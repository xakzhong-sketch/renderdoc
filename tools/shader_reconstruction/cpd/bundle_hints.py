from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional

from .schemas import AnalyzerWarning, BundleHint, Finding


KEYWORDS = [
    "customprim",
    "customprimitive",
    "perinstance",
    "primitivedata",
    "blend",
    "mask",
    "layer",
    "variation",
    "instance",
]

PARAM_RE = re.compile(r"[_A-Za-z][A-Za-z0-9_]{2,}")


def load_bundle_hints(bundle_root: Optional[Path], warnings: List[AnalyzerWarning], max_files: int = 2000) -> List[BundleHint]:
    if bundle_root is None:
        return []
    if not bundle_root.exists() or not bundle_root.is_dir():
        warnings.append(AnalyzerWarning("missing_bundle", "Bundle hint directory does not exist", bundle_root.as_posix()))
        return []
    hints: Dict[str, BundleHint] = {}
    files = _candidate_files(bundle_root, max_files)
    for path in files:
        rel = path.relative_to(bundle_root).as_posix()
        try:
            if path.suffix.lower() == ".json":
                _scan_json(json.loads(path.read_text(encoding="utf-8")), rel, hints)
            elif path.suffix.lower() in (".txt", ".usf", ".ush", ".hlsl", ".shader"):
                _scan_text(path.read_text(encoding="utf-8", errors="replace"), rel, "text", hints)
        except Exception as exc:
            warnings.append(AnalyzerWarning("bundle_scan_failed", f"Could not scan bundle hint file: {exc}", rel))
    return list(hints.values())


def _candidate_files(root: Path, max_files: int) -> List[Path]:
    preferred = [
        root / "analysis" / "material_layer_parameter_bindings.json",
        root / "parameters" / "material_parameters.json",
        root / "parameters" / "textures.json",
    ]
    result = [p for p in preferred if p.exists()]
    for path in root.rglob("*"):
        if len(result) >= max_files:
            break
        if path.is_file() and path not in result and path.suffix.lower() in (".json", ".txt", ".usf", ".ush", ".hlsl", ".shader"):
            result.append(path)
    return result


def _scan_json(value: Any, rel: str, hints: Dict[str, BundleHint]) -> None:
    if isinstance(value, dict):
        for key, item in value.items():
            _scan_text(str(key), rel, "json_key", hints)
            _scan_json(item, rel, hints)
    elif isinstance(value, list):
        for item in value:
            _scan_json(item, rel, hints)
    elif isinstance(value, str):
        _scan_text(value, rel, "json_value", hints)


def _scan_text(text: str, rel: str, source_kind: str, hints: Dict[str, BundleHint]) -> None:
    for match in PARAM_RE.finditer(text):
        name = match.group(0)
        lowered = name.lower()
        keywords = [kw for kw in KEYWORDS if kw in lowered]
        if not keywords:
            continue
        key = f"{rel}:{name}"
        if key not in hints:
            hints[key] = BundleHint(
                id=f"hint_{len(hints):04d}",
                name=name,
                category=_category_for_name(lowered),
                source_path=rel,
                source_kind=source_kind,
                keywords=keywords,
            )


def _category_for_name(lowered: str) -> str:
    if "texture" in lowered or "tex" in lowered:
        return "texture"
    if "switch" in lowered or "toggle" in lowered:
        return "switch"
    if "color" in lowered or "vector" in lowered or "mask" in lowered:
        return "vector"
    if "scalar" in lowered or "blend" in lowered:
        return "scalar"
    return "unknown"


def match_bundle_hints(hints: List[BundleHint], findings: List[Finding]) -> List[BundleHint]:
    for hint in hints:
        hint_terms = set(hint.keywords)
        for finding in findings:
            haystack = " ".join(
                [
                    finding.name,
                    finding.kind,
                    " ".join(finding.reasons),
                    str(finding.source.get("display_name", "")),
                    str(finding.source.get("record_display_name", "")),
                ]
            ).lower()
            if any(term in haystack for term in hint_terms):
                hint.matched = True
                hint.matched_finding_ids.append(finding.id)
                finding.bundle_hints.append(hint.to_dict())
    return hints

