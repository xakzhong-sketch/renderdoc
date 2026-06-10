import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from cpd.disassembly_loads import build_dxil_register_map, parse_dxbc_line, parse_dxil_line
from cpd.schemas import ShaderDisassemblyFile


class DisassemblyLoadTests(unittest.TestCase):
    def setUp(self):
        self.disasm = ShaderDisassemblyFile(
            id="ps:0",
            stage="ps",
            path="Shaders/ps/disassembly_default.txt",
            backend="dxbc",
            line_count=1,
        )

    def test_static_cbuffer_load(self):
        loads = parse_dxbc_line(self.disasm, "mul r0.xyzw, cb0[12].z, r1.xyzw", 4, 0, 0)
        self.assertEqual(len(loads), 1)
        self.assertEqual(loads[0].resource_class, "constant_buffer")
        self.assertEqual(loads[0].register, "cb0")
        self.assertEqual(loads[0].slot, 0)
        self.assertEqual(loads[0].byte_offset, 192)
        self.assertEqual(loads[0].component_offset, 8)
        self.assertEqual(loads[0].parse_status, "parsed_static_offset")

    def test_dynamic_cbuffer_load(self):
        loads = parse_dxbc_line(self.disasm, "add r0.x, cb1[r2.x + 3].x, r0.x", 9, 0, 0)
        self.assertEqual(len(loads), 1)
        self.assertEqual(loads[0].register, "cb1")
        self.assertIsNone(loads[0].byte_offset)
        self.assertEqual(loads[0].parse_status, "parsed_dynamic_offset")

    def test_structured_load(self):
        line = "ld_structured_indexable(structured_buffer, stride=16) r0.xyzw, r1.x, l(32), t12.xyzw"
        loads = parse_dxbc_line(self.disasm, line, 12, 0, 0)
        self.assertEqual(len(loads), 1)
        self.assertEqual(loads[0].resource_class, "structured_buffer")
        self.assertEqual(loads[0].register, "t12")
        self.assertEqual(loads[0].slot, 12)
        self.assertEqual(loads[0].byte_offset, 32)

    def test_raw_uav_load(self):
        line = "ld_raw r0.xyzw, l(64), u3.xyzw"
        loads = parse_dxbc_line(self.disasm, line, 16, 0, 0)
        self.assertEqual(len(loads), 1)
        self.assertEqual(loads[0].resource_class, "uav_buffer")
        self.assertEqual(loads[0].register, "u3")
        self.assertEqual(loads[0].register_class, "uav")
        self.assertEqual(loads[0].byte_offset, 64)

    def test_dxil_cbuffer_and_structured_load(self):
        disasm = ShaderDisassemblyFile(
            id="cs:0",
            stage="cs",
            path="Shaders/cs/disassembly_default.txt",
            backend="dxil",
            line_count=4,
        )
        text = "StructuredBuffer<float4> StructuredBuffer2 : register(t12, space0);"
        register_map = build_dxil_register_map(text)
        self.assertEqual(register_map["StructuredBuffer2"], ("srv", 12))

        cbuffer = parse_dxil_line(
            disasm,
            "_dx.types.CBufRet.f32 _341 = __cbuffer3_262.Load4(byte_offset = 320);",
            10,
            0,
            0,
            register_map,
        )
        self.assertEqual(cbuffer[0].register, "cb3")
        self.assertEqual(cbuffer[0].byte_offset, 320)
        self.assertEqual(cbuffer[0].parse_status, "parsed_static_offset")

        structured = parse_dxil_line(
            disasm,
            "_dx.types.ResRet.f32 _462 = __StructuredBuffer2_461.Load(_460);",
            11,
            0,
            0,
            register_map,
        )
        self.assertEqual(structured[0].register, "t12")
        self.assertEqual(structured[0].parse_status, "parsed_dynamic_offset")


if __name__ == "__main__":
    unittest.main()
