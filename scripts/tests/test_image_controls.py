"""Exercise CAS and FSR controls with hidden Tk and an isolated registry."""
import tkinter as tk
import unittest
from unittest import mock

from test_fg_controls import Registry, gui


class ImageControlsTests(unittest.TestCase):
    def setUp(self):
        self.registry = Registry()
        for module in (gui, gui.game_profiles, gui.runtime_selection):
            patch = mock.patch.object(module, "winreg", self.registry)
            patch.start()
            self.addCleanup(patch.stop)
        self.root = tk.Tk()
        self.root.withdraw()
        self.addCleanup(self.root.destroy)

    def app(self, runtime=None):
        with mock.patch.object(gui.runtime_selection, "read_selection",
                               return_value=runtime or gui.runtime_selection.CUSTOM_LABEL):
            app = gui.VDXRRegistryApp(self.root)
        app.stop_live_status()
        return app

    def test_read_only_startup_loads_saved_cas_and_native_fsr(self):
        self.registry.values["sharpen"] = (75, 4)
        app = self.app()
        self.assertEqual(app.image_values["sharpen"].get(), "75")
        self.assertEqual(app.image_values["upscaling"].get(), "100")
        self.assertEqual(self.registry.writes, [])

    def test_only_explicit_apply_writes_each_global_dword(self):
        app = self.app()
        app.image_values["sharpen"].set("75")
        app.image_values["upscaling"].set("90")
        self.assertEqual(self.registry.writes, [])
        app.image_apply_buttons["sharpen"].invoke()
        self.assertEqual(self.registry.writes, [("sharpen", 75, 4)])
        self.assertIn("during play", app.status.get())
        app.image_apply_buttons["upscaling"].invoke()
        self.assertEqual(self.registry.write_paths,
                         [(gui.REGISTRY_PATH, "sharpen", 75, 4), (gui.REGISTRY_PATH, "upscaling", 90, 4)])
        self.assertIn("restart", app.status.get().lower())
        self.assertTrue(all(access & self.registry.KEY_WOW64_64KEY for _, access in self.registry.access))

    def test_writer_rejects_out_of_range_and_non_integer(self):
        for name, value in (("sharpen", -1), ("sharpen", 101), ("sharpen", True),
                            ("upscaling", 0), ("upscaling", 49), ("upscaling", 101), ("upscaling", "90")):
            with self.subTest(name=name, value=value), self.assertRaises(ValueError):
                gui.write_value(name, value)
        self.assertEqual(self.registry.writes, [])

    def test_invalid_or_denied_write_restores_current_saved_setting(self):
        self.registry.values["upscaling"] = (90, 4)
        app = self.app()
        for value in ("0", "", "92.5"):
            with self.subTest(value=value):
                app.image_values["upscaling"].set(value)
                app.image_apply_buttons["upscaling"].invoke()
                self.assertEqual(app.image_values["upscaling"].get(), "90")
                self.assertIn("50", app.status.get())
        app.image_values["upscaling"].set("95")
        self.registry.values["upscaling"] = (85, 4)
        self.registry.denied = True
        app.image_apply_buttons["upscaling"].invoke()
        self.assertEqual(app.image_values["upscaling"].get(), "85")
        self.assertIn("administrator", app.status.get().lower())
        self.assertEqual(self.registry.writes, [])

    def test_wrong_saved_type_is_shown_as_unknown_without_write(self):
        self.registry.values["upscaling"] = ("0", 1)
        app = self.app()
        self.assertEqual(app.image_values["upscaling"].get(), "")
        self.assertIn("Invalid saved FSR render scale", app.status.get())
        self.assertEqual(self.registry.writes, [])

    def test_readback_failure_cannot_report_saved(self):
        self.registry.values["sharpen"] = (75, 4)
        app = self.app()
        app.image_values["sharpen"].set("90")
        with mock.patch.object(self.registry, "SetValueEx", return_value=None):
            app.image_apply_buttons["sharpen"].invoke()
        self.assertIn("readback", app.status.get().lower())
        self.assertEqual(app.image_values["sharpen"].get(), "75")

    def test_profile_selection_and_nr_reset_do_not_write_image_settings(self):
        self.registry.values.update(sharpen=(75, 4), upscaling=(90, 4))
        app = self.app()
        self.assertTrue(app.load_scope(r"F:\TestFolder\TestGame.exe"))
        self.assertTrue(app.image_apply_buttons["sharpen"].instate(["disabled"]))
        app.image_values["sharpen"].set("20")
        app.save_image_setting("sharpen")  # Stale/programmatic event must obey scope.
        self.assertTrue(app.save_profile())
        self.assertTrue(app.load_scope(None))
        self.assertEqual(app.image_values["sharpen"].get(), "75")
        app.reset_defaults()
        self.assertEqual(self.registry.values["sharpen"], (75, 4))
        self.assertEqual(self.registry.values["upscaling"], (90, 4))
        self.assertFalse(any(name in ("sharpen", "upscaling") for name, _, _ in self.registry.writes))

    def test_stock_mode_cannot_edit_image_settings(self):
        app = self.app(gui.runtime_selection.STOCK_LABEL)
        for button in app.image_apply_buttons.values():
            self.assertTrue(button.instate(["disabled"]))
            button.invoke()
        app.image_values["sharpen"].set("75")
        app.save_image_setting("sharpen")
        self.assertEqual(self.registry.writes, [])
