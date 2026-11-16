"""Quiesced next-launch Stock/Custom selection; never modifies Custom manifests."""
import contextlib
import copy
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

try:
    import winreg
except ImportError:  # CPU fixtures can inject their platform on other hosts.
    winreg = None

STOCK_LABEL = "Stock VDXR"
CUSTOM_LABEL = "Custom VDXR"
SECTIONS = ("ShippingExe", "SteamProductInfos")


def _json(data):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("Duplicate JSON key in launch-tree data")
            result[key] = value
        return result
    def invalid(value):
        raise ValueError("Nonfinite JSON value in launch-tree data")
    return json.loads(data.decode("utf-8-sig"), object_pairs_hook=unique, parse_constant=invalid)


def _path(value):
    if not isinstance(value, (str, Path)) or not str(value) or any(c in str(value) for c in '\0\r\n"'):
        raise ValueError("Launch-tree paths must be absolute paths without control characters")
    path = Path(value)
    if not path.is_absolute():
        raise ValueError("Launch-tree paths must be absolute")
    return path


def _same(a, b):
    return os.path.normcase(os.path.normpath(str(a))) == os.path.normcase(os.path.normpath(str(b)))


def _load(config_path, platform, label):
    path = _path(config_path)
    platform.assert_trusted_config(path)
    config = _json(platform.read_bytes(path))
    if not isinstance(config, dict) or type(config.get("schema")) is not int or config["schema"] != 1:
        raise ValueError("Launch-tree configuration schema 1 is required")
    required = {"library", "caches", "streamer_path", "custom_runtime_path", "settings_registry_path", "redirect_name"}
    if not required.issubset(config):
        raise ValueError("Required launch-tree configuration fields are missing")
    library = config["library"]
    if not isinstance(library, dict) or not {"registry_path", "custom_value", "custom_kind", "stock_root"}.issubset(library):
        raise ValueError("The saved Oculus library configuration is incomplete")
    registry = library["registry_path"]
    if not isinstance(registry, str) or not registry.startswith("S-1-") or "\\Software\\" not in registry or "\0" in registry:
        raise ValueError("An explicit interactive-user Oculus library registry path is required")
    if type(library["custom_kind"]) is not int or library["custom_kind"] not in (1, 2):
        raise ValueError("Saved OriginalPath must retain its string registry type")
    custom = _path(library["custom_value"])
    stock = _path(library["stock_root"])
    if not custom.is_dir() or not (custom / "Software").is_dir() or not (custom / "Manifests").is_dir():
        raise ValueError("The original Custom library and Software/Manifests directories are required")
    if _same(platform.resolve(custom), platform.resolve(stock)):
        raise ValueError("Stock and Custom launch trees must be separate")
    if label == STOCK_LABEL:
        if not (stock / "Manifests").is_dir() or not any((stock / "Manifests").iterdir()):
            raise ValueError("The prepared Stock manifests are missing")
        if platform.resolve(stock / "Manifests").is_relative_to(platform.resolve(custom / "Manifests")):
            raise ValueError("Stock manifests must be independent of the original Custom manifests")
        if not _same(platform.resolve(stock / "Software"), platform.resolve(custom / "Software")):
            raise ValueError("Stock Software must resolve to the original Custom Software directory")
    for name in ("streamer_path", "custom_runtime_path"):
        if not _path(config[name]).is_file():
            raise ValueError("A configured Streamer or Custom runtime file is missing")
    if Path(config["custom_runtime_path"]).name.casefold() != "virtualdesktop-openxr.dll":
        raise ValueError("The configured Custom runtime must be virtualdesktop-openxr.dll")
    if not isinstance(config["settings_registry_path"], str) or \
            config["settings_registry_path"].casefold() != r"software\virtualdesktop-openxr" or config.get("redirect_name") != "redirect_to":
        raise ValueError("Launch-tree selection may only change the owned runtime redirect")
    paths = set()
    if not isinstance(config.get("caches"), list) or not config["caches"]:
        raise ValueError("Owned cache templates are required")
    for cache in config["caches"]:
        if not isinstance(cache, dict) or not {"path", "custom", "stock"}.issubset(cache):
            raise ValueError("Launch-cache configuration is incomplete")
        cache_path = _path(cache["path"])
        normalized = os.path.normcase(str(platform.resolve(cache_path)))
        if normalized in paths:
            raise ValueError("Duplicate launch cache path")
        paths.add(normalized)
        for root in (custom / "Manifests", stock / "Manifests"):
            if platform.resolve(cache_path).is_relative_to(platform.resolve(root)):
                raise ValueError("A cache patch cannot write launch manifests")
        for side in ("custom", "stock"):
            patch = cache[side]
            if not isinstance(patch, dict) or set(patch) - set(SECTIONS):
                raise ValueError("Only owned launch cache sections may be patched")
            for section, records in patch.items():
                if not isinstance(records, dict):
                    raise ValueError("Cache patch keys must be explicit objects")
                for key, record in records.items():
                    if not isinstance(key, str) or not isinstance(record, dict) or type(record.get("present")) is not bool:
                        raise ValueError("Every cache patch must distinguish missing from null")
                    if set(record) != ({"present", "value"} if record["present"] else {"present"}):
                        raise ValueError("Malformed presence/value cache patch")
        if {s: set(v) for s, v in cache["custom"].items()} != {s: set(v) for s, v in cache["stock"].items()}:
            raise ValueError("Stock and Custom must own the same cache keys")
    for field in ("source_files", "stock_files"):
        if label == STOCK_LABEL and not isinstance(config.get(field, []), list):
            raise ValueError("Prepared asset hash guards must be lists")
    guards = [*config.get("source_files", []), *config.get("stock_files", [])] if label == STOCK_LABEL else []
    for source in guards:
        if not isinstance(source, dict) or not {"path", "sha256"}.issubset(source) or \
                not isinstance(source["sha256"], str) or not re.fullmatch(r"[0-9a-f]{64}", source["sha256"]):
            raise ValueError("Prepared asset hash guard is malformed")
        source_path = _path(source["path"])
        if os.path.normcase(str(platform.resolve(source_path))) in paths:
            raise ValueError("Immutable Custom source cannot also be a writable cache")
        if hashlib.sha256(platform.read_bytes(source_path)).hexdigest() != source["sha256"]:
            raise ValueError("Prepared launch assets changed or are missing; refresh the Stock launch-tree inventory")
    if not isinstance(config.get("game_executables"), list) or not config["game_executables"]:
        raise ValueError("A nonempty inventory of actual game executable paths is required")
    for game in config["game_executables"]:
        _path(game)
    config["_config_path"] = str(path)
    return config


