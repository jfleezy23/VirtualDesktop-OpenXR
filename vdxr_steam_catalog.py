"""Resolve installed Steam metadata for saved executable identities."""

from collections import defaultdict
from itertools import islice
import ntpath
from pathlib import Path
import re
import winreg

from vdxr_local_files import local_directory, read_local_bytes


_LIBRARY_LIMIT = 2 * 1024 * 1024
_MANIFEST_LIMIT = 256 * 1024
_MANIFEST_COUNT_LIMIT = 4096
_APP_ID = r"[1-9][0-9]{0,9}"
_MANIFEST_NAME = re.compile(rf"appmanifest_({_APP_ID})\.acf", re.IGNORECASE)
_VDF_TOKEN = re.compile(r'\s+|//[^\r\n]*|[{}]|"(?:[^"\\\r\n]|\\[^\r\n])*"')
_VDF_ESCAPE = re.compile(r'\\([\\"])')


def _vdf(path, limit):
    data = read_local_bytes(path, limit)
    if not data:
        return {}
    try:
        source = data.decode("utf-8-sig")
    except UnicodeDecodeError:
        return {}

    stack = [{}]
    key = None
    position = 0
    for match in _VDF_TOKEN.finditer(source):
        if match.start() != position:
            return {}
        position = match.end()
        token = match[0]
        if token.isspace() or token.startswith("//"):
            continue
        if token == "{":
            if key is None or len(stack) == 32:
                return {}
            child = {}
            stack[-1][key] = child
            stack.append(child)
            key = None
        elif token == "}":
            if key is not None or len(stack) == 1:
                return {}
            stack.pop()
        else:
            value = _VDF_ESCAPE.sub(r"\1", token[1:-1])
            if key is None:
                key = value.casefold()
                if key in stack[-1]:
                    return {}
            else:
                stack[-1][key] = value
                key = None
    return stack[0] if position == len(source) and len(stack) == 1 and key is None else {}


def _local_path(value):
    if not isinstance(value, (str, Path)):
        return None
    path = Path(value)
    if (path.is_absolute() and re.fullmatch(r"[A-Za-z]:", path.drive)
            and all(part != ".." and not ntpath.isreserved(part) for part in path.parts[1:])):
        return path
    return None


def _registered_steam_roots():
    for hive, flags, name in (
        (winreg.HKEY_CURRENT_USER, 0, "SteamPath"),
        (winreg.HKEY_LOCAL_MACHINE, winreg.KEY_WOW64_32KEY, "InstallPath"),
        (winreg.HKEY_LOCAL_MACHINE, winreg.KEY_WOW64_64KEY, "InstallPath"),
    ):
        try:
            with winreg.OpenKey(hive, r"Software\Valve\Steam", 0, winreg.KEY_READ | flags) as key:
                value, kind = winreg.QueryValueEx(key, name)
            if kind == winreg.REG_EXPAND_SZ:
                yield winreg.ExpandEnvironmentStrings(value)
            elif kind == winreg.REG_SZ:
                yield value
        except (OSError, ValueError):
            continue


def _libraries(client):
    yield client
    folder_config = _vdf(client / "steamapps" / "libraryfolders.vdf", _LIBRARY_LIMIT)
    folders = folder_config.get("libraryfolders")
    if not isinstance(folders, dict):
        return
    for index, entry in folders.items():
        if re.fullmatch(r"[0-9]{1,10}", index):
            value = entry.get("path") if isinstance(entry, dict) else entry
            if (library := _local_path(value)) is not None:
                yield library


def _manifest(steamapps, app_id):
    document = _vdf(steamapps / f"appmanifest_{app_id}.acf", _MANIFEST_LIMIT)
    state = document.get("appstate")
    if not isinstance(state, dict) or state.get("appid") != app_id:
        return None
    install = state.get("installdir")
    if (not isinstance(install, str) or not install or install in {".", ".."}
            or any(char in install for char in "\\/") or ntpath.isreserved(install)):
        return None
    title = state.get("name")
    label = title.strip() if isinstance(title, str) and title.isprintable() else None
    return install, label or None


def _installed_targets(steamapps, targets):
    common = steamapps / "common"
    result = defaultdict(set)
    for identity, executable in targets.items():
        if not executable.is_relative_to(common):
            continue
        relative = executable.relative_to(common)
        if len(relative.parts) < 2:
            continue
        with local_directory(executable.parent) as directory:
            if directory is not None:
                result[Path(relative.parts[0])].add(identity)
    return result


def _manifest_ids(steamapps):
    with local_directory(steamapps) as directory:
        if directory is None:
            return []
        matching = (match[1] for child in directory.iterdir()
                    if (match := _MANIFEST_NAME.fullmatch(child.name)))
        found = list(islice(matching, _MANIFEST_COUNT_LIMIT + 1))
    return found if len(found) <= _MANIFEST_COUNT_LIMIT else None


def _single(items):
    return next(iter(items)) if len(items) == 1 else None


def _cached_app(clients, app_id):
    folders = []
    for client in clients:
        cache = client / "appcache" / "librarycache" / app_id
        with local_directory(cache) as directory:
            if directory is not None:
                folders.append(cache)
    folder = _single(folders)
    return (None, app_id, folder) if folder is not None else None


def steam_metadata(identities, *, frame_app_ids=None, steam_roots=None):
    """Map normalized executables to (manifest title, app ID, cache folder).

    The caller may supply an app ID from an exact executable or unique install
    directory match. The title and cache folder may be None.
    A conflicting manifest leaves the executable to the caller's existing metadata.
    """
    targets = {identity: path for identity in identities
               if (path := _local_path(identity)) is not None and path.suffix.casefold() == ".exe"}
    if not targets:
        return {}
    frame_ids = {identity: app_id for identity, app_id in (frame_app_ids or {}).items()
                 if identity in targets and isinstance(app_id, str) and re.fullmatch(_APP_ID, app_id)}
    roots = _registered_steam_roots() if steam_roots is None else steam_roots
    clients = tuple(dict.fromkeys(path for value in roots if (path := _local_path(value)) is not None))

    contained = defaultdict(set)
    known_apps = defaultdict(set)
    unscanned = set()
    requested = set(frame_ids.values())
    for client in clients:
        for library in dict.fromkeys(_libraries(client)):
            steamapps = library / "steamapps"
            installs = _installed_targets(steamapps, targets)
            app_ids = _manifest_ids(steamapps) if installs else []
            if app_ids is None:
                unscanned.update(identity for group in installs.values() for identity in group)
                continue
            for app_id in requested.union(app_ids):
                manifest = _manifest(steamapps, app_id)
                if manifest is None:
                    continue
                install, title = manifest
                metadata = (title, app_id, client / "appcache" / "librarycache" / app_id)
                if app_id in requested:
                    known_apps[app_id].add(metadata)
                for identity in installs.get(Path(install), ()):
                    contained[identity].add(metadata)

    result = {}
    for identity in targets.keys() - unscanned:
        hint = frame_ids.get(identity)
        matches = contained[identity]
        metadata = _single(matches)
        if metadata is not None and (hint is None or hint == metadata[1]):
            result[identity] = metadata
        elif not matches and hint is not None:
            metadata = _single(known_apps[hint])
            if not known_apps[hint]:
                metadata = _cached_app(clients, hint) or (None, hint, None)
            if metadata is not None:
                result[identity] = metadata
    return result
