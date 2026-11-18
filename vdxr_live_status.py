"""Read-only Windows live status v1. No runtime loading or settings writes.

All native resources are owned for one discovery pass. The parser sees only
copied bytes, and invalid/torn channels never become zero counters.
"""
import ctypes
from ctypes import wintypes
from dataclasses import dataclass
import queue
import struct
import threading
import time

MAGIC = 0x3153544154534456
TOTAL_BYTES = 768
CHANNEL_BYTES = 256
UNKNOWN = (1 << 64) - 1
PRODUCER = ('accepted_base_frames', 'successfully_processed_nr_pairs', 'latest_nr_enabled',
            'coverage_percent', 'processing_percent', 'style', 'intensity_percent',
            'local_tone_percent', 'local_structure_percent', 'skin_structure_percent',
            'fg_context_initialized', 'effective_fg_mode', 'effective_backend',
            'effective_depth_mode', 'submitted_guide_mask', 'early_preparations',
            'early_consumptions', 'latest_prepare_hresult', 'latest_capture_hresult',
            'route_reason', 'last_base_serial')
PRESENTER = ('successful_pc_handoffs', 'distinct_image_handoffs', 'successful_synthetic_handoffs',
             'repeated_image_handoffs', 'distinct_identity_available', 'last_source_serial', 'last_role')
SESSION = ('magic', 'schema', 'total_bytes', 'process_id', 'creation_filetime', 'session_id',
           'lifecycle', 'configured_fg_mode', 'configured_backend', 'configured_depth_mode',
           'graphics_api', 'refresh_millihz')


@dataclass(frozen=True)
class Snapshot:
    executable: str
    session: dict
    producer: dict | None
    presenter: dict | None

    @property
    def identity(self):
        return tuple(self.session[name] for name in ('process_id', 'creation_filetime', 'session_id'))


@dataclass(frozen=True)
class Observation:
    snapshot: Snapshot
    rates: dict
    requested: dict | None


def _channel(data, names, allowed):
    if len(data) != CHANNEL_BYTES:
        return None
    words = struct.unpack('<32Q', data)
    if not words[0] or words[0] & 1 or any(words[len(names) + 1:]):
        return None
    values = dict(zip(names, words[1:]))
    if any(values[name] not in choices for name, choices in allowed.items()):
        return None
    return values


def parse_snapshot(data, pid, creation, executable):
    """Parse independent, already sequence-validated copied channels."""
    if len(data) != TOTAL_BYTES:
        return None
    session = _channel(data[:256], SESSION, {
        'magic': (MAGIC,), 'schema': (1,), 'total_bytes': (768,), 'process_id': (pid,),
        'creation_filetime': (creation,), 'lifecycle': (1,), 'configured_fg_mode': (0, 2, 3, 4),
        'configured_backend': (0, 1), 'configured_depth_mode': (0, 1, 2),
        'graphics_api': (0, 11, 12, 13, 14)})
    if not session or not session['session_id']:
        return None
    allowed = {name: range(101) for name in ('coverage_percent', 'intensity_percent',
               'local_tone_percent', 'local_structure_percent', 'skin_structure_percent')}
    allowed.update(latest_nr_enabled=(0, 1), processing_percent=range(50, 101), style=range(3),
                   fg_context_initialized=(0, 1), effective_fg_mode=(0, 2, 3, 4),
                   effective_backend=(0, 1, UNKNOWN), effective_depth_mode=(0, 1, 2, UNKNOWN),
                   submitted_guide_mask=range(4), route_reason=range(7))
    producer = _channel(data[256:512], PRODUCER, allowed)
    if producer and any(producer[name] != UNKNOWN and producer[name] > 0xffffffff
                        for name in ('latest_prepare_hresult', 'latest_capture_hresult')):
        producer = None
    presenter = _channel(data[512:], PRESENTER, {'distinct_identity_available': (0, 1),
                                                'last_role': (0, 1, 2, 3, UNKNOWN)})
    return Snapshot(executable, session, producer, presenter)


def copy_channel(read, offset):
    """At most three bounded sequence/copy/sequence attempts; no live ctypes view."""
    for _ in range(3):
        before = read(offset, 8)
        if len(before) != 8:
            return None
        sequence, = struct.unpack('<Q', before)
        if not sequence or sequence & 1:
            continue
        payload = read(offset, CHANNEL_BYTES)
        after = read(offset, 8)
        if len(payload) == CHANNEL_BYTES and before == after and payload[:8] == before:
            return bytes(payload)
    return None


