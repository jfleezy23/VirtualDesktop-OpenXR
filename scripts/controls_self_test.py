"""Exercise the shipped Tk app using real Windows registry APIs in a private hive.

The override is process-local and must succeed before constructing the controls.
Nothing in this mode writes the production HKLM settings or loads a graphics DLL.
"""
import ctypes
from ctypes import wintypes
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import time
import tkinter as tk
from tkinter import ttk
import uuid
import winreg

import vdxr_registry_gui as gui


def snapshot():
    result = {}
    for path in (gui.REGISTRY_PATH, gui.runtime_selection.RUNTIME_REGISTRY_PATH):
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, path, 0,
                                winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
                values = []
                for index in range(winreg.QueryInfoKey(key)[1]):
                    values.append(winreg.EnumValue(key, index))
                result[path] = sorted(values)
        except FileNotFoundError:
            result[path] = None
    return result


def run(result_path):
    result = {"passed": False, "frozen": bool(getattr(sys, "frozen", False)),
              "elevated": gui.is_elevated(), "checks": []}
    root = None
    target = None
    temporary = None
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

    def read(name, path=gui.REGISTRY_PATH):
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, path, 0,
                            winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
            return winreg.QueryValueEx(key, name)

    def set_value(name, value, kind=winreg.REG_DWORD, path=gui.REGISTRY_PATH):
        with winreg.CreateKeyEx(winreg.HKEY_LOCAL_MACHINE, path, 0,
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
        root.withdraw()
        app = gui.VDXRRegistryApp(root)
        root.update()
        check(snapshot() == before, "normal startup is read-only")
        check(app.variables[-1].get() == 100 and not app.enabled.get(), "fresh defaults: NR off, coverage 100")
        check(app.processing_resolution.get() == 100, "fresh processing resolution is native 100")
        runtime = gui.runtime_selection
        runtime_path = runtime.RUNTIME_REGISTRY_PATH
        check(app.runtime_selection.get() == runtime.STOCK_LABEL, "missing redirect displays Stock VDXR")
        temporary = tempfile.TemporaryDirectory(prefix="VDXRControlsSelfTest-")
        host = Path(temporary.name) / "virtualdesktop-openxr.dll"
        host.write_bytes(b"controls self-test fixture; never loaded")
        host.with_name("CUSTOM-RUNTIME.json").write_text(json.dumps({
            "schema": 1, "self_redirect_guard": True,
            "host_sha256": hashlib.sha256(host.read_bytes()).hexdigest(),
            "source_revision": "private-self-test",
        }), encoding="utf-8")
        set_value(runtime.CUSTOM_PATH_VALUE_NAME, str(host), winreg.REG_SZ, runtime_path)
        set_value("PrivateRuntimeMarker", 19, path=runtime_path)
        nr_before_switch = snapshot()[gui.REGISTRY_PATH]
        runtime_selector = control(ttk.Combobox, "textvariable", app.runtime_selection)
        check(str(runtime_selector.cget("state")) == "readonly", "runtime selector restricts input")
        for label in (runtime.CUSTOM_LABEL, runtime.STOCK_LABEL, runtime.CUSTOM_LABEL):
            runtime_selector.set(label)
            runtime_selector.event_generate("<<ComboboxSelected>>")
            root.update()
            check(runtime.read_selection() == label, f"actual runtime switch: {label}")
            if label == runtime.CUSTOM_LABEL:
                check(read(runtime.REDIRECT_VALUE_NAME, runtime_path) == (str(host), winreg.REG_SZ),
                      "custom writes real REG_SZ redirect in private HKLM")
            else:
                check(not any(value[0] == runtime.REDIRECT_VALUE_NAME for value in snapshot()[runtime_path]),
                      "stock removes only redirect")
            check(read(runtime.CUSTOM_PATH_VALUE_NAME, runtime_path) == (str(host), winreg.REG_SZ)
                  and read("PrivateRuntimeMarker", runtime_path) == (19, winreg.REG_DWORD),
                  "switch retains managed path and unrelated runtime value")
        check(snapshot()[gui.REGISTRY_PATH] == nr_before_switch, "runtime switching preserves NR and FG values")
        runtime_before_nr = snapshot()[runtime_path]
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
        app.processing_resolution_slider.set(90)
        wait_for_callbacks()
        check(read(gui.PROCESSING_RESOLUTION_VALUE_NAME) == (90, winreg.REG_DWORD)
              and read("FoveationSize")[0] == 94, "processing resolution saves 90 independently of coverage")
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
        app.processing_resolution_slider.set(85)
        app.on_close()
        root = None
        check(read("FoveationSize")[0] == 95, "closing flushes pending coverage")
        check(read(gui.PROCESSING_RESOLUTION_VALUE_NAME) == (85, winreg.REG_DWORD),
              "closing flushes pending processing resolution")
        root = tk.Tk()
        root.withdraw()
        app = gui.VDXRRegistryApp(root)
        check(app.variables[-1].get() == 95 and app.enabled.get(), "restart restores saved values")
        check(app.processing_resolution.get() == 85, "restart restores processing resolution")
        control(ttk.Button, "text", "Reset NR settings").invoke()
        check([read(name)[0] for name in gui.VALUE_NAMES] == list(gui.VALUE_DEFAULTS), "reset persists every NR default immediately")
        check(read(gui.ENABLE_VALUE_NAME)[0] == 0, "reset disables NR")
        check(read(gui.PROCESSING_RESOLUTION_VALUE_NAME) == (100, winreg.REG_DWORD)
              and app.processing_resolution.get() == 100, "reset restores native processing resolution")
        check(read(gui.FRAME_GENERATION_VALUE_NAME)[0] == 3 and read(gui.FRAME_GENERATION_BACKEND_NAME)[0] == 1,
              "NR reset preserves FG selections")
        check(snapshot()[runtime_path] == runtime_before_nr and app.runtime_selection.get() == runtime.CUSTOM_LABEL,
              "NR and FG changes and NR reset preserve runtime selection")
        app.on_close()
        root = None
        for name in ("DLSSNR_Intensity", gui.ENABLE_VALUE_NAME, gui.FRAME_GENERATION_VALUE_NAME,
                     gui.FRAME_GENERATION_BACKEND_NAME, gui.PROCESSING_RESOLUTION_VALUE_NAME):
            set_value(name, "malformed", winreg.REG_SZ)
        set_value(runtime.REDIRECT_VALUE_NAME, 8, path=runtime_path)
        malformed_before = snapshot()
        root = tk.Tk()
        root.withdraw()
        app = gui.VDXRRegistryApp(root)
        check("invalid" in app.status.get().lower(), "malformed settings open with warning")
        check(not app.enabled.get() and app.variables[1].get() == 100, "malformed settings have safe displayed defaults")
        check(app.processing_resolution.get() == 100, "malformed processing resolution displays native 100")
        check(app.runtime_selection.get() == runtime.OTHER_LABEL and snapshot() == malformed_before,
              "malformed runtime selection opens honestly without writes")
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
        for suffix in ("\\" + gui.REGISTRY_PATH, "\\" + gui.runtime_selection.RUNTIME_REGISTRY_PATH,
                       r"\SOFTWARE\Virtual Desktop, Inc.", r"\SOFTWARE", ""):
            try:
                winreg.DeleteKeyEx(winreg.HKEY_CURRENT_USER, owned + suffix, winreg.KEY_WOW64_64KEY)
            except FileNotFoundError:
                pass
            except OSError as error:
                result["error"] = f"Private-key cleanup failed: {error}"
        if temporary is not None:
            temporary.cleanup()
        try:
            check(snapshot() == original, "both production registry keys unchanged")
        except Exception as error:
            result["error"] = str(error)
        result["passed"] = not result.get("error")
        # Exclusive creation prevents accidentally overwriting an existing file.
        with Path(result_path).open("x", encoding="utf-8") as output:
            json.dump(result, output, indent=2)
    return 0 if result["passed"] else 1
