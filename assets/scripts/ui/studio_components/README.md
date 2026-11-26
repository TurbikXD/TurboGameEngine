# StudioComponents examples in ORBIT Mission Control

MIT source: [sircfenner/StudioComponents](https://github.com/sircfenner/StudioComponents),
pinned commit `ceb9d451ec5f53adec6e539d4ea4a6a986882fd6`.
Original source and complete copyright/license are retained beside the ports.

- [LoadingDots original](https://github.com/sircfenner/StudioComponents/blob/ceb9d451ec5f53adec6e539d4ea4a6a986882fd6/src/Components/LoadingDots.luau): three phased binding animations, Heartbeat subscription and cleanup.
- [ProgressBar original](https://github.com/sircfenner/StudioComponents/blob/ceb9d451ec5f53adec6e539d4ea4a6a986882fd6/src/Components/ProgressBar.luau): clipped dual-color progress text and controlled value/formatter.
- [Visual component gallery](https://sircfenner.github.io/StudioComponents/): references for the original Studio controls.

Port changes are explicit: package imports use the engine's `require("React")`;
standalone CommonProps annotations become local table types; Studio-only color
and modifier enum references become private string keys consumed by `Theme.luau`.
The theme is React context supplied by the surrounding scene, not a fake Studio
service. Font and sizing constants are local. ProgressBar handles Max <= 0 and
zero progress without creating an infinite UDim2 scale. Otherwise its dual-label
tree and LoadingDots' binding/effect/cleanup logic follow the retained sources.

This is a two-component port, not support for the whole StudioComponents library.
Source API comments are documented here; complete originals and their MIT
notices remain alongside the ports. `ProgressBar` accepts `Value`, optional `Max`
(default 1) and an optional `Formatter(current, max)` string callback. Use it for
detailed loading progress. `LoadingDots` provides a compact activity indicator
with binding-driven animation and effect cleanup.
ORBIT adds its own mission reducer, crew selection, animated orbits and waveforms,
interactive mission progression, theme changes and portal dialog around them.
The separate tutorial workshop remains available via the SOURCE button.
