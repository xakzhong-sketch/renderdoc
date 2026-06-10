import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class CliEndToEndTests(unittest.TestCase):
    def test_cli_generates_reports(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            export = root / "EID_1_Draw"
            (export / "ConstantBuffers" / "ps").mkdir(parents=True)
            (export / "ResourceBuffers").mkdir(parents=True)
            (export / "Shaders" / "ps").mkdir(parents=True)
            (export / "manifest.json").write_text("{}", encoding="utf-8")
            (export / "drawcall.json").write_text(json.dumps({"event_id": 1, "api": "D3D11", "draw_name": "DrawIndexed"}), encoding="utf-8")
            (export / "pipeline_state.json").write_text("{}", encoding="utf-8")
            (export / "shader_reconstruction_index.json").write_text(json.dumps({"event_id": 1, "api": "D3D11"}), encoding="utf-8")
            (export / "ConstantBuffers" / "ps" / "cb.raw.bin").write_bytes(struct.pack("<32f", *[float(i) for i in range(32)]))
            (export / "ResourceBuffers" / "primitive.raw.bin").write_bytes(struct.pack("<128f", *[float(i) for i in range(128)]))
            (export / "Shaders" / "ps" / "disassembly_default.txt").write_text(
                "\n".join(
                    [
                        "dcl_constantbuffer cb0[16], immediateIndexed",
                        "mul r0.xyzw, cb0[4].xyzw, r1.xyzw",
                        "ld_raw r2.xyzw, l(320), t12.xyzw",
                    ]
                ),
                encoding="utf-8",
            )
            (export / "ConstantBuffers" / "constant_buffers.json").write_text(
                json.dumps(
                    {
                        "buffers": [
                            {
                                "stage": "ps",
                                "binding": "b000",
                                "slot": 0,
                                "display_name": "PrimitiveUniformBuffer",
                                "raw_path": "ConstantBuffers/ps/cb.raw.bin",
                                "byte_size": "128",
                                "shader_reconstruction_priority": "high",
                            }
                        ]
                    }
                ),
                encoding="utf-8",
            )
            (export / "ResourceBuffers" / "resource_buffers.json").write_text(
                json.dumps(
                    {
                        "buffers": [
                            {
                                "stage": "ps",
                                "binding": "r012",
                                "display_name": "PrimitiveSceneData",
                                "shader_resource_name": "PrimitiveSceneData",
                                "path": "ResourceBuffers/primitive.raw.bin",
                                "descriptor": {"access": {"index": "12", "descriptor_type": "ReadOnlyResource"}},
                                "byte_size": "512",
                                "exported_byte_size": "512",
                            }
                        ]
                    }
                ),
                encoding="utf-8",
            )
            cmd = [sys.executable, str(ROOT / "cpd_summary.py"), "--dc-export", str(export), "--verbose"]
            proc = subprocess.run(cmd, cwd=str(ROOT), text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            report = export / "Analysis" / "CPD" / "custom_primitive_data_summary.md"
            summary = export / "Analysis" / "CPD" / "custom_primitive_data_summary.json"
            self.assertTrue(report.exists())
            self.assertTrue(summary.exists())
            data = json.loads(summary.read_text(encoding="utf-8"))
            self.assertGreaterEqual(data["counts"]["shader_loads"], 2)
            self.assertGreaterEqual(data["counts"]["buffer_windows"], 2)


if __name__ == "__main__":
    unittest.main()

