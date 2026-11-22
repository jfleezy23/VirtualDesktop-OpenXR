"""Local game metadata enriches saved profiles without changing executable identity."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import winreg

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))


class GameCatalogTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(importlib.util.find_spec("vdxr_game_catalog"), "Controls game catalog is missing")
        import vdxr_game_catalog
        self.catalog = vdxr_game_catalog.catalog_entries
        fixture = tempfile.TemporaryDirectory()
        self.addCleanup(fixture.cleanup)
        self.root = Path(fixture.name)
        self.library = self.root / "Oculus"
        self.manifests = self.library / "Manifests"
        self.manifests.mkdir(parents=True)
        self.frame = self.root / "profiles.json"
        self.meta_titles = self.root / "meta-titles.json"
        self.steam = self.root / "Steam"
        self.oculus_install = self.root / "Oculus Runtime"

    def entries(self, *executables):
        with patch("winreg.OpenKey"), patch("winreg.QueryValueEx") as query_value:
            query_value.return_value = str(self.oculus_install), winreg.REG_SZ
            return self.catalog(executables, library_roots=[self.library], frame_profiles=self.frame,
                                steam_roots=[self.steam], meta_titles=self.meta_titles)

    def write_json(self, path, data):
        path.write_text(json.dumps(data), encoding="utf-8")

    def write_art(self, path):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"fixture metadata only")
        return path

    def write_core_art(self, canonical):
        return self.write_art(self.oculus_install / "CoreData" / "Software" / "StoreAssets" /
                              (canonical + "_assets") / "cover_landscape_image.jpg")

    def test_owned_manifest_keeps_exact_profile_identity_and_preferred_cover(self):
        executable = str(self.root / "Install" / "OrbitGame.exe")
        unrelated = str(self.root / "Other" / "OrbitGame.exe")
        label = "Orbit Beyond (VDXR)"
        self.write_json(self.manifests / "owned.json", {
            "canonicalName": label,
            "launchFile": str(self.root / "Launchers" / "wrapper.exe"),
            "x-vdxr-local-entry": {"owner": "VDXR local catalog", "originalExecutable": executable},
        })
        cover = self.write_art(self.library / "Software" / "StoreAssets" /
                               (label + "_assets") / "cover_landscape_image.jpg")
        self.write_json(self.frame, {"Library": {"Games": [{
            "Name": "Frame Orbit",
            "Executable": executable,
            "HeroPath": str(self.write_art(self.root / "art" / "frame.jpg")),
        }]}})
        matched, other = self.entries(executable.upper().replace("\\", "/"), unrelated)
        self.assertEqual(matched.executable, executable.upper().replace("\\", "/"))
        self.assertEqual(matched.title, "Orbit Beyond")
        self.assertEqual(matched.cover, cover)
        self.assertEqual(other.title, "Orbit Game")
        self.assertIsNone(other.cover)

    def test_native_relative_launch_target_gets_library_cover(self):
        executable = self.library / "Software" / "example-game" / "Bin" / "RedSky.exe"
        self.write_json(self.manifests / "example-game.json", {
            "canonicalName": "example-game", "launchFile": "Bin/RedSky.exe",
        })
        self.write_core_art("example-game")
        cover = self.write_art(self.library / "Software" / "StoreAssets" /
                               "example-game_assets" / "cover_landscape_image.jpg")
        self.assertEqual(self.entries(str(executable))[0].cover, cover)

    def test_new_meta_executable_gets_title_and_cover_from_its_own_package(self):
        executable = self.library / "Software" / "test-publisher-codename" / "Bin" / "Game.exe"
        app_id = "1234567890123456"
        self.write_json(self.manifests / "example.json", {
            "canonicalName": "test-publisher-codename", "appId": app_id, "launchFile": "Bin/Game.exe",
        })
        self.write_json(self.meta_titles, {app_id: "Le Rêve VR"})
        cover = self.write_core_art("test-publisher-codename")
        self.write_json(self.frame, {"Library": {"Games": [{
            "Name": "Test Publisher", "Executable": str(executable),
        }]}})
        unrelated = self.library / "Software" / "Unrelated" / "Bin" / "Game2.exe"
        matched, other = self.entries(str(executable), str(unrelated))
        self.assertEqual((matched.title, matched.cover), ("Le Rêve VR", cover))
        self.assertEqual((other.title, other.cover), ("Game2", None))

    def test_native_display_name_works_without_an_external_title_cache(self):
        executable = self.library / "Software" / "test-publisher-codename" / "Game.exe"
        self.write_json(self.manifests / "example.json", {
            "canonicalName": "test-publisher-codename", "displayName": "The Game", "launchFile": "Game.exe",
        })
        self.assertEqual(self.entries(str(executable))[0].title, "The Game")

    def test_invalid_expand_string_skips_only_the_broken_library_value(self):
        from vdxr_game_catalog import _registered_libraries
        invalid = str(self.library) + "\0hidden"
        with (patch("winreg.OpenKey"), patch("winreg.QueryInfoKey", return_value=(1, 0, 0)),
              patch("winreg.EnumKey", return_value="Example"),
              patch("winreg.QueryValueEx", side_effect=[(invalid, winreg.REG_EXPAND_SZ),
                                                       (str(self.library), winreg.REG_SZ)])):
            self.assertEqual(list(_registered_libraries()), [str(self.library)])

    def test_new_steam_executable_uses_install_manifest_and_header_without_frame_entry(self):
        steamapps = self.steam / "steamapps"
        executable = steamapps / "common" / "Ocean VR" / "Bin" / "Orbit.exe"
        executable.parent.mkdir(parents=True)
        executable.touch()
        (steamapps / "appmanifest_2345.acf").write_text(
            '"AppState" { "appid" "2345" "installdir" "Ocean VR" "name" "Ocean Horizon VR" }',
            encoding="utf-8")
        cover = self.write_art(self.steam / "appcache" / "librarycache" / "2345" / "header.jpg")
        matched = self.entries(str(executable))[0]
        self.assertEqual((matched.executable, matched.title, matched.cover),
                         (str(executable), "Ocean Horizon VR", cover))

    def test_conflicting_frame_app_id_cannot_promote_its_steam_header(self):
        steamapps = self.steam / "steamapps"
        executable = steamapps / "common" / "Ocean VR" / "Orbit.exe"
        executable.parent.mkdir(parents=True)
        executable.touch()
        (steamapps / "appmanifest_2345.acf").write_text(
            '"AppState" { "appid" "2345" "installdir" "Ocean VR" "name" "Ocean Horizon VR" }',
            encoding="utf-8")
        folder = self.steam / "appcache" / "librarycache" / "2346"
        poster = self.write_art(folder / "poster.jpg")
        self.write_art(folder / "header.jpg")
        self.write_json(self.frame, {"Library": {"Games": [{
            "Name": "Fallback metadata", "InstallDirectory": str(executable.parent),
            "SteamAppKey": "steam.app.2346", "PosterPath": str(poster),
        }]}})
        self.assertEqual(self.entries(str(executable))[0].cover, poster)

    def test_protected_publisher_uses_verified_title_and_keeps_prior_art(self):
        executable = self.library / "Software" / "bulkypix-game" / "Game.exe"
        app_id = "1234567890123456"
        self.write_json(self.manifests / "example.json", {
            "canonicalName": "bulkypix-game", "appId": app_id, "launchFile": "Game.exe",
        })
        hero = self.write_art(self.root / "hero.jpg")
        poster = self.write_art(self.root / "poster.jpg")
        self.write_json(self.frame, {"Library": {"Games": [{
            "Name": "Bulky Pix", "Executable": str(executable),
            "PosterPath": str(poster), "HeroPath": str(hero),
        }]}})
        matched = self.entries(str(executable))[0]
        self.assertEqual((matched.title, matched.cover), ("Bulky Pix", hero))
        self.write_json(self.meta_titles, {app_id: "Proper Game Title"})
        matched = self.entries(str(executable))[0]
        self.assertEqual((matched.title, matched.cover, matched.store), ("Proper Game Title", hero, "Meta"))

    def test_native_game_uses_core_art_outside_its_library(self):
        install = self.library / "Software" / "example-game"
        launch = install / "Launcher.exe"
        shipping = install / "Bin" / "Windows" / "RedSky.exe"
        other = install.with_name("example-game-2") / "Bin" / "Windows" / "RedSky.exe"
        self.write_json(self.manifests / "example-game.json", {
            "canonicalName": "example-game", "launchFile": "Launcher.exe",
        })
        cover = self.write_core_art("example-game")
        self.assertEqual([entry.cover for entry in self.entries(str(launch), str(shipping), str(other))],
                         [cover, cover, None])

    def test_native_containment_requires_one_manifest_and_exact_match_still_wins(self):
        install = self.library / "Software" / "example-game"
        launch = install / "Launcher.exe"
        shipping = install / "Bin" / "Windows" / "RedSky.exe"
        self.write_json(self.manifests / "example-game.json", {
            "canonicalName": "example-game", "launchFile": "Launcher.exe",
        })
        self.write_json(self.manifests / "duplicate.json", {
            "canonicalName": "example-game", "launchFile": "Alternate.exe",
        })
        cover = self.write_core_art("example-game")
        self.assertEqual([entry.cover for entry in self.entries(str(launch), str(shipping))], [cover, None])

    def test_owned_art_beats_native_core_art(self):
        executable = self.library / "Software" / "example-game" / "Game.exe"
        self.write_json(self.manifests / "example-game.json", {
            "canonicalName": "example-game", "launchFile": "Game.exe",
        })
        self.write_json(self.manifests / "owned.json", {
            "canonicalName": "My Game (VDXR)",
            "x-vdxr-local-entry": {"owner": "VDXR local catalog", "originalExecutable": str(executable)},
        })
        self.write_core_art("example-game")
        owned = self.write_art(self.library / "Software" / "StoreAssets" /
                               "My Game (VDXR)_assets" / "cover_landscape_image.jpg")
        self.assertEqual(self.entries(str(executable))[0].cover, owned)

    def test_native_launch_path_cannot_escape_its_install_directory(self):
        executable = self.library / "Software" / "Outside.exe"
        self.write_json(self.manifests / "example-game.json", {
            "canonicalName": "example-game", "launchFile": "../Outside.exe",
        })
        self.write_art(self.library / "Software" / "StoreAssets" /
                       "example-game_assets" / "cover_landscape_image.jpg")
        self.assertIsNone(self.entries(str(executable))[0].cover)

    def test_invalid_manifest_and_escaping_paths_leave_a_readable_fallback(self):
        path = self.root / "Private Machine Directory" / "CrystalCavesVR-Win64-Shipping.exe"
        (self.manifests / "corrupt.json").write_bytes(b"{broken")
        self.write_json(self.manifests / "unowned.json", {
            "canonicalName": "Incorrect Owner",
            "x-vdxr-local-entry": {"owner": "Other", "originalExecutable": str(path)},
        })
        self.write_json(self.manifests / "escapes.json", {
            "canonicalName": "../Private Machine Directory", "launchFile": str(path),
        })
        item = self.entries(str(path))[0]
        self.assertEqual(item.executable, str(path))
        self.assertEqual(item.title, "Crystal Caves VR")
        self.assertIsNone(item.cover)

    def test_frame_exact_match_beats_containment_and_prefers_poster(self):
        install = self.root / "Games" / "Shared"
        exe = install / "Bin" / "SpaceQuest.exe"
        hero = self.write_art(self.root / "art" / "hero.jpg")
        poster = self.write_art(self.root / "art" / "poster.jpg")
        self.write_json(self.frame, {"Library": {"Games": [
            {"Name": "Other Variant", "InstallDirectory": str(install)},
            {"Name": "SpaceQuest VR", "Executable": str(exe), "InstallDirectory": str(install),
             "HeroPath": str(hero), "PosterPath": str(poster)},
        ]}})
        item = self.entries(str(exe))[0]
        self.assertEqual(item.title, "SpaceQuest VR")
        self.assertEqual(item.cover, poster)
        poster.unlink()
        self.assertEqual(self.entries(str(exe))[0].cover, hero)

    def test_frame_uses_only_unambiguous_install_directory_containment(self):
        install = self.root / "Games" / "Ocean"
        ambiguous = self.root / "Games" / "Shared"
        poster = self.write_art(self.root / "art" / "poster.jpg")
        self.write_json(self.frame, {"Library": {"Games": [
            {"Name": "Ocean Horizon", "InstallDirectory": str(install), "PosterPath": str(poster)},
            {"Name": "First Shared Game", "InstallDirectory": str(ambiguous)},
            {"Name": "Second Shared Game", "InstallDirectory": str(ambiguous / "Bin")},
        ]}})
        original = str(install / "Bin" / "Game.exe")
        matched, similar, unrelated_title = self.entries(original, str(install.with_name("Ocean2") / "Game.exe"),
                                                        str(ambiguous / "Bin" / "FlightVR.exe"))
        self.assertEqual((matched.executable, matched.title, matched.cover), (original, "Ocean Horizon", poster))
        self.assertEqual(similar.title, "Game")
        self.assertEqual(unrelated_title.title, "Flight VR")

    def test_frame_does_not_attribute_known_game_to_another_executable(self):
        install = self.root / "Games" / "Shared"
        other = str(install / "Bin" / "OrbitGame.exe")
        poster = self.write_art(self.root / "art" / "poster.jpg")
        for configured in (str(install / "Launcher.exe"), r"invalid\relative.exe"):
            with self.subTest(configured=configured):
                self.write_json(self.frame, {"Library": {"Games": [{
                    "Name": "Known Game", "InstallDirectory": str(install),
                    "Executable": configured, "PosterPath": str(poster),
                }]}})
                item = self.entries(other)[0]
                self.assertEqual((item.title, item.cover), ("Orbit Game", None))

    def test_steam_frame_records_use_widescreen_headers_over_portrait_and_hero(self):
        steam_cache = self.root / "Steam" / "appcache" / "librarycache"
        app_ids = ("1234", "5738")
        first, second = (steam_cache / app_id for app_id in app_ids)
        widescreen = [
            self.write_art(first / "header.jpg"),
            self.write_art(second / ("b" * 40) / "library_header.jpg"),
        ]
        keys = [str(self.root / f"Game{app_id}.exe") for app_id in app_ids]
        self.write_json(self.frame, {"Library": {"Games": [
            {"Name": "Game", "Executable": exe, "SteamAppKey": f"steam.app.{app_id}",
             "PosterPath": str(self.write_art(folder / ("a" * 40) / "library_capsule.jpg")),
             "HeroPath": str(self.write_art(folder / "library_hero.jpg"))}
            for exe, app_id, folder in zip(keys, app_ids, (first, second))
        ]}})
        self.assertEqual([item.cover for item in self.entries(*keys)], widescreen)

    def test_steam_header_fallback_requires_the_exact_app_cache_id(self):
        exe = str(self.root / "Game.exe")
        cache = self.root / "Steam" / "appcache" / "librarycache" / "111"
        self.write_art(cache / "header.jpg")
        poster = self.write_art(cache / "library_600x900.jpg")
        self.write_json(self.frame, {"Library": {"Games": [
            {"Name": "Game", "Executable": exe, "SteamAppKey": "steam.app.222", "PosterPath": str(poster)}
        ]}})
        self.assertEqual(self.entries(exe)[0].cover, poster)

    def test_steam_header_fallback_uses_a_unique_frame_install(self):
        folder = self.root / "Repacked Game"
        executable = str(folder / "Engine" / "Game.exe")
        assets = self.steam / "appcache" / "librarycache" / "1234"
        poster = self.write_art(assets / "library_600x900.jpg")
        wide = self.write_art(assets / "header.jpg")
        game = {"Name": "Repacked Game", "InstallDirectory": str(folder),
                "SteamAppKey": "steam.app.1234", "PosterPath": str(poster)}
        self.write_json(self.frame, {"Library": {"Games": [game]}})
        self.assertEqual(self.entries(executable)[0].cover, wide)
        game["Executable"] = str(folder / "Different.exe")
        self.write_json(self.frame, {"Library": {"Games": [game]}})
        self.assertIsNone(self.entries(executable)[0].cover)

    def test_frame_falls_back_from_unsupported_art_to_supported_poster(self):
        executable = str(self.root / "Game.exe")
        hero = self.write_art(self.root / "art" / "hero.webp")
        poster = self.write_art(self.root / "art" / "poster.jpg")
        self.write_json(self.frame, {"Library": {"Games": [{
            "Name": "Local Game", "Executable": executable,
            "HeroPath": str(hero), "PosterPath": str(poster),
        }]}})
        self.assertEqual(self.entries(executable)[0].cover, poster)

    def test_frame_ignores_remote_art_and_retains_local_poster(self):
        executable = str(self.root / "Game.exe")
        poster = self.write_art(self.root / "art" / "poster.jpg")
        self.write_json(self.frame, {"Library": {"Games": [{
            "Name": "Local Game", "Executable": executable, "PosterPath": str(poster),
            "HeroPath": r"\\remote\library\hero.jpg",
        }]}})
        self.assertEqual(self.entries(executable)[0].cover, poster)


if __name__ == "__main__":
    unittest.main()