def _equal(a, b):
    return json.dumps(a, sort_keys=True) == json.dumps(b, sort_keys=True)


def _merge(data, patch, counterpart):
    if not isinstance(data, dict):
        raise ValueError("Launch cache root must be an object")
    result = copy.deepcopy(data)
    for section, records in patch.items():
        target = result.setdefault(section, {})
        if not isinstance(target, dict):
            raise ValueError("Launch cache section must be an object")
        for key, record in records.items():
            other = counterpart[section][key]
            if section == "SteamProductInfos" and record["present"] and other["present"] and \
                    isinstance(record["value"], dict) and isinstance(other["value"], dict):
                current = target.setdefault(key, {})
                if not isinstance(current, dict):
                    raise ValueError("Owned Steam product record must be an object")
                desired, alternate = record["value"], other["value"]
                for member in desired.keys() | alternate.keys():
                    if (member in desired) == (member in alternate) and _equal(desired.get(member), alternate.get(member)):
                        continue
                    if member in desired:
                        current[member] = copy.deepcopy(desired[member])
                    else:
                        current.pop(member, None)
                continue
            if record["present"]:
                target[key] = copy.deepcopy(record["value"])
            else:
                target.pop(key, None)
    return result


def _registry(config):
    return (("HKU", config["library"]["registry_path"], "OriginalPath"),
            ("HKLM", config["settings_registry_path"], config["redirect_name"]))


