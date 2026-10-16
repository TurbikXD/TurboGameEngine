# Visual UI and controls

The engine's React Luau renderer supports `UIGradient`, `ImageLabel`,
`ImageButton`, `TextBox`, `ScrollingFrame`, `UIGridLayout`, `UIScale`,
`UISizeConstraint` and `UIAspectRatioConstraint` alongside the existing hosts.
These are native UI objects, usable through React elements or UI Instances.
The upstream React packages are unchanged.

## Gradients

```lua
local React = require("React")
local e = React.createElement

return e("Frame", {
    Size = UDim2.fromOffset(420, 180),
    BackgroundColor3 = Color3.new(1, 1, 1),
}, {
    e("UICorner", { CornerRadius = UDim.new(0, 18) }),
    e("UIGradient", {
        Color = ColorSequence.new({
            ColorSequenceKeypoint.new(0, Color3.fromRGB(32, 220, 210)),
            ColorSequenceKeypoint.new(0.5, Color3.fromRGB(76, 94, 228)),
            ColorSequenceKeypoint.new(1, Color3.fromRGB(196, 84, 225)),
        }),
        Transparency = NumberSequence.new(0),
        Rotation = 25,
        Offset = Vector2.zero,
        Enabled = true,
    }),
})
```

The gradient multiplies its parent's colors and opacity; use a white parent
color to preserve the sequence colors. It does not recursively recolor children.
Color and transparency interpolate between keypoints. `Rotation` is in degrees;
`Offset` is relative to the parent. Properties can use React bindings, including
animated rotation/offset. Rounded corners and clipping apply to images/gradients.

`ColorSequence`, `ColorSequenceKeypoint`, `NumberSequence`,
`NumberSequenceKeypoint` and `Rect` are immutable globals with compatible
`typeof` tags; they are also exported by `ReactTurbo`. Sequences accept 2–20
strictly ordered keypoints with endpoints at 0 and 1. Unsupported or non-finite
values fail during property validation. NumberSequence envelopes are stored;
UI transparency uses the deterministic keypoint values, not randomized envelopes.

## Images

```lua
e("ImageLabel", {
    Image = "textures/ui_demo/outpost.png",
    Size = UDim2.fromOffset(360, 180),
    BackgroundTransparency = 1,
    ScaleType = Enum.ScaleType.Crop,
    ImageColor3 = Color3.new(1, 1, 1),
    ImageTransparency = 0,
})

e("ImageButton", {
    Image = "textures/icons/play.png",
    HoverImage = "textures/icons/play_hover.png",
    PressedImage = "textures/icons/play_pressed.png",
    Size = UDim2.fromOffset(48, 48),
    BackgroundTransparency = 1,
    [React.Event.Activated] = function() print("Play") end,
})
```

- Paths resolve under the active project's `assets`; `assets/` is an optional
  prefix. PNG, JPEG, BMP, TGA and supported stb_image formats work; editable P3
  PPM and binary P6 PPM are supported too. UTF-8 project/file paths work on Windows.
- `Stretch`, `Fit`, `Crop`, `Tile` and nine-slice `Slice` use `Enum.ScaleType`.
  `ImageRectOffset`/`ImageRectSize` select atlas pixels. `TileSize` is a UDim2;
  `SliceCenter` is a Rect and `SliceScale` controls edge sizing.
- `IsLoaded` is read-only and publishes through `GetPropertyChangedSignal` or
  `React.Change.IsLoaded`. An empty Image draws no image. Missing or invalid
  nonempty images show a placeholder and a diagnostic instead of dereferencing
  an invalid texture. Hover/pressed image variants use the same loader.
- Decoding and GPU upload use the existing async resource queue. Changed image
  files reload during Play; the old good image remains while a replacement loads,
  and a failed edit preserves it. File changes are polled at 500 ms intervals.
