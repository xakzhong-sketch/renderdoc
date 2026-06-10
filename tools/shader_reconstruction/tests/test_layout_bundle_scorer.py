import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from cpd.bundle_hints import load_bundle_hints, match_bundle_hints
from cpd.finding_scorer import apply_bundle_hint_scores, build_findings
from cpd.resource_candidates import find_resource_candidates
from cpd.schemas import AnalyzerWarning, BufferWindow, ExportContext, ResourceBufferRecord, ResolvedShaderLoad, ShaderLoad
from cpd.ue_layout import load_layout_fields, match_layouts


class LayoutBundleScorerTests(unittest.TestCase):
    def test_layout_and_bundle_hint_do_not_alone_make_high_confidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            layout = root / "UE5_5.json"
            layout.write_text(
                json.dumps(
                    {
                        "version": 1,
                        "engine": "UE5.5",
                        "source": "test",
                        "layouts": {
                            "PrimitiveSceneData": {
                                "fields": [
                                    {"name": "CustomPrimitiveData", "offset": 320, "type": "float4[]", "count": 2}
                                ]
                            }
                        },
                    }
                ),
                encoding="utf-8",
            )
            bundle = root / "bundle"
            (bundle / "parameters").mkdir(parents=True)
            (bundle / "parameters" / "material_parameters.json").write_text(
                json.dumps({"scalar": "_Blend4_CustomPrim_Mask"}), encoding="utf-8"
            )
            warnings = []
            _, fields = load_layout_fields(layout, warnings)
            hints = load_bundle_hints(bundle, warnings)
            self.assertEqual(len(fields), 1)
            self.assertTrue(any(h.name == "_Blend4_CustomPrim_Mask" for h in hints))

            ctx = ExportContext(export_root=root)
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
                    resource_id="res",
                    raw_path="missing.bin",
                    raw_exists=False,
                )
            )
            candidates = find_resource_candidates(ctx)
            load = ShaderLoad(
                id="ps:0:1:1",
                stage="ps",
                disassembly_file="Shaders/ps/disassembly_default.txt",
                line_number=1,
                instruction_text="ld_raw r0, l(320), t12",
                backend="dxbc",
                resource_class="raw_buffer",
                register="t12",
                slot=12,
                register_class="srv",
                byte_offset=320,
                parse_status="parsed_static_offset",
            )
            resolved = [
                ResolvedShaderLoad(
                    load=load,
                    resolution_status="missing_raw_file",
                    resource_kind="resource_buffer",
                    record_id="res:ps:srv:12",
                    record_display_name="PrimitiveSceneData",
                    file_byte_offset=320,
                    record=ctx.resource_buffers[0].to_dict(),
                )
            ]
            windows = [
                BufferWindow(
                    id="win_0000",
                    load_id=load.id,
                    record_id="res:ps:srv:12",
                    raw_path="missing.bin",
                    start_offset=None,
                    requested_size=0,
                    actual_size=0,
                    status="missing_raw_file",
                )
            ]
            hits = match_layouts(resolved, windows, fields)
            findings = build_findings(candidates, resolved, windows, hits)
            match_bundle_hints(hints, findings)
            apply_bundle_hint_scores(findings, hints)
            self.assertTrue(findings)
            self.assertNotEqual(findings[0].confidence_label, "high")


if __name__ == "__main__":
    unittest.main()

