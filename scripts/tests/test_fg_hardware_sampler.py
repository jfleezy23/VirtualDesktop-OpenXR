"""Compile the prepared production helpers, catching a return to four-load taps."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
FXC = Path("C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/fxc.exe")


class HardwareSamplerTests(unittest.TestCase):
    def test_every_registered_synthesis_entry_compiles(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            subprocess.run([
                "python", str(ROOT / "scripts/prepare-fg-vendor.py"),
                "--source", str(ROOT / "nr-frame-generation/vendor/ofxr"),
                "--output", str(output),
            ], check=True, capture_output=True, text=True)
            cmake = (ROOT / "nr-frame-generation/CMakeLists.txt").read_text(encoding="utf-8")
            recipes = re.findall(r'"([a-z0-9_]+)\|([cpv]s_5_1)\|([A-Za-z0-9_]+)"', cmake)
            self.assertGreaterEqual(len(recipes), 11, "compile the entire registered synthesis family")
            for name, profile, entry in recipes:
                with self.subTest(entry=name):
                    defines = ["/D", "XRFG_NVIDIA_LUMA_INPUT=1"] if name == "nvidia_pack_luma_input" else []
                    subprocess.run([str(FXC), "/nologo", "/T", profile, "/E", entry,
                                    *defines, "/O3", "/WX", "/Ges", "/Fo", str(output / (name + ".cso")),
                                    str(output / "frame_synthesis.hlsl")],
                                   check=True, capture_output=True, text=True)

    def test_compiled_source_helpers_use_one_filtered_tap_each(self):
        # This catches accidental restoration of manual taps, or a missing sampler
        # declaration. Inspect compiled instructions, rather than source spelling.
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            subprocess.run([
                "python", str(ROOT / "scripts/prepare-fg-vendor.py"),
                "--source", str(ROOT / "nr-frame-generation/vendor/ofxr"),
                "--output", str(output),
            ], check=True, capture_output=True, text=True)
            shader = output / "sampler-probe.hlsl"
            shader.write_text((output / "frame_synthesis.hlsl").read_text(encoding="utf-8") + """
float4 SamplerProbe(float4 position : SV_Position) : SV_Target {
    return bilinear_previous_source(position.xy * 0.7381, Slice) +
           bilinear_current_source(position.xy * 0.3927, Slice);
}
""", encoding="utf-8")
            assembly = output / "sampler-probe.asm"
            subprocess.run([str(FXC), "/nologo", "/T", "ps_5_1", "/E", "SamplerProbe",
                            "/O3", "/WX", "/Ges", "/Fc", str(assembly),
                            "/Fo", str(output / "probe.cso"), str(shader)],
                           check=True, capture_output=True, text=True)
            instructions = assembly.read_text(encoding="utf-8")
            samples = [line for line in instructions.splitlines()
                       if line.lstrip().startswith("sample_l")]
            loads = [line for line in instructions.splitlines()
                     if line.lstrip().startswith("ld_")]
            self.assertEqual(len(samples), 2, "each source helper must compile to one filtered tap")
            self.assertEqual(loads, [], "manual source taps must be eliminated")


if __name__ == "__main__":
    unittest.main()