def _targets(config, label):
    if label not in (CUSTOM_LABEL, STOCK_LABEL):
        raise ValueError("Select Stock VDXR or Custom VDXR")
    library = config["library"]
    custom = label == CUSTOM_LABEL
    return ((library["custom_value"] if custom else library["stock_root"], library["custom_kind"]),
            (config["custom_runtime_path"], 1) if custom else None)


def _matches(config, label, platform, *, caches=True):
    keys, targets = _registry(config), _targets(config, label)
    # OriginalPath retains its exact saved spelling and registry type. The
    # runtime redirect names a file, so equivalent Windows path spelling agrees.
    if platform.read_value(*keys[0]) != targets[0]:
        return False
    redirect = platform.read_value(*keys[1])
    target = targets[1]
    if target is None:
        if redirect is not None:
            return False
    elif redirect is None or redirect[1] != target[1] or not _same(redirect[0], target[0]):
        return False
    if not caches:
        return True
    side = "custom" if label == CUSTOM_LABEL else "stock"
    other = "stock" if side == "custom" else "custom"
    for cache in config["caches"]:
        data = _json(platform.read_bytes(cache["path"]))
        if not _equal(_merge(data, cache[side], cache[other]), data):
            return False
    return True


def _same_executable(a, b):
    if not isinstance(a, str) or not isinstance(b, str) or not a or not b:
        return False
    def executable(value):
        match = re.match(r'^(?:"([^\"]+\.exe)"|(.+?\.exe))(?=\s|$)', value.strip(), re.IGNORECASE)
        return (match[1] or match[2]) if match else value
    return _same(executable(a), executable(b))


def _stock_has_custom_routes(config, platform):
    """Permit vendor discovery, but reject reintroduced known Custom wrappers."""
    for cache in config["caches"]:
        data = _json(platform.read_bytes(cache["path"]))
        for section, records in cache["custom"].items():
            current = data.get(section, {})
            if not isinstance(current, dict):
                raise ValueError("Launch cache section must be an object")
            for key, custom in records.items():
                stock = cache["stock"][section][key]
                if not custom["present"] or _equal(custom, stock) or key not in current:
                    continue
                if stock["present"] and _equal(current[key], stock["value"]):
                    continue
                if section == "ShippingExe" and _same_executable(current[key], custom["value"]):
                    return True
                if section == "SteamProductInfos" and isinstance(custom["value"], dict) and isinstance(current[key], dict):
                    stock_value = stock.get("value", {})
                    stock_value = stock_value if isinstance(stock_value, dict) else {}
                    for member, value in custom["value"].items():
                        field = member.casefold()
                        if member not in current[key] or field not in ("executable", "exe", "launchtype") or \
                                (member in stock_value and _equal(value, stock_value[member])):
                            continue
                        actual = current[key].get(member)
                        if member in stock_value and _equal(actual, stock_value[member]):
                            continue
                        if (_same_executable(actual, value) if field in ("executable", "exe") else _equal(actual, value)):
                            return True
    return False


def inspect_selection(config_path, selection_label, *, platform=None):
    """Read configuration and active alignment without creating registry keys or stopping apps."""
    platform = platform or WindowsPlatform()
    config = _load(config_path, platform, selection_label)
    _targets(config, selection_label)
    # Vendor discovery can resolve native ShippingExe entries after startup.
    # Active mode is the library/redirect contract, not an immutable cache dump.
    return _matches(config, selection_label, platform, caches=selection_label == CUSTOM_LABEL) and \
        (selection_label != STOCK_LABEL or not _stock_has_custom_routes(config, platform))


def activate(config_path, selection_label, custom_runtime_path, *, platform=None):
    """Switch owned launch routes and redirect together with exception rollback.

    Returns True only when an originally running Streamer was restarted. Failed
    writes, readback or restart restore exact cache bytes and typed registry values.
    """
    try:
        return _activate(config_path, selection_label, custom_runtime_path, platform=platform)
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        raise OSError(str(error)) from error


