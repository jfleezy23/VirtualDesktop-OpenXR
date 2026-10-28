"""Shared Controls profile schema; runtime peer is game_profiles.h.

Profiles are DWORD overrides below Profiles/<FNV1a-64 of normalized UTF-16LE
executable path>. ExecutablePath metadata guards hash collisions. No launcher
integration or temporary global rewrites are needed.
"""
import ctypes
import ntpath
import winreg

REGISTRY_PATH = r"SOFTWARE\Virtual Desktop, Inc.\OpenXR"
DEFAULTS = {
    "DLSSNR_Enabled": 0, "DLSSNR_Style": 0, "DLSSNR_Intensity": 100,
    "DLSSNR_LocalToneStrength": 20, "DLSSNR_LocalStructureStrength": 70,
    "DLSSNR_SkinStructureStrength": 50, "FoveationSize": 100,
    "DLSSNR_ProcessingResolution": 100, "FrameGenerationMode": 0,
    "FrameGenerationBackend": 0,
}


def normalize_executable(path):
    path = str(path).replace("/", "\\")
    if path[:8].lower() == "\\\\?\\unc\\":
        path = "\\\\" + path[8:]
    elif path.startswith("\\\\?\\"):
        path = path[4:]
    drive, tail = ntpath.splitdrive(path)
    if not drive or not tail.startswith("\\"):
        raise ValueError("Select an absolute executable path.")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    full_path = kernel.GetFullPathNameW
    full_path.argtypes = (ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_wchar_p, ctypes.c_void_p)
    full_path.restype = ctypes.c_uint32
    size = full_path(path, 0, None, None)
    if not size:
        raise ctypes.WinError(ctypes.get_last_error())
    buffer = ctypes.create_unicode_buffer(size)
    if not 0 < full_path(path, size, buffer, None) < size:
        raise OSError("Executable path normalization failed")
    lower = kernel.LCMapStringEx
    lower.argtypes = (ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_wchar_p, ctypes.c_int,
                      ctypes.c_wchar_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ssize_t)
    lower.restype = ctypes.c_int
    length = lower("", 0x100, buffer.value, -1, None, 0, None, None, 0)
    result = ctypes.create_unicode_buffer(length)
    if not length or not lower("", 0x100, buffer.value, -1, result, length, None, None, 0):
        raise ctypes.WinError(ctypes.get_last_error())
    return result.value


def profile_id(executable):
    value = 14695981039346656037
    for byte in normalize_executable(executable).encode("utf-16-le"):
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return f"{value:016x}"


def validate(name, value, kind=winreg.REG_DWORD):
    if name not in DEFAULTS or kind != winreg.REG_DWORD or type(value) is not int:
        raise ValueError(f"Invalid {name}: an integer DWORD is required.")
    allowed = ((0, 2, 3, 4) if name == "FrameGenerationMode" else
               (0, 1) if name in ("DLSSNR_Enabled", "FrameGenerationBackend") else
               range(3) if name == "DLSSNR_Style" else
               range(50, 101) if name == "DLSSNR_ProcessingResolution" else range(101))
    if value not in allowed:
        raise ValueError(f"Invalid {name}: value is outside its supported range.")
    return value


class ProfileStore:
    def __init__(self, root=winreg.HKEY_LOCAL_MACHINE, base=REGISTRY_PATH):
        self.root = root
        self.base = base

    def path(self, executable):
        return self.base if executable is None else self.base + "\\Profiles\\" + profile_id(executable)

    def _read(self, path, name):
        try:
            with winreg.OpenKey(self.root, path, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
                return winreg.QueryValueEx(key, name)
        except FileNotFoundError:
            return None

    def _check_identity(self, executable, *, allow_missing_metadata=False):
        if executable is None:
            return
        path = self.path(executable)
        try:
            with winreg.OpenKey(self.root, path, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY):
                pass
        except FileNotFoundError:
            return
        stored = self._read(path, "ExecutablePath")
        if stored is None and allow_missing_metadata:
            return
        if stored != (normalize_executable(executable), winreg.REG_SZ):
            raise ValueError("Profile executable identity is missing or mismatched; profile rejected.")

    def load(self, executable=None):
        self._check_identity(executable)
        values, sources = {}, {}
        for name, default in DEFAULTS.items():
            stored = self._read(self.path(executable), name)
            source = "profile" if executable is not None else "global"
            if stored is None and executable is not None:
                stored, source = self._read(self.base, name), "global"
            values[name] = default if stored is None else validate(name, *stored)
            sources[name] = "default" if stored is None else source
        return values, sources

    def save(self, executable, values):
        # Validate the whole draft before opening a writable key.
        for name, value in values.items():
            validate(name, value)
        self._check_identity(executable)
        with winreg.CreateKeyEx(self.root, self.path(executable), 0,
                                winreg.KEY_SET_VALUE | winreg.KEY_WOW64_64KEY) as key:
            if executable is not None:
                winreg.SetValueEx(key, "ExecutablePath", 0, winreg.REG_SZ, normalize_executable(executable))
            for name, value in values.items():
                winreg.SetValueEx(key, name, 0, winreg.REG_DWORD, value)
        for name, value in values.items():
            if self._read(self.path(executable), name) != (value, winreg.REG_DWORD):
                raise OSError(f"{name} readback did not match the selection")

    def reset(self, executable):
        if executable is None:
            raise ValueError("Global reset must be saved explicitly as a new draft.")
        # Interrupted creation can leave an unidentifiable key at this exact computed
        # identity. Reset may remove it, but present mismatched metadata remains guarded.
        self._check_identity(executable, allow_missing_metadata=True)
        try:
            winreg.DeleteKeyEx(self.root, self.path(executable), winreg.KEY_WOW64_64KEY)
        except FileNotFoundError:
            pass

    def list_profiles(self, errors=None):
        result = []
        errors = [] if errors is None else errors
        try:
            with winreg.OpenKey(self.root, self.base + "\\Profiles", 0,
                                winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
                count = winreg.QueryInfoKey(key)[0]
                for index in range(count):
                    try:
                        child = winreg.EnumKey(key, index)
                        stored = self._read(self.base + "\\Profiles\\" + child, "ExecutablePath")
                        if not stored or stored[1] != winreg.REG_SZ or profile_id(stored[0]) != child:
                            raise ValueError("Invalid executable identity metadata")
                        self._check_identity(stored[0])
                        result.append(stored[0])
                    except (OSError, ValueError):
                        errors.append("Skipped a game profile with invalid or unreadable executable identity metadata.")
        except FileNotFoundError:
            pass
        return sorted(result)
