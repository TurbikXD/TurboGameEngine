# Crash fixes and local diagnostics

An exit code of 3 is a common MSVC abort result, not a unique cause. The editor
path was tested independently of Play. A quoted Luau literal with 32,768
characters reproduced `std::regex_error(error_stack)` in the previous syntax
tokenizer. With no top-level catch this terminated the application. Luau
highlighting now uses a linear scanner. A second rendered reproducer was a valid
quoted string with an escaped newline: comment/string scanning accessed
`line[line.size()]`, triggering a CRT assertion/abort. That boundary is now
checked. UTF-8 boundary reads and stale selections
after document replacement were also repaired. Script loading is bounded to
2 MiB even if the file grows between metadata inspection and reading.

`engine_script_editor_tests` creates an actual ImGui frame and renders/colorizes
long literals/tokens, malformed UTF-8, replacement selections and undo paths.
The UI Demo scripts were additionally colorized through this rendered path.
This reproduces a matching class of editor failure; the original user's crash
cannot be uniquely identified from an old log without a stack.

The scripting runtime also protects host-side behavior setup, Inspector module
and field access, callbacks, field synchronization and reload attachment. A
frozen behavior table or hostile metatable reports a script error rather than
letting a Luau exception escape C++. Regression scenarios are in
`engine_scripting_tests`. Structured React errors retain their raw `message`
without invoking user `__tostring` metamethods.

On Windows, a fatal failure creates a uniquely named directory under
`%LOCALAPPDATA%/TurboGameEngine/crash-reports`. Preserve `crash.dmp`, `engine.log`
and `report.json` together. The report records the build ID, commit, UTC time,
scene, last recorded action, exception code and faulting thread. Use the
matching executable and PDB to resolve the stack. Each process has its own log
snapshot, including Standalone Play and network test windows.

`logs/crash-last.txt` remains a short summary in the working directory for
Play Session diagnostics. See [Lab 5](lab5/README.md) for the report format,
package checks and symbolized stack workflow.

The UI image path has independent safeguards: immutable bounded encoded
snapshots, dimension validation before decode, bounded cache/pending loads and
GPU-fence lifetime pins. See [visual UI](roblox-ui-visuals.md).
