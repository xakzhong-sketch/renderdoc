from __future__ import annotations

import re
from pathlib import Path
from typing import Iterable, List, Optional, Tuple

from .schemas import ExportContext, ShaderDisassemblyFile, ShaderLoad, parse_int


CBUFFER_RE = re.compile(r"\bcb(?P<slot>\d+)\s*\[\s*(?P<index>[^\]]+)\s*\](?:\.(?P<channels>[xyzwrgba]+))?", re.IGNORECASE)
REGISTER_RE = re.compile(r"\b(?P<class>[tu])(?P<slot>\d+)(?:\.(?P<channels>[xyzwrgba]+))?\b", re.IGNORECASE)
OP_RE = re.compile(r"^\s*(?:(?P<dest>[a-zA-Z0-9_]+\d*)\s*=\s*)?(?P<op>[a-zA-Z_][a-zA-Z0-9_]*)")
IMM_RE = re.compile(r"\bl\(\s*(-?\d+)\s*\)")
DXIL_RESOURCE_DECL_RE = re.compile(
    r"\b(?P<name>(?:RW)?(?:StructuredBuffer|ByteAddressBuffer)\d+)\b.*?:\s*register\(\s*(?P<prefix>[tu])(?P<slot>\d+)",
    re.IGNORECASE,
)
DXIL_CBUFFER_LOAD_RE = re.compile(
    r"__(?:cbuffer)(?P<slot>\d+)_\w+\.Load(?:4)?\(\s*byte_offset\s*=\s*(?P<offset>-?\d+)\s*\)",
    re.IGNORECASE,
)
DXIL_BUFFER_LOAD_RE = re.compile(
    r"__(?P<name>(?:RW)?(?:StructuredBuffer|ByteAddressBuffer)\d+)_\w+\.Load\((?P<args>[^)]*)\)",
    re.IGNORECASE,
)


def _static_int_expr(expr: Optional[str]) -> Optional[int]:
    if expr is None:
        return None
    text = expr.strip()
    value = parse_int(text)
    if value is not None:
        return value
    match = IMM_RE.search(text)
    if match:
        return int(match.group(1))
    return None


def _component_offset(channels: Optional[str]) -> Optional[int]:
    if not channels:
        return None
    channel = channels[0].lower()
    mapping = {"x": 0, "r": 0, "y": 4, "g": 4, "z": 8, "b": 8, "w": 12, "a": 12}
    return mapping.get(channel)


def _clean_line(line: str) -> str:
    return line.split("//", 1)[0].strip()


def parse_disassembly_loads(ctx: ExportContext, files: Optional[Iterable[ShaderDisassemblyFile]] = None) -> Tuple[List[ShaderLoad], List[ShaderLoad]]:
    loads: List[ShaderLoad] = []
    unresolved: List[ShaderLoad] = []
    selected = list(files if files is not None else ctx.disassembly_files)
    for file_index, disasm in enumerate(selected):
        path = ctx.abs_path(disasm.path)
        if not path or not path.exists():
            continue
        register_map = build_dxil_register_map(text) if disasm.backend == "dxil" else {}
        if disasm.backend not in ("dxbc", "dxil", "unknown"):
            unsupported = ShaderLoad(
                id=f"{disasm.stage}:{file_index}:0:0",
                stage=disasm.stage,
                disassembly_file=disasm.path,
                line_number=0,
                instruction_text=f"backend {disasm.backend} is not parsed by MVP parser",
                backend=disasm.backend,
                resource_class="unknown",
                register="",
                slot=None,
                register_class="",
                confidence=0.0,
                parse_status="unsupported_backend",
            )
            unresolved.append(unsupported)
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        ordinal = 0
        for line_number, line in enumerate(text.splitlines(), start=1):
            if disasm.backend == "dxil":
                parsed = parse_dxil_line(disasm, line, line_number, file_index, ordinal, register_map)
            else:
                parsed = parse_dxbc_line(disasm, line, line_number, file_index, ordinal)
            for load in parsed:
                ordinal += 1
                load.id = f"{disasm.stage}:{file_index}:{line_number}:{ordinal}"
                loads.append(load)
                if load.parse_status not in ("parsed_static_offset", "declaration"):
                    unresolved.append(load)
    return loads, unresolved


def build_dxil_register_map(text: str) -> dict[str, tuple[str, int]]:
    mapping: dict[str, tuple[str, int]] = {}
    for line in text.splitlines():
        match = DXIL_RESOURCE_DECL_RE.search(line)
        if not match:
            continue
        prefix = match.group("prefix").lower()
        slot = int(match.group("slot"))
        mapping[match.group("name")] = ("uav" if prefix == "u" else "srv", slot)
    return mapping


