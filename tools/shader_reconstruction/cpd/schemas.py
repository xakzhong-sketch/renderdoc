from __future__ import annotations

from dataclasses import asdict, dataclass, field, is_dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional


STAGE_ORDER = ["vs", "hs", "ds", "gs", "ps", "cs", "as", "ms"]


def to_jsonable(value: Any) -> Any:
    if isinstance(value, Path):
        return value.as_posix()
    if is_dataclass(value):
        return {k: to_jsonable(v) for k, v in asdict(value).items()}
    if isinstance(value, dict):
        return {str(k): to_jsonable(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [to_jsonable(v) for v in value]
    return value


def parse_int(value: Any, default: Optional[int] = None) -> Optional[int]:
    if value is None:
        return default
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        return int(value)
    text = str(value).strip()
    if not text:
        return default
    try:
        if text.lower().startswith("0x"):
            return int(text, 16)
        return int(text)
    except ValueError:
        return default


def normalize_stage(stage: Any) -> str:
    text = str(stage or "").strip().lower()
    aliases = {
        "vertex": "vs",
        "vertex shader": "vs",
        "pixel": "ps",
        "fragment": "ps",
        "pixel shader": "ps",
        "compute": "cs",
        "compute shader": "cs",
        "geometry": "gs",
        "hull": "hs",
        "domain": "ds",
        "mesh": "ms",
        "task": "as",
        "amplification": "as",
    }
    return aliases.get(text, text)


def stable_float(value: float) -> Any:
    if value != value:
        return "NaN"
    if value == float("inf"):
        return "Inf"
    if value == float("-inf"):
        return "-Inf"
    return value


@dataclass
class AnalyzerWarning:
    code: str
    message: str
    path: Optional[str] = None
    detail: Dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> Dict[str, Any]:
        return to_jsonable(self)


@dataclass
class ConstantBufferRecord:
    id: str
    stage: str
    slot: Optional[int]
    binding: str
    display_name: str
    block_name: str
    renderdoc_buffer_name: str
    resource_id: str
    byte_offset: int = 0
    byte_size: int = 0
    raw_path: Optional[str] = None
    json_path: Optional[str] = None
    csv_path: Optional[str] = None
    priority: str = "medium"
    recommended: Optional[bool] = None
    raw_exists: bool = False
    source: Dict[str, Any] = field(default_factory=dict)

    def names(self) -> List[str]:
        return [self.display_name, self.block_name, self.renderdoc_buffer_name, self.resource_id]

    def to_dict(self) -> Dict[str, Any]:
        return to_jsonable(self)


@dataclass
class ResourceBufferRecord:
    id: str
    stage: str
    slot: Optional[int]
    register_class: str
    binding: str
    display_name: str
    shader_resource_name: str
    renderdoc_resource_name: str
    resource_id: str
    descriptor_type: str = ""
    byte_offset: int = 0
    requested_byte_size: int = 0
    exported_byte_size: int = 0
    byte_size: int = 0
    stride: Optional[int] = None
    raw_path: Optional[str] = None
    priority: str = "medium"
    recommended: Optional[bool] = None
    truncated: bool = False
    raw_exists: bool = False
    source: Dict[str, Any] = field(default_factory=dict)

    def names(self) -> List[str]:
        return [
            self.display_name,
            self.shader_resource_name,
            self.renderdoc_resource_name,
            self.resource_id,
            self.binding,
        ]

    def to_dict(self) -> Dict[str, Any]:
        return to_jsonable(self)


@dataclass
class ShaderDisassemblyFile:
    id: str
    stage: str
    path: str
    backend: str
    line_count: int
    preferred: bool = False

    def to_dict(self) -> Dict[str, Any]:
        return to_jsonable(self)


@dataclass
class ResourceCandidate:
    id: str
    kind: str
    record_id: str
    stage: str
    slot: Optional[int]
    binding: str
    display_name: str
    priority: str
    score: float
    reasons: List[str] = field(default_factory=list)
    caveats: List[str] = field(default_factory=list)
    source: Dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> Dict[str, Any]:
        return to_jsonable(self)


@dataclass
class ShaderLoad:
    id: str
    stage: str
    disassembly_file: str
    line_number: int
    instruction_text: str
    backend: str
    resource_class: str
    register: str
    slot: Optional[int]
    register_class: str
    byte_offset: Optional[int] = None
    component_offset: Optional[int] = None
    element_index_expr: Optional[str] = None
    member_offset_expr: Optional[str] = None
    channels: Optional[str] = None
    confidence: float = 0.5
    parse_status: str = "partial"

    def to_dict(self) -> Dict[str, Any]:
        return to_jsonable(self)


@dataclass
class ResolvedShaderLoad:
    load: ShaderLoad
    resolution_status: str
    resource_kind: Optional[str] = None
    resource_id: Optional[str] = None
    record_id: Optional[str] = None
    record_display_name: Optional[str] = None
    raw_path: Optional[str] = None
    raw_exists: bool = False
    raw_file_size: Optional[int] = None
    file_byte_offset: Optional[int] = None
    confidence: float = 0.0
    reasons: List[str] = field(default_factory=list)
    warnings: List[str] = field(default_factory=list)
    record: Dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> Dict[str, Any]:
        data = to_jsonable(self)
        data["load"] = self.load.to_dict()
        return data


@dataclass
class BufferWindow:
    id: str
    load_id: str
    record_id: Optional[str]
    raw_path: Optional[str]
    start_offset: Optional[int]
    requested_size: int
    actual_size: int
    status: str
    rows: List[Dict[str, Any]] = field(default_factory=list)
    reason: Optional[str] = None
    truncated: bool = False
    source: Dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> Dict[str, Any]:
        return to_jsonable(self)


@dataclass
class LayoutField:
    layout_name: str
    name: str
    offset: int
    field_type: str
    size: int
    count: Optional[int] = None

    def to_dict(self) -> Dict[str, Any]:
        return to_jsonable(self)


@dataclass
class LayoutHit:
    load_id: Optional[str]
    window_id: Optional[str]
    layout_name: str
    field_name: str
    field_relative_offset: int
    match_kind: str
    confidence: float

    def to_dict(self) -> Dict[str, Any]:
        return to_jsonable(self)


@dataclass
class BundleHint:
    id: str
    name: str
    category: str
    source_path: str
    source_kind: str
    keywords: List[str] = field(default_factory=list)
    matched: bool = False
    matched_finding_ids: List[str] = field(default_factory=list)

    def to_dict(self) -> Dict[str, Any]:
        return to_jsonable(self)


@dataclass
class Finding:
    id: str
    name: str
    kind: str
    confidence: float
    confidence_label: str
    source: Dict[str, Any] = field(default_factory=dict)
    shader_evidence_ids: List[str] = field(default_factory=list)
    window_ids: List[str] = field(default_factory=list)
    layout_hits: List[Dict[str, Any]] = field(default_factory=list)
    bundle_hints: List[Dict[str, Any]] = field(default_factory=list)
    reasons: List[str] = field(default_factory=list)
    caveats: List[str] = field(default_factory=list)

    def to_dict(self) -> Dict[str, Any]:
        return to_jsonable(self)


@dataclass
class ExportContext:
    export_root: Path
    capture: Dict[str, Any] = field(default_factory=dict)
    manifest: Dict[str, Any] = field(default_factory=dict)
    drawcall: Dict[str, Any] = field(default_factory=dict)
    pipeline_state: Dict[str, Any] = field(default_factory=dict)
    shader_index: Dict[str, Any] = field(default_factory=dict)
    constant_buffers: List[ConstantBufferRecord] = field(default_factory=list)
    resource_buffers: List[ResourceBufferRecord] = field(default_factory=list)
    disassembly_files: List[ShaderDisassemblyFile] = field(default_factory=list)
    warnings: List[AnalyzerWarning] = field(default_factory=list)

    def rel(self, path: Path) -> str:
        try:
            return path.relative_to(self.export_root).as_posix()
        except ValueError:
            return path.as_posix()

    def abs_path(self, maybe_relative: Optional[str]) -> Optional[Path]:
        if not maybe_relative:
            return None
        path = Path(maybe_relative)
        if path.is_absolute():
            return path
        return self.export_root / path

    def to_dict(self) -> Dict[str, Any]:
        return {
            "export_root": str(self.export_root),
            "capture": self.capture,
            "constant_buffers": [x.to_dict() for x in self.constant_buffers],
            "resource_buffers": [x.to_dict() for x in self.resource_buffers],
            "disassembly_files": [x.to_dict() for x in self.disassembly_files],
            "warnings": [x.to_dict() for x in self.warnings],
        }