def _activate(config_path, selection_label, custom_runtime_path, *, platform=None):
    platform = platform or WindowsPlatform()
    with platform.mutex():
        config = _load(config_path, platform, selection_label)
        targets = _targets(config, selection_label)
        if not _same(_path(custom_runtime_path), config["custom_runtime_path"]):
            raise ValueError("Custom runtime path does not match the protected launch-tree configuration")
        keys = _registry(config)
        redirect = platform.read_value(*keys[1])
        if redirect is not None and (redirect[1] != 1 or not _same(redirect[0], config["custom_runtime_path"])):
            raise ValueError("An unrelated runtime override is active; launch-tree selection did not overwrite it")
        if _matches(config, selection_label, platform, caches=selection_label == CUSTOM_LABEL) and \
                (selection_label != STOCK_LABEL or not _stock_has_custom_routes(config, platform)):
            return False
        platform.ensure_no_games(config)
        was_running = platform.streamer_running(config)
        # Stop and verify the server is gone before taking the transaction's
        # snapshots: Streamer's exit can flush its own cached game inventory.
        snapshots = {}
        originals = None
        changed = False
        try:
            platform.stop_streamer(config)
            platform.ensure_no_games(config)
            originals = [platform.read_value(*key) for key in keys]
            allowed_roots = (config["library"]["custom_value"], config["library"]["stock_root"])
            if originals[0] is None or originals[0][1] != config["library"]["custom_kind"] or not any(_same(originals[0][0], p) for p in allowed_roots):
                raise ValueError("The active library root is unrelated to the prepared Stock/Custom trees")
            if originals[1] is not None and (originals[1][1] != 1 or not _same(originals[1][0], config["custom_runtime_path"])):
                raise ValueError("The runtime override changed before the switch")
            side = "custom" if selection_label == CUSTOM_LABEL else "stock"
            outputs = {}
            for cache in config["caches"]:
                path = cache["path"]
                snapshots[path] = platform.read_bytes(path)
                other = "stock" if side == "custom" else "custom"
                outputs[path] = json.dumps(_merge(_json(snapshots[path]), cache[side], cache[other]), ensure_ascii=False,
                                          allow_nan=False, indent=2).encode("utf-8") + b"\n"
            changed = True  # Include a partially failed first atomic write in rollback.
            for path, data in outputs.items():
                platform.write_bytes(path, data)
            for key, target in zip(keys, targets):
                platform.write_value(*key, target)
            if not _matches(config, selection_label, platform):
                raise OSError("Launch-tree selection readback did not match")
            if was_running:
                platform.restart_streamer(config)
            return was_running
        except Exception as error:
            try:
                if changed:
                    # A failed restart may have partially launched Streamer.
                    # Quiesce it again before restoring files it can cache.
                    if not platform.streamer_quiesced(config):
                        platform.stop_streamer(config)
                    for path, data in snapshots.items():
                        platform.write_bytes(path, data)
                    for key, record in zip(keys, originals):
                        platform.write_value(*key, record)
                    if any(platform.read_bytes(p) != data for p, data in snapshots.items()) or any(
                            platform.read_value(*key) != record for key, record in zip(keys, originals)):
                        raise OSError("Launch-tree rollback readback did not match")
                if was_running:
                    platform.restart_streamer(config)
            except Exception as rollback_error:
                raise OSError(f"Launch-tree switch failed ({error}); recovery also failed ({rollback_error}). Streamer was not reported ready.") from error
            raise


