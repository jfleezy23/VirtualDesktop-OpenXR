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
        self.addCleanup(self.cleanup_root)

    def cleanup_root(self):
        for callback in self.root.tk.call("after", "info"):
            self.root.after_cancel(callback)
        self.root.destroy()

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
        app.processing_resolution.set(100)
        self.assertIsNotNone(app.foveation_save_id)
        self.assertIsNotNone(app.processing_resolution_save_id)
        original = self.registry.SetValueEx

        def deny_processing(key, name, reserved, kind, value):
            if name == SETTING:
                raise PermissionError("fixture processing denial")
            original(key, name, reserved, kind, value)

        with mock.patch.object(self.registry, "SetValueEx", side_effect=deny_processing), \
                mock.patch.object(self.root, "destroy") as destroy:
            app.on_close()
            destroy.assert_not_called()
            self.assertIsNone(app.foveation_save_id)
            self.assertIsNone(app.processing_resolution_save_id)
            writes_at_close = list(self.registry.writes)
            self.wait()
        self.assertEqual(self.registry.writes, writes_at_close)
        self.assertIn("administrator", app.status.get().lower())

    def test_denied_close_reconciles_both_pending_sliders_before_retry(self):
        self.registry.values["FoveationSize"] = (100, 4)
        app = self.app()
        app.variables[-1].set(94)
        app.processing_resolution.set(100)
        self.assertIsNotNone(app.foveation_save_id)
        self.assertIsNotNone(app.processing_resolution_save_id)
        self.registry.denied = True
        with mock.patch.object(self.root, "destroy") as destroy:
            app.on_close()
            destroy.assert_not_called()
            self.assertEqual(app.variables[-1].get(), 100)
            self.assertEqual(app.processing_resolution.get(), 100)
            self.assertIsNone(app.foveation_save_id)
            self.assertIsNone(app.processing_resolution_save_id)
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

    def test_saved_mixed_pairs_are_read_only_and_both_sliders_remain_editable(self):
        for coverage, processing in ((95, 80), (90, 85), (90, 80), (100, 80), (95, 100)):
            with self.subTest(coverage=coverage, processing=processing):
                self.registry.values.update({"FoveationSize": (coverage, 4), SETTING: (processing, 4)})
                before = self.registry.values.copy()
                self.registry.writes.clear()
                app = self.app()
                self.assertEqual(self.registry.values, before)
                self.assertEqual(self.registry.writes, [])
                self.assertEqual((app.variables[-1].get(), app.processing_resolution.get()), (coverage, processing))
                self.assertEqual(str(app.slider_widgets[-1].cget("state")), "normal")
                self.assertEqual(str(app.processing_resolution_slider.cget("state")), "normal")

    def test_processing_save_preserves_coverage_appearance_nr_fg_and_runtime(self):
        runtime_path = gui.runtime_selection.RUNTIME_REGISTRY_PATH
        self.registry.keys[runtime_path] = {"unrelated": (19, 4)}
        self.registry.values.update({"FoveationSize": (95, 4), "FrameGenerationMode": (3, 4),
                                     "FrameGenerationBackend": (0, 4), "DLSSNR_Style": (2, 4),
                                     "DLSSNR_Intensity": (61, 4), "DLSSNR_LocalToneStrength": (23, 4)})
        before = self.registry.values.copy()
        app = self.app()
        app.processing_resolution.set(80)
        self.wait()
        self.assertEqual({key: value for key, value in self.registry.values.items() if key != SETTING}, before)
        self.assertEqual(self.registry.keys[runtime_path], {"unrelated": (19, 4)})
        self.assertEqual(app.variables[-1].get(), 95)
        self.assertEqual(app.frame_generation.get(), "OFXR 3×")
        self.assertEqual(app.frame_generation_backend.get(), "NVIDIA (default)")

    def test_both_pending_callbacks_save_mixed_pair_independently(self):
        for processing_first in (False, True):
            with self.subTest(processing_first=processing_first):
                self.registry.values.update({"FoveationSize": (100, 4), SETTING: (100, 4)})
                self.registry.writes.clear()
                app = self.app()
                adjustments = ((app.processing_resolution, 80), (app.variables[-1], 95))
                for variable, value in adjustments if processing_first else reversed(adjustments):
                    variable.set(value)
                self.assertIsNotNone(app.foveation_save_id)
                self.assertIsNotNone(app.processing_resolution_save_id)
                self.assertEqual((app.variables[-1].get(), app.processing_resolution.get()), (95, 80))
                self.assertEqual(self.registry.writes, [])
                self.wait(200)
                self.assertEqual(self.registry.writes, [])
                self.wait(250)
                self.assertCountEqual(self.registry.writes, [("FoveationSize", 95, 4), (SETTING, 80, 4)])

    def test_rapid_changes_debounce_each_slider_to_90_85(self):
        app = self.app()
        for coverage, processing in ((95, 80), (92, 75), (90, 85)):
            app.variables[-1].set(coverage)
            app.processing_resolution.set(processing)
        self.wait()
        self.assertCountEqual(self.registry.writes, [("FoveationSize", 90, 4), (SETTING, 85, 4)])
        self.assertIsNone(app.foveation_save_id)
        self.assertIsNone(app.processing_resolution_save_id)

    def test_close_flushes_90_85_once_without_late_callbacks(self):
        app = self.app()
        app.variables[-1].set(90)
        app.processing_resolution.set(85)
        with mock.patch.object(self.root, "destroy") as destroy:
            app.on_close()
            destroy.assert_called_once()
        self.wait()
        self.assertCountEqual(self.registry.writes, [("FoveationSize", 90, 4), (SETTING, 85, 4)])

    def test_mixed_pair_can_edit_coverage_without_processing_write(self):
        self.registry.values.update({"FoveationSize": (95, 4), SETTING: (80, 4)})
        app = self.app()
        app.variables[-1].set(90)
        self.wait()
        self.assertEqual(self.registry.writes, [("FoveationSize", 90, 4)])
        self.assertEqual(self.registry.values[SETTING], (80, 4))
        self.assertEqual(app.processing_resolution.get(), 80)

    def test_direct_coverage_writes_preserve_reduced_processing(self):
        self.registry.values[SETTING] = (80, 4)
        app = self.app()
        app.variables[-1].set(95)
        self.assertTrue(app.save_foveation_size(app.variables[-1]))
        self.assertTrue(app.save_nr_value(len(gui.VALUE_NAMES) - 1, tk.IntVar(value=90)))
        self.wait()
        self.assertEqual(self.registry.writes, [("FoveationSize", 95, 4), ("FoveationSize", 90, 4)])
        self.assertEqual(self.registry.values[SETTING], (80, 4))

    def test_missing_and_invalid_coverage_startup_remains_read_only(self):
        for stored in (None, ("100", 1), (101, 4)):
            with self.subTest(stored=stored):
                self.registry.values[SETTING] = (80, 4)
                if stored is None:
                    self.registry.values.pop("FoveationSize", None)
                else:
                    self.registry.values["FoveationSize"] = stored
                before = self.registry.values.copy()
                self.registry.writes.clear()
                app = self.app()
                self.assertEqual(self.registry.values, before)
                self.assertEqual(self.registry.writes, [])
                self.assertEqual((app.variables[-1].get(), app.processing_resolution.get()), (100, 80))
                self.assertIn("not saved" if stored is None else "invalid", app.status.get().lower())
                self.assertEqual(str(app.slider_widgets[-1].cget("state")), "normal")

    def test_processing_save_does_not_persist_missing_coverage(self):
        self.registry.values.pop("FoveationSize")
        app = self.app()
        app.processing_resolution.set(80)
        self.wait()
        self.assertNotIn("FoveationSize", self.registry.values)
        self.assertEqual(self.registry.writes, [(SETTING, 80, 4)])

    def test_enable_requires_verified_missing_coverage_before_nr_on(self):
        self.registry.values.pop("FoveationSize")
        self.registry.values.update({SETTING: (80, 4), "DLSSNR_Enabled": (0, 4)})
        app = self.app()
        original = self.registry.SetValueEx

        def write(key, name, reserved, kind, value):
            if name != "FoveationSize":
                original(key, name, reserved, kind, value)

        with mock.patch.object(self.registry, "SetValueEx", side_effect=write):
            app.enabled.set(True)
            app.on_enabled_changed()
        self.assertFalse(app.enabled.get())
        self.assertEqual(self.registry.values["DLSSNR_Enabled"], (0, 4))
        self.assertNotIn("FoveationSize", self.registry.values)
        self.assertIn("error", app.status.get().lower())
        self.assertIn("not saved", app.status.get().lower())

    def test_enable_flushes_mixed_pair_before_nr_on(self):
        self.registry.values["DLSSNR_Enabled"] = (0, 4)
        app = self.app()
        app.variables[-1].set(95)
        app.processing_resolution.set(80)
        app.enabled.set(True)
        app.on_enabled_changed()
        self.assertEqual(self.registry.values["FoveationSize"], (95, 4))
        self.assertEqual(self.registry.values[SETTING], (80, 4))
        self.assertEqual(self.registry.writes[-1], ("DLSSNR_Enabled", 1, 4))
        self.wait()
        self.assertEqual([entry for entry in self.registry.writes if entry[0] in ("FoveationSize", SETTING)],
                         [("FoveationSize", 95, 4), (SETTING, 80, 4)])

    def test_returning_to_native_processing_preserves_partial_coverage(self):
        self.registry.values.update({"FoveationSize": (95, 4), SETTING: (80, 4)})
        app = self.app()
        app.processing_resolution.set(100)
        self.assertEqual(str(app.slider_widgets[-1].cget("state")), "normal")
        app.variables[-1].set(90)
        self.wait()
        self.assertEqual(self.registry.values[SETTING], (100, 4))
        self.assertEqual(self.registry.values["FoveationSize"], (90, 4))

    def test_processing_failure_reconciles_external_mixed_pair_without_coverage_write(self):
        self.registry.values.update({SETTING: (100, 4), "FoveationSize": (95, 4)})
        app = self.app()
        original = self.registry.SetValueEx

        def write(key, name, reserved, kind, value):
            if name == SETTING:
                self.registry.values.update({SETTING: (85, 4), "FoveationSize": (90, 4)})
                raise PermissionError("fixture processing denial")
            original(key, name, reserved, kind, value)

        with mock.patch.object(self.registry, "SetValueEx", side_effect=write):
            app.processing_resolution.set(80)
            self.wait()
        self.assertEqual((app.variables[-1].get(), app.processing_resolution.get()), (90, 85))
        self.assertEqual(self.registry.writes, [])
        self.assertIn("administrator", app.status.get().lower())
        self.assertEqual(str(app.slider_widgets[-1].cget("state")), "normal")

    def test_coverage_failure_reconciles_external_mixed_pair_and_cancels_pending_processing(self):
        self.registry.values.update({SETTING: (80, 4), "FoveationSize": (95, 4)})
        app = self.app()
        app.variables[-1].set(92)
        app.processing_resolution.set(75)
        self.registry.values.update({SETTING: (85, 4), "FoveationSize": (90, 4)})
        self.registry.denied = True
        self.assertFalse(app.save_foveation_size(app.variables[-1]))
        self.wait()
        self.assertEqual((app.variables[-1].get(), app.processing_resolution.get()), (90, 85))
        self.assertEqual(self.registry.writes, [])
        self.assertIsNone(app.processing_resolution_save_id)
        self.assertIn("administrator", app.status.get().lower())

    def test_failed_close_reconciles_mixed_pair_and_allows_retry(self):
        self.registry.values.update({SETTING: (80, 4), "FoveationSize": (95, 4)})
        app = self.app()
        app.variables[-1].set(90)
        app.processing_resolution.set(85)
        self.registry.denied = True
        with mock.patch.object(self.root, "destroy") as destroy:
            app.on_close()
            destroy.assert_not_called()
            self.assertEqual((app.variables[-1].get(), app.processing_resolution.get()), (95, 80))
            self.assertIsNone(app.foveation_save_id)
            self.assertIsNone(app.processing_resolution_save_id)
            self.wait()
            self.assertEqual(self.registry.writes, [])
            self.registry.denied = False
            app.variables[-1].set(90)
            app.processing_resolution.set(85)
            app.on_close()
            destroy.assert_called_once()
        self.assertCountEqual(self.registry.writes, [("FoveationSize", 90, 4), (SETTING, 85, 4)])

    def test_failed_processing_close_never_flushes_reconciled_coverage(self):
        original_read = self.registry.QueryValueEx
        original_write = self.registry.SetValueEx

        def write(key, name, reserved, kind, value):
            if name == SETTING:
                raise PermissionError("fixture processing write denial")
            original_write(key, name, reserved, kind, value)

        def read(key, name):
            if name == "FoveationSize":
                raise OSError("fixture coverage readback denial")
            return original_read(key, name)

        for coverage_readable in (False, True):
            with self.subTest(coverage_readable=coverage_readable):
                self.registry.values.update({SETTING: (80, 4), "FoveationSize": (95, 4)})
                self.registry.writes.clear()
                app = self.app()
                app.variables[-1].set(90)
                app.processing_resolution.set(85)
                self.assertIsNotNone(app.foveation_save_id)
                self.assertIsNotNone(app.processing_resolution_save_id)
                with mock.patch.object(self.root, "destroy") as destroy:
                    with mock.patch.object(self.registry, "SetValueEx", side_effect=write), \
                            mock.patch.object(self.registry, "QueryValueEx",
                                              side_effect=original_read if coverage_readable else read):
                        app.on_close()
                        destroy.assert_not_called()
                        self.assertEqual(self.registry.values["FoveationSize"], (95, 4))
                        self.assertEqual(self.registry.writes, [])
                        self.assertEqual(app.variables[-1].get(), 95 if coverage_readable else 100)
                        self.assertEqual(app.processing_resolution.get(), 80)
                        self.assertIn("administrator", app.status.get().lower())
                        if not coverage_readable:
                            self.assertIn("cannot read foveationsize", app.status.get().lower())
                        self.assertIsNone(app.foveation_save_id)
                        self.assertIsNone(app.processing_resolution_save_id)
                        self.wait()
                        self.assertEqual(self.registry.writes, [])
                    app.variables[-1].set(90)
                    app.processing_resolution.set(85)
                    app.on_close()
                    destroy.assert_called_once()
                self.wait()
                self.assertCountEqual(self.registry.writes, [("FoveationSize", 90, 4), (SETTING, 85, 4)])

    def test_readback_error_exposes_unverified_defaults_during_reconciliation(self):
        app = self.app()
        original = self.registry.QueryValueEx

        def read(key, name):
            if name in ("FoveationSize", SETTING):
                raise OSError("fixture readback denial")
            return original(key, name)

        with mock.patch.object(self.registry, "QueryValueEx", side_effect=read):
            app.processing_resolution.set(80)
            self.wait()
        self.assertEqual(self.registry.values.get(SETTING), (80, 4))
        self.assertEqual((app.variables[-1].get(), app.processing_resolution.get()), (100, 100))
        self.assertIn("cannot read foveationsize", app.status.get().lower())
        self.assertIn("cannot read " + SETTING.lower(), app.status.get().lower())
        self.assertNotIn("saved nr processing", app.status.get().lower())

    def test_unknown_processing_warns_but_allows_independent_coverage_save(self):
        self.registry.values.update({SETTING: (80, 4), "FoveationSize": (95, 4)})
        original = self.registry.QueryValueEx

        def read(key, name):
            if name == SETTING:
                raise OSError("fixture processing read denial")
            return original(key, name)

        with mock.patch.object(self.registry, "QueryValueEx", side_effect=read):
            app = self.app()
            self.assertEqual(self.registry.writes, [])
            self.assertIn("cannot read", app.status.get().lower())
            self.assertEqual(app.processing_resolution.get(), 100)
            self.assertEqual(str(app.slider_widgets[-1].cget("state")), "normal")
            app.variables[-1].set(90)
            self.wait()
        self.assertEqual(self.registry.writes, [("FoveationSize", 90, 4)])
        self.assertEqual(self.registry.values[SETTING], (80, 4))
        self.assertIn("unverified", app.status.get().lower())
        app.refresh_processing_resolution()
        self.assertEqual(app.processing_resolution.get(), 80)

    def test_failed_refresh_or_reset_keeps_read_warning_without_locking_coverage(self):
        original_read = self.registry.QueryValueEx
        original_write = self.registry.SetValueEx

        def read(key, name):
            if name == SETTING:
                raise OSError("fixture processing read denial")
            return original_read(key, name)

        def write(key, name, reserved, kind, value):
            if name == SETTING:
                raise PermissionError("fixture processing write denial")
            original_write(key, name, reserved, kind, value)

        for operation in ("refresh", "reset"):
            with self.subTest(operation=operation):
                self.registry.values.update({SETTING: (80, 4), "FoveationSize": (95, 4)})
                app = self.app()
                with mock.patch.object(self.registry, "QueryValueEx", side_effect=read), \
                        mock.patch.object(self.registry, "SetValueEx", side_effect=write):
                    if operation == "reset":
                        app.reset_defaults()
                        app.enabled.set(True)
                        app.set_slider_state()
                    else:
                        app.refresh_processing_resolution()
                    self.assertEqual(app.processing_resolution.get(), 100)
                    self.assertIn("cannot read", app.status.get().lower())
                    self.assertEqual(str(app.slider_widgets[-1].cget("state")), "normal")
                    app.variables[-1].set(90)
                    self.wait()
                    self.assertEqual(self.registry.values["FoveationSize"], (90, 4))
                    self.assertIn("unverified", app.status.get().lower())

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
