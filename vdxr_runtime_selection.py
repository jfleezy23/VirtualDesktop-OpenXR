"""Select the next-launch VDXR host without loading runtime code.

The managed path and adjacent manifest are installed by the administrator.
This module changes only redirect_to, using the 64-bit HKLM registry view.
"""
import hashlib
import json
import os
from pathlib import Path
import winreg


RUNTIME_REGISTRY_PATH = r"SOFTWARE\VirtualDesktop-OpenXR"
REDIRECT_VALUE_NAME = "redirect_to"
CUSTOM_PATH_VALUE_NAME = "custom_runtime_path"
STOCK_LABEL = "Stock VDXR"
CUSTOM_LABEL = "Custom VDXR"
OTHER_LABEL = "Other/unknown override"
CHOICES = (STOCK_LABEL, CUSTOM_LABEL)


def read_path(name):
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, RUNTIME_REGISTRY_PATH, 0,
                            winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
            value, kind = winreg.QueryValueEx(key, name)
    except FileNotFoundError:
        return None
    if kind != winreg.REG_SZ or not isinstance(value, str) or not value or "\0" in value:
        raise ValueError(f"Invalid saved runtime {name}; expected an absolute REG_SZ path.")
    path = Path(value)
    if not path.is_absolute():
        raise ValueError(f"Invalid saved runtime {name}; path must be absolute.")
    return path


def validated_custom_runtime():
    path = read_path(CUSTOM_PATH_VALUE_NAME)
    if path is None:
        raise ValueError("Custom VDXR is not installed; managed runtime path is missing.")
    if path.name.casefold() != "virtualdesktop-openxr.dll" or not path.is_file():
        raise ValueError("Custom VDXR requires the installed virtualdesktop-openxr.dll host.")
    try:
        metadata = json.loads(path.with_name("CUSTOM-RUNTIME.json").read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise ValueError("Cannot validate the installed CUSTOM-RUNTIME.json manifest.") from error
    if not isinstance(metadata, dict) or type(metadata.get("schema")) is not int or metadata["schema"] != 1 or \
            metadata.get("self_redirect_guard") is not True:
        raise ValueError("Custom VDXR manifest must confirm schema 1 and the self-redirect guard.")
    with path.open("rb") as host:
        digest = hashlib.file_digest(host, "sha256").hexdigest()
    if metadata.get("host_sha256") != digest:
        raise ValueError("Custom VDXR host does not match its installed manifest hash.")
    return path


def read_selection():
    redirect = read_path(REDIRECT_VALUE_NAME)
    if redirect is None:
        return STOCK_LABEL
    managed = read_path(CUSTOM_PATH_VALUE_NAME)
    if managed is None or os.path.normcase(os.path.normpath(redirect)) != os.path.normcase(os.path.normpath(managed)):
        return OTHER_LABEL
    validated_custom_runtime()
    return CUSTOM_LABEL


def write_selection(label):
    if label == CUSTOM_LABEL:
        path = validated_custom_runtime()
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, RUNTIME_REGISTRY_PATH, 0,
                            winreg.KEY_SET_VALUE | winreg.KEY_WOW64_64KEY) as key:
            winreg.SetValueEx(key, REDIRECT_VALUE_NAME, 0, winreg.REG_SZ, str(path))
    elif label == STOCK_LABEL:
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, RUNTIME_REGISTRY_PATH, 0,
                                winreg.KEY_SET_VALUE | winreg.KEY_WOW64_64KEY) as key:
                winreg.DeleteValue(key, REDIRECT_VALUE_NAME)
        except FileNotFoundError:
            pass
    else:
        raise ValueError("Select Stock VDXR or Custom VDXR.")
    if read_selection() != label:
        raise OSError("Runtime selection readback did not match; the selection is not verified.")