class WindowsPlatform:
    def assert_trusted_config(self, path):
        expected = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "VDXR-NR-FG/LaunchTrees/config.json"
        if not _same(path, expected) or path.is_symlink() or not _same(path.resolve(), expected):
            raise ValueError("Launch-tree configuration must be the administrator-installed protected config.json")

    def resolve(self, path):
        return Path(path).resolve()

    @contextlib.contextmanager
    def mutex(self):
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateMutexW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR]
        kernel.CreateMutexW.restype = wintypes.HANDLE
        kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel.ReleaseMutex.argtypes = [wintypes.HANDLE]
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        handle = kernel.CreateMutexW(None, False, r"Global\VDXR-NR-FG.LaunchTreeSwitch")
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())
        acquired = False
        try:
            result = kernel.WaitForSingleObject(handle, 30000)
            if result not in (0, 0x80):
                raise OSError("Another launch-tree switch is still running")
            acquired = True
            yield
        finally:
            if acquired:
                kernel.ReleaseMutex(handle)
            kernel.CloseHandle(handle)

    def read_value(self, hive, path, name):
        root = winreg.HKEY_USERS if hive == "HKU" else winreg.HKEY_LOCAL_MACHINE
        try:
            with winreg.OpenKey(root, path, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
                return winreg.QueryValueEx(key, name)
        except FileNotFoundError:
            return None

    def write_value(self, hive, path, name, record):
        root = winreg.HKEY_USERS if hive == "HKU" else winreg.HKEY_LOCAL_MACHINE
        with winreg.OpenKey(root, path, 0, winreg.KEY_SET_VALUE | winreg.KEY_WOW64_64KEY) as key:
            if record is None:
                try:
                    winreg.DeleteValue(key, name)
                except FileNotFoundError:
                    pass
            else:
                winreg.SetValueEx(key, name, 0, record[1], record[0])

    def read_bytes(self, path):
        return Path(path).read_bytes()

    def write_bytes(self, path, data):
        path = Path(path)
        # Replace within the same directory; restore the original DACL on the
        # replacement before rename so a SYSTEM cache keeps its access rules.
        advapi = ctypes.WinDLL("advapi32", use_last_error=True)
        advapi.GetFileSecurityW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, ctypes.c_void_p,
                                          wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
        advapi.SetFileSecurityW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, ctypes.c_void_p]
        size = wintypes.DWORD()
        advapi.GetFileSecurityW(str(path), 4, None, 0, ctypes.byref(size))
        if not size.value:
            raise ctypes.WinError(ctypes.get_last_error())
        security = ctypes.create_string_buffer(size.value)
        if not advapi.GetFileSecurityW(str(path), 4, security, size.value, ctypes.byref(size)):
            raise ctypes.WinError(ctypes.get_last_error())
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".vdxr-launch-", delete=False) as stream:
                temporary = Path(stream.name)
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            if not advapi.SetFileSecurityW(str(temporary), 4, security):
                raise ctypes.WinError(ctypes.get_last_error())
            os.replace(temporary, path)
            temporary = None
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)

    def _helper(self, config, action):
        base = Path(sys.executable).parent / "source" if getattr(sys, "frozen", False) else Path(__file__).parent
        helper = base / "scripts/Restart-StreamerInteractive.ps1"
        executable = Path(os.environ.get("SystemRoot", "C:/Windows")) / "System32/WindowsPowerShell/v1.0/powershell.exe"
        result = subprocess.run([str(executable), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                                 "-File", str(helper), "-Action", action, "-ConfigPath", config["_config_path"]],
                                capture_output=True, encoding="utf-8", errors="replace", timeout=60,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        if result.returncode:
            raise OSError(result.stderr.strip() or "Virtual Desktop launch-tree OS handoff failed")
        return json.loads(result.stdout)

    def ensure_no_games(self, config):
        status = self._helper(config, "Check")
        if status["games"]:
            raise RuntimeError("Close the running VR game before changing launch trees")

    def streamer_running(self, config):
        status = self._helper(config, "Check")
        if status["server_pids"] and not status["streamer_pids"]:
            raise RuntimeError("Virtual Desktop Server is running without Streamer; close it before changing launch trees")
        return bool(status["streamer_pids"])

    def streamer_quiesced(self, config):
        status = self._helper(config, "Quiesced")
        return not status["streamer_pids"] and not status["server_pids"]

    def stop_streamer(self, config):
        self._helper(config, "Stop")

    def restart_streamer(self, config):
        self._helper(config, "Restart")
