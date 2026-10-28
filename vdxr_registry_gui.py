import ctypes
import os
import subprocess
import sys
import tkinter as tk
from tkinter import font as tkfont
from tkinter import filedialog, messagebox, ttk
import winreg

import vdxr_runtime_selection as runtime_selection
import vdxr_game_profiles as game_profiles


REGISTRY_PATH = r"SOFTWARE\Virtual Desktop, Inc.\OpenXR"
ENABLE_VALUE_NAME = "DLSSNR_Enabled"
PROCESSING_RESOLUTION_VALUE_NAME = "DLSSNR_ProcessingResolution"
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
FRAME_GENERATION_MODES = {"Off": 0, "OFXR 2×": 2, "OFXR 3×": 3, "OFXR 4×": 4}
FRAME_GENERATION_BACKEND_NAME = "FrameGenerationBackend"
FRAME_GENERATION_BACKENDS = {"NVIDIA (default)": 0, "FidelityFX (full resolution)": 1}

COLORS = {
    "background": "#101217", "panel": "#1A1D25", "border": "#2B303C",
    "control": "#252A36", "hover": "#30384A", "text": "#F3F5FA",
    "muted": "#A6AEBD", "disabled": "#7D889C", "accent": "#8AA6FF",
    "blue": "#698BFF", "selection": "#3B54A5", "amber": "#DDC18B",
}


def rounded_image(root, width, height, radius, color):
    """Small native Tk image used by the switch and button theme elements."""
    image = tk.PhotoImage(master=root, width=width, height=height)
    for y in range(height):
        distance = max(radius - y - 0.5, y - (height - radius) + 0.5, 0)
        inset = round(radius - max(0, radius * radius - distance * distance) ** 0.5)
        image.put(color, to=(inset, y, width - inset, y + 1))
    return image


def apply_dark_theme(root):
    if hasattr(root, "_controls_theme_images"):
        return
    root.configure(background=COLORS["background"])
    families = set(tkfont.families(root))
    body_font = "Segoe UI Variable Text" if "Segoe UI Variable Text" in families else "Segoe UI"
    heading_font = "Segoe UI Variable Display Semib" if "Segoe UI Variable Display Semib" in families else "Segoe UI Semibold"
    number_font = "Segoe UI Variable Text Semibold" if "Segoe UI Variable Text Semibold" in families else "Segoe UI Semibold"
    root.option_add("*TCombobox.font", (body_font, 11))
    for option, value in {
        "background": COLORS["control"], "foreground": COLORS["text"],
        "selectBackground": COLORS["selection"], "selectForeground": COLORS["text"],
        "font": (body_font, 11),
    }.items():
        root.option_add(f"*TCombobox*Listbox.{option}", value)
    style = ttk.Style(root)
    style.theme_use("clam")
    style.configure(".", font=(body_font, 11))
    style.configure("App.TFrame", background=COLORS["background"])
    style.configure("Card.TFrame", background=COLORS["panel"])
    style.configure("App.TLabel", background=COLORS["background"], foreground=COLORS["text"])
    style.configure("Title.TLabel", background=COLORS["background"], foreground=COLORS["text"], font=(heading_font, 24))
    style.configure("Subtitle.TLabel", background=COLORS["background"], foreground=COLORS["muted"])
    style.configure("Card.TLabel", background=COLORS["panel"], foreground=COLORS["text"])
    style.configure("Hint.TLabel", background=COLORS["panel"], foreground=COLORS["muted"])
    style.configure("Section.TLabel", background=COLORS["panel"], foreground=COLORS["text"], font=(heading_font, 15))
    style.configure("Live.TLabel", background=COLORS["panel"], foreground=COLORS["accent"])
    style.configure("Restart.TLabel", background=COLORS["panel"], foreground=COLORS["amber"])
    style.configure("Value.TLabel", background=COLORS["control"], foreground=COLORS["text"], padding=(8, 2), font=(number_font, 11))
    style.configure("Dark.TCombobox", fieldbackground=COLORS["control"], background=COLORS["control"],
                    foreground=COLORS["text"], arrowcolor=COLORS["muted"], bordercolor=COLORS["border"],
                    lightcolor=COLORS["control"], darkcolor=COLORS["control"], padding=(10, 8))
    style.map("Dark.TCombobox", fieldbackground=[("readonly", COLORS["control"])],
              foreground=[("disabled", COLORS["disabled"]), ("readonly", COLORS["text"])],
              selectbackground=[("readonly", COLORS["control"])], selectforeground=[("readonly", COLORS["text"])],
              bordercolor=[("focus", COLORS["accent"])], arrowcolor=[("active", COLORS["accent"])])

    switch_images = []
    for selected in (False, True):
        image = rounded_image(root, 42, 24, 12, COLORS["blue"] if selected else "#444D60")
        center = 30 if selected else 12
        for y in range(4, 20):
            half = round(max(0, 8 * 8 - (y - 11.5) ** 2) ** 0.5)
            image.put("#FFFFFF", to=(center - half, y, center + half, y + 1))
        switch_images.append(image)
    style.element_create("Controls.Switch", "image", switch_images[0], ("selected", switch_images[1]))
    style.layout("Switch.TCheckbutton", [("Checkbutton.padding", {"sticky": "nswe", "children": [
        ("Controls.Switch", {"side": "left", "sticky": ""}),
        ("Checkbutton.focus", {"side": "left", "children": [("Checkbutton.label", {"sticky": "nswe"})]}),
    ]})])
    style.configure("Switch.TCheckbutton", background=COLORS["panel"], foreground=COLORS["text"],
                    focuscolor=COLORS["accent"], padding=(0, 2))
    style.map("Switch.TCheckbutton", background=[("active", COLORS["panel"])])
    button_images = [rounded_image(root, 24, 24, 8, color)
                     for color in (COLORS["control"], COLORS["hover"], "#3C4965")]
    style.element_create("Controls.Button", "image", button_images[0],
                         ("pressed", button_images[2]), ("active", button_images[1]), border=8, sticky="nswe")
    style.layout("Soft.TButton", [("Controls.Button", {"sticky": "nswe", "children": [
        ("Button.padding", {"sticky": "nswe", "children": [
            ("Button.focus", {"sticky": "nswe", "children": [("Button.label", {"sticky": "nswe"})]}),
        ]}),
    ]})])
    style.configure("Soft.TButton", background=COLORS["background"], foreground=COLORS["text"],
                    padding=(14, 5), focuscolor=COLORS["accent"])
    root._controls_theme_images = switch_images + button_images


