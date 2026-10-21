import ctypes
import os
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
VALUE_DEFAULTS = (0, 100, 20, 70, 50, 66)
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
    script = os.path.abspath(sys.argv[0])
    parameters = " ".join(f'"{argument}"' for argument in [script, *sys.argv[1:]])
    result = ctypes.windll.shell32.ShellExecuteW(
        None, "runas", executable, parameters, None, 1
    )
    if result <= 32:
        raise OSError(f"Could not request administrator access (error {result}).")


def read_values():
    values = list(VALUE_DEFAULTS)
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, REGISTRY_PATH, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
            for index, name in enumerate(VALUE_NAMES):
                try:
                    value, _ = winreg.QueryValueEx(key, name)
                    values[index] = max(0, min(VALUE_MAXIMUMS[index], int(value)))
                except FileNotFoundError:
                    pass
    except FileNotFoundError:
        pass
    return values


def read_enabled():
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, REGISTRY_PATH, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
            value, _ = winreg.QueryValueEx(key, ENABLE_VALUE_NAME)
            return bool(int(value))
    except FileNotFoundError:
        return False


def write_value(name, value):
    maximum = 2 if name == "DLSSNR_Style" else 100
    value = max(0, min(maximum, int(value)))
    with winreg.CreateKeyEx(
        winreg.HKEY_LOCAL_MACHINE, REGISTRY_PATH, 0, winreg.KEY_SET_VALUE | winreg.KEY_WOW64_64KEY
    ) as key:
        winreg.SetValueEx(key, name, 0, winreg.REG_DWORD, int(value))


class VDXRRegistryApp:
    def __init__(self, root):
        self.root = root
        self.root.title("DLSS-NR Settings")
        self.root.resizable(False, False)

        frame = ttk.Frame(root, padding=16)
        frame.grid()

        ttk.Label(frame, text="VDXR registry values", font=("Segoe UI", 12, "bold")).grid(
            row=0, column=0, columnspan=3, sticky="w", pady=(0, 12)
        )

        self.enabled = tk.BooleanVar(value=read_enabled())
        ttk.Checkbutton(
            frame,
            text="Enable DLSS-NR",
            variable=self.enabled,
            command=self.on_enabled_changed,
        ).grid(row=1, column=0, columnspan=3, sticky="w", pady=(0, 8))
        ttk.Button(
            frame,
            text="Reset to defaults",
            command=self.reset_defaults,
        ).grid(row=2, column=0, columnspan=3, sticky="w", pady=(0, 8))

        self.variables = []
        self.slider_widgets = []
        self.foveation_save_id = None
        for index, value in enumerate(read_values(), start=0):
            variable = tk.IntVar(value=value)
            variable.trace_add("write", lambda *_args, i=index, v=variable: self.save(i, v))
            self.variables.append(variable)

            ttk.Label(frame, text=VALUE_NAMES[index], width=31).grid(
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

        fg_error = None
        try:
            mode = read_frame_generation_mode()
        except (ValueError, OSError) as error:
            mode, fg_error = 0, str(error)
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
            backend, fg_error = 0, str(error)
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
        ttk.Label(frame, text="OFXR works with NR on or off; restart after changing FG.").grid(
            row=11, column=0, columnspan=3, sticky="w",
        )
        ttk.Label(frame, text="Turn SSW off in Virtual Desktop when testing OFXR.").grid(
            row=12, column=0, columnspan=3, sticky="w",
        )
        ttk.Label(frame, text="With NR enabled, use at least 90% coverage (100% recommended).").grid(
            row=13, column=0, columnspan=3, sticky="w",
        )
        ttk.Label(frame, text="FidelityFX is experimental; restart after changing the backend.").grid(
            row=14, column=0, columnspan=3, sticky="w",
        )
        self.status = tk.StringVar(value=fg_error or REGISTRY_PATH)
        ttk.Label(frame, textvariable=self.status).grid(
            row=15, column=0, columnspan=3, sticky="w", pady=(12, 0)
        )
        self.set_slider_state()

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
            write_value(ENABLE_VALUE_NAME, int(self.enabled.get()))
            self.set_slider_state()
            state = "enabled" if self.enabled.get() else "disabled"
            self.status.set(f"DLSS-NR {state}")
        except PermissionError:
            self.status.set("Administrator access is required to write HKLM.")
        except OSError as error:
            self.status.set(f"Registry error: {error}")

    def reset_defaults(self):
        try:
            self.enabled.set(False)
            write_value(ENABLE_VALUE_NAME, 0)
            for variable, default in zip(self.variables, VALUE_DEFAULTS):
                variable.set(default)
            self.set_slider_state()
            self.status.set("Reset to defaults")
        except PermissionError:
            self.status.set("Administrator access is required to write HKLM.")
        except OSError as error:
            self.status.set(f"Registry error: {error}")

    def save(self, index, variable):
        if VALUE_NAMES[index] == "FoveationSize":
            if self.foveation_save_id is not None:
                self.root.after_cancel(self.foveation_save_id)
            self.foveation_save_id = self.root.after(
                400, self.save_foveation_size, variable
            )
            return

        try:
            write_value(VALUE_NAMES[index], variable.get())
            self.status.set(f"Saved {VALUE_NAMES[index]} = {variable.get()}")
        except PermissionError:
            self.status.set("Administrator access is required to write HKLM.")
        except OSError as error:
            self.status.set(f"Registry error: {error}")

    def save_foveation_size(self, variable):
        self.foveation_save_id = None
        try:
            write_value("FoveationSize", variable.get())
            self.status.set(f"Saved FoveationSize = {variable.get()}")
        except PermissionError:
            self.status.set("Administrator access is required to write HKLM.")
        except OSError as error:
            self.status.set(f"Registry error: {error}")


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
