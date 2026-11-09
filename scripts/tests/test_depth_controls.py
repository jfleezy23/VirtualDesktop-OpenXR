"""Depth requests stay independent of FG multiplier, backend and NR settings."""
import tkinter as tk
from pathlib import Path
import re
import unittest
from unittest import mock

from test_fg_controls import Registry, gui


DEPTH = "FrameGenerationDepthMode"


class DepthContractTests(unittest.TestCase):
    def test_absent_controls_request_matches_the_host_profile_schema(self):
        header = Path(__file__).resolve().parents[2] / "virtualdesktop-openxr/game_profiles.h"
        rule = re.search(
            r'\{"FrameGenerationDepthMode",\s*L"FrameGenerationDepthMode",\s*(\d+),\s*(\d+),\s*(\d+)\}',
            header.read_text(encoding="utf-8-sig"),
        )
        self.assertIsNotNone(rule, "Host depth request rule is missing from the shared profile schema")
        fallback, minimum, maximum = map(int, rule.groups())
        registry = Registry()
        with mock.patch.object(gui, "winreg", registry):
            self.assertEqual(gui.read_frame_generation_depth_mode(), fallback,
                             "Absent Controls request disagrees with the runtime's actual schema fallback")
            self.assertEqual(gui.game_profiles.DEFAULTS[DEPTH], fallback)
            for value in range(minimum, maximum + 1):
                registry.values[DEPTH] = (value, registry.REG_DWORD)
                self.assertEqual(gui.read_frame_generation_depth_mode(), value)
        self.assertEqual(registry.writes, [], "Reading the shared defaults must not repair saved settings")


class DepthFixture(unittest.TestCase):
    def setUp(self):
        self.registry = Registry()
        for module in (gui, gui.runtime_selection, gui.game_profiles):
            patch = mock.patch.object(module, "winreg", self.registry)
            patch.start()
            self.addCleanup(patch.stop)
        self.root = tk.Tk()
        self.root.withdraw()
        self.addCleanup(self.root.destroy)

    def app(self, store=None):
        app = gui.VDXRRegistryApp(self.root, profile_store=store)
        self.assertTrue(hasattr(app, "frame_generation_depth_mode"), "Missing requested reconstruction mode")
        return app


