"""Read-only live status: frozen wire, sampling, Win32 ownership and scope tests."""
import ctypes
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import Mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
try:
    import vdxr_live_status as live
except ImportError:
    live = None


def fixture(**changes):
    channels = [[0] * 32 for _ in range(3)]
    channels[0][:13] = [2, 0x3153544154534456, 1, 768, 42, 123, 1, 1, 2, 0, 1, 12, 90000]
    channels[1][:22] = [2, 10, 9, 1, 100, 100, 0, 100, 20, 70, 50, 1, 2, 0, 1, 3, 5, 4, 0, 0, 0, 10]
    channels[2][:8] = [2, 20, 15, 10, 5, 1, 10, 2]
    for key, value in changes.items():
        channel, index = map(int, key.split('_'))
        channels[channel][index] = value
    return b''.join(struct.pack('<32Q', *channel) for channel in channels)


class LiveTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(live, 'live status module must exist')

    def parse(self, data=None):
        return live.parse_snapshot(data or fixture(), 42, 123, r'C:\Steam\Game\Game.exe')

    def test_exact_wire_and_identity(self):
        sample = self.parse()
        self.assertEqual(len(fixture()), 768)
        self.assertEqual(sample.identity, (42, 123, 1))
        self.assertEqual(sample.producer['early_consumptions'], 4)
        self.assertEqual(sample.presenter['successful_pc_handoffs'], 20)

    def test_session_rejects_short_version_identity_reserved_odd_ended(self):
        for data in (fixture()[:-1], fixture(**{'0_2': 2}), fixture(**{'0_4': 7}),
                     fixture(**{'0_13': 1}), fixture(**{'0_0': 3}), fixture(**{'0_7': 2}),
                     fixture(**{'0_8': 1}), fixture(**{'0_11': 15})):
            with self.subTest(data=data[:64]):
                self.assertIsNone(self.parse(data))

    def test_independent_channels_reject_unknown_and_out_of_range(self):
        for index, value in ((3, 2), (4, 101), (5, 49), (7, 101), (12, 1), (13, 5),
                             (15, 4), (18, 1 << 32), (20, 7), (22, 1), (0, 0)):
            sample = self.parse(fixture(**{f'1_{index}': value}))
            self.assertIsNone(sample.producer)
            self.assertIsNotNone(sample.presenter)
        self.assertIsNone(self.parse(fixture(**{'2_5': 2})).presenter)
        sample = self.parse(fixture(**{'1_13': (1 << 64) - 1, '2_7': (1 << 64) - 1}))
        self.assertIsNotNone(sample.producer)
        self.assertIsNotNone(sample.presenter)

    def test_bounded_copy_rejects_torn_and_odd(self):
        calls = []
        def torn(offset, length):
            calls.append((offset, length))
            if length == 8:
                return struct.pack('<Q', len(calls) * 2)
            return fixture()[offset:offset + length]
        self.assertIsNone(live.copy_channel(torn, 0))
        self.assertEqual(len(calls), 9)
        calls.clear()
        def odd(offset, length):
            calls.append(1)
            return struct.pack('<Q', 3)
        self.assertIsNone(live.copy_channel(odd, 0))
        self.assertEqual(len(calls), 3)

    def test_first_rates_unknown_then_elapsed_and_no_progress(self):
        tracker = live.RateTracker()
        self.assertIsNone(tracker.update(self.parse(), 10)['pc'])
        rates = tracker.update(self.parse(fixture(**{'2_1': 30, '2_2': 20, '2_4': 10, '1_1': 14})), 12)
        self.assertEqual(rates['pc'], 5)
        self.assertEqual(rates['distinct'], 2.5)
        self.assertEqual(rates['base'], 2)
        self.assertEqual(tracker.update(self.parse(fixture(**{'2_1': 30, '2_2': 20, '2_4': 10, '1_1': 14})), 13)['pc'], 0)

    def test_resets_for_regression_session_missing_elapsed_and_distinct_unknown(self):
        tracker = live.RateTracker()
        tracker.update(self.parse(), 10)
        self.assertIsNone(tracker.update(self.parse(fixture(**{'2_1': 19})), 11)['pc'])
        self.assertIsNone(tracker.update(self.parse(fixture(**{'0_6': 2})), 12)['pc'])
        tracker.update(None, 13)
        self.assertIsNone(tracker.update(self.parse(), 14)['pc'])
        self.assertIsNone(tracker.update(self.parse(), 14)['pc'])
        self.assertIsNone(tracker.update(self.parse(fixture(**{'2_5': 0})), 15)['distinct'])

    def test_distinct_window_rejects_hidden_unknown_handoff_then_recovers(self):
        tracker = live.RateTracker()
        tracker.update(self.parse(), 10)
        gap = tracker.update(self.parse(fixture(**{'2_1': 30, '2_2': 19, '2_4': 8})), 11)
        self.assertEqual(gap['pc'], 10)
        self.assertIsNone(gap['distinct'])
        self.assertIsNone(gap['repeated'])
        recovered = tracker.update(self.parse(fixture(**{'2_1': 40, '2_2': 24, '2_4': 13})), 12)
        self.assertEqual(recovered['distinct'], 5)
        self.assertEqual(recovered['repeated'], 5)
        unavailable = tracker.update(self.parse(fixture(**{'2_5': 0, '2_1': 50, '2_2': 29, '2_4': 18})), 13)
        self.assertIsNone(unavailable['distinct'])
        restored = tracker.update(self.parse(fixture(**{'2_1': 60, '2_2': 34, '2_4': 23})), 14)
        self.assertIsNone(restored['distinct'])
        next_known = tracker.update(self.parse(fixture(**{'2_1': 70, '2_2': 39, '2_4': 28})), 15)
        self.assertEqual(next_known['distinct'], 5)

    def test_effective_requests_use_running_full_path_not_editor_basename(self):
        store = Mock()
        store.load.return_value = ({'FrameGenerationMode': 4, 'FrameGenerationBackend': 0,
                                    'FrameGenerationDepthMode': 1, 'DLSSNR_Enabled': 0}, {})
        requested = live.load_requested(store, r'C:\Meta\Game\Game.exe')
        store.load.assert_called_once_with(r'C:\Meta\Game\Game.exe')
        pending = live.pending_changes(self.parse(), requested)
        self.assertTrue(pending['restart'])
        self.assertTrue(pending['nr'])
        self.assertIsNone(live.load_requested(store, ''))

    def test_default_session_activity_and_explicit_duplicate_paths(self):
        overlay = self.parse()
        game = self.parse(fixture(**{'0_6': 2}))
        sessions = [live.Observation(overlay, {'base': 0, 'pc': 100}, None),
                    live.Observation(game, {'base': 20, 'pc': 50}, None)]
        self.assertEqual(live.choose_session(sessions).snapshot.identity, game.identity)
        self.assertNotEqual(live.session_label(overlay), live.session_label(game))