def dark_title_bar(root):
    """Use system window chrome; unsupported DWM versions remain harmless."""
    if sys.platform != "win32":
        return False
    try:
        from ctypes import wintypes
        ancestor = ctypes.WinDLL("user32").GetAncestor
        ancestor.argtypes = (wintypes.HWND, wintypes.UINT)
        ancestor.restype = wintypes.HWND
        attribute = ctypes.WinDLL("dwmapi").DwmSetWindowAttribute
        attribute.argtypes = (wintypes.HWND, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD)
        attribute.restype = ctypes.c_long
        handle = ancestor(root.winfo_id(), 2)
        enabled = wintypes.BOOL(1)
        if not handle:
            return False
        applied = attribute(handle, 20, ctypes.byref(enabled), ctypes.sizeof(enabled)) == 0
        # Windows 11 caption/text colors; older DWM versions simply reject these.
        for option, color in ((35, COLORS["background"]), (36, COLORS["text"])):
            red, green, blue = (int(color[index:index + 2], 16) for index in (1, 3, 5))
            value = wintypes.DWORD(red | green << 8 | blue << 16)
            attribute(handle, option, ctypes.byref(value), ctypes.sizeof(value))
        return applied
    except (OSError, AttributeError, tk.TclError):
        return False


class RoundedPanel(tk.Canvas):
    """Decorative card around ordinary, keyboard-accessible Tk controls."""
    def __init__(self, parent):
        super().__init__(parent, background=COLORS["background"], highlightthickness=0, borderwidth=0)
        self.content = ttk.Frame(self, style="Card.TFrame")
        self.window = self.create_window(20, 14, anchor="nw", window=self.content)
        self.content.bind("<Configure>", self.fit_content)
        self.bind("<Configure>", self.paint)

    def fit_content(self, _event=None):
        height = self.content.winfo_reqheight() + 28
        width = self.content.winfo_reqwidth() + 40
        if int(self.cget("height")) != height or int(self.cget("width")) != width:
            self.configure(height=height, width=width)

    def paint(self, event):
        width, height, radius = event.width - 1, event.height - 1, 18
        self.itemconfigure(self.window, width=max(1, width - 40))
        self.delete("card")
        self.create_polygon(radius, 1, width - radius, 1, width, 1, width, radius,
                            width, height - radius, width, height, width - radius, height,
                            radius, height, 1, height, 1, height - radius, 1, radius, 1, 1,
                            smooth=True, splinesteps=24, fill=COLORS["panel"],
                            outline=COLORS["border"], tags="card")
        self.tag_lower("card")


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
        raise ValueError("Invalid saved frame-generation setting; select Off, OFXR 2×, OFXR 3× or OFXR 4×.")
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


