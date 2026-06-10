import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from cpd.export_index import load_export_context


class ExportIndexTests(unittest.TestCase):
    def test_loads_constant_and_resource_buffers(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "ConstantBuffers").mkdir()
            (root / "ConstantBuffers" / "ps").mkdir()
            (root / "ResourceBuffers").mkdir()
            (root / "Shaders" / "ps").mkdir(parents=True)
            (root / "manifest.json").write_text("{}", encoding="utf-8")
            (root / "drawcall.json").write_text(json.dumps({"event_id": 7, "api": "D3D11"}), encoding="utf-8")
            (root / "pipeline_state.json").write_text("{}", encoding="utf-8")
            (root / "shader_reconstruction_index.json").write_text("{}", encoding="utf-8")
            (root / "ConstantBuffers" / "ps" / "cb.raw.bin").write_bytes(b"\x00" * 64)
            (root / "ResourceBuffers" / "buf.raw.bin").write_bytes(b"\x00" * 64)
            (root / "Shaders" / "ps" / "disassembly_default.txt").write_text("dcl_constantbuffer cb0[4]\n", encoding="utf-8")
            (root / "ConstantBuffers" / "constant_buffers.json").write_text(
                json.dumps(
                    {
                        "buffers": [
                            {
                                "stage": "ps",
                                "binding": "b000",
                                "slot": 0,
                                "display_name": "PrimitiveUniformBuffer",
                                "raw_path": "ConstantBuffers/ps/cb.raw.bin",
                                "byte_size": "64",
                            }
                        ]
                    }
                ),
                encoding="utf-8",
            )
            (root / "ResourceBuffers" / "resource_buffers.json").write_text(
                json.dumps(
                    {
                        "buffers": [
                            {
                                "stage": "ps",
                                "binding": "r012",
                                "display_name": "PrimitiveSceneData",
                                "shader_resource_name": "PrimitiveSceneData",
                                "path": "ResourceBuffers/buf.raw.bin",
                                "descriptor": {"access": {"index": "12", "descriptor_type": "ReadOnlyResource"}},
                                "byte_size": "64",
                            }
                        ]
                    }
                ),
                encoding="utf-8",
            )
            ctx = load_export_context(root)
            self.assertEqual(ctx.capture["event_id"], 7)
            self.assertEqual(len(ctx.constant_buffers), 1)
            self.assertTrue(ctx.constant_buffers[0].raw_exists)
            self.assertEqual(len(ctx.resource_buffers), 1)
            self.assertEqual(ctx.resource_buffers[0].slot, 12)
            self.assertEqual(ctx.resource_buffers[0].register_class, "srv")
            self.assertEqual(len(ctx.disassembly_files), 1)


if __name__ == "__main__":
    unittest.main()

