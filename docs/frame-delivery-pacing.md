# Frame delivery and pacing

The 2026.11.15.1 release uses the selected C++ host with improved **2× recovery after slow frames**.
OFXR 2×, 3× and 4× remain available, with NR on or off. Their multipliers request one, two or three generated
images between real frames; they do not guarantee equivalent game-FPS gains or headset display rates.

## 2× recovery

The runtime preserves the two native positions and original targets already issued for a 2× application frame.
A partially late group can still use useful generated content before its original real-frame target. When the
whole group has expired, ordinary delivery consumes the owned positions and the runtime can establish fresh
history for the next generated/real pair. The returned prediction and reservation frontier stay consistent.

This avoids losing future 2× cadence solely because a previous pair was late. It does not move the missed
targets into the future or count ordinary fallback frames as generated frames. Unsupported input, a discarded
frame or an application-supplied alternate display time does not authorize that recovery path.

The focused 2× reservation repair has no new toggle and adds no fixed pacing sleep. Saved NR processing,
coverage and appearance settings are preserved. The complete release also includes the delivery baseline below.

## Other delivery changes since November 10

For stretched **4×** source intervals, generated content is sampled at the group's original reserved native-slot
times. Each fraction expresses that slot time within the previous/current real-frame interval. The November 10
package instead redistributed those samples uniformly across the interval at one-quarter, one-half and
three-quarters. This sampling change keeps the original native deadlines, slot order and real B endpoint.

The selected host also includes early NR staging before the capacity wait on eligible paths, guarded last-role
copy handoff for 2×/3×/4×, and updated DXC-built D3D12 preprocessing shaders. These baseline changes are separate
from the focused 2× reservation repair; their presence is not a new live 3×/4× or NR performance claim.

## Copy completion and fallback

A generated image must satisfy its actual GPU-copy completion fence before handoff. When a queued local copy
is pending, the worker can wait on its completion event in short bounded slices, ending at the applicable
original target. An event can wake early; the fence remains the readiness authority. A pending or too-late
image uses the existing real-frame or permitted current-group fallback rather than an uncompleted image.

The producer context is released only when the independent-delivery and ownership conditions permit it.
GPU completion, real-frame readiness, depth ordering, bank ownership and retirement remain protected.
The native SDK's Wait/Begin/End ordering is retained. These protections do not eliminate application loading,
GPU contention, network stalls or long native SDK calls.

## What to expect

Start with NR off and **OFXR 2× / NVIDIA**, with Virtual Desktop's **SSW Off**. Compare the same scene and
motion, then try 3× or 4× separately. NR remains independent; enabling NR with FG requires at least 90% coverage.
Restart the game after changing the FG multiplier, backend, reconstruction request or runtime selection.

The selected 2× recovery behavior does not establish a new performance gain for every multiplier, NR setting,
graphics API or game. CPU handoff times are not headset scanout measurements. Smoothness, an FPS counter or a
saved selector alone cannot prove that every generated image reached the headset.

Install the matching host, FG module and `CUSTOM-RUNTIME.json` from the
[2026.11.15.1 release](releases/2026.11.15.1.md). Use its corresponding-source archive and build manifests when
rebuilding; do not mix intermediate headers or old object files. See the [quickstart](NR-FG-QUICKSTART.md),
[side-by-side setup](SIDE-BY-SIDE-RUNTIME.md) and [retained module guide](build-retained-fg.md).
The compatible licensed NR component is supplied separately and is not included or downloaded.
