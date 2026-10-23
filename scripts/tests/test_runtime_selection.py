"""Runtime selection validates real files but writes only an isolated registry."""
import ctypes
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from scripts.tests.test_fg_controls import Registry

try:
    import vdxr_runtime_selection as runtime
except ModuleNotFoundError:
    runtime = None


class RuntimeSelectionTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(runtime, "Missing runtime selection support")
        self.registry = Registry()
        patch = mock.patch.object(runtime, "winreg", self.registry)
        patch.start()
        self.addCleanup(patch.stop)
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.host = Path(temporary.name) / "virtualdesktop-openxr.dll"
        self.host.write_bytes(b"test fixture; never load as a DLL")
        self.manifest = self.host.with_name("CUSTOM-RUNTIME.json")
        self.metadata = {"schema": 1, "self_redirect_guard": True,
                         "host_sha256": hashlib.sha256(self.host.read_bytes()).hexdigest(),
                         "source_revision": "test-fixture"}
        self.save_manifest()

    def save_manifest(self):
        self.manifest.write_text(json.dumps(self.metadata), encoding="utf-8")

    def managed(self, redirected=False):
        values = {runtime.CUSTOM_PATH_VALUE_NAME: (str(self.host), self.registry.REG_SZ),
                  "unrelated": (42, self.registry.REG_DWORD)}
        if redirected:
            values[runtime.REDIRECT_VALUE_NAME] = (str(self.host), self.registry.REG_SZ)
        self.registry.keys[runtime.RUNTIME_REGISTRY_PATH] = values
        return values

    def test_missing_key_and_missing_redirect_are_read_only_stock(self):
        self.assertEqual(runtime.read_selection(), "Stock VDXR")
        self.assertNotIn(runtime.RUNTIME_REGISTRY_PATH, self.registry.keys)
        values = self.managed()
        self.assertEqual(runtime.read_selection(), "Stock VDXR")
        self.assertIn(runtime.CUSTOM_PATH_VALUE_NAME, values)
        self.assertEqual(self.registry.writes, [])

    def test_valid_managed_redirect_is_custom_without_loading_code(self):
        self.managed(redirected=True)
        with mock.patch.object(ctypes, "WinDLL", side_effect=AssertionError("must not load DLL")):
            self.assertEqual(runtime.read_selection(), "Custom VDXR")
        self.assertEqual(self.registry.writes, [])

    def test_unmanaged_override_is_honestly_unknown(self):
        values = self.managed()
        values[runtime.REDIRECT_VALUE_NAME] = (str(self.host.with_name("other.dll")), 1)
        self.assertEqual(runtime.read_selection(), "Other/unknown override")
        del values[runtime.CUSTOM_PATH_VALUE_NAME]
        self.assertEqual(runtime.read_selection(), "Other/unknown override")

    def test_custom_switch_writes_only_redirect_in_64bit_runtime_key(self):
        values = self.managed()
        nr_before = self.registry.values.copy()
        runtime.write_selection("Custom VDXR")
        self.assertEqual(values[runtime.REDIRECT_VALUE_NAME], (str(self.host), 1))
        self.assertEqual(self.registry.write_paths,
                         [(runtime.RUNTIME_REGISTRY_PATH, "redirect_to", str(self.host), 1)])
        self.assertEqual(self.registry.values, nr_before)
        self.assertEqual(values["unrelated"], (42, 4))
        self.assertEqual(values[runtime.CUSTOM_PATH_VALUE_NAME], (str(self.host), 1))
        self.assertTrue(all(access & 256 for _, access in self.registry.access))

    def test_stock_deletes_only_redirect_and_retains_managed_path(self):
        values = self.managed(redirected=True)
        runtime.write_selection("Stock VDXR")
        self.assertNotIn(runtime.REDIRECT_VALUE_NAME, values)
        self.assertEqual(values[runtime.CUSTOM_PATH_VALUE_NAME], (str(self.host), 1))
        self.assertEqual(values["unrelated"], (42, 4))
        self.assertEqual(self.registry.write_paths, [(runtime.RUNTIME_REGISTRY_PATH, "redirect_to", None, None)])

    def test_stock_is_idempotent_without_creating_missing_key(self):
        runtime.write_selection("Stock VDXR")
        self.assertNotIn(runtime.RUNTIME_REGISTRY_PATH, self.registry.keys)
        self.managed()
        runtime.write_selection("Stock VDXR")
        self.assertEqual(self.registry.writes, [])

    def test_malformed_redirects_raise_without_writing(self):
        values = self.managed()
        for value in [(7, 4), ("", 1), ("relative.dll", 1), ("bad\x00path", 1)]:
            with self.subTest(value=value):
                values[runtime.REDIRECT_VALUE_NAME] = value
                with self.assertRaises(ValueError):
                    runtime.read_selection()
        self.assertEqual(self.registry.writes, [])

    def test_invalid_managed_paths_cannot_enable_custom(self):
        values = self.managed()
        for value in [(str(self.host), 4), ("relative/virtualdesktop-openxr.dll", 1),
                      (str(self.host.with_name("arbitrary.dll")), 1),
                      (str(self.host.parent / "missing" / self.host.name), 1)]:
            with self.subTest(value=value):
                values[runtime.CUSTOM_PATH_VALUE_NAME] = value
                with self.assertRaises((ValueError, OSError)):
                    runtime.write_selection("Custom VDXR")
        self.assertEqual(self.registry.writes, [])

    def test_missing_managed_metadata_cannot_enable_custom(self):
        with self.assertRaises((ValueError, OSError)):
            runtime.write_selection("Custom VDXR")
        self.managed()
        self.manifest.unlink()
        with self.assertRaises((ValueError, OSError)):
            runtime.write_selection("Custom VDXR")
        self.assertEqual(self.registry.writes, [])

    def test_manifest_schema_guard_and_hash_are_required(self):
        self.managed()
        original = self.metadata.copy()
        for field, value in [("schema", 2), ("schema", True), ("self_redirect_guard", False),
                             ("self_redirect_guard", 1), ("host_sha256", "0" * 64),
                             ("host_sha256", None)]:
            with self.subTest(field=field, value=value):
                self.metadata = {**original, field: value}
                self.save_manifest()
                with self.assertRaises(ValueError):
                    runtime.write_selection("Custom VDXR")
        for text in ("not JSON", "[]", "null"):
            self.manifest.write_text(text)
            with self.assertRaises(ValueError):
                runtime.write_selection("Custom VDXR")
        self.assertEqual(self.registry.writes, [])

    def test_changed_host_cannot_appear_as_verified_custom(self):
        self.managed(redirected=True)
        self.host.write_bytes(b"changed since installer recorded the manifest")
        with self.assertRaises(ValueError):
            runtime.read_selection()

    def test_permission_errors_propagate_without_settings_writes(self):
        self.managed()
        self.registry.denied = True
        with self.assertRaises(PermissionError):
            runtime.write_selection("Custom VDXR")
        with self.assertRaises(PermissionError):
            runtime.write_selection("Stock VDXR")
        with mock.patch.object(self.registry, "OpenKey", side_effect=PermissionError("fixture read denial")):
            with self.assertRaises(PermissionError):
                runtime.read_selection()
        self.assertEqual(self.registry.writes, [])

    def test_failed_write_or_delete_readback_is_not_success(self):
        values = self.managed()
        with mock.patch.object(self.registry, "SetValueEx", return_value=None):
            with self.assertRaises(OSError):
                runtime.write_selection("Custom VDXR")
        values[runtime.REDIRECT_VALUE_NAME] = (str(self.host), 1)
        with mock.patch.object(self.registry, "DeleteValue", return_value=None):
            with self.assertRaises(OSError):
                runtime.write_selection("Stock VDXR")

    def test_unknown_choices_are_rejected_without_writing(self):
        for label in ("Other/unknown override", "custom", ""):
            with self.assertRaises(ValueError):
                runtime.write_selection(label)
        self.assertEqual(self.registry.writes, [])


if __name__ == "__main__":
    unittest.main()