class Win32API:
    """Explicit read-only OpenFileMappingW API; never creates missing sections."""
    def __init__(self):
        if ctypes.sizeof(ctypes.c_void_p) != 8:
            raise OSError('Live status requires Windows x64')
        self.kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        definitions = {
            'CreateToolhelp32Snapshot': ([wintypes.DWORD, wintypes.DWORD], wintypes.HANDLE),
            'Process32FirstW': ([wintypes.HANDLE, ctypes.c_void_p], wintypes.BOOL),
            'Process32NextW': ([wintypes.HANDLE, ctypes.c_void_p], wintypes.BOOL),
            'OpenProcess': ([wintypes.DWORD, wintypes.BOOL, wintypes.DWORD], wintypes.HANDLE),
            'GetProcessTimes': ([wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4, wintypes.BOOL),
            'QueryFullProcessImageNameW': ([wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                          ctypes.POINTER(wintypes.DWORD)], wintypes.BOOL),
            'GetExitCodeProcess': ([wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)], wintypes.BOOL),
            'OpenFileMappingW': ([wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR], wintypes.HANDLE),
            'MapViewOfFile': ([wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
                               ctypes.c_size_t], ctypes.c_void_p),
            'UnmapViewOfFile': ([ctypes.c_void_p], wintypes.BOOL),
            'CloseHandle': ([wintypes.HANDLE], wintypes.BOOL),
        }
        for name, (args, result) in definitions.items():
            function = getattr(self.kernel, name)
            function.argtypes, function.restype = args, result

    def pids(self):
        class ProcessEntry(ctypes.Structure):
            _fields_ = [('dwSize', wintypes.DWORD), ('cntUsage', wintypes.DWORD),
                        ('th32ProcessID', wintypes.DWORD), ('th32DefaultHeapID', ctypes.c_size_t),
                        ('th32ModuleID', wintypes.DWORD), ('cntThreads', wintypes.DWORD),
                        ('th32ParentProcessID', wintypes.DWORD), ('pcPriClassBase', wintypes.LONG),
                        ('dwFlags', wintypes.DWORD), ('szExeFile', wintypes.WCHAR * 260)]
        handle = self.kernel.CreateToolhelp32Snapshot(2, 0)
        if handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            entry = ProcessEntry()
            entry.dwSize = ctypes.sizeof(entry)
            result = []
            ok = self.kernel.Process32FirstW(handle, ctypes.byref(entry))
            while ok:
                result.append(entry.th32ProcessID)
                ok = self.kernel.Process32NextW(handle, ctypes.byref(entry))
            if ctypes.get_last_error() != 18:  # ERROR_NO_MORE_FILES
                raise ctypes.WinError(ctypes.get_last_error())
            return result
        finally:
            self.close(handle)

    def open_process(self, pid):
        handle = self.kernel.OpenProcess(0x1000, False, pid)  # QUERY_LIMITED_INFORMATION
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())
        return handle

    def process_identity(self, process):
        creation, exit_time, kernel_time, user_time = (wintypes.FILETIME() for _ in range(4))
        if not self.kernel.GetProcessTimes(process, ctypes.byref(creation), ctypes.byref(exit_time),
                                           ctypes.byref(kernel_time), ctypes.byref(user_time)):
            raise ctypes.WinError(ctypes.get_last_error())
        path, length = ctypes.create_unicode_buffer(32768), wintypes.DWORD(32768)
        if not self.kernel.QueryFullProcessImageNameW(process, 0, path, ctypes.byref(length)):
            raise ctypes.WinError(ctypes.get_last_error())
        return (creation.dwHighDateTime << 32) | creation.dwLowDateTime, path.value

    def alive(self, process):
        code = wintypes.DWORD()
        if not self.kernel.GetExitCodeProcess(process, ctypes.byref(code)) or code.value != 259:
            return False
        # Exit code 259 alone is ambiguous if the application actually exits with it.
        times = [wintypes.FILETIME() for _ in range(4)]
        return bool(self.kernel.GetProcessTimes(process, *(ctypes.byref(value) for value in times))) and not (
            times[1].dwLowDateTime or times[1].dwHighDateTime)

    def open_mapping(self, name):
        handle = self.kernel.OpenFileMappingW(4, False, name)  # FILE_MAP_READ
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())
        return handle

    def map_view(self, mapping):
        address = self.kernel.MapViewOfFile(mapping, 4, 0, 0, TOTAL_BYTES)
        if not address:
            raise ctypes.WinError(ctypes.get_last_error())
        return address

    def copy(self, view, offset, size):
        return ctypes.string_at(view + offset, size)

    def unmap(self, view):
        self.kernel.UnmapViewOfFile(view)

    def close(self, handle):
        self.kernel.CloseHandle(handle)