class FakeAPI:
    def __init__(self, fail=None):
        self.fail = fail
        self.closed = []
        self.unmapped = []
        self.opens = []
    def pids(self): return [42]
    def open_process(self, pid):
        if self.fail == 'open_process': raise OSError('denied')
        return 'process'
    def process_identity(self, process):
        if self.fail == 'identity': raise OSError('denied')
        return 123, r'C:\Neutral\Game.exe'
    def alive(self, process): return self.fail != 'dead'
    def open_mapping(self, name):
        self.opens.append(name)
        if self.fail == 'open_mapping': raise OSError('missing')
        return 'mapping'
    def map_view(self, mapping):
        if self.fail == 'map_view': raise OSError('denied')
        return 'view'
    def copy(self, view, offset, size):
        if self.fail == 'copy': raise OSError('read failed')
        return fixture()[offset:offset + size]
    def unmap(self, view): self.unmapped.append(view)
    def close(self, handle): self.closed.append(handle)


class ReaderTests(unittest.TestCase):
    def setUp(self): self.assertIsNotNone(live, 'live status module must exist')
    def test_reader_mapping_identity_and_closes_every_pass(self):
        api = FakeAPI()
        samples = live.discover(api)
        self.assertEqual(len(samples), 1)
        self.assertEqual(api.opens, [r'Local\VDXR.LiveStatus.v1.42.000000000000007b'])
        self.assertEqual(api.closed, ['mapping', 'process'])
        self.assertEqual(api.unmapped, ['view'])
    def test_failure_paths_close_only_owned_resources(self):
        for fail, handles, views in (('open_process', [], []), ('identity', ['process'], []),
                                    ('dead', ['process'], []), ('open_mapping', ['process'], []),
                                    ('map_view', ['mapping', 'process'], []),
                                    ('copy', ['mapping', 'process'], ['view'])):
            with self.subTest(fail=fail):
                api = FakeAPI(fail)
                self.assertEqual(live.discover(api), [])
                self.assertEqual(api.closed, handles)
                self.assertEqual(api.unmapped, views)

