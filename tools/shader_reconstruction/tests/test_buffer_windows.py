import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from cpd.buffer_windows import decode_window, extract_buffer_windows
from cpd.schemas import ExportContext, ResolvedShaderLoad, ShaderLoad


class BufferWindowTests(unittest.TestCase):
    def test_decode_window(self):
        data = struct.pack("<4f", 1.0, 2.0, 3.0, 4.0)
        rows = decode_window(data, 32)
        self.assertEqual(rows[0]["offset"], 32)
        self.assertEqual(rows[0]["float4"], [1.0, 2.0, 3.0, 4.0])
        self.assertEqual(rows[0]["uint4"][0], 1065353216)

    def test_extract_window_for_cbuffer(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            raw = root / "ConstantBuffers" / "ps"
            raw.mkdir(parents=True)
            raw_file = raw / "cb.bin"
            raw_file.write_bytes(bytes(range(128)))
            ctx = ExportContext(export_root=root)
            load = ShaderLoad(
                id="ps:0:1:1",
                stage="ps",
                disassembly_file="Shaders/ps/disassembly_default.txt",
                line_number=1,
                instruction_text="mov r0, cb0[4]",
                backend="dxbc",
                resource_class="constant_buffer",
                register="cb0",
                slot=0,
                register_class="cb",
                byte_offset=64,
                parse_status="parsed_static_offset",
            )
            resolved = ResolvedShaderLoad(
                load=load,
                resolution_status="resolved",
                resource_kind="constant_buffer",
                record_id="cb:ps:0",
                raw_path="ConstantBuffers/ps/cb.bin",
                raw_exists=True,
                file_byte_offset=64,
                record={"priority": "high"},
            )
            windows = extract_buffer_windows(ctx, [resolved], max_window_bytes=64)
            self.assertEqual(windows[0].status, "decoded")
            self.assertEqual(windows[0].start_offset, 32)
            self.assertEqual(windows[0].actual_size, 64)


if __name__ == "__main__":
    unittest.main()