- Headers are checked against an immutable bounded file snapshot before decoding:
  maximum 8192 per side, 16 megapixels and 128 MiB of encoded input.
  The UI cache reserves at most 64 MiB of decoded RGBA data (including pending
  replacements), at most 128 entries and four new requests per frame. Textures
  referenced by submitted GPU frames stay pinned until their fence completes;
  these in-flight pins are additional to the working cache.
- HTTP downloads, Roblox CDN authentication, `ImageContent` and changing the
  image sampler through `ResampleMode` are not implemented. Import files locally.

To keep a port's existing Roblox IDs, create `assets/ui_images.json`:

```json
{
  "rbxassetid://42": "textures/ui_demo/outpost.png",
  "rbxassetid://43": "textures/icons/play.png"
}
```

Then `Image = "rbxassetid://42"` resolves locally. This map is limited to 64 KiB
and 256 entries, polled every second, and retains the last good map on malformed
edits. Alias targets obey the same project containment rules. It grants no access
to Roblox-hosted assets.

## Layout and input

`UIScale.Scale` scales UI geometry and text. `UISizeConstraint.MinSize/MaxSize`
bound the size; `UIAspectRatioConstraint` supplies `AspectRatio`, `AspectType`
and `DominantAxis`. `UIGridLayout` supplies `CellSize`, `CellPadding`,
`FillDirectionMaxCells`, `FillDirection`, `StartCorner`, alignment and sorting.
Derived geometry is read-only, like existing list-layout geometry.

`ScrollingFrame` uses `CanvasSize`, `CanvasPosition`, `AutomaticCanvasSize`,
`ScrollingDirection`, `ScrollBarThickness` and `ScrollingEnabled`. It clips its
contents and exposes `AbsoluteCanvasSize`/`AbsoluteWindowSize`. `TextBox` adds
`TextEditable`, `ClearTextOnFocus`, `MultiLine`, `PlaceholderText` and
`PlaceholderColor3` to the supported text properties. Use `React.Change.Text`
for controlled text and `Focused`/`FocusLost` for focus changes. Buttons expose
hover and mouse down/up signals as well as `Activated`/`MouseButton1Click`.
Native events remain deferred and protected, and unmount/Stop clears subscriptions.
TextBox editing uses native ImGui alignment (left aligned); it does not reproduce
all Roblox text alignment, cursor/selection APIs, RichText or TextScaled. Use
`CaptureFocus()`, `ReleaseFocus(enterPressed?)` and `IsFocused()` for focus control.
Uncontrolled text/scroll positions survive unrelated React renders; removing a
previously controlled prop restores its default.

The compatibility guide lists the remaining platform limits. A synchronous
engine UI renderer is not a complete Roblox DataModel or Roblox input service.
Unsupported features must fail clearly rather than silently accepting fake props.

## Demo and verification

Open **UI Demo → Play → Визуал** for gradients, authored image assets, image
buttons and interactive controls. Existing NOVA and ORBIT pages are preserved.
Restart after updating the native binary. `UI.getNativeFeatures()` reports a
frozen `{version, images, gradient, controls, canvasGroups}` table from native bootstrap;
version 2 enables the visual feature set, version 3 adds
[flattened CanvasGroup layers](canvas-groups.md). Projects can show a restart hint for older
binaries even when their Luau files have already refreshed.

Tests cover immutable values and bad input (`engine_react_runtime_tests`), native
geometry/input/draw commands (`engine_gameplay_state_tests`), and contained
paths/aliases/cache/live image replacement/oversized headers
(`engine_project_asset_tests`, `engine_texture_loader_tests`). Demo `.checks`
renders actual ImGui draw data, including the image textures, in a CPU fixture.

API references: [UIGradient](https://create.roblox.com/docs/reference/engine/classes/UIGradient),
[ImageLabel](https://create.roblox.com/docs/reference/engine/classes/ImageLabel),
[ImageButton](https://create.roblox.com/docs/reference/engine/classes/ImageButton).
