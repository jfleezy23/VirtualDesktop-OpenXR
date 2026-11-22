"""NVIDIA flow preferences are isolated to each game profile."""
from test_depth_controls import DepthFixture


PRESET = "FrameGenerationNvidiaFlowPreset"


class NvidiaFlowPresetTests(DepthFixture):
    executable = r"C:\ProfileFixtures\Nvidia\fixture.exe"
    other = r"C:\ProfileFixtures\Other\fixture.exe"

    def test_profile_draft_round_trips_without_changing_global_settings(self):
        app = self.app()
        store = app.profile_store
        self.registry.values[PRESET] = (2, self.registry.REG_DWORD)
        globals_before = self.registry.values.copy()
        self.assertEqual(store.load()[0][PRESET], 0)
        self.assertEqual(store.load(self.executable)[0][PRESET], 0)
        with self.assertRaises(ValueError):
            store.save(None, {PRESET: 1})
        self.assertEqual(str(app.nvidia_flow_preset_selector.cget("state")), "disabled")
        self.assertEqual(app.nvidia_flow_preset.get(), "Medium")
        app.load_scope(self.executable)
        self.assertEqual(str(app.nvidia_flow_preset_selector.cget("state")), "readonly")
        self.assertEqual(tuple(app.nvidia_flow_preset_selector.cget("values")), ("Fast", "Medium", "Slow"))
        for label, value in (("Fast", 1), ("Slow", 2), ("Medium", 0)):
            before = store.load(self.executable)[0][PRESET]
            app.nvidia_flow_preset_selector.set(label)
            app.nvidia_flow_preset_selector.event_generate("<<ComboboxSelected>>")
            self.assertTrue(app.profile_dirty)
            self.assertEqual(store.load(self.executable)[0][PRESET], before)
            self.assertTrue(app.save_profile())
            self.assertEqual(store.load(self.executable)[0][PRESET], value)
            self.assertEqual(self.registry.values, globals_before)
            self.assertEqual(store.load(self.other)[0][PRESET], 0)
            self.assertEqual(app.nvidia_flow_preset.get(), label)

    def test_fidelityfx_and_stock_disable_selector_without_rewriting_preset(self):
        app = self.app()
        store = app.profile_store
        store.save(self.executable, {PRESET: 2, "FrameGenerationBackend": 1})
        app.load_scope(self.executable)
        self.assertEqual(str(app.nvidia_flow_preset_selector.cget("state")), "disabled")
        self.assertEqual(app.nvidia_flow_preset.get(), "Slow")
        app.frame_generation_backend.set("NVIDIA (default)")
        app.on_frame_generation_backend_changed()
        self.assertEqual(str(app.nvidia_flow_preset_selector.cget("state")), "readonly")
        app.saved_runtime_selection = "Stock VDXR"
        app.update_runtime_controls()
        self.assertEqual(str(app.nvidia_flow_preset_selector.cget("state")), "disabled")
        self.assertEqual(store.load(self.executable)[0][PRESET], 2)

    def test_invalid_saved_preset_falls_back_without_disabling_other_settings(self):
        app = self.app()
        store = app.profile_store
        store.save(self.executable, {"FrameGenerationMode": 3, "DLSSNR_Intensity": 87})
        self.registry.keys[store.path(self.executable)][PRESET] = ("broken", self.registry.REG_SZ)
        values, sources = store.load(self.executable)
        self.assertEqual((values[PRESET], sources[PRESET]), (0, "invalid"))
        self.assertEqual(values["FrameGenerationMode"], 3)
        self.assertEqual(values["DLSSNR_Intensity"], 87)
        app.load_scope(self.executable)
        self.assertTrue(app.profile_valid)
        self.assertEqual(app.nvidia_flow_preset.get(), "Medium")
        self.assertIn(PRESET, app.status.get())
        self.assertEqual(store._read(store.path(self.executable), PRESET), ("broken", self.registry.REG_SZ))
        app.reset_defaults()
        self.assertEqual(store.load(self.executable)[1][PRESET], "default")
