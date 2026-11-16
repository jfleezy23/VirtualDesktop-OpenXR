"""Prepare a separate Stock launch tree and owned cache templates offline.

Custom inputs are read and hash-guarded, never changed. This tool does not create
junctions, write registry values, touch installed caches, or launch any program.
The CLI only stages under bin/stock-launch-switch-20261113 in this workspace.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path, PureWindowsPath
import re
from typing import Callable

LAUNCH_FIELDS = ('launchFile', 'launchParameters', 'launchFile2D', 'launchParameters2D')
CACHE_MAPS = ('ShippingExe', 'SteamProductInfos')
LOCAL_OWNER = 'VDXR local catalog'
INJECTOR_FLAGS = frozenset(('/clean-openxr', '/openxr', '/wait'))


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def _bytes(path: Path) -> bytes:
    try:
        return path.read_bytes()
    except OSError as error:
        raise ValueError('an input file is missing or unreadable') from error


def _no_duplicates(pairs):
    result = {}
    for key, value in pairs:
        _require(key not in result, 'duplicate JSON object key')
        result[key] = value
    return result


def _reject_constant(_):
    raise ValueError('nonfinite JSON number')


def _object(data: bytes) -> dict:
    try:
        value = json.loads(data.decode('utf-8-sig'), object_pairs_hook=_no_duplicates,
                           parse_constant=_reject_constant)
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ValueError('invalid UTF-8 JSON input') from error
    _require(isinstance(value, dict), 'JSON input must be an object')
    return value


def _json_bytes(value: dict) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + '\n').encode('utf-8')


def file_guard(path: Path, data: bytes | None = None) -> dict:
    return {'path': str(path.resolve()), 'sha256': hashlib.sha256(_bytes(path) if data is None else data).hexdigest()}


def verify_guards(guards: list[dict]) -> None:
    for guard in guards:
        _require(hashlib.sha256(_bytes(Path(guard['path']))).hexdigest() == guard['sha256'],
                 'a guarded input changed during preparation')


def windows_absolute(value: str) -> PureWindowsPath:
    _require(isinstance(value, str) and value and not any(ord(c) < 32 for c in value),
             'absolute Windows path required')
    path = PureWindowsPath(value)
    _require(path.is_absolute() and re.fullmatch(r'[A-Za-z]:', path.drive) is not None,
             'fully qualified local Windows drive path required')
    _require('..' not in path.parts and all(':' not in p for p in path.parts[1:]),
             'parent traversal and alternate streams are unsupported')
    _require(not any(p.endswith((' ', '.')) for p in path.parts[1:]), 'ambiguous Windows path component')
    return path


def _relative(value: str, *, one_part: bool = False) -> PureWindowsPath:
    _require(isinstance(value, str) and value and not any(ord(c) < 32 for c in value),
             'nonempty relative Windows path required')
    path = PureWindowsPath(value)
    _require(not path.drive and not path.root and '..' not in path.parts and
             not any(':' in p or p.endswith((' ', '.')) for p in path.parts),
             'launch path escapes its software entry')
    _require(not one_part or len(path.parts) == 1, 'canonical entry must be one path component')
    return path


def _exists(path: PureWindowsPath) -> bool:
    # Tests inject a fixture-only resolver, so POSIX paths cannot pass as Windows paths.
    _require(os.name == 'nt', 'real target validation requires Windows')
    return Path(str(path)).is_file()


def _vendor_targets(full: dict, software: PureWindowsPath, exists: Callable) -> dict:
    canonical = _relative(full.get('canonicalName'), one_part=True)
    targets = {}
    for field in ('launchFile', 'launchFile2D'):
        value = full.get(field)
        if field == 'launchFile2D' and value in (None, ''):
            continue
        target = software / canonical / _relative(value)
        targets[field] = {'path': str(target), 'exists': bool(exists(target))}
    return {'launchable': targets['launchFile']['exists'], 'targets': targets,
            'all_original_targets_present': all(item['exists'] for item in targets.values())}


def command_tokens(command: str) -> list[tuple[str, int, int]]:
    """Windows quote/backslash parsing with original spans, without executing it."""
    _require(isinstance(command, str) and not any(ord(c) < 32 and c != '\t' for c in command),
             'invalid launch command')
    result = []
    i = 0
    while i < len(command):
        while i < len(command) and command[i] in ' \t':
            i += 1
        if i == len(command):
            break
        start, quoted, value = i, False, []
        while i < len(command) and (quoted or command[i] not in ' \t'):
            slashes = 0
            while i < len(command) and command[i] == '\\':
                slashes += 1
                i += 1
            if i < len(command) and command[i] == '"':
                value.extend('\\' * (slashes // 2))
                if slashes % 2:
                    value.append('"')
                elif quoted and i + 1 < len(command) and command[i + 1] == '"':
                    value.append('"')
                    i += 1
                else:
                    quoted = not quoted
                i += 1
            else:
                value.extend('\\' * slashes)
                if i < len(command) and (quoted or command[i] not in ' \t'):
                    value.append(command[i])
                    i += 1
        _require(not quoted, 'unclosed launch-command quote')
        result.append((''.join(value), start, i))
    return result


def original_argument_tail(command: str, original: str) -> str:
    expected = str(windows_absolute(original)).casefold()
    tokens = command_tokens(command)
    matches = []
    for index, (value, _, _) in enumerate(tokens):
        try:
            if str(windows_absolute(value)).casefold() == expected:
                matches.append(index)
        except ValueError:
            pass
    _require(len(matches) == 1, 'original executable token is missing or ambiguous')
    index = matches[0]
    _require(all(token.casefold() in INJECTOR_FLAGS for token, _, _ in tokens[:index]),
             'unrecognized prefix before original executable')
    # Preserve the game's original argument substring, including whitespace/quotes.
    return command[tokens[index][2]:]


def _manifest_plan(source: Path, software_root: str, path_exists: Callable) -> dict:
    _require(source.is_dir(), 'Custom manifest source directory is missing')
    software = windows_absolute(software_root)
    entries = sorted(source.iterdir(), key=lambda p: p.name.casefold())
    _require(all(p.is_file() and not p.is_symlink() for p in entries),
             'manifest source contains an unsupported directory or link')
    _require(all(p.suffix.casefold() in ('.json', '.mini') for p in entries),
             'manifest source contains an unsupported file type')
    raw = {p.name: _bytes(p) for p in entries}
    docs = {name: _object(data) for name, data in raw.items()}
    _require(len({name.casefold() for name in raw}) == len(raw), 'case-colliding manifest filenames')
    guards = [file_guard(p, raw[p.name]) for p in entries]
    output, unsupported, availability = {}, [], []
    counts = dict(vendor_pairs=0, modified_minis=0, unchanged_minis=0,
                  omitted_steam_aliases=0, native_local_aliases=0, unsupported_aliases=0,
                  unavailable_vendor_entries=0)
    consumed_minis = set()
    for name, full in docs.items():
        if Path(name).suffix.casefold() != '.json':
            continue
        mini_name = name + '.mini'
        metadata = full.get('x-vdxr-local-entry')
        if metadata is not None:
            _require(mini_name not in docs, 'generated entry unexpectedly has a mini sibling')
            alias = hashlib.sha256(name.encode('utf-8')).hexdigest()[:16]
            try:
                _require(isinstance(metadata, dict) and metadata.get('owner') == LOCAL_OWNER,
                         'generated entry ownership is unsupported')
                target = windows_absolute(metadata.get('originalExecutable'))
                _require(path_exists(target), 'original generated-entry executable is missing')
                if str(metadata.get('store', '')).casefold() == 'steam':
                    counts['omitted_steam_aliases'] += 1
                    continue
                _require(metadata.get('store') in (None, '', 'Oculus', 'Standalone'),
                         'generated entry store is unsupported')
                helper = windows_absolute(full.get('launchFile'))
                _require(path_exists(helper), 'existing generated-entry launcher is missing')
                stock = copy.deepcopy(full)
                stock['launchFile'] = str(target)
                stock['launchParameters'] = original_argument_tail(full.get('launchParameters'), str(target))
                label = full.get('canonicalName')
                _require(isinstance(label, str) and label, 'generated entry label is missing')
                stock['canonicalName'] = label.removesuffix(' (VDXR)')
                _require(bool(stock['canonicalName']), 'generated native label would be empty')
                output[name] = _json_bytes(stock)
                counts['native_local_aliases'] += 1
            except ValueError as error:
                counts['unsupported_aliases'] += 1
                unsupported.append({'entry': alias, 'reason': str(error)})
            continue
        _require(mini_name in docs, 'vendor full manifest has no mini sibling')
        mini = docs[mini_name]
        _require(mini.get('appId') == full.get('appId') and
                 mini.get('canonicalName') == full.get('canonicalName'), 'full/mini identity mismatch')
        checked = _vendor_targets(full, software, path_exists)
        checked['entry'] = hashlib.sha256(name.encode('utf-8')).hexdigest()[:16]
        availability.append(checked)
        counts['unavailable_vendor_entries'] += not checked['all_original_targets_present']
        stock = copy.deepcopy(mini)
        for field in LAUNCH_FIELDS:
            if field in full:
                stock[field] = copy.deepcopy(full[field])
            else:
                stock.pop(field, None)
        changed = stock != mini
        output[name] = raw[name]  # Original full JSON remains byte-exact.
        output[mini_name] = _json_bytes(stock) if changed else raw[mini_name]
        counts['vendor_pairs'] += 1
        counts['modified_minis' if changed else 'unchanged_minis'] += 1
        consumed_minis.add(mini_name)
    _require(consumed_minis == {name for name in docs if Path(name).suffix.casefold() == '.mini'},
             'unpaired vendor mini manifest')
    return {'contents': output, 'source_files': guards, 'counts': counts,
            'unsupported_aliases': unsupported, 'availability': availability}


def _new_destination(source: Path, destination: Path) -> None:
    _require(not destination.exists(), 'staging destination must be new')
    _require(not destination.resolve().is_relative_to(source.resolve()),
             'staging destination must be separate from Custom')


def _write_manifest_plan(plan: dict, destination: Path) -> list[dict]:
    verify_guards(plan['source_files'])
    destination.mkdir(parents=True, exist_ok=False)
    for name, data in plan['contents'].items():
        with (destination / name).open('xb') as output:
            output.write(data)
    verify_guards(plan['source_files'])
    return [{'relative': name, 'sha256': hashlib.sha256(data).hexdigest()}
            for name, data in sorted(plan['contents'].items())]


def prepare_manifest_tree(source: Path, destination: Path, software_root: str, *,
                          path_exists: Callable | None = None) -> dict:
    source, destination = Path(source), Path(destination)
    _new_destination(source, destination)
    plan = _manifest_plan(source, software_root, path_exists or _exists)
    inventory = _write_manifest_plan(plan, destination)
    return {key: value for key, value in plan.items() if key != 'contents'} | {'stock_files': inventory}


def _state(mapping: dict, key: str) -> dict:
    return {'present': True, 'value': copy.deepcopy(mapping[key])} if key in mapping else {'present': False}


def _cache_maps(cache: dict) -> None:
    _require(isinstance(cache, dict), 'cache must be a JSON object')
    for name in CACHE_MAPS:
        _require(isinstance(cache.get(name, {}), dict), 'owned cache collections must be JSON maps')


def _keys(keys) -> list[str]:
    _require(isinstance(keys, (list, tuple)) and all(isinstance(k, str) and k for k in keys),
             'owned cache keys must be explicit nonempty strings')
    _require(len(set(keys)) == len(keys), 'duplicate owned cache key')
    return list(keys)


def make_cache_templates(current: dict, originals: list[dict], shipping_keys: list[str],
                         steam_keys: list[str]) -> dict:
    _require(isinstance(originals, list) and len(originals) == 2, 'two corroborating originals required')
    for cache in [current, *originals]:
        _cache_maps(cache)
    result = {'custom': {}, 'stock': {}}
    for name, keys in zip(CACHE_MAPS, (shipping_keys, steam_keys)):
        result['custom'][name], result['stock'][name] = {}, {}
        for key in _keys(keys):
            first, second = (_state(cache.get(name, {}), key) for cache in originals)
            # JSON equality must retain null/absence, boolean/integer and nested record distinctions.
            _require(json.dumps(first, sort_keys=True, allow_nan=False) ==
                     json.dumps(second, sort_keys=True, allow_nan=False),
                     'corroborating original cache records disagree')
            result['stock'][name][key] = first
            result['custom'][name][key] = _state(current.get(name, {}), key)
    return result


def apply_cache_template(current: dict, template: dict) -> dict:
    """Pure fixture/reference application; backend owns live transactions."""
    result = copy.deepcopy(current)
    _cache_maps(result)
    _require(set(template) <= set(CACHE_MAPS), 'template attempts to own unrelated cache fields')
    for name, records in template.items():
        _require(isinstance(records, dict), 'cache patch must be a map')
        for key, state in records.items():
            _require(isinstance(state, dict) and type(state.get('present')) is bool and
                     set(state) == ({'present', 'value'} if state['present'] else {'present'}),
                     'invalid wrapped cache state')
            if state['present']:
                result.setdefault(name, {})[key] = copy.deepcopy(state['value'])
            elif name in result:
                result[name].pop(key, None)
    return result


def read_original_backups(inventory: Path) -> tuple[list[dict], list[dict]]:
    raw = _bytes(inventory)
    metadata = _object(raw)
    backups = metadata.get('backups')
    _require(isinstance(backups, list) and len(backups) == 2, 'two pinned original backups required')
    result, guards, seen = [], [file_guard(inventory, raw)], set()
    for entry in backups:
        _require(isinstance(entry, dict) and isinstance(entry.get('path'), str) and
                 re.fullmatch(r'[0-9a-f]{64}', entry.get('sha256', '')) is not None,
                 'invalid original backup pin')
        path = Path(entry['path'])
        _require(path.is_absolute() and path.resolve() not in seen, 'distinct absolute original backups required')
        seen.add(path.resolve())
        data = _bytes(path)
        _require(hashlib.sha256(data).hexdigest() == entry['sha256'], 'original backup hash mismatch')
        result.append(_object(data))
        guards.append(file_guard(path, data))
    return result, guards


def infer_owned_shipping_keys(current: dict, roots: list[str], *, path_exists: Callable | None = None) -> list[str]:
    _cache_maps(current)
    approved = [windows_absolute(root) for root in roots]
    _require(bool(approved), 'managed launcher roots required for ownership discovery')
    exists, owned = path_exists or _exists, []
    for key, value in current.get('ShippingExe', {}).items():
        if not isinstance(value, str):
            continue
        try:
            path = windows_absolute(value)
        except ValueError:
            continue
        if any(path.is_relative_to(root) for root in approved):
            _require(exists(path), 'an owned Custom launcher target is missing')
            owned.append(key)
    return sorted(owned)


def prepare_bundle(spec_path: Path, output: Path, *, path_exists: Callable | None = None) -> dict:
    """Create scratch-only trees plus the backend's schema-1 configuration fragment."""
    spec_raw = _bytes(spec_path)
    spec = _object(spec_raw)
    _require(type(spec.get('schema')) is int and spec['schema'] == 1, 'preparation schema 1 required')
    custom = windows_absolute(spec.get('custom_root'))
    stock = windows_absolute(spec.get('stock_root'))
    _require(custom != stock and custom / 'Manifests' != stock / 'Manifests', 'separate Stock root required')
    source = Path(spec.get('custom_manifests', str(custom / 'Manifests')))
    output = Path(output)
    _new_destination(source, output)
    exists = path_exists or _exists
    originals, guards = read_original_backups(Path(spec['backup_inventory']))
    guards.append(file_guard(spec_path, spec_raw))
    extra_guards = spec.get('source_files', [])
    _require(isinstance(extra_guards, list), 'source file guards must be a list')
    for guard in extra_guards:
        _require(isinstance(guard, dict) and isinstance(guard.get('path'), str) and
                 Path(guard['path']).is_absolute() and
                 re.fullmatch(r'[0-9a-f]{64}', guard.get('sha256', '')) is not None,
                 'invalid source file guard')
    guards.extend(copy.deepcopy(extra_guards))
    verify_guards(guards)
    cache_entries = spec.get('caches')
    _require(isinstance(cache_entries, list) and cache_entries, 'explicit account cache snapshots required')
    caches = []
    for cache in cache_entries:
        snapshot = Path(cache['source'])
        _require(snapshot.is_absolute(), 'absolute cache snapshot path required')
        live_path = windows_absolute(cache['path'])
        _require(str(PureWindowsPath(str(snapshot))).casefold() != str(live_path).casefold(),
                 'cache template input must be a separate immutable snapshot')
        data = _bytes(snapshot)
        current = _object(data)
        shipping = cache.get('owned_shipping_keys')
        if shipping is None:
            shipping = infer_owned_shipping_keys(current, spec.get('managed_launcher_roots', []), path_exists=exists)
        elif spec.get('managed_launcher_roots'):
            inferred = infer_owned_shipping_keys(current, spec['managed_launcher_roots'], path_exists=exists)
            _require(set(_keys(shipping)) == set(inferred),
                     'explicit ShippingExe ownership differs from scoped current launchers')
        steam = cache.get('owned_steam_keys', [])
        if 'allowed_steam_keys' in spec:
            _require(set(_keys(steam)) <= set(_keys(spec['allowed_steam_keys'])),
                     'Steam cache patch exceeds the corroborated owned scope')
        templates = make_cache_templates(current, originals, shipping, steam)
        caches.append({'path': cache['path'], **templates})
        guards.append(file_guard(snapshot, data))
    plan = _manifest_plan(source, str(custom / 'Software'), exists)
    expected_counts = spec.get('expected_counts', {})
    _require(isinstance(expected_counts, dict), 'expected manifest counts must be an object')
    for name, expected_count in expected_counts.items():
        _require(name in plan['counts'] and type(expected_count) is int and
                 plan['counts'][name] == expected_count, 'manifest qualification counts differ from the specification')
    guards.extend(plan['source_files'])
    verify_guards(guards)
    stock_files = _write_manifest_plan(plan, output / 'Manifests')
    library = copy.deepcopy(spec.get('library', {}))
    library.update(custom_root=spec['custom_root'], stock_root=spec['stock_root'])
    fragment = {'schema': 1, 'library': library, 'caches': caches, 'source_files': guards,
                'preparation': {'stock_manifests_source': str((output / 'Manifests').resolve()),
                                'stock_files': stock_files, 'counts': plan['counts'],
                                'unsupported_aliases': plan['unsupported_aliases'],
                                'availability': plan['availability'],
                                'software_junction_target': str(custom / 'Software'),
                                'junction_created': False}}
    for name in ('streamer_path', 'settings_registry_path', 'redirect_name', 'custom_runtime_path'):
        if name in spec:
            fragment[name] = copy.deepcopy(spec[name])
    (output / 'PREPARATION.json').write_bytes(_json_bytes(fragment))
    verify_guards(guards)
    return fragment


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--spec', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args(argv)
    scratch = Path(__file__).resolve().parents[1] / 'bin/stock-launch-switch-20261113'
    _require(args.output.is_absolute() and args.output.resolve().is_relative_to(scratch.resolve()),
             'CLI output must stay inside the dedicated workspace scratch directory')
    fragment = prepare_bundle(args.spec, args.output)
    print(json.dumps({'prepared': True, **fragment['preparation']['counts'],
                      'cache_accounts': len(fragment['caches']), 'custom_sources_unchanged': True,
                      'installed_files_modified': False, 'junction_created': False}))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, KeyError, TypeError, OSError) as error:
        # Avoid echoing catalog titles, executable names or account paths into summaries.
        print(json.dumps({'prepared': False, 'error_type': type(error).__name__,
                          'reason': str(error) if isinstance(error, ValueError) else 'invalid or inaccessible input'}))
        raise SystemExit(1)
