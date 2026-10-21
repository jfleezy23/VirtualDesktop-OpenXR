import ctypes
import os
import subprocess
import sys
import tkinter as tk
from tkinter import messagebox, ttk
import winreg


#REGISTRY_PATH = r"SOFTWARE\VirtualDesktop-OpenXR"
REGISTRY_PATH = r"SOFTWARE\Virtual Desktop, Inc.\OpenXR"
ENABLE_VALUE_NAME = "DLSSNR_Enabled"
VALUE_NAMES = (
    "DLSSNR_Style",
    "DLSSNR_Intensity",
    "DLSSNR_LocalToneStrength",
    "DLSSNR_LocalStructureStrength",
    "DLSSNR_SkinStructureStrength",
    "FoveationSize",
)
VALUE_MAXIMUMS = (2, 100, 100, 100, 100, 100)
VALUE_DEFAULTS = (0, 100, 20, 70, 50, 100)
VALUE_LABELS = ("NR style (0–2)", "NR strength", "Local tone", "Structure detail", "Skin detail", "NR coverage (%)")
FRAME_GENERATION_VALUE_NAME = "FrameGenerationMode"
FRAME_GENERATION_MODES = {"Off": 0, "OFXR 2×": 2, "OFXR 3×": 3}
FRAME_GENERATION_BACKEND_NAME = "FrameGenerationBackend"
FRAME_GENERATION_BACKENDS = {"NVIDIA (default)": 0, "FidelityFX (full resolution)": 1}


def read_frame_generation_backend():
    try:
        with winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE, REGISTRY_PATH, 0,
            winreg.KEY_READ | winreg.KEY_WOW64_64KEY,
        ) as key:
            value, kind = winreg.QueryValueEx(key, FRAME_GENERATION_BACKEND_NAME)
    except FileNotFoundError:
        return 0
    if kind != winreg.REG_DWORD or value not in FRAME_GENERATION_BACKENDS.values():
        raise ValueError("Invalid saved frame-generation backend; select NVIDIA or FidelityFX.")
    return value


def write_frame_generation_backend(value):
    if value not in FRAME_GENERATION_BACKENDS.values():
        raise ValueError("Unsupported frame-generation backend")
    with winreg.CreateKeyEx(
        winreg.HKEY_LOCAL_MACHINE, REGISTRY_PATH, 0,
        winreg.KEY_SET_VALUE | winreg.KEY_WOW64_64KEY,
    ) as key:
        winreg.SetValueEx(key, FRAME_GENERATION_BACKEND_NAME, 0, winreg.REG_DWORD, value)
    if read_frame_generation_backend() != value:
        raise OSError("Frame-generation backend readback did not match the selection")


def read_frame_generation_mode():
    try:
        with winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE, REGISTRY_PATH, 0,
            winreg.KEY_READ | winreg.KEY_WOW64_64KEY,
        ) as key:
            value, kind = winreg.QueryValueEx(key, FRAME_GENERATION_VALUE_NAME)
    except FileNotFoundError:
        return 0
    if kind != winreg.REG_DWORD or value not in FRAME_GENERATION_MODES.values():
        raise ValueError("Invalid saved frame-generation setting; select Off, OFXR 2× or OFXR 3×.")
    return value


def write_frame_generation_mode(value):
    if value not in FRAME_GENERATION_MODES.values():
        raise ValueError("Unsupported frame-generation mode")
    with winreg.CreateKeyEx(
        winreg.HKEY_LOCAL_MACHINE, REGISTRY_PATH, 0,
        winreg.KEY_SET_VALUE | winreg.KEY_WOW64_64KEY,
    ) as key:
        winreg.SetValueEx(key, FRAME_GENERATION_VALUE_NAME, 0, winreg.REG_DWORD, value)
    if read_frame_generation_mode() != value:
        raise OSError("Frame-generation setting readback did not match the selection")


def is_elevated():
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except AttributeError:
        return False