def discover(api, stop=None):
    samples = []
    for pid in api.pids():
        if stop is not None and stop.is_set():
            break
        process = mapping = view = None
        try:
            process = api.open_process(pid)
            creation, executable = api.process_identity(process)
            if not api.alive(process):
                continue
            mapping = api.open_mapping(f'Local\\VDXR.LiveStatus.v1.{pid}.{creation:016x}')
            view = api.map_view(mapping)
            read = lambda offset, size: api.copy(view, offset, size)
            session = copy_channel(read, 0)
            if session is None:
                continue
            producer = copy_channel(read, 256)
            presenter = copy_channel(read, 512)
            # Session replacement/ending cannot mix producer/presenter generations.
            if session != copy_channel(read, 0) or not api.alive(process):
                continue
            if api.process_identity(process) != (creation, executable):
                continue
            sample = parse_snapshot(session + (producer or bytes(256)) + (presenter or bytes(256)),
                                    pid, creation, executable)
            if sample is not None:
                samples.append(sample)
        except (OSError, ValueError, struct.error):
            pass  # Missing/inaccessible publishers are a neutral unavailable capability.
        finally:
            if view is not None:
                api.unmap(view)
            if mapping is not None:
                api.close(mapping)
            if process is not None:
                api.close(process)
    return samples


RATE_COUNTERS = {'base': ('producer', 'accepted_base_frames'),
                 'nr': ('producer', 'successfully_processed_nr_pairs'),
                 'prepared': ('producer', 'early_preparations'),
                 'used': ('producer', 'early_consumptions'),
                 'pc': ('presenter', 'successful_pc_handoffs'),
                 'distinct': ('presenter', 'distinct_image_handoffs'),
                 'synthetic': ('presenter', 'successful_synthetic_handoffs'),
                 'repeated': ('presenter', 'repeated_image_handoffs')}


class RateTracker:
    def __init__(self):
        self.previous = None

    def update(self, sample, now):
        rates = dict.fromkeys(RATE_COUNTERS)
        previous = self.previous
        self.previous = (sample, now) if sample else None
        if not sample or not previous or previous[0].identity != sample.identity:
            return rates
        old, before = previous
        elapsed = now - before
        if elapsed <= 0:
            return rates
        for channel in ('producer', 'presenter'):
            current, earlier = getattr(sample, channel), getattr(old, channel)
            counters = [(key, field) for key, (group, field) in RATE_COUNTERS.items() if group == channel]
            if current is None or earlier is None or any(current[field] < earlier[field] for _, field in counters):
                continue
            for key, field in counters:
                if key in ('distinct', 'repeated'):
                    known = current['distinct_identity_available'] and earlier['distinct_identity_available']
                    total_delta = current['successful_pc_handoffs'] - earlier['successful_pc_handoffs']
                    partition_delta = (current['distinct_image_handoffs'] - earlier['distinct_image_handoffs']
                                       + current['repeated_image_handoffs'] - earlier['repeated_image_handoffs'])
                    # An unknown-identity End between polls can be hidden by a known latest End.
                    if not known or partition_delta != total_delta:
                        continue
                rates[key] = (current[field] - earlier[field]) / elapsed
        return rates


def load_requested(store, executable):
    if not executable:
        return None
    try:
        values, _ = store.load(executable)  # Full identity, independent of editor selection.
        return values
    except (OSError, ValueError):
        return None


def pending_changes(sample, requested):
    if requested is None:
        return {'restart': None, 'nr': None}
    session, producer = sample.session, sample.producer
    restart = any(requested.get(setting) != session[field] for setting, field in (
        ('FrameGenerationMode', 'configured_fg_mode'), ('FrameGenerationBackend', 'configured_backend'),
        ('FrameGenerationDepthMode', 'configured_depth_mode')))
    nr_fields = (('DLSSNR_Enabled', 'latest_nr_enabled'), ('FoveationSize', 'coverage_percent'),
                 ('DLSSNR_ProcessingResolution', 'processing_percent'), ('DLSSNR_Style', 'style'),
                 ('DLSSNR_Intensity', 'intensity_percent'), ('DLSSNR_LocalToneStrength', 'local_tone_percent'),
                 ('DLSSNR_LocalStructureStrength', 'local_structure_percent'),
                 ('DLSSNR_SkinStructureStrength', 'skin_structure_percent'))
    nr = None if producer is None else any(requested[name] != producer[field]
               for name, field in nr_fields if name in requested)
    return {'restart': restart, 'nr': nr}


def session_label(sample):
    pid, _, session = sample.identity
    return f'{sample.executable}  · PID {pid} · session {session}'


def choose_session(observations):
    # Recent accepted app frames outrank presentation-only activity (e.g. overlay).
    return max(observations, key=lambda item: (item.rates.get('base') or 0,
                                               item.rates.get('pc') or 0), default=None)


