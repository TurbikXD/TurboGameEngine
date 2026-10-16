#!/usr/bin/env node
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const ghidra = fileURLToPath(new URL('./local/ghidra_12.1.4_PUBLIC', import.meta.url));
if (existsSync(ghidra)) {
  process.env.GHIDRA_INSTALL_DIR ??= ghidra;
  process.env.REA_ANALYSIS_PROVIDER ??= 'ghidra';
}
// REA 6.5.0 requires a finite deadline; this is its largest supported Node timer.
// Keep the Codex tool timeout in setup.ps1 above this import/analysis budget.
process.env.REA_GHIDRA_STARTUP_TIMEOUT_MS ??= '2147483647';

const entry = new URL('./node_modules/rea-agents/scripts/rea.mjs', import.meta.url);
if (!existsSync(entry)) {
  process.stderr.write('REA is missing. Run tools/rea/setup.ps1 first.\n');
  process.exitCode = 1;
} else {
  await import(entry.href);
}
