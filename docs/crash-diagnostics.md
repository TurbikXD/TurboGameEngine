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

New fatal failures write `logs/crash-last.txt` relative to the process's working
directory, independently of the normal logger. It records the exception/abort
category, available message, UTC timestamp, process/thread IDs, module offsets
and a reporting stack. A native Windows fault also records its fault address.
This is a diagnostic record, not a recovery mechanism or a symbolized minidump.
Use the matching executable/PDB to resolve offsets. Assertions and native faults
remain visible; they are not suppressed. No global Windows crash settings change.

For a new report, preserve `crash-last.txt` and the final `logs/engine.log`
interval, the build configuration, project and the action before the failure.
Standalone Play has its own temporary working directory/logs. A missing report
does not prove a normal exit (external termination and some low-level failures
can bypass handlers).

The UI image path has independent safeguards: immutable bounded encoded
snapshots, dimension validation before decode, bounded cache/pending loads and
GPU-fence lifetime pins. See [visual UI](roblox-ui-visuals.md).