def relaunch_as_admin():
    executable = sys.executable
    arguments = sys.argv[1:]
    if not getattr(sys, "frozen", False):
        arguments = [os.path.abspath(sys.argv[0]), *arguments]
    parameters = subprocess.list2cmdline(arguments)
    result = ctypes.windll.shell32.ShellExecuteW(
        None, "runas", executable, parameters, None, 1
    )
    if result <= 32:
        raise OSError(f"Could not request administrator access (error {result}).")


def read_nr_value(name, default, maximum, errors):
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, REGISTRY_PATH, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
            value, kind = winreg.QueryValueEx(key, name)
    except FileNotFoundError:
        if name == "FoveationSize":
            errors.append("NR coverage is not saved; showing 100%. Toggle NR off/on to apply displayed defaults.")
        return default
    except OSError:
        errors.append(f"Cannot read {name}; showing its default.")
        return default
    if kind != winreg.REG_DWORD or not isinstance(value, int) or not 0 <= value <= maximum:
        errors.append(f"Invalid saved {name}; showing its default.")
        return default
    return value


def read_values(errors=None):
    errors = [] if errors is None else errors
    return [read_nr_value(name, default, maximum, errors)
            for name, default, maximum in zip(VALUE_NAMES, VALUE_DEFAULTS, VALUE_MAXIMUMS)]


def read_enabled(errors=None):
    return bool(read_nr_value(ENABLE_VALUE_NAME, 0, 1, [] if errors is None else errors))


def write_value(name, value):
    maximum = 1 if name == ENABLE_VALUE_NAME else VALUE_MAXIMUMS[VALUE_NAMES.index(name)]
    value = max(0, min(maximum, int(value)))
    with winreg.CreateKeyEx(
        winreg.HKEY_LOCAL_MACHINE, REGISTRY_PATH, 0, winreg.KEY_SET_VALUE | winreg.KEY_WOW64_64KEY
    ) as key:
        winreg.SetValueEx(key, name, 0, winreg.REG_DWORD, int(value))
    with winreg.OpenKey(
        winreg.HKEY_LOCAL_MACHINE, REGISTRY_PATH, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY
    ) as key:
        stored, kind = winreg.QueryValueEx(key, name)
    if kind != winreg.REG_DWORD or stored != value:
        raise OSError(f"{name} readback did not match the selection")