def parse_dxil_line(
    disasm: ShaderDisassemblyFile,
    line: str,
    line_number: int,
    file_index: int,
    ordinal: int,
    register_map: dict[str, tuple[str, int]],
) -> List[ShaderLoad]:
    text = _clean_line(line)
    if not text:
        return []
    result: List[ShaderLoad] = []

    cbuffer = DXIL_CBUFFER_LOAD_RE.search(text)
    if cbuffer:
        slot = int(cbuffer.group("slot"))
        byte_offset = int(cbuffer.group("offset"))
        result.append(
            ShaderLoad(
                id=f"{disasm.stage}:{file_index}:{line_number}:{ordinal}",
                stage=disasm.stage,
                disassembly_file=disasm.path,
                line_number=line_number,
                instruction_text=line.strip(),
                backend=disasm.backend,
                resource_class="constant_buffer",
                register=f"cb{slot}",
                slot=slot,
                register_class="cb",
                byte_offset=byte_offset,
                member_offset_expr=str(byte_offset),
                confidence=0.85,
                parse_status="parsed_static_offset",
            )
        )

    buffer_load = DXIL_BUFFER_LOAD_RE.search(text)
    if buffer_load:
        name = buffer_load.group("name")
        register_class, slot = register_map.get(name, ("uav" if name.lower().startswith("rw") else "srv", None))
        args = buffer_load.group("args").strip()
        byte_offset = _static_int_expr(args)
        if byte_offset is None and args.startswith("{") and args.endswith("}"):
            parts = [part.strip() for part in args.strip("{}").split(",")]
            byte_offset = _static_int_expr(parts[0] if parts else None)
        resource_class = "uav_buffer" if register_class == "uav" else (
            "byte_address_buffer" if name.lower().startswith("byteaddressbuffer") else "structured_buffer"
        )
        result.append(
            ShaderLoad(
                id=f"{disasm.stage}:{file_index}:{line_number}:{ordinal}",
                stage=disasm.stage,
                disassembly_file=disasm.path,
                line_number=line_number,
                instruction_text=line.strip(),
                backend=disasm.backend,
                resource_class=resource_class,
                register=f"{'u' if register_class == 'uav' else 't'}{slot}" if slot is not None else name,
                slot=slot,
                register_class=register_class,
                byte_offset=byte_offset,
                element_index_expr=args,
                member_offset_expr=str(byte_offset) if byte_offset is not None else args,
                confidence=0.7 if byte_offset is not None else 0.45,
                parse_status="parsed_static_offset" if byte_offset is not None else "parsed_dynamic_offset",
            )
        )

    return result


def parse_dxbc_line(disasm: ShaderDisassemblyFile, line: str, line_number: int, file_index: int, ordinal: int) -> List[ShaderLoad]:
    text = _clean_line(line)
    if not text:
        return []
    lower = text.lower()
    result: List[ShaderLoad] = []

    for match in CBUFFER_RE.finditer(text):
        slot = int(match.group("slot"))
        index_expr = match.group("index").strip()
        channels = match.group("channels")
        index_value = _static_int_expr(index_expr)
        is_decl = lower.startswith("dcl_constantbuffer")
        byte_offset = index_value * 16 if index_value is not None and not is_decl else None
        status = "declaration" if is_decl else ("parsed_static_offset" if byte_offset is not None else "parsed_dynamic_offset")
        result.append(
            ShaderLoad(
                id=f"{disasm.stage}:{file_index}:{line_number}:{ordinal}",
                stage=disasm.stage,
                disassembly_file=disasm.path,
                line_number=line_number,
                instruction_text=line.strip(),
                backend=disasm.backend,
                resource_class="constant_buffer",
                register=f"cb{slot}",
                slot=slot,
                register_class="cb",
                byte_offset=byte_offset,
                component_offset=_component_offset(channels),
                element_index_expr=index_expr,
                member_offset_expr=None,
                channels=channels,
                confidence=0.9 if byte_offset is not None else 0.55,
                parse_status=status,
            )
        )

    op_match = OP_RE.match(text)
    op = op_match.group("op").lower() if op_match else ""
    if op.startswith("ld") or op in ("load", "bufferload"):
        registers = list(REGISTER_RE.finditer(text))
        if registers:
            resource_match = registers[-1]
            register_class = resource_match.group("class").lower()
            slot = int(resource_match.group("slot"))
            channels = resource_match.group("channels")
            resource_class = _resource_class_for_op(op, register_class)
            prefix = text[: resource_match.start()]
            immediates = [int(m.group(1)) for m in IMM_RE.finditer(prefix)]
            element_expr = None
            member_expr = None
            byte_offset = None
            if "structured" in op:
                if len(immediates) >= 2:
                    element_expr = str(immediates[-2])
                    member_expr = str(immediates[-1])
                    byte_offset = immediates[-1]
                elif len(immediates) == 1:
                    member_expr = str(immediates[-1])
                    byte_offset = immediates[-1]
            elif immediates:
                member_expr = str(immediates[-1])
                byte_offset = immediates[-1]
            status = "parsed_static_offset" if byte_offset is not None else "parsed_dynamic_offset"
            result.append(
                ShaderLoad(
                    id=f"{disasm.stage}:{file_index}:{line_number}:{ordinal}",
                    stage=disasm.stage,
                    disassembly_file=disasm.path,
                    line_number=line_number,
                    instruction_text=line.strip(),
                    backend=disasm.backend,
                    resource_class=resource_class,
                    register=f"{register_class}{slot}",
                    slot=slot,
                    register_class="uav" if register_class == "u" else "srv",
                    byte_offset=byte_offset,
                    element_index_expr=element_expr,
                    member_offset_expr=member_expr,
                    channels=channels,
                    confidence=0.75 if byte_offset is not None else 0.5,
                    parse_status=status,
                )
            )

    return result


def _resource_class_for_op(op: str, register_class: str) -> str:
    if register_class == "u":
        return "uav_buffer"
    if "structured" in op:
        return "structured_buffer"
    if "raw" in op:
        return "raw_buffer"
    return "byte_address_buffer"
