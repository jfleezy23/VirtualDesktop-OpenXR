"""Launch-tree transactions use only temporary files and an injected registry/OS."""
import contextlib
import base64
import copy
import hashlib
import json
import os
import subprocess
from pathlib import Path
import tempfile
import unittest
from unittest import mock

try:
    import vdxr_launch_trees as trees
except ModuleNotFoundError:
    trees = None


class Platform:
    def __init__(self, custom, stock):
        self.custom, self.stock = custom, stock
        self.values = {}
        self.running = True
        self.server_running = False
        self.games = False
        self.log = []
        self.write_count = 0
        self.fail_write = 0
        self.fail_start_once = False
        self.fail_stop = False
        self.bad_read_once = False
        self.fail_registry_once = False
        self.ignore_registry_once = False
        self.partial_stop_failure = False

    def assert_trusted_config(self, path):
        pass

    @contextlib.contextmanager
    def mutex(self):
        self.log.append("lock")
        yield
        self.log.append("unlock")

    def resolve(self, path):
        path = Path(path)
        return (self.custom / "Software" if path == self.stock / "Software" else path).resolve()

    def read_value(self, hive, path, name):
        return copy.deepcopy(self.values.get((hive, path, name)))

    def write_value(self, hive, path, name, record):
        self.log.append(("registry", hive, path, name))
        if self.fail_registry_once:
            self.fail_registry_once = False
            raise OSError("injected registry failure")
        if self.ignore_registry_once:
            self.ignore_registry_once = False
            return
        if record is None:
            self.values.pop((hive, path, name), None)
        else:
            self.values[hive, path, name] = copy.deepcopy(record)

    def read_bytes(self, path):
        self.log.append(("read", str(path)))
        data = Path(path).read_bytes()
        if self.bad_read_once and self.write_count >= self.bad_read_once and Path(path).name.endswith("-cache.json"):
            self.bad_read_once = False
            return b"{}"
        return data

    def write_bytes(self, path, data):
        self.write_count += 1
        self.log.append(("write", str(path)))
        if self.fail_write == self.write_count:
            raise OSError("injected file failure")
        Path(path).write_bytes(data)

    def ensure_no_games(self, config):
        self.log.append("games")
        if self.games:
            raise RuntimeError("A VR game is running")

    def streamer_running(self, config):
        if self.server_running and not self.running:
            raise RuntimeError("Server is running without Streamer")
        return self.running

    def streamer_quiesced(self, config):
        self.log.append("vd-only-status")
        return not self.running and not self.server_running

    def stop_streamer(self, config):
        self.log.append("stop")
        if self.fail_stop:
            if self.partial_stop_failure:
                self.running = False
            raise OSError("Streamer/server did not exit")
        self.running = False
        self.server_running = False

    def restart_streamer(self, config):
        if self.running:
            self.log.append("verified-existing-normal-user")
            return
        self.log.append("restart-normal-user")
        if self.fail_start_once:
            self.fail_start_once = False
            self.running = True  # A partial handoff must be quiesced before rollback.
            raise OSError("normal-user restart verification failed")
        self.running = True


class PlatformPathResolutionTests(unittest.TestCase):
    def test_fake_junction_canonicalizes_its_destination_on_either_drive(self):
        # Emulate the runner's short-name TEMP without requiring its alias or D:.
        for drive in ("C", "D"):
            for user in ("runneradmin", "RUNNER~1"):
                with self.subTest(drive=drive, user=user):
                    base = Path(f"{drive}:/fixture-profiles/{user}/Temp/launch-tree-fixture")
                    platform = Platform(base / "Custom", base / "Stock")
                    expected = Path(str(platform.custom / "Software").replace("RUNNER~1", "runneradmin"))
                    def canonicalize(path):
                        return Path(str(path).replace("RUNNER~1", "runneradmin"))
                    with mock.patch.object(Path, "resolve", autospec=True, side_effect=canonicalize):
                        self.assertEqual(platform.resolve(platform.stock / "Software"), expected)
                        self.assertEqual(platform.resolve(platform.custom / "Software"), expected)
                        self.assertNotEqual(platform.resolve(base / "Unrelated" / "Software"), expected)


class LaunchTreeTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(trees, "Missing launch-tree transaction backend")
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.custom = self.base / "Custom"
        self.stock = self.base / "Stock"
        for p in (self.custom / "Software", self.custom / "Manifests", self.stock / "Manifests", self.stock / "Software"):
            p.mkdir(parents=True)
        self.source = self.custom / "Manifests" / "original.mini"
        self.original_source = b'{"launch": "untouched original custom command"}\r\n'
        self.source.write_bytes(self.original_source)
        (self.stock / "Manifests" / "stock.mini").write_bytes(b"stock")
        self.runtime = self.base / "virtualdesktop-openxr.dll"
        self.runtime.write_bytes(b"never loaded")
        self.streamer = self.base / "VirtualDesktop.Streamer.exe"
        self.streamer.write_bytes(b"never executed")
        self.cache = self.base / "user-cache.json"
        self.cache2 = self.base / "system-cache.json"
        self.custom_command = '"C:\\Apps\\Bridge.exe" --title "Δ game" --argument "two words"'
        custom_patch = {"ShippingExe": {"owned": {"present": True, "value": self.custom_command},
                                         "null": {"present": True, "value": None}},
                        "SteamProductInfos": {"42": {"present": True, "value": {"exe": self.custom_command, "flag": None}}}}
        stock_patch = {"ShippingExe": {"owned": {"present": True, "value": "Original.exe"},
                                        "null": {"present": False}},
                       "SteamProductInfos": {"42": {"present": True, "value": {"exe": "Stock.exe"}}}}
        self.initial_cache = {"ShippingExe": {"owned": self.custom_command, "null": None, "other": "unrelated"},
                              "SteamProductInfos": {"42": {"exe": self.custom_command, "flag": None}, "55": {"other": True}},
                              "Unrelated": {"unicode": "天空", "value": [None, 1, False]}}
        for path in (self.cache, self.cache2):
            path.write_text(json.dumps(self.initial_cache, ensure_ascii=False), encoding="utf-8")
        self.library_path = r"S-1-5-21-123\Software\Oculus VR, LLC\Oculus\Libraries\fixed"
        self.config = {"schema": 1,
                       "library": {"registry_path": self.library_path, "custom_value": str(self.custom) + "/",
                                   "custom_kind": 1, "stock_root": str(self.stock)},
                       "caches": [{"path": str(path), "custom": copy.deepcopy(custom_patch), "stock": copy.deepcopy(stock_patch)}
                                  for path in (self.cache, self.cache2)],
                       "streamer_path": str(self.streamer), "custom_runtime_path": str(self.runtime),
                       "settings_registry_path": r"SOFTWARE\VirtualDesktop-OpenXR", "redirect_name": "redirect_to",
                       "game_executables": [str(self.custom / "Software/game.exe")],
                       "source_files": [{"path": str(self.source), "sha256": hashlib.sha256(self.original_source).hexdigest()}]}
        self.config_path = self.base / "config.json"
        self.save()
        self.platform = Platform(self.custom, self.stock)
        self.platform.values = {("HKU", self.library_path, "OriginalPath"): (self.config["library"]["custom_value"], 1),
                                ("HKU", self.library_path, "Path"): ("KEEP PATH EXACT", 2),
                                ("HKLM", self.config["settings_registry_path"], "redirect_to"): (str(self.runtime), 1),
                                ("HKLM", self.config["settings_registry_path"], "NRSetting"): (85, 4)}

    def save(self):
        self.config_path.write_text(json.dumps(self.config, ensure_ascii=False), encoding="utf-8")

    def activate(self, label):
        return trees.activate(self.config_path, label, self.runtime, platform=self.platform)

    def test_round_trip_preserves_original_custom_files_values_and_unrelated_state(self):
        before_values = copy.deepcopy(self.platform.values)
        self.assertTrue(self.activate("Stock VDXR"))
        for path in (self.cache, self.cache2):
            state = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(state["ShippingExe"]["owned"], "Original.exe")
            self.assertNotIn("null", state["ShippingExe"])
            self.assertEqual(state["Unrelated"], self.initial_cache["Unrelated"])
            self.assertEqual(state["SteamProductInfos"]["55"], {"other": True})
        self.assertTrue(self.activate("Custom VDXR"))
        self.assertEqual(self.platform.values, before_values)
        for path in (self.cache, self.cache2):
            self.assertEqual(json.loads(path.read_text(encoding="utf-8")), self.initial_cache)
        self.assertEqual(self.source.read_bytes(), self.original_source)
        self.assertFalse(any(event[0] == "write" and event[1] == str(self.source)
                             for event in self.platform.log if isinstance(event, tuple)))

    def test_stop_finishes_before_transaction_cache_snapshot(self):
        self.activate("Stock VDXR")
        stop = self.platform.log.index("stop")
        first_write = next(i for i, e in enumerate(self.platform.log) if isinstance(e, tuple) and e[0] == "write")
        self.assertTrue(any(isinstance(e, tuple) and e == ("read", str(self.cache))
                            for e in self.platform.log[stop:first_write]))
        self.assertEqual(self.platform.log.count("restart-normal-user"), 1)

    def test_idempotent_aligned_selection_does_not_restart_or_write(self):
        self.assertFalse(self.activate("Custom VDXR"))
        self.assertEqual(self.platform.write_count, 0)
        self.assertNotIn("stop", self.platform.log)

    def test_closed_streamer_stays_closed(self):
        self.platform.running = False
        self.assertFalse(self.activate("Stock VDXR"))
        self.assertFalse(self.platform.running)
        self.assertNotIn("restart-normal-user", self.platform.log)

    def test_partial_write_and_readback_failure_restore_exact_files_and_registry(self):
        for fault in ("write", "readback"):
            with self.subTest(fault=fault):
                initial = {p: p.read_bytes() for p in (self.cache, self.cache2)}
                registry = copy.deepcopy(self.platform.values)
                self.platform.fail_write = self.platform.write_count + 2 if fault == "write" else 0
                self.platform.bad_read_once = self.platform.write_count + 1 if fault == "readback" else False
                with self.assertRaises(OSError):
                    self.activate("Stock VDXR")
                self.assertEqual({p: p.read_bytes() for p in initial}, initial)
                self.assertEqual(self.platform.values, registry)
                self.assertTrue(self.platform.running)

    def test_restart_failure_quiesces_partial_restart_and_rolls_back(self):
        initial = self.cache.read_bytes()
        registry = copy.deepcopy(self.platform.values)
        self.platform.fail_start_once = True
        with self.assertRaises(OSError):
            self.activate("Stock VDXR")
        self.assertEqual(self.cache.read_bytes(), initial)
        self.assertEqual(self.platform.values, registry)
        self.assertTrue(self.platform.running)
        self.assertEqual(self.platform.log.count("stop"), 2)

    def test_running_game_stop_failure_unknown_redirect_or_bad_junction_never_writes(self):
        self.platform.games = True
        with self.assertRaises(OSError):
            self.activate("Stock VDXR")
        self.platform.games = False
        self.platform.fail_stop = True
        with self.assertRaises(OSError):
            self.activate("Stock VDXR")
        self.platform.fail_stop = False
        self.platform.values["HKLM", self.config["settings_registry_path"], "redirect_to"] = ("other.dll", 1)
        with self.assertRaises(ValueError):
            self.activate("Stock VDXR")
        self.assertEqual(self.platform.write_count, 0)

    def test_custom_inspection_requires_owned_cache_routes_and_is_read_only(self):
        self.assertTrue(trees.inspect_selection(self.config_path, "Custom VDXR", platform=self.platform))
        state = json.loads(self.cache.read_text())
        del state["ShippingExe"]["null"]
        self.cache.write_text(json.dumps(state))
        self.assertFalse(trees.inspect_selection(self.config_path, "Custom VDXR", platform=self.platform))
        self.assertEqual(self.platform.write_count, 0)

    def test_custom_root_with_stock_owned_routes_is_unverified_until_repaired(self):
        data = json.loads(self.cache.read_text(encoding="utf-8"))
        data["ShippingExe"]["owned"] = "Original.exe"
        self.cache.write_text(json.dumps(data), encoding="utf-8")
        self.assertFalse(trees.inspect_selection(self.config_path, "Custom VDXR", platform=self.platform))
        self.assertTrue(self.activate("Custom VDXR"))
        self.assertTrue(trees.inspect_selection(self.config_path, "Custom VDXR", platform=self.platform))

    def test_relative_config_paths_changed_custom_sources_and_bad_stock_tree_rejected(self):
        self.config["caches"][0]["path"] = "relative.json"
        self.save()
        with self.assertRaises(ValueError):
            self.activate("Stock VDXR")
        self.config["caches"][0]["path"] = str(self.cache)
        self.save()
        self.source.write_bytes(b"changed custom launch command")
        with self.assertRaises(ValueError):
            self.activate("Stock VDXR")
        self.assertEqual(self.platform.write_count, 0)

    def test_registry_write_failure_and_ignored_write_readback_roll_back(self):
        for fault in ("fail_registry_once", "ignore_registry_once"):
            with self.subTest(fault=fault):
                originals = {p: p.read_bytes() for p in (self.cache, self.cache2)}
                registry = copy.deepcopy(self.platform.values)
                setattr(self.platform, fault, True)
                with self.assertRaises(OSError):
                    self.activate("Stock VDXR")
                self.assertEqual(self.platform.values, registry)
                self.assertEqual({p: p.read_bytes() for p in originals}, originals)

    def test_bad_stock_software_target_and_manifest_cache_alias_are_rejected(self):
        original = self.platform.resolve
        self.platform.resolve = lambda path: self.base / "wrong" if Path(path) == self.stock / "Software" else original(path)
        with self.assertRaises(ValueError):
            self.activate("Stock VDXR")
        self.platform.resolve = original
        self.config["caches"][0]["path"] = str(self.source)
        self.save()
        with self.assertRaises(ValueError):
            self.activate("Stock VDXR")
        self.assertEqual(self.source.read_bytes(), self.original_source)
        self.assertEqual(self.platform.write_count, 0)

    def test_stock_vendor_resolved_paths_do_not_trigger_a_repeat_restart(self):
        self.activate("Stock VDXR")
        for path in (self.cache, self.cache2):
            state = json.loads(path.read_text(encoding="utf-8"))
            state["ShippingExe"]["null"] = "vendor-resolved-native.exe"
            path.write_text(json.dumps(state), encoding="utf-8")
        self.platform.log.clear()
        self.assertTrue(trees.inspect_selection(self.config_path, "Stock VDXR", platform=self.platform))
        self.assertFalse(self.activate("Stock VDXR"))
        self.assertNotIn("stop", self.platform.log)
        self.assertTrue(self.activate("Custom VDXR"))
        self.assertIsNone(json.loads(self.cache.read_text(encoding="utf-8"))["ShippingExe"]["null"])

    def test_saved_original_registry_type_and_custom_path_spelling_are_restored(self):
        self.config["library"]["custom_kind"] = 2
        self.save()
        self.platform.values["HKU", self.library_path, "OriginalPath"] = (self.config["library"]["custom_value"], 2)
        self.activate("Stock VDXR")
        self.activate("Custom VDXR")
        self.assertEqual(self.platform.values["HKU", self.library_path, "OriginalPath"],
                         (self.config["library"]["custom_value"], 2))
        self.assertEqual(self.platform.values["HKU", self.library_path, "Path"], ("KEEP PATH EXACT", 2))

    def test_cache_record_presence_contract_and_unknown_labels_fail_before_writes(self):
        self.config["caches"][0]["stock"]["ShippingExe"]["null"] = None
        self.save()
        with self.assertRaises(ValueError):
            self.activate("Stock VDXR")
        self.config["caches"][0]["stock"]["ShippingExe"]["null"] = {"present": False}
        self.save()
        with self.assertRaises(ValueError):
            self.activate("Other/unknown override")
        self.assertEqual(self.platform.write_count, 0)

    def test_partial_initial_stop_failure_restores_original_running_state_without_writes(self):
        self.platform.fail_stop = True
        self.platform.partial_stop_failure = True
        with self.assertRaises(OSError):
            self.activate("Stock VDXR")
        self.assertTrue(self.platform.running)
        self.assertEqual(self.platform.write_count, 0)
        self.assertEqual(self.platform.log.count("restart-normal-user"), 1)

    def test_failed_stop_with_existing_normal_streamer_does_not_duplicate_start(self):
        self.platform.fail_stop = True
        with self.assertRaises(OSError):
            self.activate("Stock VDXR")
        self.assertTrue(self.platform.running)
        self.assertNotIn("restart-normal-user", self.platform.log)
        self.assertIn("verified-existing-normal-user", self.platform.log)

    def test_stock_detects_and_repairs_known_custom_routes_with_vendor_metadata_drift(self):
        self.activate("Stock VDXR")
        data = json.loads(self.cache.read_text(encoding="utf-8"))
        data["ShippingExe"]["owned"] = self.custom_command
        data["SteamProductInfos"]["42"] = {"exe": self.custom_command, "vendor_new": True}
        self.cache.write_text(json.dumps(data), encoding="utf-8")
        self.assertFalse(trees.inspect_selection(self.config_path, "Stock VDXR", platform=self.platform))
        self.assertTrue(self.activate("Stock VDXR"))
        repaired = json.loads(self.cache.read_text(encoding="utf-8"))
        self.assertEqual(repaired["ShippingExe"]["owned"], "Original.exe")
        self.assertEqual(repaired["SteamProductInfos"]["42"]["exe"], "Stock.exe")
        self.assertTrue(repaired["SteamProductInfos"]["42"]["vendor_new"])

    def test_vendor_product_fields_survive_both_switches(self):
        for side in ("custom", "stock"):
            self.config["caches"][0][side]["SteamProductInfos"]["42"]["value"]["OpenXRSupport"] = False
        self.save()
        data = json.loads(self.cache.read_text(encoding="utf-8"))
        data["SteamProductInfos"]["42"].update({"OpenXRSupport": True, "new_vendor_field": [1, 2, 3]})
        self.cache.write_text(json.dumps(data), encoding="utf-8")
        self.activate("Stock VDXR")
        self.activate("Custom VDXR")
        data = json.loads(self.cache.read_text(encoding="utf-8"))
        self.assertTrue(data["SteamProductInfos"]["42"]["OpenXRSupport"])
        self.assertEqual(data["SteamProductInfos"]["42"]["new_vendor_field"], [1, 2, 3])

    def test_custom_return_survives_stock_tree_loss_and_changed_vendor_manifest(self):
        self.activate("Stock VDXR")
        (self.stock / "Manifests" / "stock.mini").unlink()
        (self.stock / "Manifests").rmdir()
        (self.stock / "Software").rmdir()
        self.stock.rmdir()
        self.source.write_bytes(b"legitimate vendor manifest update")
        self.assertTrue(self.activate("Custom VDXR"))
        self.assertTrue(trees.inspect_selection(self.config_path, "Custom VDXR", platform=self.platform))
        self.assertEqual(self.source.read_bytes(), b"legitimate vendor manifest update")

    def test_missing_or_empty_game_inventory_is_invalid_before_any_switch(self):
        for inventory in (None, [], "not-a-list"):
            with self.subTest(inventory=inventory):
                if inventory is None:
                    self.config.pop("game_executables", None)
                else:
                    self.config["game_executables"] = inventory
                self.save()
                with self.assertRaises(ValueError):
                    self.activate("Stock VDXR")
        self.assertEqual(self.platform.write_count, 0)

    def test_os_timeout_is_a_user_facing_oserror_without_routes_changed(self):
        def timeout(config):
            raise subprocess.TimeoutExpired("fixture status helper", 60)
        self.platform.ensure_no_games = timeout
        with self.assertRaises(OSError):
            self.activate("Stock VDXR")
        self.assertEqual(self.platform.write_count, 0)

    def test_known_mixed_stock_root_custom_redirect_and_cache_is_reconciled(self):
        self.platform.values["HKU", self.library_path, "OriginalPath"] = (str(self.stock), 1)
        self.assertFalse(trees.inspect_selection(self.config_path, "Stock VDXR", platform=self.platform))
        self.assertTrue(self.activate("Stock VDXR"))
        self.assertTrue(trees.inspect_selection(self.config_path, "Stock VDXR", platform=self.platform))
        self.assertNotIn(("HKLM", self.config["settings_registry_path"], "redirect_to"), self.platform.values)
        self.assertEqual(json.loads(self.cache.read_text())["ShippingExe"]["owned"], "Original.exe")

    def test_stock_owned_manifest_hashes_reject_drift_but_do_not_block_custom(self):
        manifest = self.stock / "Manifests/stock.mini"
        self.config["stock_files"] = [{"path": str(manifest), "sha256": hashlib.sha256(manifest.read_bytes()).hexdigest()}]
        self.save()
        manifest.write_bytes(b"changed")
        with self.assertRaises(ValueError):
            self.activate("Stock VDXR")
        self.assertFalse(self.activate("Custom VDXR"))
        self.assertEqual(self.platform.write_count, 0)

    def test_incomplete_config_is_valueerror_before_writes(self):
        original = copy.deepcopy(self.config)
        for field in ("library", "caches", "streamer_path", "custom_runtime_path"):
            with self.subTest(field=field):
                self.config = copy.deepcopy(original)
                del self.config[field]
                self.save()
                with self.assertRaises(ValueError):
                    self.activate("Stock VDXR")
        self.assertEqual(self.platform.write_count, 0)

    def test_missing_stock_launch_type_does_not_equal_custom_json_null(self):
        for cache in self.config["caches"]:
            cache["custom"]["SteamProductInfos"]["42"]["value"]["LaunchType"] = None
        self.save()
        self.activate("Stock VDXR")
        self.assertTrue(trees.inspect_selection(self.config_path, "Stock VDXR", platform=self.platform))
        self.assertFalse(self.activate("Stock VDXR"))

    @unittest.skipUnless(os.name == "nt", "Windows path normalization")
    def test_equivalent_custom_redirect_spelling_is_aligned_without_mutation(self):
        key = ("HKLM", self.config["settings_registry_path"], "redirect_to")
        self.platform.values[key] = (self.runtime.as_posix().upper(), 1)
        self.assertTrue(trees.inspect_selection(self.config_path, "Custom VDXR", platform=self.platform))
        self.assertFalse(self.activate("Custom VDXR"))
        self.assertEqual(self.platform.write_count, 0)
        self.assertNotIn("stop", self.platform.log)
        self.platform.values[key] = (str(self.runtime), 2)
        self.assertFalse(trees.inspect_selection(self.config_path, "Custom VDXR", platform=self.platform))

    def test_original_path_stays_exact_and_typed(self):
        key = ("HKU", self.library_path, "OriginalPath")
        self.platform.values[key] = (str(self.custom), 1)  # Different trailing separator.
        self.assertFalse(trees.inspect_selection(self.config_path, "Custom VDXR", platform=self.platform))
        self.platform.values[key] = (self.config["library"]["custom_value"], 2)
        self.assertFalse(trees.inspect_selection(self.config_path, "Custom VDXR", platform=self.platform))

    def test_stock_shipping_same_executable_different_arguments_is_verified(self):
        stock_command = '"C:\\Apps\\Bridge.exe" --stock-native'
        for cache in self.config["caches"]:
            cache["stock"]["ShippingExe"]["owned"]["value"] = stock_command
        self.save()
        self.activate("Stock VDXR")
        self.assertTrue(trees.inspect_selection(self.config_path, "Stock VDXR", platform=self.platform))
        self.platform.log.clear()
        self.assertFalse(self.activate("Stock VDXR"))
        self.assertNotIn("stop", self.platform.log)
        state = json.loads(self.cache.read_text(encoding="utf-8"))
        state["ShippingExe"]["owned"] = self.custom_command
        self.cache.write_text(json.dumps(state), encoding="utf-8")
        self.assertFalse(trees.inspect_selection(self.config_path, "Stock VDXR", platform=self.platform))

    def test_stock_steam_same_executable_different_arguments_is_verified(self):
        for cache in self.config["caches"]:
            cache["stock"]["SteamProductInfos"]["42"]["value"]["exe"] = '"C:\\Apps\\Bridge.exe" --stock-native'
        self.save()
        self.activate("Stock VDXR")
        self.assertTrue(trees.inspect_selection(self.config_path, "Stock VDXR", platform=self.platform))
        self.platform.log.clear()
        self.assertFalse(self.activate("Stock VDXR"))
        self.assertNotIn("stop", self.platform.log)
        state = json.loads(self.cache.read_text(encoding="utf-8"))
        state["SteamProductInfos"]["42"]["exe"] = self.custom_command
        self.cache.write_text(json.dumps(state), encoding="utf-8")
        self.assertFalse(trees.inspect_selection(self.config_path, "Stock VDXR", platform=self.platform))

    def test_quiesced_partial_write_rollback_skips_full_stop_that_would_fail(self):
        snapshots = {p: p.read_bytes() for p in (self.cache, self.cache2)}
        registry = copy.deepcopy(self.platform.values)
        original_stop = self.platform.stop_streamer
        stop_calls = []
        def stop(config):
            stop_calls.append(True)
            if len(stop_calls) > 1:
                raise OSError("A later full game inspection cannot run")
            return original_stop(config)
        self.platform.stop_streamer = stop
        self.platform.fail_write = 2
        with self.assertRaises(OSError):
            self.activate("Stock VDXR")
        self.assertEqual(len(stop_calls), 1)
        self.assertEqual({p: p.read_bytes() for p in snapshots}, snapshots)
        self.assertEqual(self.platform.values, registry)
        self.assertTrue(self.platform.running)

    def test_initial_server_without_streamer_is_rejected_before_stop_or_writes(self):
        self.platform.running, self.platform.server_running = False, True
        original = self.cache.read_bytes()
        with self.assertRaises(OSError):
            self.activate("Stock VDXR")
        self.assertEqual(self.cache.read_bytes(), original)
        self.assertEqual(self.platform.write_count, 0)
        self.assertNotIn("stop", self.platform.log)