def read_processing_resolution(errors=None):
    errors = [] if errors is None else errors
    value = read_nr_value(PROCESSING_RESOLUTION_VALUE_NAME, 100, 100, errors)
    if value < 50:
        errors.append("Invalid saved NR processing resolution; showing native 100%.")
        return 100
    return value


def write_processing_resolution(value):
    if type(value) is not int or not 50 <= value <= 100:
        raise ValueError("NR processing resolution must be an integer from 50 to 100.")
    write_value(PROCESSING_RESOLUTION_VALUE_NAME, value)


def write_value(name, value):
    if name == PROCESSING_RESOLUTION_VALUE_NAME:
        maximum = 100
    else:
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
    def __init__(self, root, profile_store=None):
        self.root = root
        self.root.title("VDXR NR/FG Controls")
        self.root.resizable(False, False)
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)
        apply_dark_theme(root)
        self.updating = True
        self.profile_store = profile_store or game_profiles.ProfileStore()
        self.profile_executable = None
        self.profile_dirty = False
        self.profile_valid = True
        errors = []
        self.saved_enabled = read_enabled(errors)
        self.load_processing_resolution(errors)
        self.saved_values = read_values(errors)

        # A bounded viewport keeps the existing cards reachable on scaled displays.
        self.viewport = tk.Canvas(root, background=COLORS["background"], highlightthickness=0, borderwidth=0)
        self.viewport.grid(row=0, column=0, sticky="nsew")
        scrollbar = ttk.Scrollbar(root, orient="vertical", command=self.viewport.yview)
        scrollbar.grid(row=0, column=1, sticky="ns")
        self.viewport.configure(yscrollcommand=scrollbar.set)
        frame = ttk.Frame(self.viewport, padding=(20, 16), style="App.TFrame")
        self.viewport.create_window(0, 0, anchor="nw", window=frame)
        frame.bind("<Configure>", lambda _event: self.viewport.configure(scrollregion=self.viewport.bbox("all")))
        root.bind("<MouseWheel>", lambda event: self.viewport.yview_scroll(-int(event.delta / 120), "units"), add="+")
        frame.columnconfigure(0, minsize=592)

        ttk.Label(frame, text="VDXR Controls", style="Title.TLabel").grid(
            row=0, column=0, sticky="w"
        )
        ttk.Label(frame, text="Neural rendering  /  Frame generation", style="Subtitle.TLabel").grid(
            row=1, column=0, sticky="w", pady=(2, 12)
        )

        runtime_panel = RoundedPanel(frame)
        runtime_panel.grid(row=2, column=0, sticky="ew", pady=(0, 14))
        runtime = runtime_panel.content
        runtime.columnconfigure(0, weight=1)
        ttk.Label(runtime, text="Runtime", style="Section.TLabel").grid(row=0, column=0, sticky="w")
        try:
            self.saved_runtime_selection = runtime_selection.read_selection()
            if self.saved_runtime_selection == runtime_selection.OTHER_LABEL:
                errors.append("Other/unknown runtime override selected for the next game launch.")
        except (ValueError, OSError) as error:
            self.saved_runtime_selection = runtime_selection.OTHER_LABEL
            errors.append(f"Runtime selection error: {error}")
        self.runtime_selection = tk.StringVar(value=self.saved_runtime_selection)
        runtime_selector = ttk.Combobox(
            runtime, textvariable=self.runtime_selection, values=runtime_selection.CHOICES,
            state="readonly", width=25, style="Dark.TCombobox",
        )
        runtime_selector.grid(row=0, column=1, sticky="e", padx=(16, 0))
        runtime_selector.bind("<<ComboboxSelected>>", self.on_runtime_changed)
        ttk.Label(runtime, text="For next game launch. Keep VDXR selected in Streamer.", style="Hint.TLabel").grid(
            row=1, column=0, columnspan=2, sticky="w", pady=(10, 0)
        )

        scope_panel = RoundedPanel(frame)
        scope_panel.grid(row=3, column=0, sticky="ew", pady=(0, 14))
        scope = scope_panel.content
        scope.columnconfigure(0, weight=1)
        ttk.Label(scope, text="Settings scope", style="Section.TLabel").grid(row=0, column=0, sticky="w")
        self.scope_selection = tk.StringVar(value="Globals")
        self.scope_selector = ttk.Combobox(scope, textvariable=self.scope_selection, values=("Globals",),
                                            state="readonly", width=48, style="Dark.TCombobox")
        self.scope_selector.grid(row=1, column=0, sticky="ew", pady=(8, 0))
        self.scope_selector.bind("<<ComboboxSelected>>", self.on_scope_changed)
        ttk.Button(scope, text="Add game EXE…", command=self.add_profile, style="Soft.TButton").grid(
            row=1, column=1, padx=(12, 0), pady=(8, 0))
        self.scope_feedback = tk.StringVar(value="Globals · changes save immediately. Game profiles apply automatically at launch.")
        ttk.Label(scope, textvariable=self.scope_feedback, style="Hint.TLabel", wraplength=592).grid(
            row=2, column=0, columnspan=2, sticky="w", pady=(8, 0))

        nr_panel = RoundedPanel(frame)
        nr_panel.grid(row=4, column=0, sticky="ew")
        nr = nr_panel.content
        nr.columnconfigure(1, weight=1)
        nr_header = ttk.Frame(nr, style="Card.TFrame")
        nr_header.grid(row=0, column=0, columnspan=3, sticky="ew", pady=(0, 4))
        nr_header.columnconfigure(0, weight=1)
        ttk.Label(nr_header, text="Neural rendering", style="Section.TLabel").grid(row=0, column=0, sticky="w")

        self.enabled = tk.BooleanVar(value=self.saved_enabled)
        ttk.Checkbutton(
            nr_header,
            text="  Enable NR",
            style="Switch.TCheckbutton",
            variable=self.enabled,
            command=self.on_enabled_changed,
        ).grid(row=0, column=1, sticky="e")
        ttk.Label(nr, text="Saved NR changes apply during play", style="Live.TLabel").grid(
            row=1, column=0, columnspan=3, sticky="w", pady=(0, 8)
        )

        self.variables = []
        self.slider_widgets = []
        self.foveation_save_id = None
        for index, value in enumerate(self.saved_values, start=0):
            variable = tk.IntVar(value=value)
            variable.trace_add("write", lambda *_args, i=index, v=variable: self.save(i, v))
            self.variables.append(variable)

            ttk.Label(nr, text=VALUE_LABELS[index], style="Card.TLabel").grid(
                row=index + 2, column=0, sticky="w", padx=(0, 16)
            )
            slider = tk.Scale(
                nr,
                from_=0,
                to=VALUE_MAXIMUMS[index],
                resolution=1,
                variable=variable,
                orient="horizontal",
                length=260,
                showvalue=False,
                background="#B2C4FF",
                foreground=COLORS["text"],
                troughcolor="#56627A",
                activebackground="#E7EDFF",
                sliderrelief="flat",
                sliderlength=18,
                width=8,
                borderwidth=0,
                highlightthickness=1,
                highlightbackground=COLORS["panel"],
                highlightcolor=COLORS["accent"],
                takefocus=True,
            )
            slider.grid(row=index + 2, column=1, sticky="ew")
            self.slider_widgets.append(slider)
            ttk.Label(nr, textvariable=variable, style="Value.TLabel", width=3, anchor="e").grid(
                row=index + 2, column=2, sticky="e", padx=(14, 0)
            )

        self.processing_resolution_save_id = None
        self.processing_resolution = tk.IntVar(value=self.saved_processing_resolution)
        self.processing_resolution.trace_add("write", self.on_processing_resolution_changed)
        ttk.Label(nr, text="NR processing resolution (%)", style="Card.TLabel").grid(
            row=8, column=0, sticky="w", padx=(0, 16)
        )
        self.processing_resolution_slider = tk.Scale(
            nr, from_=50, to=100, resolution=1, variable=self.processing_resolution,
            orient="horizontal", length=260, showvalue=False,
            background="#B2C4FF", foreground=COLORS["text"], troughcolor="#56627A",
            activebackground="#E7EDFF", sliderrelief="flat", sliderlength=18,
            width=8, borderwidth=0, highlightthickness=1,
            highlightbackground=COLORS["panel"], highlightcolor=COLORS["accent"], takefocus=True,
        )
        self.processing_resolution_slider.grid(row=8, column=1, sticky="ew")
        ttk.Label(nr, textvariable=self.processing_resolution, style="Value.TLabel", width=3, anchor="e").grid(
            row=8, column=2, sticky="e", padx=(14, 0)
        )
        ttk.Label(nr, text="Both percentages scale linear dimensions. Native processing 100%; try 80%.\n"
                  "Combined coverage + processing requires an updated custom runtime.",
                  style="Hint.TLabel", wraplength=592).grid(
            row=9, column=0, columnspan=3, sticky="w", pady=(10, 0)
        )
        ttk.Label(nr, text="Reduced processing with partial coverage adds an automatic inside transition;\n"
                  "the fully enhanced center is smaller.",
                  style="Hint.TLabel", wraplength=592).grid(
            row=10, column=0, columnspan=3, sticky="w", pady=(6, 0)
        )

        fg_panel = RoundedPanel(frame)
        fg_panel.grid(row=5, column=0, sticky="ew", pady=(14, 0))
        fg = fg_panel.content
        fg.columnconfigure(0, weight=1)
        fg.columnconfigure(1, weight=1)
        ttk.Label(fg, text="Frame generation", style="Section.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Label(fg, text="Restart game to apply", style="Restart.TLabel").grid(row=0, column=1, sticky="e")
        ttk.Label(fg, text="Mode", style="Card.TLabel").grid(row=1, column=0, sticky="w", pady=(14, 6))
        ttk.Label(fg, text="Optical-flow backend", style="Card.TLabel").grid(row=1, column=1, sticky="w", pady=(14, 6))
        try:
            mode = read_frame_generation_mode()
        except (ValueError, OSError) as error:
            mode = 0
            errors.append(str(error))
        self.saved_frame_generation = next(label for label, value in FRAME_GENERATION_MODES.items() if value == mode)
        self.frame_generation = tk.StringVar(value=self.saved_frame_generation)
        selector = ttk.Combobox(
            fg, textvariable=self.frame_generation, values=tuple(FRAME_GENERATION_MODES),
            state="readonly", width=18, style="Dark.TCombobox",
        )
        selector.grid(row=2, column=0, sticky="ew", padx=(0, 14))
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
        backend_selector = ttk.Combobox(
            fg, textvariable=self.frame_generation_backend, values=tuple(FRAME_GENERATION_BACKENDS),
            state="readonly", width=27, style="Dark.TCombobox",
        )
        backend_selector.grid(row=2, column=1, sticky="ew")
        backend_selector.bind("<<ComboboxSelected>>", self.on_frame_generation_backend_changed)
        ttk.Label(fg, text="NR + MFG requires coverage at least 90%. Turn SSW off in Virtual Desktop.",
                  style="Hint.TLabel", wraplength=592).grid(
            row=3, column=0, columnspan=2, sticky="w", pady=(12, 4)
        )
        ttk.Label(fg, text="Selections are saved requests, not active runtime status.", style="Hint.TLabel").grid(
            row=4, column=0, columnspan=2, sticky="w"
        )
        self.reset_button = ttk.Button(
            frame, text="Reset NR settings", command=self.reset_defaults, style="Soft.TButton",
        )
        self.reset_button.grid(row=6, column=0, sticky="w", pady=(14, 0))
        self.save_profile_button = ttk.Button(frame, text="Save game profile", command=self.save_profile,
                                               style="Soft.TButton", state="disabled")
        self.save_profile_button.grid(row=6, column=0, sticky="e", pady=(14, 0))
        self.status = tk.StringVar(value="; ".join(errors) or "Ready · changes save automatically")
        ttk.Label(frame, textvariable=self.status, style="Subtitle.TLabel", wraplength=592).grid(
            row=7, column=0, sticky="w", pady=(8, 0)
        )
        root.bind("<Map>", lambda event: root.after_idle(dark_title_bar, root) if event.widget is root else None, add="+")
        self.set_slider_state()
        self.updating = False
        self.refresh_profile_choices()
        root.update_idletasks()
        self.viewport.configure(width=frame.winfo_reqwidth(),
                                height=min(frame.winfo_reqheight(), max(200, root.winfo_screenheight() - 120)))

    def refresh_profile_choices(self):
        errors = []
        try:
            profiles = self.profile_store.list_profiles(errors)
        except (OSError, ValueError) as error:
            self.status.set(f"Profile list error: {error}")
            profiles = []
        if self.profile_executable and self.profile_executable not in profiles:
            profiles.append(self.profile_executable)
        self.scope_selector.configure(values=("Globals", *profiles))
        if errors:
            self.status.set(f"{self.status.get()}; {len(errors)} game profile(s) skipped: invalid or unreadable identity metadata.")

    def confirm_profile_switch(self):
        if not self.profile_dirty:
            return True
        choice = messagebox.askyesnocancel("Unsaved game profile", "Save this game's changes before switching?",
                                          parent=self.root)
        if choice is None:
            return False
        return self.save_profile() if choice else True

    def on_scope_changed(self, _event=None):
        selection = self.scope_selection.get()
        if self.confirm_profile_switch():
            self.load_scope(None if selection == "Globals" else selection)
        else:
            self.scope_selection.set(self.profile_executable or "Globals")

    def add_profile(self):
        if not self.confirm_profile_switch():
            return
        executable = filedialog.askopenfilename(parent=self.root, title="Select the game executable",
                                                filetypes=(("Windows executable", "*.exe"),))
        if executable:
            try:
                self.load_scope(game_profiles.normalize_executable(executable))
                self.refresh_profile_choices()
            except (OSError, ValueError) as error:
                self.status.set(f"Profile identity error: {error}")

    def load_scope(self, executable):
        # Leaving Globals must keep its existing debounced autosave contract.
        if self.profile_executable is None and executable is not None:
            if self.processing_resolution_save_id is not None and not self.save_processing_resolution():
                self.scope_selection.set("Globals")
                return False
            if self.foveation_save_id is not None and not self.save_foveation_size(self.variables[-1]):
                self.scope_selection.set("Globals")
                return False
        self.cancel_foveation_save()
        self.cancel_processing_resolution_save()
        self.profile_executable = executable
        self.scope_selection.set(executable or "Globals")
        self.reset_button.configure(text="Reset game profile" if executable else "Reset NR settings")
        self.profile_dirty = False
        self.profile_valid = True
        errors = []
        self.updating = True
        try:
            if executable is None:
                # Preserve existing global autosave semantics and displayed defaults.
                self.saved_enabled = read_enabled(errors)
                self.saved_values = read_values(errors)
                self.load_processing_resolution(errors)
                try:
                    mode, backend = read_frame_generation_mode(), read_frame_generation_backend()
                except (OSError, ValueError) as error:
                    mode, backend = 0, 0
                    errors.append(str(error))
                self.scope_feedback.set("Globals · changes save immediately. Game profiles apply automatically at launch.")
            else:
                try:
                    values, sources = self.profile_store.load(executable)
                except (OSError, ValueError) as error:
                    values, sources = game_profiles.DEFAULTS.copy(), {}
                    self.profile_valid = False
                    errors.append(f"Profile rejected: {error} Reset profile to inherit globals before saving.")
                self.saved_enabled = bool(values[ENABLE_VALUE_NAME])
                self.saved_values = [values[name] for name in VALUE_NAMES]
                self.saved_processing_resolution = values[PROCESSING_RESOLUTION_VALUE_NAME]
                self.processing_resolution_verified = self.profile_valid
                mode, backend = values[FRAME_GENERATION_VALUE_NAME], values[FRAME_GENERATION_BACKEND_NAME]
                overrides = sum(source == "profile" for source in sources.values())
                self.scope_feedback.set(
                    f"{executable}\n{overrides} game overrides · {len(sources) - overrides} inherited "
                    "from globals/defaults. New games inherit globals; Save locks current setup; Reset returns to globals."
                    if self.profile_valid else
                    f"{executable}\nProfile rejected · controls show safe display defaults. Reset overrides to recover.")
            self.enabled.set(self.saved_enabled)
            for variable, value in zip(self.variables, self.saved_values):
                variable.set(value)
            self.processing_resolution.set(self.saved_processing_resolution)
            self.saved_frame_generation = next(label for label, value in FRAME_GENERATION_MODES.items() if value == mode)
            self.saved_frame_generation_backend = next(label for label, value in FRAME_GENERATION_BACKENDS.items() if value == backend)
            self.frame_generation.set(self.saved_frame_generation)
            self.frame_generation_backend.set(self.saved_frame_generation_backend)
            self.save_profile_button.configure(state="normal" if executable is not None and self.profile_valid else "disabled")
            self.set_slider_state()
            self.status.set("; ".join(errors) or ("Showing effective game settings · missing values inherit globals/defaults."
                                                   if executable else "Globals · changes save automatically"))
        finally:
            self.updating = False
        return True

    def stage_profile_change(self):
        if self.profile_executable is None:
            return False
        if not self.profile_valid:
            self.status.set("Profile rejected · Save is blocked. Reset malformed overrides before editing.")
            return True
        if not self.updating:
            self.profile_dirty = True
            self.set_slider_state()
            self.status.set("Unsaved game profile · Save applies NR during play; FG changes require a game restart.")
        return True

    def save_profile(self):
        if self.profile_executable is None or not self.profile_valid:
            return False
        values = {name: variable.get() for name, variable in zip(VALUE_NAMES, self.variables)}
        values.update({ENABLE_VALUE_NAME: int(self.enabled.get()),
                       PROCESSING_RESOLUTION_VALUE_NAME: self.processing_resolution.get(),
                       FRAME_GENERATION_VALUE_NAME: FRAME_GENERATION_MODES[self.frame_generation.get()],
                       FRAME_GENERATION_BACKEND_NAME: FRAME_GENERATION_BACKENDS[self.frame_generation_backend.get()]})
        try:
            # Save locks the complete displayed setup, including inherited values.
            self.profile_store.save(self.profile_executable, values)
            self.load_scope(self.profile_executable)
            self.refresh_profile_choices()
            self.status.set("Game setup saved and locked · NR applies during play; restart game for FG changes.")
            return True
        except (OSError, ValueError) as error:
            self.status.set(f"Game profile save error: {error}. Some values may have been written; reload the scope to verify.")
            return False

    def on_runtime_changed(self, _event=None):
        selection = self.runtime_selection.get()
        try:
            runtime_selection.write_selection(selection)
        except (OSError, ValueError) as error:
            try:
                self.saved_runtime_selection = runtime_selection.read_selection()
            except (OSError, ValueError):
                self.saved_runtime_selection = runtime_selection.OTHER_LABEL
            self.runtime_selection.set(self.saved_runtime_selection)
            if isinstance(error, PermissionError):
                self.status.set("Runtime selection error: administrator access is required; selection not verified.")
            else:
                self.status.set(f"Runtime selection error: {error}")
        else:
            self.saved_runtime_selection = selection
            self.status.set(f"Saved {selection} for the next game launch. Keep VDXR selected in Streamer.")

    def on_frame_generation_backend_changed(self, _event=None):
        if self.stage_profile_change():
            return
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
        if self.stage_profile_change():
            return
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

    def load_processing_resolution(self, errors):
        read_errors = []
        self.saved_processing_resolution = read_processing_resolution(read_errors)
        # Keep read warnings honest without gating independent coverage edits.
        self.processing_resolution_verified = not read_errors
        errors.extend(read_errors)

    def set_slider_state(self):
        state = tk.NORMAL if self.enabled.get() else tk.DISABLED
        for slider in self.slider_widgets:
            slider.configure(state=state)
        self.processing_resolution_slider.configure(state=state)

    def on_enabled_changed(self):
        if self.stage_profile_change():
            return
        try:
            if self.enabled.get():
                # Persist the displayed coverage before explicitly enabling NR.
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
                if self.processing_resolution_save_id is not None:
                    self.cancel_processing_resolution_save()
                    if not self.save_processing_resolution():
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
        if self.profile_executable is not None:
            try:
                self.profile_store.reset(self.profile_executable)
                self.load_scope(self.profile_executable)
                self.refresh_profile_choices()
                self.status.set("Game overrides reset · inheriting globals/defaults; restart game for FG changes.")
            except (OSError, ValueError) as error:
                self.status.set(f"Profile reset error: {error}")
            return
        self.cancel_foveation_save()
        self.cancel_processing_resolution_save()
        self.updating = True
        try:
            write_value(ENABLE_VALUE_NAME, 0)
            for name, default in zip(VALUE_NAMES, VALUE_DEFAULTS):
                write_value(name, default)
            write_processing_resolution(100)
            self.status.set("NR reset: disabled, coverage and processing resolution 100%. Runtime and FG selections retained.")
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
            self.refresh_processing_resolution(errors)
            self.set_slider_state()
            self.updating = False
            if errors:
                self.status.set("; ".join(errors))

    def save(self, index, variable):
        if self.updating:
            return
        if self.stage_profile_change():
            return
        if VALUE_NAMES[index] == "FoveationSize":
            self.cancel_foveation_save()
            self.foveation_save_id = self.root.after(
                400, self.save_foveation_size, variable
            )
            return

        return self.save_nr_value(index, variable)

    def save_nr_value(self, index, variable):
        if VALUE_NAMES[index] == "FoveationSize":
            return self.save_foveation_size(variable)
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
        self.cancel_foveation_save()
        value = variable.get()
        try:
            write_value("FoveationSize", value)
        except OSError as error:
            self.spatial_save_failed("NR coverage", error)
            return False
        self.saved_values[-1] = value
        self.status.set(f"Saved NR coverage = {value}% · applies during play")
        if not self.processing_resolution_verified:
            self.status.set(f"Saved NR coverage = {value}%; NR processing resolution is unverified; showing its default.")
        return True

    def cancel_foveation_save(self):
        if self.foveation_save_id is not None:
            self.root.after_cancel(self.foveation_save_id)
            self.foveation_save_id = None

    def on_processing_resolution_changed(self, *_args):
        if self.updating:
            return
        if self.stage_profile_change():
            return
        self.cancel_processing_resolution_save()
        self.set_slider_state()
        self.processing_resolution_save_id = self.root.after(400, self.save_processing_resolution)

    def save_processing_resolution(self):
        self.cancel_processing_resolution_save()
        try:
            value = self.processing_resolution.get()
            write_processing_resolution(value)
        except (OSError, ValueError) as error:
            self.spatial_save_failed("NR processing resolution", error)
        else:
            self.saved_processing_resolution = value
            self.processing_resolution_verified = True
            self.set_slider_state()
            self.status.set(f"Saved NR processing resolution = {value}% · supported custom runtime required")
            return True
        return False

    def spatial_save_failed(self, setting, error):
        """Reconcile both settings after a possibly partial, unverified write."""
        errors = [f"{setting} error: " + (
            "administrator access is required." if isinstance(error, PermissionError) else str(error)
        )]
        self.cancel_foveation_save()
        was_updating = self.updating
        self.updating = True
        try:
            self.saved_values[-1] = read_nr_value("FoveationSize", 100, 100, errors)
            self.variables[-1].set(self.saved_values[-1])
            self.refresh_processing_resolution(errors)
        finally:
            self.updating = was_updating
        self.set_slider_state()
        self.status.set("; ".join(errors))

    def cancel_processing_resolution_save(self):
        if self.processing_resolution_save_id is not None:
            self.root.after_cancel(self.processing_resolution_save_id)
            self.processing_resolution_save_id = None

    def refresh_processing_resolution(self, errors=None):
        self.cancel_processing_resolution_save()
        read_errors = []
        self.load_processing_resolution(read_errors)
        if errors is not None:
            errors.extend(read_errors)
        elif read_errors:
            self.status.set("; ".join(read_errors))
        was_updating = self.updating
        self.updating = True
        try:
            self.processing_resolution.set(self.saved_processing_resolution)
        finally:
            self.updating = was_updating
        self.set_slider_state()

    def on_close(self):
        if self.profile_executable is not None:
            if self.confirm_profile_switch():
                self.root.destroy()
            return
        pending_processing = self.processing_resolution_save_id is not None
        pending_coverage = self.foveation_save_id is not None
        self.cancel_processing_resolution_save()
        self.cancel_foveation_save()
        error_status = None
        if pending_processing:
            if not self.save_processing_resolution():
                return
        if pending_coverage:
            if not self.save_foveation_size(self.variables[-1]):
                error_status = self.status.get()
        if error_status is not None:
            self.status.set(error_status)
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