class DepthControlsTests(DepthFixture):
    def test_absent_setting_requests_depth_assist_without_startup_writes(self):
        app = self.app()
        self.assertEqual(app.frame_generation_depth_mode.get(), "Depth assist")
        self.assertEqual(self.registry.writes, [])

    def test_three_requests_write_only_the_depth_dword_in_the_64bit_view(self):
        self.registry.values.update({"FrameGenerationMode": (3, 4), "FrameGenerationBackend": (1, 4),
                                     "DLSSNR_Style": (2, 4), "DLSSNR_Intensity": (89, 4)})
        original = self.registry.values.copy()
        app = self.app()
        for label, value in (("Standard", 0), ("Depth assist", 1), ("Depth reconstruction (experimental)", 2)):
            app.frame_generation_depth_mode.set(label)
            app.depth_mode_selector.event_generate("<<ComboboxSelected>>")
            self.assertEqual(self.registry.values[DEPTH], (value, 4))
            self.assertIn("next game launch", app.status.get().lower())
            self.assertNotIn("active", app.status.get().lower())
        self.assertEqual({key: self.registry.values[key] for key in original}, original)
        self.assertEqual([name for name, _, _ in self.registry.writes], [DEPTH] * 3)
        self.assertTrue(all(access & 256 for _, access in self.registry.access))

    def test_invalid_types_and_values_are_rejected_before_writing(self):
        self.assertTrue(callable(getattr(gui, "read_frame_generation_depth_mode", None)), "Missing depth reader")
        self.assertTrue(callable(getattr(gui, "write_frame_generation_depth_mode", None)), "Missing depth writer")
        for value in (-1, 3, 99, True, False, 1.0, "2"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                gui.write_frame_generation_depth_mode(value)
        for stored in ((-1, 4), (3, 4), (True, 4), (1.0, 4), ("2", 1), (2, 1)):
            self.registry.values[DEPTH] = stored
            with self.subTest(stored=stored), self.assertRaises(ValueError):
                gui.read_frame_generation_depth_mode()
        self.assertEqual(self.registry.writes, [])

    def test_bad_global_depth_preserves_valid_nr_and_other_fg_selections(self):
        self.registry.values.update({DEPTH: (99, 4), "FrameGenerationMode": (4, 4),
                                     "FrameGenerationBackend": (1, 4), "DLSSNR_Style": (2, 4),
                                     "DLSSNR_Intensity": (89, 4), "DLSSNR_LocalToneStrength": (33, 4)})
        app = self.app()
        for scope in (None, r"C:\ProfileFixtures\One\fixture.exe", None):
            app.load_scope(scope)
            self.assertTrue(app.enabled.get())
            self.assertEqual([v.get() for v in app.variables[:3]], [2, 89, 33])
            self.assertEqual(app.frame_generation.get(), "OFXR 4×")
            self.assertEqual(app.frame_generation_backend.get(), "FidelityFX (full resolution)")
            self.assertEqual(app.frame_generation_depth_mode.get(), "Depth assist")
            self.assertIn("invalid", app.status.get().lower())
            self.assertIn(DEPTH, app.status.get())
        self.assertEqual(self.registry.writes, [])

    def test_permission_failure_restores_the_saved_depth_request(self):
        self.registry.values[DEPTH] = (0, 4)
        app = self.app()
        self.registry.denied = True
        app.frame_generation_depth_mode.set("Depth reconstruction (experimental)")
        app.on_frame_generation_depth_mode_changed()
        self.assertEqual(app.frame_generation_depth_mode.get(), "Standard")
        self.assertEqual(self.registry.values[DEPTH], (0, 4))
        self.assertIn("administrator", app.status.get().lower())

    def test_readback_mismatch_does_not_claim_the_depth_request_was_saved(self):
        app = self.app()
        with mock.patch.object(self.registry, "SetValueEx", return_value=None):
            app.frame_generation_depth_mode.set("Standard")
            app.on_frame_generation_depth_mode_changed()
        self.assertEqual(app.frame_generation_depth_mode.get(), "Depth assist")
        self.assertIn("error", app.status.get().lower())

    def test_unsupported_backend_keeps_reconstruction_requested_and_explains_fallback(self):
        self.registry.values.update({DEPTH: (2, 4), "FrameGenerationBackend": (1, 4)})
        app = self.app()
        self.assertEqual(app.frame_generation_depth_mode.get(), "Depth reconstruction (experimental)")
        self.assertIn("unsupported", app.depth_mode_feedback.get().lower())
        self.assertIn("standard fg", app.depth_mode_feedback.get().lower())
        self.assertEqual(self.registry.writes, [])
        app.frame_generation_backend.set("NVIDIA (default)")
        app.on_frame_generation_backend_changed()
        self.assertIn("requires usable depth", app.depth_mode_feedback.get().lower())
        self.assertIn("nvidia", app.depth_mode_feedback.get().lower())
        self.assertEqual(self.registry.values[DEPTH], (2, 4))
        app.reset_defaults()
        self.assertEqual(app.frame_generation_depth_mode.get(), "Depth reconstruction (experimental)")
        self.assertNotIn(DEPTH, [name for name, _, _ in self.registry.writes])

    def test_fidelityfx_depth_assist_explains_the_same_standard_fallback(self):
        self.registry.values.update({DEPTH: (1, 4), "FrameGenerationBackend": (1, 4)})
        app = self.app()
        self.assertEqual(app.frame_generation_depth_mode.get(), "Depth assist")
        self.assertIn("unsupported", app.depth_mode_feedback.get().lower())
        self.assertIn("standard fg", app.depth_mode_feedback.get().lower())
        self.assertEqual(self.registry.writes, [])
        app.frame_generation_depth_mode.set("Standard")
        app.update_depth_mode_feedback()
        self.assertNotIn("unsupported", app.depth_mode_feedback.get().lower())


class DepthProfileTests(DepthFixture):
    def setUp(self):
        super().setUp()
        self.store = gui.game_profiles.ProfileStore(self.registry.HKEY_CURRENT_USER, r"Software\DepthProfileFixtures")
        self.exe = r"C:\ProfileFixtures\One\fixture.exe"

    def test_global_reconstruction_is_inherited_but_explicit_game_choices_are_preserved(self):
        self.store.save(None, {DEPTH: 2, "FrameGenerationMode": 4, "DLSSNR_Enabled": 1})
        for mode in (0, 1, 2):
            self.store.save(self.exe, {DEPTH: mode, "DLSSNR_Intensity": 89})
            before = len(self.registry.writes)
            app = self.app(self.store)
            app.load_scope(self.exe)
            values, sources = self.store.load(self.exe)
            self.assertEqual(values[DEPTH], mode)
            self.assertEqual(sources[DEPTH], "profile")
            self.assertEqual(values["FrameGenerationMode"], 4)
            self.assertEqual(values["DLSSNR_Enabled"], 1)
            self.assertEqual(values["DLSSNR_Intensity"], 89)
            self.assertEqual(len(self.registry.writes), before)
        self.store.reset(self.exe)
        values, sources = self.store.load(self.exe)
        self.assertEqual((values[DEPTH], sources[DEPTH]), (2, "global"))

    def test_old_complete_profile_inherits_depth_assist_then_save_locks_the_new_field(self):
        old_setup = {"DLSSNR_Enabled": 1, "DLSSNR_Style": 2, "DLSSNR_Intensity": 89,
                     "DLSSNR_LocalToneStrength": 33, "DLSSNR_LocalStructureStrength": 70,
                     "DLSSNR_SkinStructureStrength": 50, "FoveationSize": 100,
                     "DLSSNR_ProcessingResolution": 100, "FrameGenerationMode": 3,
                     "FrameGenerationBackend": 1}
        self.store.save(self.exe, old_setup)
        values, sources = self.store.load(self.exe)
        self.assertEqual(values.get(DEPTH), 1, "Old profile must inherit Depth assist without resetting appearance")
        self.assertEqual(sources[DEPTH], "default")
        app = self.app(self.store)
        app.load_scope(self.exe)
        self.assertTrue(app.save_profile())
        self.store.save(None, {DEPTH: 0})
        values, sources = self.store.load(self.exe)
        self.assertEqual(values[DEPTH], 1)
        self.assertEqual({key: values[key] for key in old_setup}, old_setup)
        self.assertEqual(set(sources.values()), {"profile"})
        app.reset_defaults()
        self.assertEqual(app.frame_generation_depth_mode.get(), "Standard")

    def test_all_depth_modes_roundtrip_as_independent_complete_game_drafts(self):
        self.assertIn(DEPTH, gui.game_profiles.DEFAULTS, "Depth mode is absent from the complete profile schema")
        original = dict(gui.game_profiles.DEFAULTS, DLSSNR_Enabled=1, DLSSNR_Intensity=89,
                        FrameGenerationMode=4, FrameGenerationBackend=1)
        self.store.save(None, original)
        app = self.app(self.store)
        for label, value in (("Standard", 0), ("Depth assist", 1), ("Depth reconstruction (experimental)", 2)):
            app.load_scope(self.exe)
            writes = len(self.registry.writes)
            app.frame_generation_depth_mode.set(label)
            app.on_frame_generation_depth_mode_changed()
            self.assertEqual(len(self.registry.writes), writes, "Game edits must remain drafts")
            self.assertTrue(app.profile_dirty)
            self.assertTrue(app.save_profile())
            values, sources = self.store.load(self.exe)
            self.assertEqual(values, dict(original, FrameGenerationDepthMode=value))
            self.assertEqual(set(sources.values()), {"profile"})
            self.assertEqual(self.store.load()[0], original)

    def test_invalid_depth_profile_does_not_reset_nr_appearance_or_write_repairs(self):
        self.assertIn(DEPTH, gui.game_profiles.DEFAULTS, "Missing depth profile validation")
        original = dict(gui.game_profiles.DEFAULTS, DLSSNR_Enabled=1, DLSSNR_Style=2,
                        DLSSNR_Intensity=89, DLSSNR_LocalToneStrength=33,
                        DLSSNR_LocalStructureStrength=64, DLSSNR_SkinStructureStrength=48,
                        FrameGenerationMode=3, FrameGenerationBackend=1)
        self.store.save(self.exe, original)
        self.registry.keys[self.store.path(self.exe)][DEPTH] = (9, 4)
        with self.assertRaisesRegex(ValueError, DEPTH):
            self.store.load(self.exe)
        writes = len(self.registry.writes)
        app = self.app(self.store)
        app.load_scope(self.exe)
        self.assertTrue(app.enabled.get())
        self.assertEqual([v.get() for v in app.variables], [2, 89, 33, 64, 48, 100])
        self.assertEqual(app.frame_generation.get(), "OFXR 3×")
        self.assertEqual(app.frame_generation_backend.get(), "FidelityFX (full resolution)")
        self.assertIn(DEPTH, app.status.get())
        self.assertIn("invalid", app.status.get().lower())
        self.assertTrue(app.profile_valid)
        self.assertEqual(len(self.registry.writes), writes)
        for invalid in (-1, 3, True, "2", 2.0):
            with self.subTest(invalid=invalid), self.assertRaisesRegex(ValueError, DEPTH):
                self.store.save(self.exe, {"DLSSNR_Intensity": 5, DEPTH: invalid})
        self.assertEqual(len(self.registry.writes), writes)


if __name__ == "__main__":
    unittest.main()
