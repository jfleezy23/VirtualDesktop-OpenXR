"""Read local display metadata for the executable identities supplied by Controls."""
from dataclasses import dataclass
import json
import ntpath
import os
from pathlib import Path, PureWindowsPath
import re
import winreg

from vdxr_game_profiles import normalize_executable
from vdxr_local_files import is_local_file, local_directory, read_local_bytes
from vdxr_steam_catalog import steam_metadata

_OCULUS_KEY = r"Software\Oculus VR, LLC\Oculus"
_OCULUS_LIBRARIES = _OCULUS_KEY + r"\Libraries"
_METADATA_LIMIT = 8 * 1024 * 1024
_IMAGE_EXTENSIONS = {".jpeg", ".jpg", ".png"}
_STEAM_APP_KEY = re.compile(r"steam\.app\.([1-9][0-9]{0,9})")
_META_APP_ID = re.compile(r"[1-9][0-9]{12,17}")
_PROTECTED_PUBLISHER = re.compile(r"bulky[\s_-]*pi(?:x|cks)", re.IGNORECASE)


@dataclass(frozen=True)
class GameEntry:
    executable: str
    title: str
    cover: Path | None
    store: str | None = None


def _safe_parts(parts):
    return all(part != ".." and not os.path.isreserved(part) for part in parts)


def _relative_path(value):
    if not isinstance(value, str) or not value:
        return None
    path = PureWindowsPath(value)
    if path.anchor or not path.parts or not _safe_parts(path.parts):
        return None
    return path


def _local_path(value):
    if not isinstance(value, (str, Path)):
        return None
    path = Path(value)
    if not path.is_absolute() or not re.fullmatch(r"[a-zA-Z]:", path.drive) or not _safe_parts(path.parts[1:]):
        return None
    return path


def _document(path):
    data = read_local_bytes(path, _METADATA_LIMIT)
    if not data:
        return {}
    try:
        value = json.loads(data.decode("utf-8-sig"))
        return value if isinstance(value, dict) else {}
    except (ValueError, RecursionError):
        return {}


def _cover(path):
    if path is not None and path.suffix.lower() in _IMAGE_EXTENSIONS and is_local_file(path):
        return path
    return None


def _identity(path):
    try:
        return normalize_executable(path)
    except (OSError, ValueError):
        return None


def _registered_libraries():
    access = winreg.KEY_READ | winreg.KEY_WOW64_64KEY
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, _OCULUS_LIBRARIES, 0, access) as libraries:
            for index in range(winreg.QueryInfoKey(libraries)[0]):
                try:
                    with winreg.OpenKey(libraries, winreg.EnumKey(libraries, index), 0, access) as library:
                        for name in ("OriginalPath", "Path"):
                            try:
                                value, kind = winreg.QueryValueEx(library, name)
                                if kind == winreg.REG_EXPAND_SZ:
                                    yield winreg.ExpandEnvironmentStrings(value)
                                elif kind == winreg.REG_SZ:
                                    yield value
                            except (OSError, ValueError):
                                continue
                except OSError:
                    continue
    except OSError:
        return


def _oculus_core_assets():
    access = winreg.KEY_READ | winreg.KEY_WOW64_32KEY
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, _OCULUS_KEY, 0, access) as oculus:
            value, kind = winreg.QueryValueEx(oculus, "Base")
        if kind == winreg.REG_EXPAND_SZ:
            value = winreg.ExpandEnvironmentStrings(value)
        elif kind != winreg.REG_SZ:
            return None
    except (OSError, ValueError):
        return None
    install = _local_path(value)
    return install / "CoreData" / "Software" / "StoreAssets" if install is not None else None


def _title(value):
    if isinstance(value, str) and len(value) <= 256 and value.isprintable() and "\\" not in value:
        label = value.strip()
        drive, root, _ = ntpath.splitroot(label)
        if not drive and not root:
            return label or None
    return None


