from pathlib import Path
import sys
import tempfile
import unittest


@unittest.skipUnless(sys.platform == "win32", "Requires Windows file handles")
class LocalFilesTests(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.path = Path(folder.name)
        from vdxr_local_files import is_local_file, local_directory, read_local_bytes

        self.is_local_file = is_local_file
        self.local_directory = local_directory
        self.read_local_bytes = read_local_bytes

    def test_reads_exact_limit_and_uses_binary_mode(self):
        file = self.path / "metadata.txt"
        file.write_bytes(b"a\r\n\x1ab")
        self.assertEqual(self.read_local_bytes(file, 5), b"a\r\n\x1ab")
        self.assertIsNone(self.read_local_bytes(file, 4))
        self.assertTrue(self.is_local_file(file))
        self.assertFalse(self.is_local_file(self.path))

    def test_rejects_nonlocal_and_special_paths(self):
        for file in (Path(r"relative.txt"), Path(r"\\invalid.example\share\manifest.json"),
                     self.path / ".." / "file", self.path / "CON.txt", self.path / "name:stream"):
            with self.subTest(file=file):
                self.assertIsNone(self.read_local_bytes(file, 50))

    def test_rejects_junction_without_following_it(self):
        import _winapi

        target = self.path / "target"
        target.mkdir()
        file = target / "metadata.txt"
        file.write_bytes(b"fixture")
        junction = self.path / "junction"
        _winapi.CreateJunction(str(target), str(junction))
        self.addCleanup(junction.rmdir)
        self.assertIsNone(self.read_local_bytes(junction / file.name, 50))
        self.assertFalse(self.is_local_file(junction / file.name))
        with self.local_directory(junction) as directory:
            self.assertIsNone(directory)

    def test_enumerates_by_handle_and_releases_directory(self):
        manifests = self.path / "manifests"
        manifests.mkdir()
        for name in ("VR.JSON", "other.txt"):
            (manifests / name).write_bytes(b"fixture")
        with self.local_directory(manifests) as directory:
            self.assertIsNotNone(directory)
            self.assertEqual([path.name for path in directory.glob("*.json")], ["VR.JSON"])
            self.assertCountEqual([path.name for path in directory.iterdir()], ["VR.JSON", "other.txt"])
            with self.assertRaises(PermissionError):
                manifests.rename(self.path / "moved")
        manifests.rename(self.path / "moved")
