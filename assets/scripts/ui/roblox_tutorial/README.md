# Imported Roblox React Luau tutorial

Source: [minimapletinytools/react-lua-tutorial](https://github.com/minimapletinytools/react-lua-tutorial),
commit `cd6a5c5f7e8e84e292190958b1e7c89872ec24c9`.
The project accompanies the author's [How To: React + Roblox](https://devforum.roblox.com/t/how-to-react-roblox/2964543) guide.

- `Tutorial.original.lua`: complete original [Tutorial.lua](https://github.com/minimapletinytools/react-lua-tutorial/blob/cd6a5c5f7e8e84e292190958b1e7c89872ec24c9/src/shared/Tutorial.lua), retained for comparison; not executed.
- `Components.luau`: `MyColorfulClickableSquare`, `MyBoopMachine`, `useToggleState`, `MyToggleWoggle`, and `MyReallyReallyCuteFrame`, with **unchanged function bodies**.
- `LICENSE`: original MIT license, copyright 2024 minimapletinytools.

Port changes: replace the two Roblox package-location lines with
`local React = require("React")`; export only the selected components. No
component logic, Roblox datatype, Enum property, event or host name was changed.
Explanatory comments live in Markdown; the Luau strict-checking directive stays
in the port. The `useToggleState` pattern is credited to
[boyned's React notes](https://blog.boyned.com/articles/things-i-learned-using-react/).
The original file contains additional examples requiring unsupported Roblox
services/hosts; they are deliberately excluded from the executable module.

The user's **UI Demo** project contains identical vendored files. Its sixth
**Roblox** tab opens the styled ORBIT mission scene, including separately
attributed StudioComponents controls. **SOURCE** opens this tutorial workshop:
four unchanged components plus an `Instance` / geometry signal / `ReactRoblox`
root-and-portal probe. Start the rebuilt engine, open **UI Demo**, press Play and
choose that tab. The existing five tabs and scene source IDs are preserved.
