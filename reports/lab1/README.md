# Published Lab 1 evidence

- `release-verified/`: 15 successful Tracy 0.13.1 captures; original CSV exports;
  `runs.json` metadata; `summary.json`, `per-run.csv`, `aggregate.csv` and
  `measurements.md`. Three runs per mode, one executable hash.
- `stress-final/`: six stress scenarios with results and logs.
- `validation/`: the historical build/test/live-demo verification.
- `../lab1-defense/`: final seven-slide PPTX and visible-demo screenshot.

The measurements were recorded on **24 September 2026**. Their limitations and
interpretation are explained in [results](../../docs/lab1/results.md).
Absolute machine paths in metadata and logs are historical context, not a
requirement to clone into that directory. Relative trace/CSV paths are kept.
CSV and Tracy bytes are preserved by `.gitattributes`.

See [the reproduction protocol](../../tools/lab1/README.md) and
[the defense guide](../../docs/lab1/README.md). The fixture files are generated,
not committed. Failed diagnostic runs and unrelated reports are excluded.
