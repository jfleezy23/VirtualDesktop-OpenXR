"""Real hidden Tk controls with a registry mock; never touch production settings."""

import time
import tkinter as tk
import unittest
from unittest import mock

from test_fg_controls import Registry, gui


SETTING = "DLSSNR_ProcessingResolution"


class SpatialControlsTests(unittest.TestCase):
    def setUp(self):
        self.registry = Registry()
        for module in (gui, gui.runtime_selection):
            patch = mock.patch.object(module, "winreg", self.registry)
            patch.start()
            self.addCleanup(patch.stop)
        self.root = tk.Tk()
        self.root.withdraw()
        self.addCleanup(self.root.destroy)

    def app(self):
        app = gui.VDXRRegistryApp(self.root)
        self.assertTrue(hasattr(app, "processing_resolution"), "Missing independent processing resolution control")
        return app

    def wait(self, milliseconds=450):
        end = time.monotonic() + milliseconds / 1000
        while time.monotonic() < end:
            self.root.update()
            time.sleep(0.01)

    def test_missing_setting_shows_native_100_without_startup_writes(self):
        app = self.app()
        self.assertEqual(app.processing_resolution.get(), 100)
        self.assertEqual(self.registry.writes, [])

    def test_saved_90_and_limits_are_loaded_without_writes(self):
        for value in (50, 90, 100):
            with self.subTest(value=value):
                self.registry.values[SETTING] = (value, 4)
                app = self.app()
                self.assertEqual(app.processing_resolution.get(), value)
        self.assertEqual(self.registry.writes, [])

    def test_malformed_or_out_of_range_settings_show_native_100(self):
        for stored in ((49, 4), (101, 4), (-1, 4), ("90", 1), (90, 1), (b"90", 3), (90.5, 4)):
            with self.subTest(stored=stored):
                self.registry.values[SETTING] = stored
                app = self.app()
                self.assertEqual(app.processing_resolution.get(), 100)
                self.assertIn("invalid", app.status.get().lower())
        self.assertEqual(self.registry.writes, [])

    def test_writer_rejects_invalid_percentages_without_registry_writes(self):
        self.assertTrue(callable(getattr(gui, "write_processing_resolution", None)), "Missing validated processing writer")
        for value in (49, 101, -1, "90", 90.5, True):
            with self.subTest(value=value), self.assertRaises(ValueError):
                gui.write_processing_resolution(value)
        self.assertEqual(self.registry.writes, [])

    def test_one_adjustment_saves_after_400ms_in_64bit_registry_view(self):
        app = self.app()
        app.processing_resolution.set(90)
        self.assertEqual(self.registry.writes, [])
        self.wait(200)
        self.assertEqual(self.registry.writes, [])
        self.wait(250)
        self.assertEqual(self.registry.writes, [(SETTING, 90, 4)])
        self.assertTrue(all(access & 256 for _, access in self.registry.access))
        self.assertNotIn("active", app.status.get().lower())

    def test_rapid_adjustments_save_only_last_value(self):
        app = self.app()
        for value in (85, 75, 90):
            app.processing_resolution.set(value)
        self.wait()
        self.assertEqual(self.registry.writes, [(SETTING, 90, 4)])

    def test_denied_write_restores_actual_saved_value_after_external_change(self):
        self.registry.values[SETTING] = (90, 4)
        app = self.app()
        app.processing_resolution.set(75)
        self.registry.values[SETTING] = (95, 4)
        self.registry.denied = True
        self.wait()
        self.assertEqual(app.processing_resolution.get(), 95)
        self.assertEqual(self.registry.writes, [])
        self.assertIn("administrator", app.status.get().lower())

    def test_failed_readback_restores_actual_value_without_claiming_saved(self):
        self.registry.values[SETTING] = (90, 4)
        app = self.app()
        with mock.patch.object(self.registry, "SetValueEx", return_value=None):
            app.processing_resolution.set(75)
            self.wait()
        self.assertEqual(app.processing_resolution.get(), 90)
        self.assertIn("error", app.status.get().lower())

    def test_refresh_cancels_pending_write_and_loads_actual_value(self):
        app = self.app()
        app.processing_resolution.set(75)
        self.registry.values[SETTING] = (90, 4)
        app.refresh_processing_resolution()
        self.wait()
        self.assertEqual(app.processing_resolution.get(), 90)
        self.assertEqual(self.registry.writes, [])

    def test_reset_saves_native_100_and_cancels_pending_adjustment(self):
        self.registry.values[SETTING] = (90, 4)
        self.registry.values["FrameGenerationMode"] = (3, 4)
        app = self.app()
        app.processing_resolution.set(75)
        app.reset_defaults()
        self.wait()
        self.assertEqual(self.registry.values[SETTING], (100, 4))
        self.assertEqual(app.processing_resolution.get(), 100)
        self.assertEqual([entry for entry in self.registry.writes if entry[0] == SETTING], [(SETTING, 100, 4)])
        self.assertEqual(self.registry.values["FrameGenerationMode"], (3, 4))

    def test_failed_reset_rereads_actual_saved_processing_resolution(self):
        self.registry.values[SETTING] = (90, 4)
        app = self.app()
        app.processing_resolution.set(75)
        self.registry.denied = True
        app.reset_defaults()
        self.wait()
        self.assertEqual(app.processing_resolution.get(), 90)
        self.assertEqual(self.registry.writes, [])

    def test_close_flushes_pending_processing_once_and_cancels_late_callback(self):
        app = self.app()
        app.processing_resolution.set(90)
        with mock.patch.object(self.root, "destroy"):
            app.on_close()
        self.wait()
        self.assertEqual(self.registry.writes, [(SETTING, 90, 4)])

    def test_close_denied_flush_restores_saved_processing_and_stays_open(self):
        self.registry.values[SETTING] = (95, 4)
        app = self.app()
        app.processing_resolution.set(90)
        self.registry.denied = True
        with mock.patch.object(self.root, "destroy") as destroy:
            app.on_close()
        self.wait()
        self.assertEqual(app.processing_resolution.get(), 95)
        self.assertEqual(self.registry.writes, [])
        destroy.assert_not_called()

    def test_failed_close_cancels_both_pending_timers(self):
        app = self.app()
        app.variables[-1].set(94)
        app.processing_resolution.set(90)
        original = self.registry.SetValueEx

        def deny_processing(key, name, reserved, kind, value):
            if name == SETTING:
                raise PermissionError("fixture processing denial")
            original(key, name, reserved, kind, value)

        with mock.patch.object(self.registry, "SetValueEx", side_effect=deny_processing), \
                mock.patch.object(self.root, "destroy"):
            app.on_close()
            writes_at_close = list(self.registry.writes)
            self.wait()
        self.assertEqual(self.registry.writes, writes_at_close)

    def test_denied_close_reconciles_both_pending_sliders_before_retry(self):
        self.registry.values["FoveationSize"] = (100, 4)
        app = self.app()
        app.variables[-1].set(94)
        app.processing_resolution.set(90)
        self.registry.denied = True
        with mock.patch.object(self.root, "destroy") as destroy:
            app.on_close()
            destroy.assert_not_called()
            self.assertEqual(app.variables[-1].get(), 100)
            self.assertEqual(app.processing_resolution.get(), 100)
            self.registry.denied = False
            app.on_close()
            destroy.assert_called_once()
        self.assertEqual(self.registry.writes, [])

    def test_explicit_enable_flushes_pending_processing_before_enabling(self):
        self.registry.values["DLSSNR_Enabled"] = (0, 4)
        app = self.app()
        app.processing_resolution.set(90)
        app.enabled.set(True)
        app.on_enabled_changed()
        self.assertEqual(self.registry.values.get(SETTING), (90, 4))
        self.assertEqual(self.registry.writes[-1], ("DLSSNR_Enabled", 1, 4))
        self.wait()
        self.assertEqual([entry for entry in self.registry.writes if entry[0] == SETTING], [(SETTING, 90, 4)])

    def test_enable_preserves_saved_processing_and_persists_displayed_coverage(self):
        self.registry.values[SETTING] = (90, 4)
        self.registry.values["DLSSNR_Enabled"] = (0, 4)
        self.registry.values.pop("FoveationSize")
        app = self.app()
        app.enabled.set(True)
        app.on_enabled_changed()
        self.assertEqual(self.registry.values[SETTING], (90, 4))
        self.assertEqual(self.registry.values["FoveationSize"], (100, 4))
        self.assertEqual(self.registry.values["DLSSNR_Enabled"], (1, 4))

    def test_processing_save_preserves_coverage_nr_fg_and_runtime(self):
        runtime_path = gui.runtime_selection.RUNTIME_REGISTRY_PATH
        self.registry.keys[runtime_path] = {"unrelated": (19, 4)}
        self.registry.values.update({"FoveationSize": (94, 4), "FrameGenerationMode": (3, 4), "FrameGenerationBackend": (1, 4)})
        before = self.registry.values.copy()
        app = self.app()
        app.processing_resolution.set(90)
        self.wait()
        self.assertEqual({key: value for key, value in self.registry.values.items() if key != SETTING}, before)
        self.assertEqual(self.registry.keys[runtime_path], {"unrelated": (19, 4)})
        self.assertEqual(gui.VALUE_NAMES[-1], "FoveationSize")
        self.assertEqual(app.variables[-1].get(), 94)

    def test_fg_and_runtime_callbacks_preserve_processing_selection(self):
        self.registry.values[SETTING] = (90, 4)
        app = self.app()
        app.frame_generation.set("OFXR 3×")
        app.on_frame_generation_changed()
        app.frame_generation_backend.set("FidelityFX (full resolution)")
        app.on_frame_generation_backend_changed()
        app.runtime_selection.set(gui.runtime_selection.STOCK_LABEL)
        app.on_runtime_changed()
        self.assertEqual(app.processing_resolution.get(), 90)
        self.assertEqual(self.registry.values[SETTING], (90, 4))
        self.assertNotIn(SETTING, [name for name, *_ in self.registry.writes])


if __name__ == "__main__":
    unittest.main()
