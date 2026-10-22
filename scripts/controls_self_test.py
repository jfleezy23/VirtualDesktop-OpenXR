"""Exercise the shipped Tk app using real Windows registry APIs in a private hive.

The override is process-local and must succeed before constructing the controls.
Nothing in this mode writes the production HKLM settings or loads a graphics DLL.
"""
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import sys
import time
import tkinter as tk
from tkinter import ttk
import uuid
import winreg

import vdxr_registry_gui as gui


def snapshot():
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, gui.REGISTRY_PATH, 0,
                            winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
            values = []
            for index in range(winreg.QueryInfoKey(key)[1]):
                values.append(winreg.EnumValue(key, index))
            return sorted(values)
    except FileNotFoundError:
        return None


def run(result_path):
    result = {"passed": False, "frozen": bool(getattr(sys, "frozen", False)),
              "elevated": gui.is_elevated(), "checks": []}
    root = None
    target = None
    mapped = False
    owned = r"Software\VDXRControlsSelfTest-" + uuid.uuid4().hex
    override = ctypes.WinDLL("advapi32", use_last_error=True).RegOverridePredefKey
    override.argtypes = [wintypes.HKEY, wintypes.HKEY]
    override.restype = wintypes.LONG
    hklm = wintypes.HKEY(ctypes.c_int32(int(winreg.HKEY_LOCAL_MACHINE)).value)
    original = snapshot()

    def check(condition, name):
        if not condition:
            raise AssertionError(name)
        result["checks"].append(name)

    def read(name):
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, gui.REGISTRY_PATH, 0,
                            winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
            return winreg.QueryValueEx(key, name)

    def set_value(name, value, kind=winreg.REG_DWORD):
        with winreg.CreateKeyEx(winreg.HKEY_LOCAL_MACHINE, gui.REGISTRY_PATH, 0,
                               winreg.KEY_ALL_ACCESS | winreg.KEY_WOW64_64KEY) as key:
            winreg.SetValueEx(key, name, 0, kind, value)

    def wait_for_callbacks(milliseconds=450):
        end = time.monotonic() + milliseconds / 1000
        while time.monotonic() < end:
            root.update()
            time.sleep(0.01)

    def control(widget_type, option, value):
        def descendants(parent):
            for child in parent.winfo_children():
                yield child
                yield from descendants(child)
        matches = [widget for widget in descendants(root)
                   if isinstance(widget, widget_type) and str(widget.cget(option)) == str(value)]
        if len(matches) != 1:
            raise AssertionError(f"Expected one {widget_type.__name__} for {option}={value}, found {len(matches)}")
        return matches[0]

    try:
        target = winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, owned, 0,
                                   winreg.KEY_ALL_ACCESS | winreg.KEY_WOW64_64KEY)
        code = override(hklm, wintypes.HKEY(int(target)))
        if code:
            raise OSError(code, "Private registry override failed; test aborted")
        mapped = True
        set_value("PrivateTestMarker", 1)
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, owned + "\\" + gui.REGISTRY_PATH,
                            0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
            check(winreg.QueryValueEx(key, "PrivateTestMarker")[0] == 1, "process-private registry mapping")
        before = snapshot()
        root = tk.Tk()
        app = gui.VDXRRegistryApp(root)
        root.update()
        check(snapshot() == before, "normal startup is read-only")
        check(app.variables[-1].get() == 100 and not app.enabled.get(), "fresh defaults: NR off, coverage 100")
        toggle = control(ttk.Checkbutton, "variable", app.enabled)
        toggle.invoke()
        check(read("FoveationSize") == (100, winreg.REG_DWORD), "enable persists actual coverage 100")
        check(read(gui.ENABLE_VALUE_NAME) == (1, winreg.REG_DWORD), "actual NR toggle")
        for index, scale in enumerate(app.slider_widgets[:-1]):
            requested = 1 if index == 0 else 77 + index
            scale.set(requested)
            root.update()
            check(read(gui.VALUE_NAMES[index]) == (requested, winreg.REG_DWORD)
                  and app.variables[index].get() == requested, f"slider {gui.VALUE_NAMES[index]}")
        app.slider_widgets[-1].set(94)
        wait_for_callbacks()
        check(read("FoveationSize")[0] == 94, "coverage debounce persists")
        mode_selector = control(ttk.Combobox, "textvariable", app.frame_generation)
        backend_selector = control(ttk.Combobox, "textvariable", app.frame_generation_backend)
        check(str(mode_selector.cget("state")) == str(backend_selector.cget("state")) == "readonly",
              "FG selectors restrict input to supported choices")
        for label, value in gui.FRAME_GENERATION_MODES.items():
            mode_selector.set(label)
            mode_selector.event_generate("<<ComboboxSelected>>")
            root.update()
            check(read(gui.FRAME_GENERATION_VALUE_NAME)[0] == value, f"FG mode {value}")
        for label, value in gui.FRAME_GENERATION_BACKENDS.items():
            backend_selector.set(label)
            backend_selector.event_generate("<<ComboboxSelected>>")
            root.update()
            check(read(gui.FRAME_GENERATION_BACKEND_NAME)[0] == value, f"FG backend {value}")
        app.slider_widgets[-1].set(95)
        app.on_close()
        root = None
        check(read("FoveationSize")[0] == 95, "closing flushes pending coverage")
        root = tk.Tk()
        app = gui.VDXRRegistryApp(root)
        check(app.variables[-1].get() == 95 and app.enabled.get(), "restart restores saved values")
        control(ttk.Button, "text", "Reset NR settings").invoke()
        check([read(name)[0] for name in gui.VALUE_NAMES] == list(gui.VALUE_DEFAULTS), "reset persists every NR default immediately")
        check(read(gui.ENABLE_VALUE_NAME)[0] == 0, "reset disables NR")
        check(read(gui.FRAME_GENERATION_VALUE_NAME)[0] == 3 and read(gui.FRAME_GENERATION_BACKEND_NAME)[0] == 1,
              "NR reset preserves FG selections")
        app.on_close()
        root = None
        for name in ("DLSSNR_Intensity", gui.ENABLE_VALUE_NAME, gui.FRAME_GENERATION_VALUE_NAME,
                     gui.FRAME_GENERATION_BACKEND_NAME):
            set_value(name, "malformed", winreg.REG_SZ)
        root = tk.Tk()
        app = gui.VDXRRegistryApp(root)
        check("invalid" in app.status.get().lower(), "malformed settings open with warning")
        check(not app.enabled.get() and app.variables[1].get() == 100, "malformed settings have safe displayed defaults")
        result.update(tcl_patchlevel=root.tk.call("info", "patchlevel"),
                      tk_patchlevel=root.tk.call("set", "tk_patchLevel"),
                      executable=sys.executable, bundle_root=getattr(sys, "_MEIPASS", None))
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
    finally:
        if root is not None:
            root.destroy()
        if mapped:
            code = override(hklm, None)
            if code:
                result["error"] = f"Could not restore registry mapping: {code}"
        if target is not None:
            target.Close()
        for suffix in ("\\" + gui.REGISTRY_PATH, r"\SOFTWARE\Virtual Desktop, Inc.", r"\SOFTWARE", ""):
            try:
                winreg.DeleteKeyEx(winreg.HKEY_CURRENT_USER, owned + suffix, winreg.KEY_WOW64_64KEY)
            except FileNotFoundError:
                pass
            except OSError as error:
                result["error"] = f"Private-key cleanup failed: {error}"
        if not result.get("error"):
            try:
                check(snapshot() == original, "production registry unchanged")
            except Exception as error:
                result["error"] = str(error)
        result["passed"] = not result.get("error")
        # Exclusive creation prevents accidentally overwriting an existing file.
        with Path(result_path).open("x", encoding="utf-8") as output:
            json.dump(result, output, indent=2)
    return 0 if result["passed"] else 1