class ReaderBoundaryTests(unittest.TestCase):
    def test_session_replaced_between_channels_is_discarded(self):
        class Restart(FakeAPI):
            def __init__(self):
                super().__init__()
                self.session_copies = 0
            def copy(self, view, offset, size):
                if offset == 0 and size == 256:
                    self.session_copies += 1
                data = fixture(**{'0_6': 2}) if self.session_copies > 1 else fixture()
                return data[offset:offset + size]
        api = Restart()
        self.assertEqual(live.discover(api), [])
        self.assertEqual(api.closed, ['mapping', 'process'])
        self.assertEqual(api.unmapped, ['view'])

    def test_process_exits_or_identity_changes_after_copy(self):
        class Changes(FakeAPI):
            def __init__(self, identity):
                super().__init__()
                self.calls = 0
                self.change_identity = identity
                self.alive_calls = 0
            def process_identity(self, process):
                self.calls += 1
                return (124 if self.change_identity and self.calls > 1 else 123), r'C:\\Neutral\\Game.exe'
            def alive(self, process):
                self.alive_calls += 1
                return self.change_identity or self.alive_calls == 1
        for identity in (True, False):
            api = Changes(identity)
            self.assertEqual(live.discover(api), [])
            self.assertEqual(api.closed, ['mapping', 'process'])

    def test_unavailable_channel_resets_only_its_rates(self):
        tracker = live.RateTracker()
        parse = lambda data: live.parse_snapshot(data, 42, 123, r'C:\\Neutral\\Game.exe')
        tracker.update(parse(fixture()), 1)
        missing = tracker.update(parse(fixture(**{'1_0': 3, '2_1': 25})), 2)
        self.assertIsNone(missing['nr'])
        self.assertEqual(missing['pc'], 5)
        resumed = tracker.update(parse(fixture(**{'2_1': 30})), 3)
        self.assertIsNone(resumed['nr'])
        self.assertEqual(resumed['pc'], 5)


class DisplayTests(unittest.TestCase):
    def test_latest_fg_route_is_shown_without_claiming_display_success(self):
        labels = ('ordinary', 'off', 'priming', 'unavailable', 'ineligible', 'busy', 'repriming')
        for route, label in enumerate(labels):
            with self.subTest(route=route):
                sample = live.parse_snapshot(fixture(**{'1_20': route}), 42, 123, r'C:\Neutral\Game.exe')
                text = live.describe(live.Observation(sample, dict.fromkeys(live.RATE_COUNTERS), None))
                self.assertIn(f'Latest FG route: {label}', text)
        missing = live.parse_snapshot(fixture(**{'1_0': 3}), 42, 123, r'C:\Neutral\Game.exe')
        text = live.describe(live.Observation(missing, dict.fromkeys(live.RATE_COUNTERS), None))
        self.assertIn('Latest FG route unavailable', text)

    def test_stock_neutral_and_missing_rates_not_zero(self):
        self.assertTrue(hasattr(live, 'describe'), 'live card formatter must exist')
        self.assertIn('Stock', live.describe(None, stock=True))
        sample = live.parse_snapshot(fixture(), 42, 123, r'C:\Neutral\Game.exe')
        text = live.describe(live.Observation(sample, dict.fromkeys(live.RATE_COUNTERS), None))
        self.assertIn('PC submissions unavailable', text)
        self.assertIn('NR recent activity unavailable', text)
        self.assertIn('provided to FG', text)
        self.assertIn('finite depth / visual benefit unverified', text)
        self.assertNotIn('0.0/s', text)

    def test_activity_configuration_pending_and_distinct_are_separate(self):
        self.assertTrue(hasattr(live, 'describe'), 'live card formatter must exist')
        sample = live.parse_snapshot(fixture(**{'2_5': 0}), 42, 123, r'C:\Neutral\Game.exe')
        requested = {'FrameGenerationMode': 4, 'FrameGenerationBackend': 1,
                     'FrameGenerationDepthMode': 2, 'DLSSNR_Enabled': 0}
        text = live.describe(live.Observation(sample, {'base': 45, 'nr': 45, 'pc': 90,
                             'distinct': None, 'synthetic': 45, 'prepared': 1, 'used': 0}, requested))
        for expected in ('Effective NR settings: on · coverage 100% · processing 100%',
                         'restart pending', 'next real frame', 'Distinct images unavailable',
                         'Early prepared 5', 'used 4', 'NR active', 'synthetic active'):
            self.assertIn(expected, text)
        self.assertNotIn('headset', text.lower())


