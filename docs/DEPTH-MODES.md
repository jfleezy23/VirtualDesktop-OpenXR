# Choosing a reconstruction mode

In the OFXR card, use **Requested reconstruction mode** to choose how frame generation handles game depth. The selection is available for Globals and individual game profiles. It is independent of the OFXR multiplier and optical-flow backend.

| Requested mode | What it requests |
| --- | --- |
| **Standard** | Ordinary color-only frame generation. It does not request depth guidance. |
| **Depth assist** | The default for an unset request: depth-assisted endpoint preference when usable game depth and a compatible runtime are available. |
| **Depth reconstruction (experimental)** | Optional camera-aligned reconstruction using genuine depth from the game's submitted images. Requires the NVIDIA backend and a matching runtime/FG module with reconstruction support. |

A saved request applies when the next game session starts. Fully close and relaunch the game after changing reconstruction mode, OFXR multiplier or backend. Changing a selector does not replace an already running session's selection.

## Globals and game profiles

In **Globals**, changes save immediately. For a game, select its actual executable, choose the requested mode and click **Save game profile**. Selecting an executable writes nothing; unsaved game changes remain drafts.

New games inherit Globals. Saving a complete game profile locks its displayed settings, including reconstruction mode. A missing setting in a partial profile inherits the global value, then its default. Existing explicit Standard or Depth assist overrides remain in effect. **Reset game profile** removes the game's overrides and returns it to Globals. Per-game selection does not temporarily rewrite the global settings.

## Depth and backend requirements

Use the NVIDIA backend for either depth mode. A game must submit usable, correctly paired depth through the active custom runtime. Selecting reconstruction cannot manufacture missing depth or reroute a game that is using another runtime.

Missing or unusable depth keeps ordinary color FG. A module without the required reconstruction capability, or an unsupported backend, also keeps Standard FG on the selected backend. The runtime does not silently call Depth assist reconstruction. The OFXR multiplier remains separate: 2x, 3x and 4x retain their own frame-count and timing requirements.

The selector shows a **requested mode**, not proof of active reconstruction on any frame. A compatible installation and successful depth/output validation are needed to establish what a game actually uses. No mode guarantees that ghosting, disocclusion or moving-detail artifacts disappear. Compare the same scene and motion; return to Standard or Depth assist if the experimental result is worse.

Reconstruction selection does not change NR coverage, processing resolution, appearance settings, game resolution or saved runtime selection. Valid existing settings are preserved. Fresh NR coverage and processing resolution defaults remain **100%**.