class StatusWorker:
    """One daemon discovers off Tk; bounded mailbox, no Tk calls from this thread."""
    def __init__(self, store, api=None):
        self.store, self.api = store, api
        self.stop_event = threading.Event()
        self.results = queue.Queue(maxsize=1)
        self.thread = threading.Thread(target=self._run, name='VDXR live status', daemon=True)
        self.thread.start()

    def _run(self):
        trackers = {}
        try:
            api = self.api or Win32API()
        except OSError:
            self._publish(([], 'Live telemetry unavailable'))
            return
        while not self.stop_event.is_set():
            try:
                snapshots = discover(api, self.stop_event)
                now = time.monotonic()
                present = {snapshot.identity for snapshot in snapshots}
                trackers = {identity: tracker for identity, tracker in trackers.items() if identity in present}
                observations = [Observation(snapshot,
                    trackers.setdefault(snapshot.identity, RateTracker()).update(snapshot, now),
                    load_requested(self.store, snapshot.executable)) for snapshot in snapshots]
                self._publish((observations, 'No custom live publisher detected'))
            except (OSError, ValueError):
                trackers.clear()
                self._publish(([], 'Live discovery unavailable'))
            self.stop_event.wait(1)

    def _publish(self, result):
        try:
            self.results.get_nowait()
        except queue.Empty:
            pass
        self.results.put_nowait(result)

    def latest(self):
        try:
            return self.results.get_nowait()
        except queue.Empty:
            return None

    def stop(self):
        self.stop_event.set()  # Do not join a worker on Tk's close path.


def describe(observation, stock=False):
    """Compact display wording keeps observed work separate from saved requests."""
    if observation is None:
        return ('Stock selected · custom live telemetry unavailable' if stock else
                'No custom live publisher detected · live activity unavailable')
    sample, rates, requested = observation.snapshot, observation.rates, observation.requested
    session, producer = sample.session, sample.producer
    def rate(label, key):
        value = rates.get(key)
        return (f'{label} unavailable' if value is None else
                f'{label}: no new submissions' if value == 0 else f'{label} {value:.1f}/s')
    def activity(label, key):
        value = rates.get(key)
        return (f'{label} recent activity unavailable' if value is None else
                f'{label} active' if value > 0 else f'{label}: no new work')
    api = {0: 'API unavailable', 11: 'D3D11', 12: 'D3D12', 13: 'Vulkan', 14: 'OpenGL'}[session['graphics_api']]
    refresh = session['refresh_millihz']
    refresh_label = f'{refresh / 1000:g} Hz reported display refresh' if refresh else 'Display refresh unavailable'
    backend = ('NVIDIA', 'FidelityFX')[session['configured_backend']]
    depth = ('Standard', 'Depth assist', 'Reconstruction')[session['configured_depth_mode']]
    fg = session['configured_fg_mode']
    configured = f'Configured FG {fg if fg else "off"} · {backend} · {depth}'
    if producer is None:
        observed = 'Effective NR/FG settings unavailable'
        guides = 'Guides provided to FG unavailable'
        early = 'Early prepared / used unavailable'
        route = 'Latest FG route unavailable'
    else:
        observed = (f'Effective NR settings: {"on" if producer["latest_nr_enabled"] else "off"}'
                    f' · coverage {producer["coverage_percent"]}% · processing {producer["processing_percent"]}%'
                    f' · FG context {"ready" if producer["fg_context_initialized"] else "not ready"}'
                    f' · effective FG {producer["effective_fg_mode"] or "off"}'
                    f' / { {0: "NVIDIA", 1: "FidelityFX"}.get(producer["effective_backend"], "backend unavailable")}'
                    f' / { {0: "Standard", 1: "Depth assist", 2: "Reconstruction"}.get(producer["effective_depth_mode"], "depth mode unavailable")}')
        mask = producer['submitted_guide_mask']
        guides = 'Guides provided to FG: ' + ('none', 'left-eye depth only (partial)', 'right-eye depth only (partial)', 'stereo depth')[mask]
        early = f'Early prepared {producer["early_preparations"]} · used {producer["early_consumptions"]}'
        route = 'Latest FG route: ' + ('ordinary', 'off', 'priming', 'unavailable',
                                      'ineligible', 'busy', 'repriming')[producer['route_reason']]
    pending = pending_changes(sample, requested)
    if requested is None:
        requested_label = 'Running EXE saved requests unavailable'
    else:
        requested_label = (f'Saved running EXE: NR {"on" if requested.get("DLSSNR_Enabled") else "off"}'
                           f' · FG {requested.get("FrameGenerationMode", "unavailable") or "off"}')
        requested_label += ' · FG restart pending' if pending['restart'] else ' · FG startup request matches'
        if pending['nr']:
            requested_label += ' · NR pending next real frame'
    return '\n'.join((f'{api} · {refresh_label} · read-only', configured,
        observed, f'{activity("NR", "nr")} · {activity("synthetic", "synthetic")}',
        f'{rate("App accepted", "base")} · {rate("PC submissions", "pc")}',
        f'{rate("Distinct images", "distinct")} · {early} · {route}',
        f'{guides} · finite depth / visual benefit unverified', requested_label))