@unittest.skipUnless(sys.platform == 'win32', 'Windows private registry APIs')
class NativeProfileTests(unittest.TestCase):
    def setUp(self):
        import uuid
        import winreg
        import vdxr_game_profiles
        self.registry = winreg
        self.base = r'Software\\VDXRLiveStatusTests\\' + uuid.uuid4().hex
        self.store = vdxr_game_profiles.ProfileStore(winreg.HKEY_CURRENT_USER, self.base)
    def tearDown(self):
        def remove_owned(path):
            try:
                with self.registry.OpenKey(self.registry.HKEY_CURRENT_USER, path) as key:
                    children = [self.registry.EnumKey(key, index)
                                for index in range(self.registry.QueryInfoKey(key)[0])]
                for child in children:
                    remove_owned(path + '\\' + child)
                self.registry.DeleteKeyEx(self.registry.HKEY_CURRENT_USER, path,
                                          self.registry.KEY_WOW64_64KEY)
            except FileNotFoundError:
                pass
        remove_owned(self.base)
    def test_store_copy_profiles_and_inherited_requests_are_independent(self):
        steam, meta = r'C:\\NeutralSteam\\Game\\Game.exe', r'C:\\NeutralMeta\\Game\\Game.exe'
        self.store.save(None, {'FrameGenerationMode': 2, 'DLSSNR_Intensity': 83})
        self.store.save(steam, {'FrameGenerationMode': 4, 'DLSSNR_Enabled': 1})
        self.store.save(meta, {'FrameGenerationMode': 2, 'DLSSNR_Enabled': 0})
        editor = live.load_requested(self.store, steam)
        running = live.load_requested(self.store, meta)
        self.assertEqual(editor['FrameGenerationMode'], 4)
        self.assertEqual(running['FrameGenerationMode'], 2)
        self.assertEqual(running['DLSSNR_Intensity'], 83)
        sample = live.parse_snapshot(fixture(), 42, 123, meta)
        self.assertEqual(live.pending_changes(sample, running), {'restart': False, 'nr': True})
        self.assertTrue(live.pending_changes(sample, editor)['restart'])
        self.assertEqual(self.store.load()[0]['FrameGenerationMode'], 2)


class NativeWireFixtureTests(unittest.TestCase):
    @unittest.skipUnless((ROOT / 'bin/early-flow-20261116/live-status-native/native-wire-v1.bin').is_file(),
                         'private real C++ publisher golden fixture not present')
    def test_real_native_publisher_bytes_match_independent_parser(self):
        data = (ROOT / 'bin/early-flow-20261116/live-status-native/native-wire-v1.bin').read_bytes()
        self.assertEqual(len(data), 768)
        pid, creation = struct.unpack_from('<2Q', data, 32)
        sample = live.parse_snapshot(data, pid, creation, r'C:\\Neutral\\NativeFixture.exe')
        self.assertIsNotNone(sample)
        self.assertEqual(sample.session['configured_fg_mode'], 3)
        self.assertEqual(sample.session['graphics_api'], 11)
        self.assertEqual(sample.producer['accepted_base_frames'], 17)
        self.assertEqual(sample.producer['successfully_processed_nr_pairs'], 17)
        self.assertEqual(sample.producer['coverage_percent'], 90)
        self.assertEqual(sample.producer['processing_percent'], 85)
        self.assertEqual(sample.producer['early_preparations'], 12)
        self.assertEqual(sample.producer['early_consumptions'], 11)
        self.assertEqual(sample.presenter['successful_pc_handoffs'], 51)
        self.assertEqual(sample.presenter['distinct_image_handoffs'], 49)
        self.assertEqual(sample.presenter['successful_synthetic_handoffs'], 32)
        self.assertEqual(sample.presenter['repeated_image_handoffs'], 2)


