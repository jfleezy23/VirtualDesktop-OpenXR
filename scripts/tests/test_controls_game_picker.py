import sys
import unittest

if sys.platform != "win32":
    raise unittest.SkipTest("Controls picker requires Windows Tk")

from pathlib import Path
from tempfile import TemporaryDirectory
import tkinter as tk
from unittest.mock import Mock, patch

import vdxr_registry_gui as gui


class ControlsPickerTests(unittest.TestCase):
    def setUp(self):
        self.root = tk.Tk()
        self.root.withdraw()
        self.addCleanup(self.root.destroy)
        store = Mock()
        store.list_profiles.return_value = []
        store.load.return_value = (gui.game_profiles.DEFAULTS, {})
        self.app = gui.VDXRRegistryApp(self.root, profile_store=store)

    def test_wide_editor_arranges_fg_and_nr_in_separate_columns(self):
        self.root.geometry(f"+{self.root.winfo_screenwidth() + 100}+0")
        self.root.deiconify()
        self.root.update()
        self.assertEqual(tuple(map(int, self.root.resizable())), (1, 1))
        self.assertGreaterEqual(self.app.editor_frame.winfo_reqwidth(), 1030)
        self.assertNotEqual(self.app.fg_selector.master.master.master,
                            self.app.processing_resolution_slider.master.master.master)
        self.assertEqual(self.app.editor_scrollbar.winfo_manager(), "")
        self.assertLessEqual(self.root.winfo_reqheight(), self.root.winfo_screenheight() - 40)

    def test_search_trace_is_released_when_picker_closes(self):
        start = len(self.root.tk.call("info", "globals", "PY_VAR*"))
        for _ in range(3):
            self.app.open_game_picker()
            self.root.update_idletasks()
            self.app.game_picker.destroy()
        self.assertIsNone(self.app.game_picker)
        self.assertEqual(len(self.root.tk.call("info", "globals", "PY_VAR*")), start)

    def test_narrow_display_stacks_controls_inside_the_window(self):
        root = tk.Tk()
        root.withdraw()
        self.addCleanup(root.destroy)
        root.winfo_screenwidth = lambda: 850
        root.winfo_screenheight = lambda: 700
        store = Mock()
        store.list_profiles.return_value = []
        store.load.return_value = (gui.game_profiles.DEFAULTS, {})
        app = gui.VDXRRegistryApp(root, profile_store=store)
        self.assertLessEqual(root.minsize()[0], 850)
        fg_column = app.fg_selector.master.master.master.grid_info()
        nr_column = app.processing_resolution_slider.master.master.master.grid_info()
        self.assertEqual(fg_column["column"], nr_column["column"])
        self.assertLess(fg_column["row"], nr_column["row"])

    def test_long_selected_title_fits_the_scope_control(self):
        executable = r"f:\many words\game.exe"
        self.app.game_entries = {executable: gui.GameEntry(executable, "WWWW " * 30, None)}
        self.app.profile_executable = executable
        self.app.update_scope_title()
        self.root.update_idletasks()
        self.assertLess(self.app.scope_selector.winfo_reqwidth(), 325)
        self.assertTrue(self.app.scope_title.get().endswith("…"))

    def test_picker_shows_cover_and_title_but_selects_exact_executable(self):
        with TemporaryDirectory() as directory:
            path = Path(directory) / "cover.png"
            cover = tk.PhotoImage(master=self.root, width=16, height=9)
            cover.put("#945ac9", to=(0, 0, 16, 9))
            cover.write(str(path), format="png")
            executable = r"f:\my game\game-win64-shipping.exe"
            self.app.profile_store.list_profiles.return_value = [executable]
            self.app.profile_store.load.return_value = (gui.game_profiles.DEFAULTS, {})
            self.assertTrue(hasattr(self.app, "open_game_picker"), "A game picker must expose artwork")
            entry = gui.GameEntry(executable, "Picture Test", path)
            with patch.object(gui, "catalog_entries", return_value=[entry]):
                self.app.open_game_picker()
                self.root.update_idletasks()
                dialog = self.app.game_picker
                buttons = [child for child in self.app.game_tiles.winfo_children() if isinstance(child, tk.Button)]
                self.assertEqual(len(buttons), 1)
                self.assertEqual(buttons[0].cget("text"), "Picture Test")
                image = buttons[0].cget("image")
                self.assertTrue(image)
                pixel = self.root.tk.call(image, "get", 84, 40)
                self.assertEqual(tuple(map(int, pixel)), (148, 90, 201))
                buttons[0].invoke()
                self.root.update_idletasks()
                self.assertFalse(dialog.winfo_exists())
                self.assertEqual(self.app.profile_executable, executable)
                self.assertEqual(self.app.scope_title.get(), "Picture Test")
                self.app.profile_store.save.assert_not_called()

    def test_add_game_resolves_display_metadata_before_saving(self):
        with TemporaryDirectory() as directory:
            executable = Path(directory) / "OrbitGame.exe"
            executable.touch()
            path = Path(directory) / "cover.png"
            photo = tk.PhotoImage(master=self.root, width=16, height=9)
            photo.put("#945ac9", to=(0, 0, 16, 9))
            photo.write(str(path), format="png")
            identity = gui.game_profiles.normalize_executable(executable)
            entry = gui.GameEntry(identity, "Orbit Horizon", path)
            self.app.profile_store.load.return_value = (gui.game_profiles.DEFAULTS, {})
            with (patch.object(gui.filedialog, "askopenfilename", return_value=str(executable)),
                  patch.object(gui, "catalog_entries", return_value=[entry]) as resolve):
                self.app.add_profile()
            resolve.assert_called_once_with([identity])
            self.assertEqual(self.app.profile_executable, identity)
            self.assertEqual(self.app.scope_title.get(), "Orbit Horizon")
            image = self.app.scope_selector.cget("image")
            self.assertEqual(self.root.tk.call(image, "get", 42, 22), (148, 90, 201))
            self.app.profile_store.save.assert_not_called()

    def test_duplicate_tiles_and_selected_scope_show_store_without_executable_path(self):
        meta = r"f:\meta library\orbit\game.exe"
        steam = r"f:\steam library\orbit\game.exe"
        self.app.profile_store.list_profiles.return_value = [steam, meta]
        with patch.object(gui, "catalog_entries", return_value=[
            gui.GameEntry(steam, "Orbit Horizon", None, "Steam"),
            gui.GameEntry(meta, "Orbit Horizon", None, "Meta"),
        ]):
            for expected_path, expected_store in ((steam, "Steam"), (meta, "Meta")):
                self.app.open_game_picker()
                self.root.update_idletasks()
                buttons = {child.cget("text"): child for child in self.app.game_tiles.winfo_children()
                           if isinstance(child, tk.Button)}
                self.assertEqual(set(buttons), {"Orbit Horizon\nMeta", "Orbit Horizon\nSteam"})
                buttons[f"Orbit Horizon\n{expected_store}"].invoke()
                self.assertEqual(self.app.profile_executable, expected_path)
                self.app.profile_store.load.assert_called_with(expected_path, errors=[])
                feedback = self.app.scope_feedback.get()
                self.assertIn(expected_store, feedback)
                self.assertNotIn(expected_path, feedback)
            self.app.profile_store.save.assert_not_called()

    def test_duplicate_edition_numbers_are_stable_when_one_path_is_filtered(self):
        first = r"f:\variants\a\game.exe"
        second = r"f:\variants\b\game.exe"
        entries = [gui.GameEntry(path, "Orbit Horizon", None) for path in (second, first)]
        self.app.profile_store.list_profiles.return_value = [second, first]
        with patch.object(gui, "catalog_entries", return_value=entries):
            self.app.open_game_picker()
            labels = [child.cget("text") for child in self.app.game_tiles.winfo_children()
                      if isinstance(child, tk.Button)]
            self.assertEqual(labels, ["Orbit Horizon\nCopy 1", "Orbit Horizon\nCopy 2"])
            search = next(child for child in self.app.game_tiles.master.winfo_children()
                          if isinstance(child, gui.ttk.Entry))
            search.insert(0, second)
            labels = [child.cget("text") for child in self.app.game_tiles.winfo_children()
                      if isinstance(child, tk.Button)]
            self.assertEqual(labels, ["Orbit Horizon\nCopy 2"])