def _fallback_title(executable):
    stem = ntpath.splitext(ntpath.basename(executable))[0]
    stem = re.sub(r"[-_](?:Win32|Win64|Windows(?:NoEditor)?)[-_](?:Shipping|Development|Test)$", "",
                  stem, flags=re.IGNORECASE)
    words = re.sub(r"(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])", " ", stem)
    words = re.sub(r"[-_]+", " ", words)
    return " ".join(word[:1].upper() + word[1:] for word in words.split()) or "Game"


def _oculus_metadata(roots, identities, titles):
    owned, native, alternate = {}, {}, {}
    protected = set()
    core_assets = _oculus_core_assets()
    by_parent = {}
    for identity in identities:
        for parent in PureWindowsPath(identity).parents:
            by_parent.setdefault(parent, []).append(identity)
    for root in dict.fromkeys(filter(None, map(_local_path, roots))):
        manifests = root / "Manifests"
        with local_directory(manifests) as directory:
            if directory is None:
                continue
            try:
                files = sorted(manifests / child.name for child in directory.glob("*.json"))
            except OSError:
                continue
        software = root / "Software"
        for file in files:
            document = _document(file)
            label = document.get("canonicalName")
            canonical = _relative_path(label)
            if canonical is None or len(canonical.parts) != 1:
                continue
            asset = Path(label + "_assets") / "cover_landscape_image.jpg"
            library_art = software / "StoreAssets" / asset
            if "x-vdxr-local-entry" in document:
                metadata = document["x-vdxr-local-entry"]
                if not isinstance(metadata, dict) or metadata.get("owner") != "VDXR local catalog":
                    continue
                path = _local_path(metadata.get("originalExecutable"))
                key = _identity(path) if path is not None else None
                if key in identities:
                    if _PROTECTED_PUBLISHER.search(label):
                        protected.add(key)
                    title, cover = owned.get(key, (None, None))
                    owned[key] = (title or _title(label.removesuffix(" (VDXR)")), cover or _cover(library_art))
                continue
            install = software / canonical
            package = _identity(install)
            contained = by_parent.get(PureWindowsPath(package), ()) if package is not None else ()
            launches = []
            for field in ("launchFile", "launchFile2D"):
                launch = _relative_path(document.get(field))
                key = _identity(install / launch) if launch is not None else None
                if key in identities:
                    launches.append(key)
            if not (launches or contained):
                continue
            if _PROTECTED_PUBLISHER.search(label):
                protected.update(launches)
                protected.update(contained)
            app_id = document.get("appId")
            if isinstance(app_id, int) and not isinstance(app_id, bool):
                app_id = str(app_id)
            title = _title(document.get("displayName"))
            if title is None and isinstance(app_id, str) and _META_APP_ID.fullmatch(app_id):
                title = _title(titles.get(app_id))
            if title and _PROTECTED_PUBLISHER.search(title):
                protected.update(launches)
                protected.update(contained)
            cover = _cover(library_art)
            if cover is None and core_assets is not None:
                cover = _cover(core_assets / asset)
            for key in launches:
                previous_title, previous_cover = native.get(key, (None, None))
                native[key] = (previous_title or title, previous_cover or cover)
            for key in contained:
                # Multiple manifest matches leave the alternate executable ambiguous.
                alternate[key] = (title, cover) if key not in alternate else None
    for key, details in alternate.items():
        if details is not None:
            previous_title, previous_cover = native.get(key, (None, None))
            native[key] = (previous_title or details[0], previous_cover or details[1])
    return owned, native, protected


def _frame_metadata(path):
    path = _local_path(path)
    library = _document(path).get("Library") if path is not None else None
    games = library.get("Games") if isinstance(library, dict) else None
    result = []
    for game in games if isinstance(games, list) else ():
        if not isinstance(game, dict) or not (title := _title(game.get("Name"))):
            continue
        executable = _local_path(game.get("Executable"))
        directory = _local_path(game.get("InstallDirectory"))
        key = _identity(executable) if executable is not None else None
        root = _identity(directory) if directory is not None and len(directory.parts) > 1 else None
        result.append((title, key, PureWindowsPath(root) if root else None, game))
    return result


