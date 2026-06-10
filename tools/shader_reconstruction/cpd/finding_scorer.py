from __future__ import annotations

from typing import Dict, Iterable, List

from .schemas import BufferWindow, BundleHint, Finding, LayoutHit, ResourceCandidate, ResolvedShaderLoad


def build_findings(
    candidates: Iterable[ResourceCandidate],
    resolved_loads: Iterable[ResolvedShaderLoad],
    windows: Iterable[BufferWindow],
    layout_hits: Iterable[LayoutHit],
) -> List[Finding]:
    candidates_by_record = {c.record_id: c for c in candidates}
    windows_by_load: Dict[str, List[BufferWindow]] = {}
    for window in windows:
        windows_by_load.setdefault(window.load_id, []).append(window)
    layout_by_load: Dict[str, List[LayoutHit]] = {}
    for hit in layout_hits:
        if hit.load_id:
            layout_by_load.setdefault(hit.load_id, []).append(hit)

    findings: Dict[str, Finding] = {}
    for resolved in resolved_loads:
        candidate = candidates_by_record.get(resolved.record_id or "")
        key = resolved.record_id or resolved.load.register or resolved.load.id
        score = 0.0
        reasons: List[str] = []
        caveats: List[str] = []

        if candidate:
            score += 0.2 * candidate.score
            reasons.append(f"resource candidate score {candidate.score}")
            if candidate.priority == "low":
                score -= 0.3
                caveats.append("candidate is low-priority renderer/system data")
        if resolved.resolution_status == "resolved":
            score += 0.4
            reasons.append("shader load resolved to exported raw data")
        else:
            caveats.append(f"load resolution status: {resolved.resolution_status}")
        if resolved.load.parse_status == "parsed_static_offset":
            score += 0.1
            reasons.append("shader load has static byte offset")
        else:
            score -= 0.2
            caveats.append("shader load has dynamic or partial offset")
        load_windows = [w for w in windows_by_load.get(resolved.load.id, []) if w.status == "decoded"]
        if load_windows:
            score += 0.05
            reasons.append("decoded buffer window is available")
        hits = layout_by_load.get(resolved.load.id, [])
        if hits:
            score += max(hit.confidence for hit in hits) * 0.2
            reasons.append("UE layout matched the load offset")
        if resolved.record.get("truncated"):
            score -= 0.2
            caveats.append("raw buffer export is truncated")
        score = max(0.0, min(1.0, score))
        name = _finding_name(candidate, resolved, hits)
        finding = findings.get(key)
        if finding is None:
            finding = Finding(
                id=f"finding_{len(findings):04d}",
                name=name,
                kind=_finding_kind(candidate, hits),
                confidence=round(score, 3),
                confidence_label=_label(score),
                source={
                    "record_id": resolved.record_id,
                    "record_display_name": resolved.record_display_name,
                    "resource_kind": resolved.resource_kind,
                    "display_name": candidate.display_name if candidate else resolved.record_display_name,
                },
                reasons=[],
                caveats=[],
            )
            findings[key] = finding
        finding.confidence = round(max(finding.confidence, score), 3)
        finding.confidence_label = _label(finding.confidence)
        finding.shader_evidence_ids.append(resolved.load.id)
        finding.window_ids.extend(w.id for w in load_windows)
        finding.layout_hits.extend(hit.to_dict() for hit in hits)
        finding.reasons.extend(x for x in reasons if x not in finding.reasons)
        finding.caveats.extend(x for x in caveats if x not in finding.caveats)

    result = list(findings.values())
    result.sort(key=lambda f: (-f.confidence, f.name))
    return result


def apply_bundle_hint_scores(findings: List[Finding], hints: List[BundleHint]) -> List[Finding]:
    matched_ids = {fid for hint in hints for fid in hint.matched_finding_ids}
    for finding in findings:
        if finding.id in matched_ids:
            finding.confidence = round(min(1.0, finding.confidence + 0.15), 3)
            finding.confidence_label = _label(finding.confidence)
            if "bundle parameter hint matched" not in finding.reasons:
                finding.reasons.append("bundle parameter hint matched")
    return findings


def _finding_name(candidate: ResourceCandidate | None, resolved: ResolvedShaderLoad, hits: List[LayoutHit]) -> str:
    if hits:
        best = sorted(hits, key=lambda h: -h.confidence)[0]
        return f"{best.layout_name}.{best.field_name}"
    if candidate:
        return candidate.display_name
    return resolved.record_display_name or resolved.load.register


def _finding_kind(candidate: ResourceCandidate | None, hits: List[LayoutHit]) -> str:
    if hits:
        blob = " ".join(hit.field_name.lower() for hit in hits)
        if "customprimitive" in blob or "custom_primitive" in blob:
            return "custom_primitive_data"
        if "instance" in blob:
            return "instance_data"
        return "ue_layout_field"
    if candidate and "primitive" in candidate.display_name.lower():
        return "primitive_data"
    if candidate and "instance" in candidate.display_name.lower():
        return "instance_data"
    return "buffer_evidence"


def _label(score: float) -> str:
    if score >= 0.75:
        return "high"
    if score >= 0.45:
        return "medium"
    return "low"

