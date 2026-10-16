# Editor fonts

`SourceSansPro-Regular.ttf` and `SourceSansPro-Semibold.ttf` are unchanged Adobe
Source Sans Pro 2.021 font files. They were copied on 2026-10-16 from
`content/fonts/` in the locally installed Roblox Studio build
`version-9b554450a0fc4e65`. The family, version, copyright and OFL license
identifiers were read from their sfnt name tables; their cmap includes Cyrillic.
Copyright 2010, 2012, 2014 Adobe Systems Incorporated, Reserved Font Name “Source”.

They are redistributed under the accompanying [SIL Open Font License 1.1](SourceSansPro-LICENSE.txt).
Its copyright notice matches these font files; the OFL text is also supplied in
Studio's `StudioFonts/SourceSansProLICENSE` (that older font package lists 2010,
2012). Upstream: [Adobe Source Sans](https://github.com/adobe-fonts/source-sans).

SHA-256:

- Regular: `c9868de61ff2bab0b5a3a6d01c4b76f299459f08c6ae2f2c0383b4f9f6bedbf3`
- Semibold: `3ba5c382a7ee6a8831bdf90192addceabe6db4278a679e67fe7e9c0226b729cf`

The portable default loads regular at 15 logical pixels and also registers
semibold at 15 pixels; it includes Latin, Greek, Cyrillic and common punctuation ranges.
No font is installed into the operating system. Missing bundled fonts leave the
existing atlas untouched.

`FiraMono-Regular.ttf` is the unchanged Fira Mono font from the same build's
`StudioFonts/` directory. SHA-256:
`8c86f2963208a353c3435e28ecb38a99ab68f14d7433ba00c1822cef9a9c1b44`.
The accompanying [OFL 1.1 license](FiraMono-LICENSE.txt) identifies the Mozilla
Foundation and Telefonica S.A., 2012–2015. Studio's local `GlobalSettings_13.xml`
stores `Font`, `Output Font`, and `CommandBarFont` as Fira Mono, 10 pt, normal
weight. At 96 DPI that is 13.333333 pixels per em. Its hhea ascent/descent span
is 1200 units for 1000 units/em, so the editor uses **16 pixels in stb's
ascent/descent-based size** to match that Qt glyph scale. It is registered after
the regular, emphasis, and native-panel UI fonts. The separate native role uses
Source Sans Pro at 15 stb pixels, close to Qt's native application default of
9 pt / 12 pixels per em (its hhea span is 1257 units per 1000 units/em).
Native category headers also register Source Sans Semibold at 15 stb pixels;
Explorer renders Source Sans at 18 stb pixels, close to its legacy 14 px/em role.
`codeFont()`, `nativeFont()`, `nativeSemiboldFont()`, and `semiboldFont()` resolve roles within the active ImGui context
and fall back to that context's default; no font pointer is cached globally.

This is a freely redistributable Studio-shipped family, not a claim that all
text in the reference screenshot uses Source Sans Pro. The recovered modern
Ribbon `GeneralTokens` instead names Builder Sans (13/15 px text tokens).
Builder font files are not bundled in these distributable assets. Their
[Roblox license](https://create.roblox.com/docs/resources/builder-font-license)
limits use to Roblox UGC and its digital promotions, so it does not cover this
independent editor's distribution. Source Sans Pro is the portable replacement;
its glyph shapes and text metrics differ from Builder Sans.

For this workspace only, the renderer may explicitly pass the ignored directory
`.local/editor-fonts` to `configureEditorFonts`. When both original
`BuilderSans-Regular.otf` and `BuilderSans-Medium.otf` are present, it loads those
at 15 logical pixels and merges the bundled Source Sans Greek/Cyrillic glyphs
which Builder lacks. A missing or incomplete local pair selects the portable
Source Sans default. The local fonts remain outside `assets/`, are not installed
globally, and must not be included in distributable engine packages. See the
local directory's provenance README for the machine-specific source and hashes.