def _steam_art_folder(path, app_id):
    if path is None:
        return None
    for folder in (path.parent, path.parent.parent):
        if (folder.name == app_id and folder.parent.name.casefold() == "librarycache"
                and folder.parent.parent.name.casefold() == "appcache"):
            return folder
    return None


def _steam_wide_cover(app_id, cache, poster, hero):
    """Find Steam's titled store header in a cache folder for this app ID."""
    folders = dict.fromkeys(filter(None, (cache, _steam_art_folder(poster, app_id),
                                         _steam_art_folder(hero, app_id))))
    for folder in folders:
        if cover := _cover(folder / "header.jpg") or _cover(folder / "library_header.jpg"):
            return cover
        if cover := _cover(folder.parent / f"{app_id}_header.jpg"):
            return cover
        with local_directory(folder) as directory:
            if directory is None:
                continue
            for child in sorted(directory.iterdir()):
                if re.fullmatch(r"[a-fA-F0-9]{40}", child.name):
                    candidate = folder / child.name
                    if cover := _cover(candidate / "library_header.jpg") or _cover(candidate / "header.jpg"):
                        return cover
    return None


def _frame_match(records, identity):
    executable = PureWindowsPath(identity)
    matches = [record for record in records if record[1] == identity]
    if not matches:
        containing = [record for record in records if record[2] in executable.parents]
        if len(containing) == 1 and containing[0][3].get("Executable") in (None, ""):
            matches = containing
    return matches[0] if len(matches) == 1 else None


def _frame_steam_id(game):
    app_key = game.get("SteamAppKey")
    if isinstance(app_key, str) and (match := _STEAM_APP_KEY.fullmatch(app_key)):
        return match[1]
    return None


def catalog_entries(executables, *, library_roots=None, frame_profiles=None, steam_roots=None, meta_titles=None):
    """Return presentation details in input order, preserving each executable string."""
    identities = [(executable, _identity(executable)) for executable in executables]
    keys = {key for _, key in identities if key is not None}
    if not keys:
        return [GameEntry(exe, _fallback_title(exe), None) for exe, _ in identities]
    if local_appdata := os.environ.get("LOCALAPPDATA"):
        if frame_profiles is None:
            frame_profiles = Path(local_appdata) / "FrameLauncher" / "profiles.json"
        if meta_titles is None:
            meta_titles = Path(local_appdata) / "VDXR-NR-FG" / "meta-titles.json"
    title_cache = _local_path(meta_titles)
    owned, native, protected = _oculus_metadata(
        _registered_libraries() if library_roots is None else library_roots, keys,
        _document(title_cache) if title_cache is not None else {},
    )
    records = _frame_metadata(frame_profiles)
    frame = {key: record for key in keys if (record := _frame_match(records, key)) is not None}
    frame_ids = {key: app_id for key, (_, _, _, game) in frame.items()
                 if (app_id := _frame_steam_id(game)) is not None}
    steam = steam_metadata(keys, frame_app_ids=frame_ids, steam_roots=steam_roots)
    entries = []
    for executable, identity in identities:
        title, cover = owned.get(identity, (None, None))
        native_title, native_cover = native.get(identity, (None, None))
        frame_title, _, _, game = frame.get(identity, (None, None, None, {}))
        poster, hero = (_local_path(game.get(field)) for field in ("PosterPath", "HeroPath"))
        steam_title, app_id, cache = steam.get(identity, (None, None, None))
        preserve = identity in protected or any(
            item and _PROTECTED_PUBLISHER.search(item) for item in (executable, title, frame_title, steam_title)
        )
        if preserve:
            title = native_title or title or frame_title
            cover = cover or native_cover or _cover(hero) or _cover(poster)
        else:
            title = native_title or _title(steam_title) or title or frame_title
            wide = _steam_wide_cover(app_id, cache, poster, hero) if app_id else None
            if native_title or native_cover:
                cover = cover or native_cover or wide
            else:
                cover = wide or cover
            cover = cover or _cover(poster) or _cover(hero)
        store = None
        if identity in native:
            store = "Meta"
        elif app_id is not None:
            store = "Steam"
        entries.append(GameEntry(executable, title or _fallback_title(executable), cover, store))
    return entries
