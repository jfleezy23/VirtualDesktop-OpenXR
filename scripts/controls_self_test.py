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


def exercise_depth_mode_selector(app, control, root, read, current_snapshot, check):
    """Drive the real readonly selector; keep the independent settings intact."""
    choices = (("Standard", 0), ("Depth assist", 1), ("Depth reconstruction (experimental)", 2))
    selector = control(ttk.Combobox, "textvariable", app.frame_generation_depth_mode)
    check(str(selector.cget("state")) == "readonly", "depth-mode: selector readonly")
    check(tuple(selector.cget("values")) == tuple(label for label, _ in choices),
          "depth-mode: selector has exactly three modes")

    def without_depth(state):
        return {path: ([row for row in values if row[0] != gui.FRAME_GENERATION_DEPTH_MODE_NAME]
                       if path == gui.REGISTRY_PATH and values is not None else values)
                for path, values in state.items()}

    independent = without_depth(current_snapshot())
    for label, value in choices:
        selector.set(label)
        selector.event_generate("<<ComboboxSelected>>")
        root.update()
        check(read(gui.FRAME_GENERATION_DEPTH_MODE_NAME) == (value, winreg.REG_DWORD),
              f"depth-mode: global DWORD {value}")
        check(without_depth(current_snapshot()) == independent,
              "depth-mode: global selection preserves other settings")


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
        check(app.frame_generation_depth_mode.get() == "Depth assist"
              and app.profile_store.load()[1][gui.FRAME_GENERATION_DEPTH_MODE_NAME] == "default",
              "depth-mode: unset request inherits default depth assist")
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
        spatial_before = snapshot()[gui.REGISTRY_PATH]
        app.slider_widgets[-1].set(95)
        app.processing_resolution_slider.set(80)
        check(app.variables[-1].get() == 95
              and str(app.slider_widgets[-1].cget("state")) == "normal",
              "combined request retains independent editable coverage")
        check(snapshot()[gui.REGISTRY_PATH] == spatial_before, "both spatial controls debounce")
        wait_for_callbacks()
        check(read(gui.PROCESSING_RESOLUTION_VALUE_NAME) == (80, winreg.REG_DWORD)
              and read("FoveationSize")[0] == 95, "independent debounce saves 95/80")
        appearance_before = [(name, *read(name)) for name in gui.VALUE_NAMES[:-1]]
        app.variables[-1].set(90)
        wait_for_callbacks()
        check(read("FoveationSize")[0] == 90 and read(gui.PROCESSING_RESOLUTION_VALUE_NAME)[0] == 80,
              "partial coverage can change while processing remains reduced")
        check([(name, *read(name)) for name in gui.VALUE_NAMES[:-1]] == appearance_before,
              "combined spatial edits preserve appearance settings")
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
        backend_selector.set("NVIDIA (default)")
        backend_selector.event_generate("<<ComboboxSelected>>")
        root.update()
        exercise_depth_mode_selector(app, control, root, read, snapshot, check)
        app.processing_resolution_slider.set(100)
        wait_for_callbacks()
        check(str(app.slider_widgets[-1].cget("state")) == "normal",
              "coverage remains editable with native processing")
        app.slider_widgets[-1].set(95)
        wait_for_callbacks()
        check(read("FoveationSize")[0] == 95, "native processing retains partial coverage support")
        app.slider_widgets[-1].set(90)
        app.processing_resolution_slider.set(85)
        app.on_close()
        root = None
        check(read("FoveationSize")[0] == 90, "closing flushes pending coverage independently")
        check(read(gui.PROCESSING_RESOLUTION_VALUE_NAME) == (85, winreg.REG_DWORD),
              "closing flushes pending processing resolution")
        root = tk.Tk()
        root.withdraw()
        app = gui.VDXRRegistryApp(root)
        check(app.variables[-1].get() == 90 and app.enabled.get(), "restart preserves saved combined 90/85 pair")
        check(app.processing_resolution.get() == 85, "restart restores processing resolution")
        check(app.frame_generation_depth_mode.get() == "Depth reconstruction (experimental)"
              and read(gui.FRAME_GENERATION_DEPTH_MODE_NAME) == (2, winreg.REG_DWORD),
              "depth-mode: global request survives restart")
        control(ttk.Button, "text", "Reset NR settings").invoke()
        check([read(name)[0] for name in gui.VALUE_NAMES] == list(gui.VALUE_DEFAULTS), "reset persists every NR default immediately")
        check(read(gui.ENABLE_VALUE_NAME)[0] == 0, "reset disables NR")
        check(read(gui.PROCESSING_RESOLUTION_VALUE_NAME) == (100, winreg.REG_DWORD)
              and app.processing_resolution.get() == 100, "reset restores native processing resolution")
        check(read(gui.FRAME_GENERATION_VALUE_NAME)[0] == 4 and read(gui.FRAME_GENERATION_BACKEND_NAME)[0] == 0,
              "NR reset preserves MFG and NVIDIA selections")
        check(read(gui.FRAME_GENERATION_DEPTH_MODE_NAME) == (2, winreg.REG_DWORD),
              "depth-mode: NR reset preserves request")
        check(snapshot()[runtime_path] == runtime_before_nr and app.runtime_selection.get() == runtime.CUSTOM_LABEL,
              "NR and FG changes and NR reset preserve runtime selection")
        depth_selector = control(ttk.Combobox, "textvariable", app.frame_generation_depth_mode)
        depth_selector.set("Standard")
        depth_selector.event_generate("<<ComboboxSelected>>")
        root.update()
        check(read(gui.FRAME_GENERATION_DEPTH_MODE_NAME) == (0, winreg.REG_DWORD),
              "depth-mode: profile inheritance baseline DWORD 0")
        globals_before_profile = snapshot()
        executable = r"C:\ProfileFixtures\One\fixture.exe"
        app.load_scope(executable)
        global_values = app.profile_store.load()[0]
        check(str(app.save_profile_button.cget("state")) == "normal" and not app.profile_dirty,
              "inherited game setup can be saved without edits")
        app.save_profile_button.invoke()
        saved, sources = app.profile_store.load(executable)
        check(saved == global_values and set(sources.values()) == {"profile"},
              "unchanged Save locks all displayed NR and FG fields")
        app.profile_store.save(None, {"DLSSNR_Intensity": 63, gui.FRAME_GENERATION_VALUE_NAME: 2,
                                      gui.FRAME_GENERATION_DEPTH_MODE_NAME: 1})
        check(app.profile_store.load(executable)[0] == global_values,
              "locked game setup survives global appearance and FG changes")
        app.reset_defaults()
        inherited, sources = app.profile_store.load(executable)
        check(inherited["DLSSNR_Intensity"] == 63 and inherited[gui.FRAME_GENERATION_VALUE_NAME] == 2
              and set(sources.values()) == {"global"}, "Reset returns game to current globals")
        check(inherited[gui.FRAME_GENERATION_DEPTH_MODE_NAME] == 1
              and sources[gui.FRAME_GENERATION_DEPTH_MODE_NAME] == "global"
              and app.frame_generation_depth_mode.get() == "Depth assist",
              "depth-mode: reset inherits changed global DWORD 1")
        app.profile_store.save(None, {"DLSSNR_Intensity": global_values["DLSSNR_Intensity"],
                                      gui.FRAME_GENERATION_VALUE_NAME: global_values[gui.FRAME_GENERATION_VALUE_NAME],
                                      gui.FRAME_GENERATION_DEPTH_MODE_NAME: global_values[gui.FRAME_GENERATION_DEPTH_MODE_NAME]})
        app.load_scope(executable)
        app.enabled.set(True)
        app.on_enabled_changed()
        app.variables[1].set(81)
        app.processing_resolution.set(80)
        app.frame_generation.set("OFXR 2×")
        app.on_frame_generation_changed()
        depth_selector.set("Depth reconstruction (experimental)")
        depth_selector.event_generate("<<ComboboxSelected>>")
        root.update()
        check(app.profile_store.load(executable)[0][gui.FRAME_GENERATION_DEPTH_MODE_NAME] == 0
              and app.frame_generation_depth_mode.get() == "Depth reconstruction (experimental)"
              and snapshot() == globals_before_profile,
              "depth-mode: game request 2 is draft until Save")
        check(snapshot() == globals_before_profile and app.profile_store.list_profiles() == [],
              "game profile edits are drafts; globals unchanged")
        check(app.save_profile(), "explicit game profile Save")
        saved, sources = app.profile_store.load(executable)
        check(saved[gui.ENABLE_VALUE_NAME] == 1 and saved["DLSSNR_Intensity"] == 81
              and saved[gui.PROCESSING_RESOLUTION_VALUE_NAME] == 80
              and saved[gui.FRAME_GENERATION_VALUE_NAME] == 2,
              "complete game NR and FG round-trip")
        check(set(sources.values()) == {"profile"} and snapshot() == globals_before_profile,
              "game Save locks every displayed field without global writes")
        check(saved[gui.FRAME_GENERATION_DEPTH_MODE_NAME] == 2
              and sources[gui.FRAME_GENERATION_DEPTH_MODE_NAME] == "profile",
              "depth-mode: saved game DWORD 2")
        app.load_scope(None)
        app.load_scope(executable)
        check(app.frame_generation_depth_mode.get() == "Depth reconstruction (experimental)"
              and app.profile_store.load(executable)[0][gui.FRAME_GENERATION_DEPTH_MODE_NAME] == 2,
              "depth-mode: saved game request survives reload")
        app.reset_defaults()
        check(app.profile_store.list_profiles() == [] and snapshot() == globals_before_profile,
              "Reset game profile removes overrides and retains globals")
        inherited, sources = app.profile_store.load(executable)
        check(inherited[gui.FRAME_GENERATION_DEPTH_MODE_NAME] == 0
              and sources[gui.FRAME_GENERATION_DEPTH_MODE_NAME] == "global"
              and app.frame_generation_depth_mode.get() == "Standard",
              "depth-mode: game reset inherits global DWORD 0")
        check(snapshot() == globals_before_profile, "depth-mode: game operations preserve globals")
        app.load_scope(None)
        app.on_close()
        root = None
        for coverage, processing in ((95, 80), (90, 85), (90, 80), (100, 80)):
            set_value("FoveationSize", coverage)
            set_value(gui.PROCESSING_RESOLUTION_VALUE_NAME, processing)
            combined_before = snapshot()
            root = tk.Tk()
            root.withdraw()
            app = gui.VDXRRegistryApp(root)
            check((app.variables[-1].get(), app.processing_resolution.get()) == (coverage, processing)
                  and snapshot() == combined_before, f"saved {coverage}/{processing} startup is read-only")
            app.on_close()
            root = None
            check(snapshot() == combined_before, f"unchanged {coverage}/{processing} close is read-only")
        for coverage in ("malformed", None):
            if coverage is None:
                with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, gui.REGISTRY_PATH, 0,
                                    winreg.KEY_SET_VALUE | winreg.KEY_WOW64_64KEY) as key:
                    winreg.DeleteValue(key, "FoveationSize")
            else:
                set_value("FoveationSize", coverage, winreg.REG_SZ)
            before = snapshot()
            root = tk.Tk()
            root.withdraw()
            app = gui.VDXRRegistryApp(root)
            check(snapshot() == before and app.variables[-1].get() == 100
                  and app.processing_resolution.get() == 80
                  and ("invalid" if coverage else "not saved") in app.status.get().lower(),
                  f"startup warns without overwriting coverage: {type(coverage).__name__}")
            app.on_close()
            root = None
        set_value(gui.FRAME_GENERATION_DEPTH_MODE_NAME, "malformed", winreg.REG_SZ)
        depth_malformed_before = snapshot()
        root = tk.Tk()
        root.withdraw()
        app = gui.VDXRRegistryApp(root)
        check(app.frame_generation_depth_mode.get() == "Depth assist"
              and gui.FRAME_GENERATION_DEPTH_MODE_NAME in app.status.get()
              and "invalid" in app.status.get().lower() and snapshot() == depth_malformed_before,
              "depth-mode: malformed startup warns without writes")
        app.on_close()
        root = None
        check(snapshot() == depth_malformed_before, "depth-mode: malformed close preserves stored request")
        set_value(gui.FRAME_GENERATION_DEPTH_MODE_NAME, 2)
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
        root.update_idletasks()
        result.update(tcl_patchlevel=root.tk.call("info", "patchlevel"),
                      tk_patchlevel=root.tk.call("set", "tk_patchLevel"),
                      window_required=[root.winfo_reqwidth(), root.winfo_reqheight()],
                      screen_size=[root.winfo_screenwidth(), root.winfo_screenheight()],
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
        for suffix in ("\\" + gui.REGISTRY_PATH + "\\Profiles",
                       "\\" + gui.REGISTRY_PATH, "\\" + gui.runtime_selection.RUNTIME_REGISTRY_PATH,
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