class WindowsStatusTests(unittest.TestCase):
    def test_vd_only_quiescence_requires_both_process_types_absent(self):
        platform = trees.WindowsPlatform()
        for streamers, servers, expected in (([], [], True), ([1], [], False), ([], [2], False), ([1], [2], False)):
            with self.subTest(streamers=streamers, servers=servers), mock.patch.object(
                    platform, "_helper", return_value={"streamer_pids": streamers, "server_pids": servers}) as helper:
                self.assertEqual(platform.streamer_quiesced({}), expected)
                helper.assert_called_once_with({}, "Quiesced")

    def test_actual_platform_rejects_initial_server_only_state(self):
        platform = trees.WindowsPlatform()
        with mock.patch.object(platform, "_helper", return_value={"streamer_pids": [], "server_pids": [2]}):
            with self.assertRaises(RuntimeError):
                platform.streamer_running({})


@unittest.skipUnless(os.name == "nt", "Windows PowerShell function fixtures")
class PowerShellHelperTests(unittest.TestCase):
    def functions(self, body, native=False):
        # Never execute the helper dispatcher. The native lifetime test opts in
        # to compiling the helper's own P/Invoke class for its disposable child.
        helper = Path(__file__).resolve().parents[1] / "Restart-StreamerInteractive.ps1"
        encoded = base64.b64encode(helper.read_bytes()).decode("ascii")
        body64 = base64.b64encode(body.encode("utf-8")).decode("ascii")
        interop = ("$add=$ast.Find({param($n) $n -is [Management.Automation.Language.CommandAst] "
                   "-and $n.GetCommandName() -eq 'Add-Type'},$false)\n"
                   ". ([scriptblock]::Create($add.Extent.Text))\n" if native else "")
        script = ("$ErrorActionPreference='Stop'\n"
                  "$source=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('" + encoded + "'))\n"
                  "$ast=[scriptblock]::Create($source).Ast\n"
                  "$definitions=@($ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst]},$false))\n"
                  "foreach($definition in $definitions){ . ([scriptblock]::Create($definition.Extent.Text)) }\n" + interop +
                  "& ([scriptblock]::Create([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('" +
                  body64 + "'))))\n")
        executable = Path(os.environ.get("SystemRoot", "C:/Windows")) / "System32/WindowsPowerShell/v1.0/powershell.exe"
        result = subprocess.run([str(executable), "-NoProfile", "-NonInteractive", "-Command", "-"],
                                input=script, capture_output=True, encoding="utf-8", timeout=30,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def test_streamer_ownership_and_restart_receipts(self):
        # Invoke production PowerShell functions. All OS observations, process
        # leases, signing checks and task/launch actions in this fixture are local fakes.
        fixture = r'''
$ErrorActionPreference='Stop'
$identity=@{sid='S-1-5-21-12-34-56-1000';session=1}
$config=@{streamer_path='C:\Configured\VirtualDesktop.Streamer.exe';game_executables=@('C:\Fixture\game.exe')}
$system='S-1-5-18'
$now=Get-Date
function Get-CimInstance($className,$Filter) {
    if($className -eq 'Win32_Service'){
        if($Filter -ne "Name='VirtualDesktop.Service.exe'"){throw 'wrong SCM lookup'}
        $script:scmReads++
        if($case -eq 'changed_scm' -and $script:scmReads -gt 1){return $null}
        return $svc
    }
    if($Filter -eq "ProcessId=70"){return $parent}
    if($Filter -eq "ProcessId=71"){return $child}
    if($Filter -like "Name='VirtualDesktop.Streamer.exe'*"){return $child}
    if(-not $Filter){return @($child,$game)}
    throw 'unexpected OS query'
}
function Invoke-CimMethod($InputObject,$MethodName) {
    if($MethodName -ne 'GetOwnerSid'){throw 'wrong method'}
    if($InputObject.ProcessId -eq 70){return @{ReturnValue=$parentCode;Sid=$parentSid}}
    return @{ReturnValue=$ownerCode;Sid=$ownerSid}
}
function Open-VDProcessLifetime($process) {
    if($case -eq 'inaccessible' -or $case -eq 'pid_reused'){throw 'cannot pin original process creation'}
    $pin=[pscustomobject]@{Creation=$process.CreationDate.ToFileTimeUtc()}
    $pin | Add-Member ScriptMethod RequireLive {if($case -eq 'exited'){throw 'exited'}}
    $pin | Add-Member ScriptMethod Dispose {}
    return $pin
}
function Test-VDProcessElevated($processId){return $userElevated}
function Get-AuthenticodeSignature($LiteralPath) {
    $service=$LiteralPath -eq $parent.ExecutablePath
    $cn=if($case -eq 'both_wrong_publisher' -or ($case -eq 'wrong_service_publisher' -and $service)){'Other Company'}else{'Virtual Desktop, Inc'}
    $cert=[pscustomobject]@{cn=$cn;Subject="CN=$cn"}
    $cert | Add-Member ScriptMethod GetNameInfo {param($type,$issuer) $this.cn}
    $valid=if(($case -eq 'service_unsigned' -and $service) -or ($case -eq 'streamer_unsigned' -and !$service)){'NotSigned'}else{'Valid'}
    return @{Status=$valid;SignerCertificate=$cert}
}
function Start-Process {throw 'UNEXPECTED_LAUNCH'}
function Register-ScheduledTask {throw 'UNEXPECTED_LAUNCH'}
function Get-Process {return @{Modules=@()}}
$results=@{}
foreach($case in @('user','user_elevated','system','foreign','session','image','service_name','service_stopped',
        'service_account','parent_pid','parent_image','parent_session','parent_owner','parent_owner_unknown',
        'child_owner_unknown','parent_newer','service_unsigned','streamer_unsigned','both_wrong_publisher',
        'wrong_service_publisher','inaccessible','pid_reused','exited','changed_scm','known_game','unknown_known_game')){
    $script:scmReads=0; $ownerSid=$system;$ownerCode=0;$parentSid=$system;$parentCode=0;$userElevated=$false
    $child=[pscustomobject]@{ProcessId=71;ParentProcessId=70;Name='VirtualDesktop.Streamer.exe';ExecutablePath=$config.streamer_path;SessionId=1;CreationDate=$now}
    $parent=[pscustomobject]@{ProcessId=70;Name='VirtualDesktop.Service.exe';ExecutablePath='C:\Different Folder\VirtualDesktop.Service.exe';SessionId=0;CreationDate=$now.AddMinutes(-2)}
    $svc=[pscustomobject]@{Name='VirtualDesktop.Service.exe';ProcessId=70;StartName='LocalSystem';State='Running';PathName='"C:\Different Folder\VirtualDesktop.Service.exe"'}
    $game=[pscustomobject]@{Name='unrelated.exe';ProcessId=74;ExecutablePath='C:\Fixture\unrelated.exe'}
    switch($case){
        'user' {$ownerSid=$identity.sid}
        'user_elevated' {$ownerSid=$identity.sid;$userElevated=$true}
        'foreign' {$ownerSid='S-1-5-21-12-34-56-999'}
        'session' {$child.SessionId=2}
        'image' {$child.ExecutablePath='C:\Foreign\VirtualDesktop.Streamer.exe'}
        'service_name' {$svc.Name='Imposter.Service.exe'}
        'service_stopped' {$svc.State='Stopped'}
        'service_account' {$svc.StartName='NetworkService'}
        'parent_pid' {$svc.ProcessId=73}
        'parent_image' {$parent.ExecutablePath='C:\Elsewhere\VirtualDesktop.Service.exe'}
        'parent_session' {$parent.SessionId=1}
        'parent_owner' {$parentSid=$identity.sid}
        'parent_owner_unknown' {$parentCode=2}
        'child_owner_unknown' {$ownerCode=2}
        'parent_newer' {$parent.CreationDate=$now.AddMinutes(1)}
        'known_game' {$game.Name='game.exe';$game.ExecutablePath=$config.game_executables[0]}
        'unknown_known_game' {$game.Name='game.exe';$game.ExecutablePath=$null}
    }
    if($case -eq 'known_game' -or $case -eq 'unknown_known_game'){
        try{Stop-Streamer $config $identity;$results[$case]='accepted'}
        catch{$results[$case]=if($_.Exception.Message -like 'Close the *'){ 'protected' }else{'wrong failure'}}
    } else {
        try{$normal=Get-VerifiedStreamerOwner $config $identity $child $false;$results[$case]=$normal}
        catch{$results[$case]='rejected'}
        if($case -in @('user','user_elevated','system')){
            try{$r=Restart-Streamer $config $identity
                $results["restart_$case"]=@($r.normal_user,$r.launch_requested,$r.streamer_owners[0])}
            catch{$results["restart_$case"]='rejected'}
        }
    }
}
# Exercise the newly requested launch branch with every OS task/action mocked.
$case='system'; $script:taskStarted=$false; $script:taskRemoved=$false; $script:scmReads=0
function Get-VDProcesses($config) {if($script:taskStarted){return $child}}
function New-ScheduledTaskAction($Execute,$Argument) {if($Argument -notlike '*-Action Launch*'){throw 'wrong handoff'}; 'action'}
function New-ScheduledTaskPrincipal($UserId,$LogonType,$RunLevel) {
    if($UserId -ne $identity.sid -or $LogonType -ne 'Interactive' -or $RunLevel -ne 'Limited'){throw 'wrong task token'}
    'principal'
}
function New-ScheduledTaskSettingsSet([switch]$AllowStartIfOnBatteries,[switch]$DontStopIfGoingOnBatteries,$ExecutionTimeLimit) {'settings'}
function Register-ScheduledTask($TaskName,$Action,$Principal,$Settings) {}
function Start-ScheduledTask($TaskName) {$script:taskStarted=$true}
function Unregister-ScheduledTask($TaskName,$Confirm) {$script:taskRemoved=$true}
$ConfigPath='fixture-config.json'
$r=Restart-Streamer $config $identity
$results['requested_brokered_restart']=@($r.normal_user,$r.launch_requested,$r.streamer_owners[0],$script:taskRemoved)
$results | ConvertTo-Json -Compress
'''
        result = self.functions(fixture)
        expected = {"user": "interactive_user", "user_elevated": "interactive_user",
                    "system": "service_brokered_system", "restart_user": [True, False, "interactive_user"],
                    "restart_user_elevated": "rejected", "restart_system": [False, False, "service_brokered_system"],
                    "known_game": "protected", "unknown_known_game": "protected",
                    "requested_brokered_restart": [False, True, "service_brokered_system", True]}
        for reason in ("foreign", "session", "image", "service_name", "service_stopped", "service_account",
                       "parent_pid", "parent_image", "parent_session", "parent_owner", "parent_owner_unknown",
                       "child_owner_unknown", "parent_newer", "service_unsigned", "streamer_unsigned",
                       "both_wrong_publisher", "wrong_service_publisher", "inaccessible", "pid_reused", "exited", "changed_scm"):
            expected[reason] = "rejected"
        self.assertEqual(result, expected)


    def test_native_lifetime_rejects_reused_or_exited_process_even_for_exit_code_259(self):
        result = self.functions(r'''
$start=New-Object Diagnostics.ProcessStartInfo
$start.FileName=Join-Path $PSHOME 'powershell.exe'
$start.Arguments='-NoProfile -NonInteractive -Command "[void][Console]::ReadLine(); exit 259"'
$start.UseShellExecute=$false; $start.RedirectStandardInput=$true; $start.CreateNoWindow=$true
$child=[Diagnostics.Process]::Start($start)
$life=$null
try {
    $row=Get-CimInstance Win32_Process -Filter "ProcessId=$($child.Id)"
    $life=Open-VDProcessLifetime $row
    $life.RequireLive()
    $stale=$false
    try { $other=[VdxrLaunchToken]::Pin($child.Id, $life.Creation-100); $other.Dispose() }
    catch { $stale=$true }
    $child.StandardInput.Close()
    if (!$child.WaitForExit(10000)) {throw 'The disposable child did not exit.'}
    $exited=$false
    try { $life.RequireLive() } catch { $exited=$true }
    @{stale=$stale;exited=$exited;code=$child.ExitCode} | ConvertTo-Json -Compress
} finally {
    $child.StandardInput.Close()
    if ($life) { $life.Dispose() }
    $child.Dispose()
}
''', native=True)
        self.assertEqual(result, {"stale": True, "exited": True, "code": 259})

    def test_configuration_normalizes_streamer_before_wmi_path_matching(self):
        result = self.functions(r'''
$ConfigPath=Join-Path $env:ProgramFiles 'VDXR-NR-FG/LaunchTrees/config.json'
function Get-Content { param($LiteralPath,[switch]$Raw,$Encoding) '{"schema":1,"streamer_path":"C:/Apps/./VirtualDesktop.Streamer.exe"}' }
$configuration=Get-Configuration
function Get-CimInstance { param($ClassName,$Filter) [pscustomobject]@{Name='VirtualDesktop.Streamer.exe';ExecutablePath='C:\Apps\VirtualDesktop.Streamer.exe';SessionId=1;ProcessId=123} }
@{path=$configuration.streamer_path;found=@(Get-VDProcesses $configuration).Count}|ConvertTo-Json -Compress
''')
        self.assertEqual(result, {"path": r"C:\Apps\VirtualDesktop.Streamer.exe", "found": 1})

    def test_wrong_current_sid_is_rejected_before_process_inspection(self):
        result = self.functions(r'''
function Get-Process { throw 'Process inspection must not run for another account.' }
$fixtureSid='S-1-5-21-999999-999999-999999-111'
if([Security.Principal.WindowsIdentity]::GetCurrent().User.Value -eq $fixtureSid){$fixtureSid+='1'}
try { Get-Identity ([pscustomobject]@{library=[pscustomobject]@{registry_path=$fixtureSid+'\Software\Fixture'}});throw 'Expected rejection' } catch { @{rejected=$_.Exception.Message -like '*configured interactive user*'}|ConvertTo-Json -Compress }
''')
        self.assertEqual(result, {"rejected": True})

    def test_entra_sid_reaches_current_identity_guard(self):
        result = self.functions(r'''
function Get-Process { throw 'Process inspection must not run for another account.' }
$fixtureSid='S-1-12-1-123-456-789-100'
if([Security.Principal.WindowsIdentity]::GetCurrent().User.Value -eq $fixtureSid){$fixtureSid+='1'}
try { Get-Identity ([pscustomobject]@{library=[pscustomobject]@{registry_path=$fixtureSid+'\Software\Fixture'}});throw 'Expected rejection' } catch { @{rejected=$_.Exception.Message -like '*configured interactive user*'}|ConvertTo-Json -Compress }
''')
        self.assertEqual(result, {"rejected": True})


@unittest.skipUnless(os.name == "nt", "Windows atomic file replacement")
class WindowsFileTests(unittest.TestCase):
    def test_atomic_replacement_uses_only_temporary_cache_file(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / "cache.json"
            cache.write_bytes(b"original")
            trees.WindowsPlatform().write_bytes(cache, b"replacement")
            self.assertEqual(cache.read_bytes(), b"replacement")
            self.assertEqual([p.name for p in Path(directory).iterdir()], ["cache.json"])


if __name__ == "__main__":
    unittest.main()
