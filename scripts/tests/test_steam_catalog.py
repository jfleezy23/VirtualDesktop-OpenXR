"""Steam titles and cache paths resolve against saved executable identities."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from vdxr_game_profiles import normalize_executable


class SteamCatalogTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(importlib.util.find_spec("vdxr_steam_catalog"), "Steam catalog is missing")
        from vdxr_steam_catalog import steam_metadata

        self.resolve = steam_metadata
        fixture = tempfile.TemporaryDirectory()
        self.addCleanup(fixture.cleanup)
        self.root = Path(fixture.name)
        self.client = self.root / "Steam"
        self.cache = self.client / "appcache" / "librarycache"
        (self.client / "steamapps").mkdir(parents=True)

    def write_manifest(self, app_id, title, install, *, library=None, declared_id=None):
        steamapps = (library or self.client) / "steamapps"
        steamapps.mkdir(parents=True, exist_ok=True)
        path = steamapps / f"appmanifest_{app_id}.acf"
        path.write_text(
            f'"AppState" {{ "appid" "{declared_id or app_id}" "name" "{title}" "installdir" "{install}" }}',
            encoding="utf-8-sig",
        )
        return steamapps / "common" / install

    def executable(self, directory, name="Game.exe"):
        path = directory / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()
        return normalize_executable(path)

    def metadata(self, *targets, frame_app_ids=None):
        return self.resolve(targets, frame_app_ids=frame_app_ids, steam_roots=[self.client])

    def test_reads_names_from_modern_and_legacy_library_entries(self):
        modern = self.root / "Second Drive"
        legacy = self.root / "Legacy Library"
        config = self.client / "steamapps" / "libraryfolders.vdf"
        modern_value, legacy_value = (str(path).replace("\\", "\\\\") for path in (modern, legacy))
        config.write_text(
            '"libraryfolders" {\n'
            f'  "1" {{ "path" "{modern_value}" "apps" {{ "111" "42" }} }}\n'
            f'  "2" "{legacy_value}"\n'
            '}\n',
            encoding="utf-8",
        )
        install = self.write_manifest("111", r'Rising Stars: \"Overtime\"', "Shared", library=modern)
        modern_exe = self.executable(install / "Binaries" / "Win64")
        legacy_exe = self.executable(self.write_manifest("222", "Boxing\u2122", "Classic", library=legacy))
        self.assertEqual(self.metadata(modern_exe, legacy_exe), {
            modern_exe: ('Rising Stars: "Overtime"', "111", self.cache / "111"),
            legacy_exe: ("Boxing\u2122", "222", self.cache / "222"),
        })

    def test_checks_install_directory_components_and_declared_app_id(self):
        install = self.write_manifest("111", "Official Title", "Arena")
        target = self.executable(install)
        sibling = self.executable(install.with_name("Arena Plus"))
        mismatched = self.executable(self.write_manifest("222", "Other Title", "Other", declared_id="333"))
        self.assertEqual(self.metadata(target, sibling, mismatched),
                         {target: ("Official Title", "111", self.cache / "111")})

    def test_rejects_ambiguous_containment_and_conflicting_exact_frame_id(self):
        duplicate = self.executable(self.write_manifest("111", "First", "Shared"))
        self.write_manifest("222", "Second", "Shared")
        conflict = self.executable(self.write_manifest("333", "Third", "Third"))
        self.assertEqual(self.metadata(duplicate, conflict, frame_app_ids={duplicate: "111", conflict: "222"}), {})

    def test_exact_frame_id_can_resolve_an_executable_outside_the_install(self):
        self.write_manifest("111", "First", "Arena")
        target = self.executable(self.root / "Custom Launch Tree")
        sibling = self.executable(self.root / "Custom Launch Tree", "Other.exe")
        self.assertEqual(self.metadata(target, sibling, frame_app_ids={target: "111"}),
                         {target: ("First", "111", self.cache / "111")})

    def test_exact_frame_id_can_reuse_verified_local_cache_without_a_manifest(self):
        (self.cache / "111").mkdir(parents=True)
        target = self.executable(self.root / "Custom Launch Tree")
        self.assertEqual(self.metadata(target, frame_app_ids={target: "111"}),
                         {target: (None, "111", self.cache / "111")})
        self.assertEqual(self.metadata(target, frame_app_ids={target: r"111\..\222"}), {})


if __name__ == "__main__":
    unittest.main()
