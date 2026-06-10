from __future__ import annotations

from typing import Iterable, List

from .schemas import ConstantBufferRecord, ExportContext, ResourceBufferRecord, ResourceCandidate


HIGH_KEYWORDS = [
    "primitiveuniformbuffer",
    "primitivescenedata",
    "gpuscene",
    "instancescenedata",
    "instancepayloaddata",
    "customprimitivedata",
    "perinstancecustomdata",
    "primitivedata",
    "instancedata",
]

MID_KEYWORDS = [
    "scene",
    "view",
    "material",
    "object",
    "transform",
    "packed",
]

LOW_KEYWORDS = [
    "zbin",
    "z-bin",
    "tile",
    "cluster",
    "lightlist",
    "light-list",
    "culling",
    "hzb",
    "depthpyramid",
    "depth-pyramid",
    "shadow",
]


def _name_blob(names: Iterable[str]) -> str:
    return " ".join(x for x in names if x).lower().replace("_", "").replace(" ", "")


def _score_names(names: Iterable[str]) -> tuple[float, str, List[str], List[str]]:
    blob = _name_blob(names)
    score = 0.1
    priority = "medium"
    reasons: List[str] = []
    caveats: List[str] = []

    for keyword in HIGH_KEYWORDS:
        if keyword in blob:
            score += 0.45
            priority = "high"
            reasons.append(f"name matches high-value UE data keyword: {keyword}")
    for keyword in MID_KEYWORDS:
        if keyword in blob:
            score += 0.12
            reasons.append(f"name matches supporting keyword: {keyword}")
    for keyword in LOW_KEYWORDS:
        if keyword in blob:
            score -= 0.4
            priority = "low"
            caveats.append(f"name matches low-priority renderer intermediate keyword: {keyword}")

    return max(0.0, min(1.0, score)), priority, reasons, caveats


def _priority_from_score(score: float, suggested: str) -> str:
    if suggested == "low":
        return "low"
    if score >= 0.58:
        return "high"
    if score < 0.22:
        return "low"
    return "medium"


def _candidate_from_cbuffer(record: ConstantBufferRecord) -> ResourceCandidate:
    score, priority, reasons, caveats = _score_names(record.names())
    if record.priority == "high":
        score += 0.12
        reasons.append("exporter marked cbuffer high priority")
    elif record.priority == "low":
        score -= 0.2
        caveats.append("exporter marked cbuffer low priority")
    if record.raw_exists:
        score += 0.08
        reasons.append("raw cbuffer bytes are available")
    else:
        caveats.append("raw cbuffer bytes are missing")
    if record.byte_size and record.byte_size <= 65536:
        score += 0.05
        reasons.append("cbuffer size is small enough for direct review")
    score = max(0.0, min(1.0, score))
    priority = _priority_from_score(score, priority)
    return ResourceCandidate(
        id=f"cand:{record.id}",
        kind="constant_buffer",
        record_id=record.id,
        stage=record.stage,
        slot=record.slot,
        binding=record.binding,
        display_name=record.display_name,
        priority=priority,
        score=round(score, 3),
        reasons=reasons or ["cbuffer is available for shader reconstruction"],
        caveats=caveats,
        source=record.to_dict(),
    )


def _candidate_from_resource(record: ResourceBufferRecord) -> ResourceCandidate:
    score, priority, reasons, caveats = _score_names(record.names())
    if record.register_class in ("srv", "uav"):
        score += 0.08
        reasons.append(f"{record.register_class.upper()} buffer is directly shader-addressable")
    if record.stride and record.stride in (16, 32, 48, 64, 80, 96, 112, 128, 256, 512):
        score += 0.08
        reasons.append(f"structured stride {record.stride} is plausible scene/instance data")
    if record.priority == "high":
        score += 0.12
        reasons.append("exporter marked resource buffer high priority")
    elif record.priority == "low":
        score -= 0.22
        caveats.append("exporter marked resource buffer low priority")
    if record.truncated:
        score -= 0.15
        caveats.append("raw export is truncated")
    if record.raw_exists:
        score += 0.06
        reasons.append("raw resource buffer bytes are available")
    else:
        caveats.append("raw resource buffer bytes are missing")
    score = max(0.0, min(1.0, score))
    priority = _priority_from_score(score, priority)
    return ResourceCandidate(
        id=f"cand:{record.id}",
        kind="resource_buffer",
        record_id=record.id,
        stage=record.stage,
        slot=record.slot,
        binding=record.binding,
        display_name=record.display_name,
        priority=priority,
        score=round(score, 3),
        reasons=reasons or ["resource buffer is available as supporting evidence"],
        caveats=caveats,
        source=record.to_dict(),
    )


def find_resource_candidates(ctx: ExportContext) -> List[ResourceCandidate]:
    candidates: List[ResourceCandidate] = []
    candidates.extend(_candidate_from_cbuffer(record) for record in ctx.constant_buffers)
    candidates.extend(_candidate_from_resource(record) for record in ctx.resource_buffers)
    candidates.sort(key=lambda c: ({"high": 0, "medium": 1, "low": 2}.get(c.priority, 1), -c.score, c.stage, c.binding))
    return candidates

