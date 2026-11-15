"""Offline fixture-only preparation; never writes an installed launch tree."""
import copy
import hashlib
import json
from pathlib import Path, PureWindowsPath
import tempfile
import unittest

try:
    from scripts import prepare_launch_trees as prep
except ImportError:
    prep = None


class PreparationTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(prep, 'offline launch-tree preparer is missing')
        scratch = Path(__file__).resolve().parents[2] / 'bin/stock-launch-switch-20261113/test-fixtures'
        scratch.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='launch-tree-', dir=scratch)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.custom = self.root / 'Custom' / 'Manifests'
        self.custom.mkdir(parents=True)
        self.output = self.root / 'Stock' / 'Manifests'
        self.exists = set()

    def file_exists(self, path):
        return str(PureWindowsPath(path)).casefold() in self.exists

    def add_target(self, path):
        self.exists.add(str(PureWindowsPath(path)).casefold())

    def write(self, name, data):
        # Deliberate noncanonical spacing/UTF-8 distinguishes semantic copying from byte copying.
        path = self.custom / name
        path.write_bytes(('\ufeff' + json.dumps(data, ensure_ascii=False, indent=3) + '\n').encode('utf-8'))
        return path

    def vendor(self, index, changed=False, two_d=False):
        name = f'entry-{index:02d}'
        original = dict(appId=str(1000 + index), canonicalName=name,
                        launchFile=r'Bin\Game.exe', launchParameters='-vr',
                        launchFile2D='', launchParameters2D='',
                        files={'original-packed.dat': {'hash': 'original'}},
                        assetFiles={'cover': {'hash': 'unchanged'}}, version=7)
        self.add_target(PureWindowsPath(r'F:\Software') / name / original['launchFile'])
        mini = copy.deepcopy(original)
        mini['files'] = {'mini-payload.dat': {'hash': 'preserve mini bytes'}}
        if changed:
            mini['launchParameters'] = '/clean-openxr /openxr /wait -vr'
        if two_d:
            original['launchFile2D'] = r'Bin\Flat.exe'
            original['launchParameters2D'] = '-windowed'
            mini['launchFile2D'] = r'Bin\Wrong.exe'
            mini['launchParameters2D'] = '-custom-flat'
            self.add_target(PureWindowsPath(r'F:\Software') / name / original['launchFile2D'])
        self.write(name + '.json', original)
        self.write(name + '.json.mini', mini)
        return original, mini

    def prepare(self):
        return prep.prepare_manifest_tree(self.custom, self.output, r'F:\Software',
                                          path_exists=self.file_exists)

    def test_32_pairs_restore_23_minis_preserve_nine_and_all_custom_bytes(self):
        for index in range(32):
            self.vendor(index, index < 23)
        before = {p.name: p.read_bytes() for p in self.custom.iterdir()}
        report = self.prepare()
        self.assertEqual(report['counts']['vendor_pairs'], 32)
        self.assertEqual(report['counts']['modified_minis'], 23)
        self.assertEqual(report['counts']['unchanged_minis'], 9)
        for name, data in before.items():
            self.assertEqual((self.custom / name).read_bytes(), data)
            if name.endswith('.json') or int(name[6:8]) >= 23:
                self.assertEqual((self.output / name).read_bytes(), data)
        for index in range(23):
            mini = json.loads((self.output / f'entry-{index:02d}.json.mini').read_text(encoding='utf-8-sig'))
            self.assertEqual(mini['launchParameters'], '-vr')
            self.assertEqual(mini['files'], {'mini-payload.dat': {'hash': 'preserve mini bytes'}})
            self.assertEqual(mini['assetFiles'], {'cover': {'hash': 'unchanged'}})
        for receipt in report['source_files']:
            self.assertEqual(hashlib.sha256(Path(receipt['path']).read_bytes()).hexdigest(), receipt['sha256'])

    def test_restore_2d_fields_only_from_full(self):
        original, mini = self.vendor(0, True, True)
        self.prepare()
        stock = json.loads((self.output / 'entry-00.json.mini').read_text(encoding='utf-8-sig'))
        for field in prep.LAUNCH_FIELDS:
            self.assertEqual(stock[field], original[field])
        self.assertEqual(stock['files'], mini['files'])

    def test_missing_optional_field_is_removed_instead_of_made_null(self):
        original, mini = self.vendor(0)
        del original['launchFile2D']
        self.write('entry-00.json', original)
        self.prepare()
        stock = json.loads((self.output / 'entry-00.json.mini').read_text(encoding='utf-8-sig'))
        self.assertNotIn('launchFile2D', stock)

    def generated(self, store=None, parameters=None):
        target = r'F:\Software\entry-local\Bín Directory\Original.exe'
        helper = r'C:\OwnedRoutes\Launcher.exe'
        self.add_target(target)
        self.add_target(helper)
        metadata = {'owner': 'VDXR local catalog', 'originalExecutable': target}
        if store:
            metadata['store'] = store
        record = {'appId': 'local-id', 'canonicalName': 'Fixture (VDXR)',
                  'launchFile': helper, 'launchParameters': parameters or
                  f'/clean-openxr /openxr /wait "{target}" -regenerateinis -hmd=OculusHMD -vr',
                  'x-vdxr-local-entry': metadata}
        self.write('local.json', record)
        return record

    def test_generated_steam_is_omitted_without_touching_custom(self):
        self.generated('Steam')
        before = (self.custom / 'local.json').read_bytes()
        report = self.prepare()
        self.assertEqual(report['counts']['omitted_steam_aliases'], 1)
        self.assertFalse((self.output / 'local.json').exists())
        self.assertEqual((self.custom / 'local.json').read_bytes(), before)

    def test_generated_local_keeps_exact_quoted_unicode_game_tail(self):
        record = self.generated()
        report = self.prepare()
        stock = json.loads((self.output / 'local.json').read_text(encoding='utf-8-sig'))
        self.assertEqual(stock['launchFile'], record['x-vdxr-local-entry']['originalExecutable'])
        self.assertEqual(stock['launchParameters'], ' -regenerateinis -hmd=OculusHMD -vr')
        self.assertEqual(stock['canonicalName'], 'Fixture')
        self.assertEqual(report['counts']['native_local_aliases'], 1)

    def test_ambiguous_or_unrecognized_prefix_is_explicitly_unsupported(self):
        self.generated(parameters='/unknown-before-target "F:\\Software\\entry-local\\Bín Directory\\Original.exe" -vr')
        report = self.prepare()
        self.assertEqual(report['counts']['unsupported_aliases'], 1)
        self.assertFalse((self.output / 'local.json').exists())
        self.assertTrue(report['unsupported_aliases'][0]['reason'])

    def test_only_prefix_injector_flags_are_dropped(self):
        target = r'F:\Software\entry-local\Bín Directory\Original.exe'
        tail = '   -vr /wait "quoted game argument"'
        self.assertEqual(prep.original_argument_tail('/openxr /wait "' + target + '"' + tail, target), tail)

    def test_missing_generated_target_is_unsupported_not_fabricated(self):
        self.generated()
        self.exists.clear()
        report = self.prepare()
        self.assertEqual(report['counts']['unsupported_aliases'], 1)
        self.assertEqual(report['counts']['native_local_aliases'], 0)
        self.assertFalse((self.output / 'local.json').exists())

    def test_asset_scope_rejects_relative_escape_absolute_or_drive_relative_launch(self):
        for launch in [r'..\outside.exe', r'C:\outside.exe', r'C:outside.exe', r'Bin\..\outside.exe']:
            with self.subTest(launch=launch):
                self.vendor(0)
                path = self.custom / 'entry-00.json'
                data = json.loads(path.read_text(encoding='utf-8-sig'))
                data['launchFile'] = launch
                self.write(path.name, data)
                with self.assertRaises(ValueError):
                    self.prepare()
                self.assertFalse(self.output.exists())

    def test_unavailable_original_is_preserved_without_claiming_launchable(self):
        self.vendor(0)
        before = {p.name: p.read_bytes() for p in self.custom.iterdir()}
        self.exists.clear()
        report = self.prepare()
        self.assertEqual(report['counts']['unavailable_vendor_entries'], 1)
        self.assertFalse(report['availability'][0]['launchable'])
        self.assertFalse(report['availability'][0]['targets']['launchFile']['exists'])
        for name, data in before.items():
            self.assertEqual((self.custom / name).read_bytes(), data)
            self.assertEqual((self.output / name).read_bytes(), data)

    def test_unpaired_mini_fails_before_output(self):
        self.vendor(0)
        (self.custom / 'entry-00.json').unlink()
        with self.assertRaises(ValueError):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_output_cannot_rewrite_custom_or_existing_tree(self):
        self.vendor(0)
        with self.assertRaises(ValueError):
            prep.prepare_manifest_tree(self.custom, self.custom, r'F:\Software', path_exists=self.file_exists)
        self.output.mkdir(parents=True)
        with self.assertRaises(ValueError):
            self.prepare()

    def caches(self):
        original = {'ShippingExe': {str(i): (None if i >= 13 else rf'Bin\Original{i}.exe') for i in range(24)},
                    'SteamProductInfos': {'2521800': {'Executable': 'original.exe', 'LaunchType': 1}},
                    'Unrelated': {'retained': True}}
        current = copy.deepcopy(original)
        current['ShippingExe'] = {str(i): rf'C:\OwnedRoutes\wrapper{i}.cmd' for i in range(24)}
        current['ShippingExe']['new-user-entry'] = 'leave-new.exe'
        current['SteamProductInfos']['2521800'] = {'Executable': 'wrapper.exe', 'LaunchType': 9}
        current['SteamProductInfos']['added'] = {'keep': 'new data'}
        return current, original

    def test_24_owned_cache_records_restore_only_owned_values_preserving_new_entries(self):
        current, original = self.caches()
        before = copy.deepcopy(current)
        patches = prep.make_cache_templates(current, [original, copy.deepcopy(original)],
                                             [str(i) for i in range(24)], ['2521800'])
        self.assertEqual(len(patches['stock']['ShippingExe']), 24)
        self.assertEqual(patches['stock']['ShippingExe']['23'], {'present': True, 'value': None})
        stock = prep.apply_cache_template(current, patches['stock'])
        self.assertEqual(stock['ShippingExe']['new-user-entry'], 'leave-new.exe')
        self.assertEqual(stock['SteamProductInfos']['added'], {'keep': 'new data'})
        self.assertEqual(stock['ShippingExe']['0'], original['ShippingExe']['0'])
        self.assertEqual(stock['SteamProductInfos']['2521800'], original['SteamProductInfos']['2521800'])
        self.assertEqual(prep.apply_cache_template(stock, patches['custom']), before)
        self.assertEqual(current, before)

    def test_null_and_absence_are_distinct_in_both_templates(self):
        current, original = self.caches()
        del current['ShippingExe']['0']
        del original['ShippingExe']['1']
        patches = prep.make_cache_templates(current, [original, copy.deepcopy(original)], ['0', '1', '23'], [])
        self.assertEqual(patches['custom']['ShippingExe']['0'], {'present': False})
        self.assertEqual(patches['stock']['ShippingExe']['1'], {'present': False})
        self.assertEqual(patches['stock']['ShippingExe']['23'], {'present': True, 'value': None})

    def test_disagreeing_originals_fail_for_missing_null_or_typed_value(self):
        current, first = self.caches()
        second = copy.deepcopy(first)
        del second['ShippingExe']['23']
        with self.assertRaises(ValueError):
            prep.make_cache_templates(current, [first, second], ['23'], [])
        second = copy.deepcopy(first)
        second['SteamProductInfos']['2521800']['LaunchType'] = True
        with self.assertRaises(ValueError):
            prep.make_cache_templates(current, [first, second], [], ['2521800'])

    def test_two_hashed_originals_are_required(self):
        current, original = self.caches()
        entries = []
        for i in range(2):
            path = self.root / f'backup-{i}.json'
            path.write_text(json.dumps(original), encoding='utf-8')
            entries.append({'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
        inventory = self.root / 'originals.json'
        inventory.write_text(json.dumps({'backups': entries}), encoding='utf-8')
        backups, guards = prep.read_original_backups(inventory)
        self.assertEqual(backups, [original, original])
        self.assertEqual(len(guards), 3)
        Path(entries[1]['path']).write_text('{}', encoding='utf-8')
        with self.assertRaises(ValueError):
            prep.read_original_backups(inventory)

    def bundle_fixture(self):
        self.vendor(0, changed=True)
        current, original = self.caches()
        backups = []
        for index in range(2):
            path = self.root / f'original-{index}.json'
            path.write_text(json.dumps(original), encoding='utf-8')
            backups.append({'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
        inventory = self.root / 'original-inventory.json'
        inventory.write_text(json.dumps({'backups': backups}), encoding='utf-8')
        snapshots = []
        for index in range(2):
            path = self.root / f'cache-{index}.json'
            path.write_text(json.dumps(current), encoding='utf-8')
            snapshots.append({'source': str(path), 'path': rf'C:\Accounts\account{index}\GameSettings.json',
                              'owned_shipping_keys': [str(i) for i in range(24)],
                              'owned_steam_keys': ['2521800']})
        for index in range(24):
            self.add_target(rf'C:\OwnedRoutes\wrapper{index}.cmd')
        sentinel = self.root / 'untouched-source.dat'
        sentinel.write_bytes(b'guarded original bytes')
        spec = {'schema': 1, 'custom_root': 'F:\\', 'stock_root': r'F:\Separate\Stock',
                'custom_manifests': str(self.custom), 'backup_inventory': str(inventory),
                'managed_launcher_roots': [r'C:\OwnedRoutes'], 'allowed_steam_keys': ['2521800'],
                'caches': snapshots, 'source_files': [prep.file_guard(sentinel)],
                'library': {'custom_value': 'F:\\', 'custom_kind': 1, 'registry_path': 'fixture-only'},
                'expected_counts': {'vendor_pairs': 1, 'modified_minis': 1}}
        path = self.root / 'spec.json'
        path.write_text(json.dumps(spec), encoding='utf-8')
        return path, spec, sentinel

    def test_bundle_outputs_backend_schema_and_guards_for_both_accounts(self):
        path, _, _ = self.bundle_fixture()
        result = prep.prepare_bundle(path, self.root / 'prepared', path_exists=self.file_exists)
        self.assertEqual(len(result['caches']), 2)
        self.assertEqual(result['library']['custom_value'], 'F:\\')
        self.assertEqual(result['preparation']['software_junction_target'], r'F:\Software')
        self.assertFalse(result['preparation']['junction_created'])
        self.assertEqual(len(result['preparation']['stock_files']), 2)
        self.assertEqual(len(result['caches'][0]['stock']['ShippingExe']), 24)
        prep.verify_guards(result['source_files'])

    def test_source_guard_and_owned_scope_fail_before_creating_bundle(self):
        path, spec, sentinel = self.bundle_fixture()
        sentinel.write_bytes(b'drift after inventory')
        with self.assertRaises(ValueError):
            prep.prepare_bundle(path, self.root / 'prepared', path_exists=self.file_exists)
        self.assertFalse((self.root / 'prepared').exists())
        spec['source_files'] = [prep.file_guard(sentinel)]
        spec['caches'][0]['owned_shipping_keys'].pop()
        path.write_text(json.dumps(spec), encoding='utf-8')
        with self.assertRaises(ValueError):
            prep.prepare_bundle(path, self.root / 'prepared', path_exists=self.file_exists)
        self.assertFalse((self.root / 'prepared').exists())


if __name__ == '__main__':
    unittest.main()
