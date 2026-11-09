"""Source-only self-test driver checks: no Tk root, EXE, or registry is used."""
import importlib.util
from pathlib import Path
import unittest


class DepthSelfTestDriverTests(unittest.TestCase):
    def module(self):
        path = Path(__file__).resolve().parents[1] / "controls_self_test.py"
        spec = importlib.util.spec_from_file_location("depth_self_test_driver", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.assertTrue(callable(getattr(module, "exercise_depth_mode_selector", None)),
                        "packaged integration check must drive all depth selector choices")
        return module

    def exercise(self, collateral=False, wrong_type=False):
        module = self.module()
        depth = "FrameGenerationDepthMode"
        values = {"DLSSNR_Enabled": (1, 4), "DLSSNR_Intensity": (83, 4),
                  "FoveationSize": (100, 4), "DLSSNR_ProcessingResolution": (100, 4),
                  "FrameGenerationMode": (4, 4), "FrameGenerationBackend": (0, 4)}
        checks = []
        app = type("App", (), {"frame_generation_depth_mode": object()})()

        class Selector:
            def cget(self, name):
                return {"state": "readonly", "values": ("Standard", "Depth assist", "Depth reconstruction (experimental)")}[name]

            def set(self, label):
                self.label = label

            def event_generate(self, event):
                if event != "<<ComboboxSelected>>":
                    raise AssertionError("driver must dispatch the real selection event")
                value = {"Standard": 0, "Depth assist": 1, "Depth reconstruction (experimental)": 2}[self.label]
                values[depth] = (value, 1 if wrong_type else 4)
                if collateral:
                    values["DLSSNR_Intensity"] = (0, 4)

        selector = Selector()

        def control(kind, option, value):
            self.assertIs(kind, module.ttk.Combobox)
            self.assertEqual(option, "textvariable")
            self.assertIs(value, app.frame_generation_depth_mode)
            return selector

        def snapshot():
            return {module.gui.REGISTRY_PATH: sorted((key, *value) for key, value in values.items()),
                    module.gui.runtime_selection.RUNTIME_REGISTRY_PATH: [("UnrelatedRuntimeValue", 17, 4)]}

        def check(condition, marker):
            if not condition:
                raise AssertionError(marker)
            checks.append(marker)

        root = type("Root", (), {"update": lambda _self: None})()
        module.exercise_depth_mode_selector(app, control, root, values.__getitem__, snapshot, check)
        return values, checks

    def test_cycles_literal_modes_and_records_exact_dwords(self):
        values, checks = self.exercise()
        self.assertEqual(values["FrameGenerationDepthMode"], (2, 4))
        self.assertEqual(values["DLSSNR_Intensity"], (83, 4))
        for value in (0, 1, 2):
            self.assertIn(f"depth-mode: global DWORD {value}", checks)
        self.assertIn("depth-mode: selector readonly", checks)

    def test_rejects_depth_event_that_changes_other_settings(self):
        with self.assertRaisesRegex(AssertionError, "preserves other settings"):
            self.exercise(collateral=True)

    def test_rejects_depth_value_with_wrong_registry_type(self):
        with self.assertRaisesRegex(AssertionError, "global DWORD 0"):
            self.exercise(wrong_type=True)


if __name__ == "__main__":
    unittest.main()
