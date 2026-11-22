from collections import Counter
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
import vdxr_live_status as live_status
from vdxr_game_art import cover_photo
from vdxr_game_catalog import GameEntry, catalog_entries


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
FRAME_GENERATION_DEPTH_MODE_NAME = "FrameGenerationDepthMode"
FRAME_GENERATION_DEPTH_MODES = {"Standard": 0, "Depth assist": 1, "Depth reconstruction (experimental)": 2}
NVIDIA_FLOW_PRESETS = {"Fast": 1, "Medium": 0, "Slow": 2}

IMAGE_SETTINGS = {
    "sharpen": ("CAS sharpening", 0, 0),
    "upscaling": ("FSR render scale", 50, 100),
}

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


def fit_title(title, font, width):
    text = title[:80]
    if text == title and font.measure(text) <= width:
        return text
    while text and font.measure(text.rstrip() + "…") > width:
        text = text[:-1]
    return text.rstrip() + "…"


def edition_labels(entries):
    """Disambiguate duplicate titles without displaying their executable paths."""
    groups = {}
    for entry in entries:
        groups.setdefault(entry.title.casefold(), []).append(entry)
    labels = {}
    for group in groups.values():
        if len(group) < 2:
            continue
        counts = Counter(entry.store for entry in group)
        positions = Counter()
        for entry in sorted(group, key=lambda game: game.executable):
            label = entry.store or "Copy"
            positions[entry.store] += 1
            if counts[entry.store] > 1:
                label = f"{label} {positions[entry.store]}"
            labels[entry.executable] = label
    return labels


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

    style.configure("Dark.TSpinbox", fieldbackground=COLORS["control"], foreground=COLORS["text"],
                    insertcolor=COLORS["text"], arrowcolor=COLORS["text"], arrowsize=14, padding=(6, 4))
    style.map("Dark.TSpinbox", foreground=[("disabled", COLORS["disabled"])],
              fieldbackground=[("disabled", COLORS["panel"])])

    style.configure("Search.TEntry", fieldbackground=COLORS["control"], foreground=COLORS["text"],
                    insertcolor=COLORS["text"], bordercolor=COLORS["border"], padding=(8, 5))

    switch_images = []
    for selected, disabled in ((False, False), (True, False), (False, True), (True, True)):
        image = rounded_image(root, 42, 24, 12, "#444D60" if disabled else (COLORS["blue"] if selected else "#444D60"))
        center = 30 if selected else 12
        for y in range(4, 20):
            half = round(max(0, 8 * 8 - (y - 11.5) ** 2) ** 0.5)
            image.put(COLORS["disabled"] if disabled else "#FFFFFF", to=(center - half, y, center + half, y + 1))
        switch_images.append(image)
    style.element_create("Controls.Switch", "image", switch_images[0],
                         ("disabled", "selected", switch_images[3]), ("disabled", switch_images[2]),
                         ("selected", switch_images[1]))
    style.layout("Switch.TCheckbutton", [("Checkbutton.padding", {"sticky": "nswe", "children": [
        ("Controls.Switch", {"side": "left", "sticky": ""}),
        ("Checkbutton.focus", {"side": "left", "children": [("Checkbutton.label", {"sticky": "nswe"})]}),
    ]})])
    style.configure("Switch.TCheckbutton", background=COLORS["panel"], foreground=COLORS["text"],
                    focuscolor=COLORS["accent"], padding=(0, 2))
    style.map("Switch.TCheckbutton", background=[("active", COLORS["panel"])],
              foreground=[("disabled", COLORS["disabled"])])
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


def read_frame_generation_depth_mode():
    try:
        with winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE, REGISTRY_PATH, 0,
            winreg.KEY_READ | winreg.KEY_WOW64_64KEY,
        ) as key:
            value, kind = winreg.QueryValueEx(key, FRAME_GENERATION_DEPTH_MODE_NAME)
    except FileNotFoundError:
        return game_profiles.DEFAULTS[FRAME_GENERATION_DEPTH_MODE_NAME]
    return game_profiles.validate(FRAME_GENERATION_DEPTH_MODE_NAME, value, kind)


def write_frame_generation_depth_mode(value):
    game_profiles.validate(FRAME_GENERATION_DEPTH_MODE_NAME, value)
    with winreg.CreateKeyEx(
        winreg.HKEY_LOCAL_MACHINE, REGISTRY_PATH, 0,
        winreg.KEY_SET_VALUE | winreg.KEY_WOW64_64KEY,
    ) as key:
        winreg.SetValueEx(key, FRAME_GENERATION_DEPTH_MODE_NAME, 0, winreg.REG_DWORD, value)
    if read_frame_generation_depth_mode() != value:
        raise OSError("Frame-generation depth request readback did not match the selection")


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


