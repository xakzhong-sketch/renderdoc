import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from cpd.resolver import resolve_shader_loads
from cpd.schemas import ConstantBufferRecord, ExportContext, ResourceBufferRecord, ShaderLoad


class ResolverTests(unittest.TestCase):
    def test_resolves_cbuffer_and_srv(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "cb.bin").write_bytes(b"\x00" * 128)
            (root / "buf.bin").write_bytes(b"\x00" * 128)
            ctx = ExportContext(export_root=root)
            ctx.constant_buffers.append(
                ConstantBufferRecord(
                    id="cb:ps:0",
                    stage="ps",
                    slot=0,
                    binding="b000",
                    display_name="PrimitiveUniformBuffer",
                    block_name="PrimitiveUniformBuffer",
                    renderdoc_buffer_name="",
                    resource_id="res1",
                    raw_path="cb.bin",
                    raw_exists=True,
                )
            )
            ctx.resource_buffers.append(
                ResourceBufferRecord(
                    id="res:ps:srv:12",
                    stage="ps",
                    slot=12,
                    register_class="srv",
                    binding="r012",
                    display_name="PrimitiveSceneData",
                    shader_resource_name="PrimitiveSceneData",
                    renderdoc_resource_name="PrimitiveSceneData",
                    resource_id="res2",
                    raw_path="buf.bin",
                    raw_exists=True,
                )
            )
            loads = [
                ShaderLoad(
                    id="ps:0:1:1",
                    stage="ps",
                    disassembly_file="Shaders/ps/disassembly_default.txt",
                    line_number=1,
                    instruction_text="mov r0, cb0[2]",
                    backend="dxbc",
                    resource_class="constant_buffer",
                    register="cb0",
                    slot=0,
                    register_class="cb",
                    byte_offset=32,
                ),
                ShaderLoad(
                    id="ps:0:2:1",
                    stage="ps",
                    disassembly_file="Shaders/ps/disassembly_default.txt",
                    line_number=2,
                    instruction_text="ld_raw r0, l(16), t12",
                    backend="dxbc",
                    resource_class="raw_buffer",
                    register="t12",
                    slot=12,
                    register_class="srv",
                    byte_offset=16,
                ),
            ]
            resolved = resolve_shader_loads(ctx, loads)
            self.assertEqual(resolved[0].resolution_status, "resolved")
            self.assertEqual(resolved[0].record_id, "cb:ps:0")
            self.assertEqual(resolved[1].resolution_status, "resolved")
            self.assertEqual(resolved[1].record_id, "res:ps:srv:12")


if __name__ == "__main__":
    unittest.main()