class VDXRRegistryApp:
    def __init__(self, root):
        self.root = root
        self.root.title("VDXR NR/FG Controls")
        self.root.resizable(False, False)
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)
        self.updating = True
        errors = []
        self.saved_values = read_values(errors)
        self.saved_enabled = read_enabled(errors)

        frame = ttk.Frame(root, padding=16)
        frame.grid()

        ttk.Label(frame, text="VDXR NR/FG Controls", font=("Segoe UI", 14, "bold")).grid(
            row=0, column=0, columnspan=3, sticky="w", pady=(0, 12)
        )

        self.enabled = tk.BooleanVar(value=self.saved_enabled)
        ttk.Checkbutton(
            frame,
            text="Enable neural rendering · applies during play",
            variable=self.enabled,
            command=self.on_enabled_changed,
        ).grid(row=1, column=0, columnspan=3, sticky="w", pady=(0, 8))
        ttk.Button(
            frame,
            text="Reset NR settings",
            command=self.reset_defaults,
        ).grid(row=2, column=0, columnspan=3, sticky="w", pady=(0, 8))

        self.variables = []
        self.slider_widgets = []
        self.foveation_save_id = None
        for index, value in enumerate(self.saved_values, start=0):
            variable = tk.IntVar(value=value)
            variable.trace_add("write", lambda *_args, i=index, v=variable: self.save(i, v))
            self.variables.append(variable)

            ttk.Label(frame, text=VALUE_LABELS[index], width=31).grid(
                row=index + 3, column=0, sticky="w", padx=(0, 8), pady=4
            )
            tk.Scale(
                frame,
                from_=0,
                to=VALUE_MAXIMUMS[index],
                resolution=1,
                variable=variable,
                orient="horizontal",
                length=260,
                showvalue=False,
                highlightthickness=0,
            ).grid(row=index + 3, column=1, sticky="ew", pady=4)
            self.slider_widgets.append(frame.grid_slaves(row=index + 3, column=1)[0])
            ttk.Label(frame, textvariable=variable, width=4, anchor="e").grid(
                row=index + 3, column=2, sticky="e", padx=(8, 0), pady=4
            )

        try:
            mode = read_frame_generation_mode()
        except (ValueError, OSError) as error:
            mode = 0
            errors.append(str(error))
        self.saved_frame_generation = next(label for label, value in FRAME_GENERATION_MODES.items() if value == mode)
        self.frame_generation = tk.StringVar(value=self.saved_frame_generation)
        ttk.Label(frame, text="Requested PC frame generation").grid(row=9, column=0, sticky="w", pady=(12, 4))
        selector = ttk.Combobox(
            frame, textvariable=self.frame_generation, values=tuple(FRAME_GENERATION_MODES),
            state="readonly", width=22,
        )
        selector.grid(row=9, column=1, columnspan=2, sticky="ew", pady=(12, 4))
        selector.bind("<<ComboboxSelected>>", self.on_frame_generation_changed)
        try:
            backend = read_frame_generation_backend()
        except (ValueError, OSError) as error:
            backend = 0
            errors.append(str(error))
        self.saved_frame_generation_backend = next(
            label for label, value in FRAME_GENERATION_BACKENDS.items() if value == backend
        )
        self.frame_generation_backend = tk.StringVar(value=self.saved_frame_generation_backend)
        ttk.Label(frame, text="PC frame-generation backend").grid(row=10, column=0, sticky="w", pady=4)
        backend_selector = ttk.Combobox(
            frame, textvariable=self.frame_generation_backend, values=tuple(FRAME_GENERATION_BACKENDS),
            state="readonly", width=30,
        )
        backend_selector.grid(row=10, column=1, columnspan=2, sticky="ew", pady=4)
        backend_selector.bind("<<ComboboxSelected>>", self.on_frame_generation_backend_changed)
        ttk.Label(frame, text="NR sliders save automatically and apply during play.").grid(
            row=11, column=0, columnspan=3, sticky="w",
        )
        ttk.Label(frame, text="Turn SSW off in Virtual Desktop when testing OFXR.").grid(
            row=12, column=0, columnspan=3, sticky="w",
        )
        ttk.Label(frame, text="100% NR coverage is recommended. Reset affects NR only.").grid(
            row=13, column=0, columnspan=3, sticky="w",
        )
        ttk.Label(frame, text="FG selections are requests, not active status. Restart after changes.").grid(
            row=14, column=0, columnspan=3, sticky="w",
        )
        self.status = tk.StringVar(value="; ".join(errors) or "Ready · changes save automatically")
        ttk.Label(frame, textvariable=self.status, wraplength=610).grid(
            row=15, column=0, columnspan=3, sticky="w", pady=(12, 0)
        )
        self.set_slider_state()
        self.updating = False

    def on_frame_generation_backend_changed(self, _event=None):
        selection = self.frame_generation_backend.get()
        try:
            write_frame_generation_backend(FRAME_GENERATION_BACKENDS[selection])
        except PermissionError:
            self.frame_generation_backend.set(self.saved_frame_generation_backend)
            self.status.set("Administrator access is required to save the frame-generation backend.")
        except (OSError, ValueError, KeyError) as error:
            self.frame_generation_backend.set(self.saved_frame_generation_backend)
            self.status.set(f"Frame-generation backend error: {error}")
        else:
            self.saved_frame_generation_backend = selection
            self.status.set(f"Saved {selection}; restart the game.")

    def on_frame_generation_changed(self, _event=None):
        selection = self.frame_generation.get()
        try:
            write_frame_generation_mode(FRAME_GENERATION_MODES[selection])
        except PermissionError:
            self.frame_generation.set(self.saved_frame_generation)
            self.status.set("Administrator access is required to save frame generation.")
        except (OSError, ValueError, KeyError) as error:
            self.frame_generation.set(self.saved_frame_generation)
            self.status.set(f"Frame-generation setting error: {error}")
        else:
            self.saved_frame_generation = selection
            self.status.set(f"Saved {selection}; restart the game.")

    def set_slider_state(self):
        state = tk.NORMAL if self.enabled.get() else tk.DISABLED
        for slider in self.slider_widgets:
            slider.configure(state=state)

    def on_enabled_changed(self):
        try:
            if self.enabled.get():
                # Runtime's missing-value default is 66%, so persist the displayed coverage first.
                self.cancel_foveation_save()
                if not self.save_foveation_size(self.variables[-1]):
                    self.enabled.set(self.saved_enabled)
                    self.set_slider_state()
                    return
                # Explicit enable also makes any displayed fallback values real, verified settings.
                for index, variable in enumerate(self.variables[:-1]):
                    if not self.save_nr_value(index, variable):
                        self.enabled.set(self.saved_enabled)
                        self.set_slider_state()
                        return
            write_value(ENABLE_VALUE_NAME, int(self.enabled.get()))
            self.saved_enabled = self.enabled.get()
            self.set_slider_state()
            state = "enabled" if self.enabled.get() else "disabled"
            self.status.set(f"DLSS-NR {state}")
        except PermissionError:
            self.enabled.set(self.saved_enabled)
            self.set_slider_state()
            self.status.set("Administrator access is required to write HKLM.")
        except OSError as error:
            self.enabled.set(self.saved_enabled)
            self.set_slider_state()
            self.status.set(f"Registry error: {error}")

    def reset_defaults(self):
        self.cancel_foveation_save()
        self.updating = True
        try:
            write_value(ENABLE_VALUE_NAME, 0)
            for name, default in zip(VALUE_NAMES, VALUE_DEFAULTS):
                write_value(name, default)
            self.status.set("NR reset: disabled, coverage 100%. FG selections retained.")
        except PermissionError:
            self.status.set("Administrator access is required to write HKLM.")
        except OSError as error:
            self.status.set(f"Registry error: {error}")
        finally:
            errors = []
            self.saved_values = read_values(errors)
            self.saved_enabled = read_enabled(errors)
            self.enabled.set(self.saved_enabled)
            for variable, value in zip(self.variables, self.saved_values):
                variable.set(value)
            self.set_slider_state()
            self.updating = False
            if errors:
                self.status.set("; ".join(errors))

    def save(self, index, variable):
        if self.updating:
            return
        if VALUE_NAMES[index] == "FoveationSize":
            self.cancel_foveation_save()
            self.foveation_save_id = self.root.after(
                400, self.save_foveation_size, variable
            )
            return

        return self.save_nr_value(index, variable)

    def save_nr_value(self, index, variable):
        try:
            write_value(VALUE_NAMES[index], variable.get())
            self.saved_values[index] = variable.get()
            self.status.set(f"Saved {VALUE_LABELS[index]} = {variable.get()} · applies during play")
            return True
        except PermissionError:
            self.status.set("Administrator access is required to write HKLM.")
        except OSError as error:
            self.status.set(f"Registry error: {error}")
        self.updating = True
        try:
            variable.set(self.saved_values[index])
        finally:
            self.updating = False
        return False

    def save_foveation_size(self, variable):
        self.foveation_save_id = None
        return self.save_nr_value(len(VALUE_NAMES) - 1, variable)

    def cancel_foveation_save(self):
        if self.foveation_save_id is not None:
            self.root.after_cancel(self.foveation_save_id)
            self.foveation_save_id = None

    def on_close(self):
        if self.foveation_save_id is not None:
            self.cancel_foveation_save()
            if not self.save_foveation_size(self.variables[-1]):
                return
        self.root.destroy()


def main():
    if sys.platform != "win32":
        raise SystemExit("This GUI is intended to run on Windows.")

    if not is_elevated():
        try:
            relaunch_as_admin()
        except OSError as error:
            messagebox.showerror("VDXR Settings", str(error))
        return

    root = tk.Tk()
    VDXRRegistryApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