def read_image_setting(name):
    label, minimum, default = IMAGE_SETTINGS[name]
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, REGISTRY_PATH, 0,
                            winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
            value, kind = winreg.QueryValueEx(key, name)
    except FileNotFoundError:
        return default
    if kind != winreg.REG_DWORD or type(value) is not int or not minimum <= value <= 100:
        raise ValueError(f"Invalid saved {label}; enter a value from {minimum} to 100 to correct it.")
    return value


def write_value(name, value):
    if name in IMAGE_SETTINGS:
        label, minimum, _ = IMAGE_SETTINGS[name]
        if type(value) is not int or not minimum <= value <= 100:
            raise ValueError(f"{label} must be a whole number from {minimum} to 100.")
        maximum = 100
    elif name == PROCESSING_RESOLUTION_VALUE_NAME:
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
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)
        apply_dark_theme(root)
        self.updating = True
        self.profile_store = profile_store or game_profiles.ProfileStore()
        self.game_entries = {}
        self.cover_images = {}
        self.game_picker = None
        self.profile_executable = None
        self.profile_dirty = False
        self.profile_valid = True
        errors = []
        self.saved_enabled = read_enabled(errors)
        self.load_processing_resolution(errors)
        self.saved_values = read_values(errors)

        root.columnconfigure(0, weight=1)
        root.rowconfigure(0, weight=1)
        self.viewport = tk.Canvas(root, background=COLORS["background"], highlightthickness=0, borderwidth=0)
        self.viewport.grid(row=0, column=0, sticky="nsew")
        self.editor_scrollbar = ttk.Scrollbar(root, orient="vertical", command=self.viewport.yview)
        self.viewport.configure(yscrollcommand=self.editor_scrollbar.set)
        frame = ttk.Frame(self.viewport, padding=(20, 16), style="App.TFrame")
        self.editor_window = self.viewport.create_window(0, 0, anchor="nw", window=frame)
        self.editor_frame = frame
        for column in (0, 1):
            frame.columnconfigure(column, weight=1, minsize=510)

        ttk.Label(frame, text="VDXR Controls", style="Title.TLabel").grid(row=0, column=0, columnspan=2, sticky="w")
        ttk.Label(frame, text="Neural rendering  /  Frame generation", style="Subtitle.TLabel").grid(
            row=1, column=0, columnspan=2, sticky="w", pady=(2, 12))
        left = ttk.Frame(frame, style="App.TFrame")
        right = ttk.Frame(frame, style="App.TFrame")
        left.grid(row=2, column=0, sticky="new", padx=(0, 7))
        right.grid(row=2, column=1, sticky="new", padx=(7, 0))
        left.columnconfigure(0, weight=1)
        right.columnconfigure(0, weight=1)

        self.live_observations = []
        self.live_selected_identity = None
        self.live_explicit_selection = False
        self.live_poll_id = None
        self.live_worker = None
        live_panel = RoundedPanel(right)
        live_panel.grid(row=1, column=0, sticky="ew", pady=(12, 0))
        live_card = live_panel.content
        live_card.columnconfigure(0, weight=1)
        ttk.Label(live_card, text="Live runtime", style="Section.TLabel").grid(row=0, column=0, sticky="w")
        self.live_selection = tk.StringVar()
        self.live_selector = ttk.Combobox(live_card, textvariable=self.live_selection, state="disabled",
                                         width=24, style="Dark.TCombobox")
        self.live_selector.grid(row=1, column=0, sticky="ew", pady=(6, 4))
        self.live_selector.bind("<<ComboboxSelected>>", self.on_live_session_changed)
        self.live_text = tk.StringVar(value="Discovering custom live sessions…")
        ttk.Label(live_card, textvariable=self.live_text, style="Hint.TLabel", wraplength=460).grid(
            row=2, column=0, sticky="w")
        root.bind("<Destroy>", self.on_live_destroy, add="+")

        runtime_panel = RoundedPanel(left)
        runtime_panel.grid(row=1, column=0, sticky="ew", pady=(12, 0))
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
        self.runtime_features_note = tk.StringVar()
        ttk.Label(runtime, textvariable=self.runtime_features_note, style="Hint.TLabel", wraplength=460).grid(
            row=2, column=0, columnspan=2, sticky="w", pady=(6, 0)
        )

        scope_panel = RoundedPanel(left)
        scope_panel.grid(row=0, column=0, sticky="ew")
        scope = scope_panel.content
        scope.columnconfigure(0, weight=1)
        ttk.Label(scope, text="Settings scope", style="Section.TLabel").grid(row=0, column=0, sticky="w")
        self.scope_selection = tk.StringVar(value="Globals")
        self.scope_title = tk.StringVar(value="Globals · Choose game…")
        self.scope_selector = ttk.Button(scope, textvariable=self.scope_title, compound="left",
                                         command=self.open_game_picker, style="Soft.TButton")
        self.scope_selector.grid(row=1, column=0, sticky="ew", pady=(8, 0))
        self.add_profile_button = ttk.Button(scope, text="Add game EXE…", command=self.add_profile, style="Soft.TButton")
        self.add_profile_button.grid(row=0, column=1, rowspan=2, padx=(10, 0), sticky="e")
        self.scope_feedback = tk.StringVar(value="Global NR and FG settings save automatically.")
        ttk.Label(scope, textvariable=self.scope_feedback, style="Hint.TLabel", wraplength=460).grid(
            row=2, column=0, columnspan=2, sticky="w", pady=(8, 0))

        nr_panel = RoundedPanel(right)
        nr_panel.grid(row=0, column=0, sticky="ew")
        nr = nr_panel.content
        nr.columnconfigure(1, weight=1)
        nr_header = ttk.Frame(nr, style="Card.TFrame")
        nr_header.grid(row=0, column=0, columnspan=3, sticky="ew", pady=(0, 4))
        nr_header.columnconfigure(0, weight=1)
        ttk.Label(nr_header, text="Neural rendering", style="Section.TLabel").grid(row=0, column=0, sticky="w")

        self.enabled = tk.BooleanVar(value=self.saved_enabled)
        self.nr_toggle = ttk.Checkbutton(
            nr_header,
            text="  Enable NR",
            style="Switch.TCheckbutton",
            variable=self.enabled,
            command=self.on_enabled_changed,
        )
        self.nr_toggle.grid(row=0, column=1, sticky="e")
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
                length=225,
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
            orient="horizontal", length=225, showvalue=False,
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
                  style="Hint.TLabel", wraplength=460).grid(
            row=9, column=0, columnspan=3, sticky="w", pady=(10, 0)
        )
        ttk.Label(nr, text="Reduced processing with partial coverage adds an automatic inside transition;\n"
                  "the fully enhanced center is smaller.",
                  style="Hint.TLabel", wraplength=460).grid(
            row=10, column=0, columnspan=3, sticky="w", pady=(6, 0)
        )

        fg_panel = RoundedPanel(left)
        fg_panel.grid(row=2, column=0, sticky="ew", pady=(12, 0))
        fg = fg_panel.content
        fg.columnconfigure(0, weight=1)
        fg.columnconfigure(1, weight=1)
        ttk.Label(fg, text="Frame generation", style="Section.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Label(fg, text="Applies next game launch", style="Restart.TLabel").grid(row=0, column=1, sticky="e")
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
        self.fg_selector = selector
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
        self.backend_selector = backend_selector
        ttk.Label(fg, text="Requested reconstruction mode", style="Card.TLabel").grid(
            row=3, column=0, sticky="w", pady=(14, 6)
        )
        ttk.Label(fg, text="NVIDIA flow preset", style="Card.TLabel").grid(
            row=3, column=1, sticky="w", pady=(14, 6)
        )
        try:
            depth_mode = read_frame_generation_depth_mode()
        except (ValueError, OSError) as error:
            depth_mode = game_profiles.DEFAULTS[FRAME_GENERATION_DEPTH_MODE_NAME]
            errors.append(f"{error} Depth assist shown; stored request unchanged.")
        self.saved_frame_generation_depth_mode = next(
            label for label, value in FRAME_GENERATION_DEPTH_MODES.items() if value == depth_mode
        )
        self.frame_generation_depth_mode = tk.StringVar(value=self.saved_frame_generation_depth_mode)
        self.depth_mode_selector = ttk.Combobox(
            fg, textvariable=self.frame_generation_depth_mode, values=tuple(FRAME_GENERATION_DEPTH_MODES),
            state="readonly", width=32, style="Dark.TCombobox",
        )
        self.depth_mode_selector.grid(row=4, column=0, sticky="ew", padx=(0, 14))
        self.depth_mode_selector.bind("<<ComboboxSelected>>", self.on_frame_generation_depth_mode_changed)
        self.nvidia_flow_preset = tk.StringVar(value="Medium")
        self.nvidia_flow_preset_selector = ttk.Combobox(
            fg, textvariable=self.nvidia_flow_preset, values=tuple(NVIDIA_FLOW_PRESETS),
            state="disabled", width=15, style="Dark.TCombobox",
        )
        self.nvidia_flow_preset_selector.grid(row=4, column=1, sticky="ew")
        self.nvidia_flow_preset_selector.bind("<<ComboboxSelected>>", self.on_nvidia_flow_preset_changed)
        self.depth_mode_feedback = tk.StringVar()
        self.update_depth_mode_feedback()
        ttk.Label(fg, textvariable=self.depth_mode_feedback, style="Hint.TLabel", wraplength=460).grid(
            row=5, column=0, columnspan=2, sticky="w", pady=(8, 0)
        )
        ttk.Label(fg, text="NR + MFG requires coverage at least 90%. Turn SSW off in Virtual Desktop.",
                  style="Hint.TLabel", wraplength=460).grid(
            row=6, column=0, columnspan=2, sticky="w", pady=(12, 4)
        )
        ttk.Label(fg, text="Selections are saved requests, not active runtime status.", style="Hint.TLabel").grid(
            row=7, column=0, columnspan=2, sticky="w"
        )
        self.build_image_controls(right, errors)
        self.reset_button = ttk.Button(
            frame, text="Reset NR settings", command=self.reset_defaults, style="Soft.TButton",
        )
        self.reset_button.grid(row=3, column=0, sticky="w", pady=(14, 0))
        self.save_profile_button = ttk.Button(frame, text="Save game profile", command=self.save_profile,
                                               style="Soft.TButton", state="disabled")
        self.save_profile_button.grid(row=3, column=1, sticky="e", pady=(14, 0))
        self.status = tk.StringVar(value="; ".join(errors) or "Ready · NR and FG settings save automatically")
        status_label = ttk.Label(frame, textvariable=self.status, style="Subtitle.TLabel", wraplength=1000)
        status_label.grid(row=4, column=0, columnspan=2, sticky="w", pady=(8, 0))
        root.bind("<Map>", lambda event: root.after_idle(dark_title_bar, root) if event.widget is root else None, add="+")
        self.update_runtime_controls()
        self.updating = False
        self.refresh_profile_choices()
        root.update_idletasks()
        available_width = max(280, root.winfo_screenwidth() - 30)
        scrollbar_width = self.editor_scrollbar.winfo_reqwidth()
        if frame.winfo_reqwidth() + scrollbar_width > available_width:
            # Keep the whole editor reachable on a narrow or heavily scaled display.
            frame.columnconfigure(0, minsize=0)
            frame.columnconfigure(1, weight=0, minsize=0)
            left.grid_configure(padx=0)
            right.grid_configure(row=3, column=0, padx=0, pady=(12, 0))
            self.reset_button.grid_configure(row=4)
            self.save_profile_button.grid_configure(row=4, column=0)
            status_label.configure(wraplength=min(460, available_width - 65))
            status_label.grid_configure(row=5, columnspan=1)
            root.update_idletasks()
        preferred_width = min(available_width - scrollbar_width, frame.winfo_reqwidth())
        self.editor_height_limit = max(180, root.winfo_screenheight() - 110)
        self.viewport.configure(width=preferred_width, height=min(frame.winfo_reqheight(), self.editor_height_limit))
        root.minsize(preferred_width + scrollbar_width, min(420, self.editor_height_limit))
        self.viewport.bind("<Configure>", self.resize_editor_viewport)
        frame.bind("<Configure>", self.resize_editor_viewport)
        root.bind("<MouseWheel>", self.scroll_editor, add="+")
        self.live_worker = live_status.StatusWorker(self.profile_store)
        self.live_poll_id = root.after(250, self.poll_live_status)

    def resize_editor_viewport(self, _event=None):
        content_height = self.editor_frame.winfo_reqheight()
        preferred_height = min(content_height, self.editor_height_limit)
        if int(self.viewport.cget("height")) != preferred_height:
            self.viewport.configure(height=preferred_height)
        width = self.viewport.winfo_width()
        if width <= 1:
            return
        self.viewport.itemconfigure(self.editor_window, width=width)
        self.viewport.configure(scrollregion=(0, 0, width, content_height))
        if content_height > self.viewport.winfo_height():
            if not self.editor_scrollbar.winfo_manager():
                self.editor_scrollbar.grid(row=0, column=1, sticky="ns")
        elif self.editor_scrollbar.winfo_manager():
            self.editor_scrollbar.grid_remove()
            self.viewport.yview_moveto(0)

    def scroll_editor(self, event):
        if self.editor_scrollbar.winfo_manager():
            self.viewport.yview_scroll(-int(event.delta / 120), "units")

    def poll_live_status(self):
        self.live_poll_id = None
        result = self.live_worker.latest()
        if result is not None:
            self.live_observations, note = result
            labels = tuple(live_status.session_label(item.snapshot) for item in self.live_observations)
            self.live_selector.configure(values=labels, state="readonly" if labels else "disabled")
            identities = {item.snapshot.identity for item in self.live_observations}
            if self.live_selected_identity not in identities:
                self.live_explicit_selection = False
                self.live_selected_identity = None
            if not self.live_explicit_selection:
                default = live_status.choose_session(self.live_observations)
                self.live_selected_identity = default.snapshot.identity if default else None
            self.render_live_status()
            if not labels and self.saved_runtime_selection != runtime_selection.STOCK_LABEL:
                self.live_text.set(note + " · live activity unavailable")
        self.live_poll_id = self.root.after(250, self.poll_live_status)

    def on_live_session_changed(self, _event=None):
        index = self.live_selector.current()
        if 0 <= index < len(self.live_observations):
            self.live_explicit_selection = True
            self.live_selected_identity = self.live_observations[index].snapshot.identity
            self.render_live_status()

    def render_live_status(self):
        selected = next((item for item in self.live_observations
                         if item.snapshot.identity == self.live_selected_identity), None)
        if selected is not None:
            self.live_selection.set(live_status.session_label(selected.snapshot))
        else:
            self.live_selection.set("No active custom live session")
        self.live_text.set(live_status.describe(selected,
            stock=self.saved_runtime_selection == runtime_selection.STOCK_LABEL))

    def stop_live_status(self):
        if self.live_worker is not None:
            self.live_worker.stop()
        if self.live_poll_id is not None:
            self.root.after_cancel(self.live_poll_id)
            self.live_poll_id = None

    def on_live_destroy(self, event):
        if event.widget is self.root:
            self.stop_live_status()

    def refresh_profile_choices(self):
        errors = []
        try:
            profiles = self.profile_store.list_profiles(errors)
        except (OSError, ValueError) as error:
            self.status.set(f"Profile list error: {error}")
            profiles = []
        if self.profile_executable and self.profile_executable not in profiles:
            profiles.append(self.profile_executable)
        self.game_entries = {entry.executable: entry for entry in catalog_entries(profiles)}
        self.update_scope_title()
        if errors:
            self.status.set(f"{self.status.get()}; {len(errors)} game profile(s) skipped: invalid or unreadable identity metadata.")

    def confirm_profile_switch(self, parent=None):
        if not self.profile_dirty:
            return True
        choice = messagebox.askyesnocancel("Unsaved game profile", "Save this game's changes before switching?",
                                          parent=parent or self.root)
        if choice is None:
            return False
        return self.save_profile() if choice else True

    def update_scope_title(self):
        if self.profile_executable is None:
            self.scope_title.set("Globals · Choose game…")
            self.scope_selector.configure(image="")
            return
        entry = self.game_entries.get(self.profile_executable)
        if entry is None:
            entry = catalog_entries([self.profile_executable])[0]
            self.game_entries[self.profile_executable] = entry
        font = tkfont.Font(root=self.root, font=ttk.Style(self.root).lookup("Soft.TButton", "font"))
        self.scope_title.set(fit_title(entry.title, font, 178))
        self.scope_selector.configure(image=self.game_cover(entry, (88, 44)))

    def game_cover(self, entry: GameEntry, size):
        key = (entry.cover, size)
        if key not in self.cover_images:
            self.cover_images[key] = (cover_photo(self.root, entry.cover, size, COLORS["background"])
                                      or rounded_image(self.root, *size, 6, COLORS["border"]))
        return self.cover_images[key]

    def open_game_picker(self):
        if self.game_picker is not None and self.game_picker.winfo_exists():
            self.game_picker.lift()
            return
        self.refresh_profile_choices()
        dialog = self.game_picker = tk.Toplevel(self.root)
        dialog.title("Choose game")
        dialog.transient(self.root)
        dialog.configure(background=COLORS["background"])
        dialog.resizable(False, False)
        dialog.bind("<Escape>", lambda _event: dialog.destroy())
        def map_picker(event):
            if event.widget is dialog:
                dark_title_bar(dialog)
                dialog.grab_set()
        dialog.bind("<Map>", map_picker)
        content = ttk.Frame(dialog, style="App.TFrame", padding=16)
        content.grid(sticky="nsew")
        content.columnconfigure(0, weight=1)
        ttk.Label(content, text="Find a game", style="App.TLabel").grid(row=0, column=0, sticky="w")
        search = tk.StringVar(master=dialog)
        search_entry = ttk.Entry(content, textvariable=search, width=34, style="Search.TEntry")
        search_entry.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(4, 12))
        self.game_tiles = ttk.Frame(content, style="App.TFrame")
        self.game_tiles.grid(row=2, column=0, columnspan=2, sticky="ew")
        for column in range(3):
            self.game_tiles.columnconfigure(column, minsize=192, weight=1)
        for row in range(2):
            self.game_tiles.rowconfigure(row, minsize=137)

        entries = sorted(self.game_entries.values(), key=lambda entry: (entry.title.casefold(), entry.executable))
        editions = edition_labels(entries)
        tile_font = tkfont.Font(root=dialog, font=ttk.Style(dialog).lookup(".", "font"))

        def choose(executable):
            if executable == self.profile_executable or (self.confirm_profile_switch(dialog) and self.load_scope(executable)):
                dialog.destroy()

        ttk.Button(content, text="Globals", style="Soft.TButton",
                   command=lambda: choose(None)).grid(row=3, column=0, sticky="w", pady=(10, 0))
        navigation = ttk.Frame(content, style="App.TFrame")
        navigation.grid(row=3, column=1, sticky="e", pady=(10, 0))
        page_label = tk.StringVar(master=dialog)
        page = 0

        def render():
            for child in self.game_tiles.winfo_children():
                child.destroy()
            query = search.get().strip().casefold()
            matches = [entry for entry in entries if query in entry.title.casefold()
                       or query in entry.executable.casefold()]
            for index, entry in enumerate(matches[page * 6:(page + 1) * 6]):
                title = fit_title(entry.title, tile_font, 174)
                if edition := editions.get(entry.executable):
                    title += "\n" + edition
                button = tk.Button(self.game_tiles, text=title, image=self.game_cover(entry, (174, 82)),
                                   compound="top", wraplength=174, relief="flat", borderwidth=0,
                                   background=COLORS["control"], foreground=COLORS["text"],
                                   activebackground=COLORS["hover"], activeforeground=COLORS["text"],
                                   highlightthickness=1, highlightcolor=COLORS["accent"],
                                   highlightbackground=COLORS["background"], padx=4, pady=4, font=tile_font,
                                   command=lambda path=entry.executable: choose(path))
                button.grid(row=index // 3, column=index % 3, sticky="nsew", padx=4, pady=4)
            pages = (len(matches) + 5) // 6
            if not matches:
                ttk.Label(self.game_tiles, text="No matching games" if entries else "No saved game profiles",
                          style="App.TLabel").grid(row=0, column=0, columnspan=3)
            page_label.set(f"{page + 1} / {pages}" if pages else "")
            previous.configure(state="normal" if page else "disabled")
            following.configure(state="normal" if page + 1 < pages else "disabled")

        def move(delta):
            nonlocal page
            page += delta
            render()

        def restart_search(*_args):
            nonlocal page
            page = 0
            render()

        previous = ttk.Button(navigation, text="Previous", style="Soft.TButton", command=lambda: move(-1))
        previous.grid(row=0, column=0)
        ttk.Label(navigation, textvariable=page_label, style="App.TLabel").grid(row=0, column=1, padx=10)
        following = ttk.Button(navigation, text="Next", style="Soft.TButton", command=lambda: move(1))
        following.grid(row=0, column=2)
        search_trace = search.trace_add("write", restart_search)

        def close_picker(event):
            if event.widget is dialog:
                search.trace_remove("write", search_trace)
                self.game_picker = None

        dialog.bind("<Destroy>", close_picker, add="+")
        render()
        search_entry.focus_set()

    def add_profile(self):
        if not self.confirm_profile_switch():
            return
        executable = filedialog.askopenfilename(parent=self.root, title="Select the game executable",
                                                filetypes=(("Windows executable", "*.exe"),))
        if executable:
            try:
                self.load_scope(game_profiles.normalize_executable(executable))
            except (OSError, ValueError) as error:
                self.status.set(f"Profile identity error: {error}")

    def build_image_controls(self, parent, errors):
        panel = RoundedPanel(parent)
        panel.grid(row=2, column=0, sticky="ew", pady=(12, 0))
        card = panel.content
        for column in (0, 2):
            card.columnconfigure(column, weight=1)
        ttk.Label(card, text="Image quality", style="Section.TLabel").grid(row=0, column=0, columnspan=2, sticky="w")
        ttk.Label(card, text="Global settings", style="Hint.TLabel").grid(row=0, column=2, columnspan=2, sticky="e")
        self.image_values = {}
        self.image_inputs = {}
        self.image_apply_buttons = {}
        for index, (name, (label, minimum, _)) in enumerate(IMAGE_SETTINGS.items()):
            column = index * 2
            ttk.Label(card, text=f"{label} (%)", style="Card.TLabel").grid(
                row=1, column=column, columnspan=2, sticky="w", padx=(0, 12), pady=(6, 0))
            variable = self.image_values[name] = tk.StringVar(master=self.root)
            spinner = self.image_inputs[name] = ttk.Spinbox(
                card, from_=minimum, to=100, width=5, textvariable=variable, style="Dark.TSpinbox")
            spinner.grid(row=2, column=column, sticky="ew", pady=(4, 0))
            spinner.bind("<Return>", lambda _event, key=name: self.save_image_setting(key))
            button = self.image_apply_buttons[name] = ttk.Button(
                card, text="Apply", command=lambda key=name: self.save_image_setting(key), style="Soft.TButton")
            button.grid(row=2, column=column + 1, sticky="e", padx=(6, 12), pady=(4, 0))
        ttk.Label(card, text="Apply to save. CAS updates live. FSR applies on game restart.",
                  style="Hint.TLabel", wraplength=460).grid(row=3, column=0, columnspan=4, sticky="w", pady=(6, 0))
        self.refresh_image_controls(errors)

    def refresh_image_controls(self, errors, names=IMAGE_SETTINGS):
        for name in names:
            try:
                self.image_values[name].set(str(read_image_setting(name)))
            except (OSError, ValueError) as error:
                self.image_values[name].set("")
                errors.append(str(error))

    def can_edit_image_settings(self):
        return self.saved_runtime_selection == runtime_selection.CUSTOM_LABEL and self.profile_executable is None

    def save_image_setting(self, name):
        if self.updating or not self.can_edit_image_settings():
            return
        label, minimum, _ = IMAGE_SETTINGS[name]
        try:
            value = int(self.image_values[name].get())
            write_value(name, value)
        except ValueError:
            message = f"{label} must be a whole number from {minimum} to 100."
        except PermissionError:
            message = f"Administrator access is required to save {label}."
        except OSError as error:
            message = str(error)
        else:
            self.image_values[name].set(str(value))
            timing = "applies during play" if name == "sharpen" else "restart the game to apply"
            self.status.set(f"Saved {label} at {value}% · {timing}.")
            return
        errors = [message]
        self.refresh_image_controls(errors, (name,))
        self.status.set(" ".join(errors))

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
        self.update_scope_title()
        self.reset_button.configure(text="Reset game profile" if executable else "Reset NR settings")
        self.profile_dirty = False
        self.profile_valid = True
        errors = []
        self.updating = True
        flow_preset = game_profiles.DEFAULTS[game_profiles.NVIDIA_FLOW_PRESET]
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
                try:
                    depth_mode = read_frame_generation_depth_mode()
                except (OSError, ValueError) as error:
                    depth_mode = game_profiles.DEFAULTS[FRAME_GENERATION_DEPTH_MODE_NAME]
                    errors.append(f"{error} Depth assist shown; stored request unchanged.")
                self.scope_feedback.set("Global NR and FG settings save automatically.")
            else:
                try:
                    values, sources = self.profile_store.load(executable, errors=errors)
                except (OSError, ValueError) as error:
                    values, sources = game_profiles.DEFAULTS.copy(), {}
                    self.profile_valid = False
                    errors.append(f"Profile rejected: {error} Reset profile to inherit globals before saving.")
                self.saved_enabled = bool(values[ENABLE_VALUE_NAME])
                self.saved_values = [values[name] for name in VALUE_NAMES]
                self.saved_processing_resolution = values[PROCESSING_RESOLUTION_VALUE_NAME]
                self.processing_resolution_verified = self.profile_valid
                mode, backend = values[FRAME_GENERATION_VALUE_NAME], values[FRAME_GENERATION_BACKEND_NAME]
                depth_mode = values[FRAME_GENERATION_DEPTH_MODE_NAME]
                flow_preset = values[game_profiles.NVIDIA_FLOW_PRESET]
                overrides = sum(source == "profile" for source in sources.values())
                inherited = sum(source in ("global", "default") for source in sources.values())
                feedback = (f"{overrides} game overrides · {inherited} inherited from globals/defaults."
                            if self.profile_valid else "Profile rejected · reset overrides to recover.")
                if edition := edition_labels(self.game_entries.values()).get(executable):
                    feedback = f"{edition} · {feedback}"
                self.scope_feedback.set(feedback)
            self.enabled.set(self.saved_enabled)
            for variable, value in zip(self.variables, self.saved_values):
                variable.set(value)
            self.processing_resolution.set(self.saved_processing_resolution)
            self.saved_frame_generation = next(label for label, value in FRAME_GENERATION_MODES.items() if value == mode)
            self.saved_frame_generation_backend = next(label for label, value in FRAME_GENERATION_BACKENDS.items() if value == backend)
            self.saved_frame_generation_depth_mode = next(
                label for label, value in FRAME_GENERATION_DEPTH_MODES.items() if value == depth_mode
            )
            self.frame_generation.set(self.saved_frame_generation)
            self.frame_generation_backend.set(self.saved_frame_generation_backend)
            self.frame_generation_depth_mode.set(self.saved_frame_generation_depth_mode)
            self.nvidia_flow_preset.set(next(label for label, value in NVIDIA_FLOW_PRESETS.items() if value == flow_preset))
            self.update_depth_mode_feedback()
            self.save_profile_button.configure(state="normal" if executable is not None and self.profile_valid else "disabled")
            self.refresh_image_controls(errors)
            self.update_runtime_controls()
            self.status.set("; ".join(errors) or ("Showing effective game settings · missing values inherit globals/defaults."
                                                   if executable else "Showing global settings."))
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
        if self.saved_runtime_selection != runtime_selection.CUSTOM_LABEL or self.profile_executable is None or not self.profile_valid:
            return False
        values = {name: variable.get() for name, variable in zip(VALUE_NAMES, self.variables)}
        values.update({ENABLE_VALUE_NAME: int(self.enabled.get()),
                       PROCESSING_RESOLUTION_VALUE_NAME: self.processing_resolution.get(),
                       FRAME_GENERATION_VALUE_NAME: FRAME_GENERATION_MODES[self.frame_generation.get()],
                       FRAME_GENERATION_BACKEND_NAME: FRAME_GENERATION_BACKENDS[self.frame_generation_backend.get()],
                       FRAME_GENERATION_DEPTH_MODE_NAME: FRAME_GENERATION_DEPTH_MODES.get(self.frame_generation_depth_mode.get()),
                       game_profiles.NVIDIA_FLOW_PRESET: NVIDIA_FLOW_PRESETS.get(self.nvidia_flow_preset.get())})
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
        if self.saved_runtime_selection == runtime_selection.CUSTOM_LABEL and selection != runtime_selection.CUSTOM_LABEL:
            if self.profile_dirty:
                if not self.confirm_profile_switch():
                    self.runtime_selection.set(self.saved_runtime_selection)
                    return
                self.load_scope(self.profile_executable)
            if ((self.processing_resolution_save_id is not None and not self.save_processing_resolution()) or
                    (self.foveation_save_id is not None and not self.save_foveation_size(self.variables[-1]))):
                self.runtime_selection.set(self.saved_runtime_selection)
                return
        self.status.set(f"Switching to {selection} and loading its launch routes…")
        self.root.update_idletasks()
        try:
            restarted = runtime_selection.write_selection(selection)
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
            if restarted:
                self.status.set(f"Saved {selection} for the next game launch. Virtual Desktop restarted; reconnect the headset.")
            else:
                self.status.set(f"Saved {selection} for the next game launch. Keep VDXR selected in Streamer.")
        self.update_runtime_controls()

    def update_runtime_controls(self):
        self.render_live_status()
        custom = self.saved_runtime_selection == runtime_selection.CUSTOM_LABEL
        for widget, state in ((self.nr_toggle, "normal"), (self.fg_selector, "readonly"),
                              (self.backend_selector, "readonly"), (self.depth_mode_selector, "readonly"),
                              (self.reset_button, "normal"), (self.add_profile_button, "normal")):
            widget.configure(state=state if custom else "disabled")
        self.save_profile_button.configure(
            state="normal" if custom and self.profile_executable is not None and self.profile_valid else "disabled")
        self.update_nvidia_flow_preset_state()
        if custom:
            message = "NR and frame generation controls apply to Custom VDXR."
        elif self.saved_runtime_selection == runtime_selection.STOCK_LABEL:
            message = "Stock VDXR does not support NR or frame generation. Your Custom settings are retained."
        else:
            message = "NR and frame generation require a verified Custom VDXR selection. Saved settings are retained."
        self.runtime_features_note.set(message)
        image_state = "normal" if self.can_edit_image_settings() else "disabled"
        for widget in (*self.image_inputs.values(), *self.image_apply_buttons.values()):
            widget.configure(state=image_state)
        self.set_slider_state()

    def on_frame_generation_backend_changed(self, _event=None):
        if self.stage_profile_change():
            self.update_depth_mode_feedback()
            self.update_nvidia_flow_preset_state()
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
        self.update_depth_mode_feedback()

    def can_edit_nvidia_flow_preset(self):
        return (self.saved_runtime_selection == runtime_selection.CUSTOM_LABEL
                and self.profile_executable is not None
                and self.profile_valid
                and FRAME_GENERATION_BACKENDS.get(self.frame_generation_backend.get()) == 0)

    def update_nvidia_flow_preset_state(self):
        self.nvidia_flow_preset_selector.configure(state="readonly" if self.can_edit_nvidia_flow_preset() else "disabled")

    def on_nvidia_flow_preset_changed(self, _event=None):
        if self.can_edit_nvidia_flow_preset():
            self.stage_profile_change()

    def update_depth_mode_feedback(self):
        mode = self.frame_generation_depth_mode.get()
        if mode == "Standard":
            message = "Standard · color-only frame generation."
        elif self.frame_generation_backend.get() != "NVIDIA (default)":
            message = "Requires usable depth · NVIDIA only. Selected backend unsupported; falls back to standard FG."
        elif mode == "Depth assist":
            message = "Requires usable depth. Missing usable depth uses standard FG."
        else:
            message = "Requires usable depth and a compatible runtime · NVIDIA only. Otherwise uses standard FG."
        self.depth_mode_feedback.set(message)

    def on_frame_generation_depth_mode_changed(self, _event=None):
        if self.stage_profile_change():
            self.update_depth_mode_feedback()
            return
        selection = self.frame_generation_depth_mode.get()
        try:
            write_frame_generation_depth_mode(FRAME_GENERATION_DEPTH_MODES[selection])
        except PermissionError:
            self.frame_generation_depth_mode.set(self.saved_frame_generation_depth_mode)
            self.status.set("Administrator access is required to save the reconstruction request.")
        except (OSError, ValueError, KeyError) as error:
            self.frame_generation_depth_mode.set(self.saved_frame_generation_depth_mode)
            self.status.set(f"Frame-generation depth request error: {error}")
        else:
            self.saved_frame_generation_depth_mode = selection
            self.status.set(f"Requested {selection} saved · applies next game launch.")
        self.update_depth_mode_feedback()

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
        active = self.enabled.get() and self.saved_runtime_selection == runtime_selection.CUSTOM_LABEL
        for slider in [*self.slider_widgets, self.processing_resolution_slider]:
            slider.configure(state=tk.NORMAL if active else tk.DISABLED,
                             background="#B2C4FF" if active else "#626A78",
                             troughcolor="#56627A" if active else "#353B46")

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