def _publisher_main():
    """Test-only publisher in a disposable process; no runtime DLL/registry."""
    import mmap
    import os
    api = live.Win32API()
    process = api.open_process(os.getpid())
    try:
        creation, _ = api.process_identity(process)
    finally:
        api.close(process)
    name = f'Local\\VDXR.LiveStatus.v1.{os.getpid()}.{creation:016x}'
    section = mmap.mmap(-1, 768, tagname=name, access=mmap.ACCESS_WRITE)
    generation = 1
    def publish(lifecycle=1, sequence=2):
        data = fixture(**{'0_4': os.getpid(), '0_5': creation, '0_6': generation,
                          '0_7': lifecycle, '1_0': sequence})
        section[:] = data
    try:
        publish()
        print('ready', flush=True)
        for command in sys.stdin:
            command = command.strip()
            if command == 'quit':
                break
            if command == 'restart':
                generation += 1
            publish(lifecycle=2 if command == 'end' else 1, sequence=3 if command == 'odd' else 2)
            print('ready', flush=True)
    finally:
        section.close()


@unittest.skipUnless(sys.platform == 'win32' and ctypes.sizeof(ctypes.c_void_p) == 8, 'Windows x64 APIs')
class NativeReaderTests(unittest.TestCase):
    def setUp(self):
        import subprocess
        self.child = subprocess.Popen([sys.executable, '-c',
            'import sys; sys.path.insert(0, sys.argv[1]); from test_live_status import _publisher_main; _publisher_main()',
            str(ROOT / 'scripts/tests')], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True)
        self.assertEqual(self.child.stdout.readline().strip(), 'ready')
        self.api = live.Win32API()
        import os
        self.assertIn(os.getpid(), self.api.pids())
        # Keep real OS operations, bound discovery to the one disposable publisher.
        self.api.pids = lambda: [self.child.pid]
    def tearDown(self):
        if self.child.poll() is None:
            self.child.communicate('quit\n', timeout=5)
        else:
            self.child.communicate(timeout=5)
    def command(self, command):
        self.child.stdin.write(command + '\n')
        self.child.stdin.flush()
        self.assertEqual(self.child.stdout.readline().strip(), 'ready')
    def test_real_cross_process_identity_odd_ended_restart_and_exit(self):
        first, = live.discover(self.api)
        self.assertEqual(first.identity[0], self.child.pid)
        self.assertTrue(ntpath_is_absolute(first.executable))
        self.command('odd')
        odd, = live.discover(self.api)
        self.assertIsNone(odd.producer)
        self.assertIsNotNone(odd.presenter)
        self.command('restart')
        restarted, = live.discover(self.api)
        self.assertNotEqual(first.identity, restarted.identity)
        self.command('end')
        self.assertEqual(live.discover(self.api), [])
        self.child.communicate('quit\n', timeout=5)
        self.assertEqual(live.discover(self.api), [])
    def test_real_missing_mapping_reader_does_not_create_or_leak_handles(self):
        import os
        from ctypes import wintypes
        kernel = self.api.kernel
        get_count = kernel.GetProcessHandleCount
        get_count.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        get_count.restype = wintypes.BOOL
        def count():
            value = wintypes.DWORD()
            self.assertTrue(get_count(wintypes.HANDLE(-1), ctypes.byref(value)))
            return value.value
        before = count()
        for _ in range(100):
            self.assertEqual(len(live.discover(self.api)), 1)
        self.assertEqual(count(), before)
        name = r'Local\VDXR.LiveStatus.v1.0.0000000000000000'
        for _ in range(20):
            with self.assertRaises(OSError):
                self.api.open_mapping(name)
        self.assertEqual(count(), before)
        actual = self.api.pids
        self.api.pids = lambda: [os.getpid()]
        self.assertEqual(live.discover(self.api), [])
        self.api.pids = actual
        self.assertEqual(count(), before)


def ntpath_is_absolute(path):
    import ntpath
    return ntpath.isabs(path)


if __name__ == '__main__':
    unittest.main()
