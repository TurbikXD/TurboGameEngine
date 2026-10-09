// Analyze raw Tracy 0.13.1 -u / -m exports, preserving independent runs.
import fs from 'node:fs';
import path from 'node:path';
import readline from 'node:readline';

const manifestPath = path.resolve(process.argv[2] ?? '');
if (!process.argv[2]) throw new Error('Usage: node tools/lab1/analyze-traces.mjs <runs.json>');
const base = path.dirname(manifestPath);
const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8').replace(/^\uFEFF/, ''));
if (manifest.schema !== 1 || manifest.runsPerMode < 3) throw new Error('Expected schema 1 and at least three runs per mode.');
if (manifest.compareWorkers) {
  if (manifest.compareGpuUpload || manifest.includeUnboundedPump ||
      !Number.isInteger(manifest.beforeWorkers) || !Number.isInteger(manifest.afterWorkers) ||
      manifest.beforeWorkers < 1 || manifest.afterWorkers < 1 || manifest.beforeWorkers > 256 || manifest.afterWorkers > 256 ||
      manifest.beforeWorkers === manifest.afterWorkers || !Number.isInteger(manifest.workerComparisonInFlight) ||
      manifest.workerComparisonInFlight < 1 || manifest.workerComparisonInFlight > 16) {
    throw new Error('Invalid or confounded worker-count experiment.');
  }
  for (const run of manifest.runs) {
    const expected = run.mode === 'before' ? manifest.beforeWorkers : manifest.afterWorkers;
    if (!['before', 'after'].includes(run.mode) || run.requestedWorkers !== expected ||
        run.runtimeConfig?.workers !== expected || run.assetInFlight !== manifest.workerComparisonInFlight ||
        run.runtimeConfig?.assetInFlight !== manifest.workerComparisonInFlight ||
        run.jobs !== '1' || run.asyncLoading !== '1' || run.gpuUpload !== '1' ||
        run.runtimeConfig?.parallelEcs !== 'true' || run.runtimeConfig?.asyncLoading !== 'true') {
      throw new Error(`Worker configuration was not verified or other toggles changed in ${run.id}.`);
    }
  }
}
const targetZones = new Set([
  'Main Frame', 'Render Prepare Transforms', 'Physics Integrate Bodies', 'Physics Build Body Proxies',
  'Render Gather', 'Render Submit', 'Job Render Transforms', 'Job Wait', 'Present',
  'PhysicsSystem Update', 'Physics Broadphase', 'Physics Narrowphase and Solver',
  'Job Physics Integrate', 'Job Physics Build Proxies',
  'Asset Dispatch', 'Asset Synchronous Load', 'Asset Decode Job', 'Load Texture CPU', 'Decode Texture',
  'Load Mesh CPU', 'Parse Mesh Data', 'Asset Main Thread Tasks', 'Asset GPU Finalize',
  'Upload Texture GPU', 'Upload Mesh GPU', 'Asset Shutdown Join',
  'Load Mesh Transfer Job', 'Load Texture Transfer Job', 'Upload Build CPU Mips',
  'Upload Transfer Submit', 'Upload Transfer Record', 'Upload Graphics Acquire',
  'Upload Mesh Publish', 'Upload Texture Publish', 'Upload Transfer Shutdown',
]);
function parseCsv(line) {
  const row = []; let value = '', quoted = false;
  for (let i = 0; i < line.length; ++i) {
    const ch = line[i];
    if (ch === '"') {
      if (quoted && line[i + 1] === '"') { value += '"'; ++i; } else quoted = !quoted;
    } else if (ch === ',' && !quoted) { row.push(value); value = ''; } else value += ch;
  }
  if (quoted) throw new Error('Multiline/unterminated CSV field encountered. Inspect raw export.');
  row.push(value);
  return row;
}
async function* csvRows(file) {
  const lines = readline.createInterface({ input: fs.createReadStream(file, { encoding: 'utf8' }), crlfDelay: Infinity });
  let headers;
  for await (let line of lines) {
    line = line.replace(/^\uFEFF/, '');
    if (!line.trim()) continue;
    const fields = parseCsv(line);
    if (!headers) { headers = fields; continue; }
    if (fields.length !== headers.length) throw new Error(`Invalid CSV field count in ${file}.`);
    yield Object.fromEntries(headers.map((name, i) => [name, fields[i]]));
  }
}
function quantile(sorted, p) {
  if (!sorted.length) return null;
  const index = (sorted.length - 1) * p, lo = Math.floor(index), hi = Math.ceil(index);
  return sorted[lo] + (sorted[hi] - sorted[lo]) * (index - lo);
}
function stats(values) {
  values.sort((a, b) => a - b);
  const sum = values.reduce((a, b) => a + b, 0);
  const over16 = values.filter(v => v > 16.67).length, over50 = values.filter(v => v > 50).length;
  return { count: values.length, total_ms: sum, mean_ms: sum / values.length,
    count_over_16_67ms: over16, count_over_50ms: over50, percent_over_16_67ms: over16 * 100 / values.length, percent_over_50ms: over50 * 100 / values.length,
    median_ms: quantile(values, 0.5), p95_ms: quantile(values, 0.95), p99_ms: quantile(values, 0.99), max_ms: values.at(-1) };
}
const runs = [];
for (const run of manifest.runs) {
  if (run.status !== 'complete') throw new Error(`Incomplete run ${run.id}; no statistics have been accepted.`);
  const markers = new Map();
  for await (const row of csvRows(path.join(base, run.messages))) {
    if (row.MessageName?.startsWith('LAB_')) {
      const timestamp = Number(row.total_ns);
      if (!Number.isSafeInteger(timestamp) || timestamp < 0) throw new Error(`Invalid marker time in ${run.id}.`);
      if (!markers.has(row.MessageName)) markers.set(row.MessageName, timestamp);
    }
  }
  if (!markers.has('LAB_RUN_START')) throw new Error(`LAB_RUN_START missing in ${run.id}; check TGE_WAIT_FOR_TRACY=1 and capture order.`);
  const runStart = markers.get('LAB_RUN_START');
  const windows = [{ name: 'steady', start: runStart + manifest.windowSeconds[0] * 1e9, end: runStart + manifest.windowSeconds[1] * 1e9 }];
  if (run.scene === 'loading') {
    if (!markers.has('LAB_LOAD_START')) throw new Error(`LAB_LOAD_START missing in ${run.id}.`);
    if (!markers.has('LAB_LOAD_COMPLETE')) throw new Error(`LAB_LOAD_COMPLETE missing in ${run.id}; trace must contain the entire loading event.`);
    const event = markers.get('LAB_LOAD_START');
    if (Math.abs((event - runStart) / 1e9 - manifest.loadAtSeconds) > 0.5) throw new Error(`Load event timing drift >0.5 s in ${run.id}.`);
    windows.push({ name: 'loading_event', start: event + manifest.loadingWindowRelativeToEventSeconds[0] * 1e9, end: event + manifest.loadingWindowRelativeToEventSeconds[1] * 1e9 });
  }
  const values = new Map(windows.map(w => [w.name, new Map()]));
  const exclusions = { invalidOrIncomplete: 0, boundaryCrossing: Object.fromEntries(windows.map(w => [w.name, 0])) };
  let latestFrameEnd = 0, earliestFrameStart = Number.POSITIVE_INFINITY;
  let completeFramesDuringLoad = 0;
  for await (const row of csvRows(path.join(base, run.zones))) {
    if (!targetZones.has(row.name)) continue;
    const start = Number(row.ns_since_start), duration = Number(row.exec_time_ns), end = start + duration;
    if (!Number.isSafeInteger(start) || !Number.isSafeInteger(duration) || start < 0 || duration <= 0 || !Number.isSafeInteger(end)) {
      ++exclusions.invalidOrIncomplete; continue;
    }
    if (row.name === 'Main Frame') {
      latestFrameEnd = Math.max(latestFrameEnd, end); earliestFrameStart = Math.min(earliestFrameStart, start);
      if (markers.has('LAB_LOAD_START') && markers.has('LAB_LOAD_COMPLETE') && start >= markers.get('LAB_LOAD_START') && end <= markers.get('LAB_LOAD_COMPLETE')) ++completeFramesDuringLoad;
    }
    for (const window of windows) {
      // A zone must have BOTH ends inside the interval. Never count an unfinished zone.
      if (start < window.start || end > window.end) {
        if (end > window.start && start < window.end) ++exclusions.boundaryCrossing[window.name];
        continue;
      }
      const zones = values.get(window.name);
      if (!zones.has(row.name)) zones.set(row.name, []);
      zones.get(row.name).push(duration / 1e6);
    }
  }
  for (const window of windows) {
    if (latestFrameEnd < window.end || earliestFrameStart > window.start) throw new Error(`Trace ${run.id} does not cover full ${window.name} window; do not silently shorten it.`);
    const zones = values.get(window.name);
    if ((zones.get('Main Frame')?.length ?? 0) < 30) throw new Error(`Insufficient complete frames in ${run.id}/${window.name}.`);
    if (run.scene === 'ecs' && !zones.has('Render Prepare Transforms')) throw new Error(`Task #2 target zone is absent in ${run.id}.`);
  }
  if (run.scene === 'loading' && run.mode === 'after' && completeFramesDuringLoad === 0) throw new Error(`No complete frame progressed during async loading in ${run.id}. Inspect the raw trace.`);
  runs.push({ id: run.id, scene: run.scene, mode: run.mode, repeat: run.repeat, workers: run.runtimeConfig?.workers ?? null,
    markers_ns_since_start: Object.fromEntries(markers), exclusions,
    loadingProgress: markers.has('LAB_LOAD_COMPLETE') ? { completeFramesDuringLoad, wallTimeMs:(markers.get('LAB_LOAD_COMPLETE') - markers.get('LAB_LOAD_START')) / 1e6 } : null,
    windows: windows.map(window => ({ name: window.name, start_ns_since_start: window.start, end_ns_since_start: window.end,
      zones: Object.fromEntries([...values.get(window.name)].map(([name, durations]) => [name, stats(durations)])) })) });
}
const metrics = ['count', 'total_ms', 'mean_ms', 'median_ms', 'p95_ms', 'p99_ms', 'max_ms','count_over_16_67ms','count_over_50ms','percent_over_16_67ms','percent_over_50ms'];
const aggregates = [];
for (const scene of [...new Set(runs.map(run => run.scene))]) {
  const modes = scene === 'loading' && manifest.includeUnboundedPump ? ['before','after','legacy-pump'] : ['before','after'];
  for (const mode of modes) {
    const group = runs.filter(run => run.scene === scene && run.mode === mode);
    if (group.length !== manifest.runsPerMode || new Set(group.map(run => run.repeat)).size !== group.length) throw new Error(`Expected ${manifest.runsPerMode} distinct ${scene}/${mode} runs.`);
    for (const window of group[0].windows.map(w => w.name)) {
      const names = new Set(group.flatMap(run => Object.keys(run.windows.find(w => w.name === window).zones)));
      for (const name of names) {
        const rows = group.map(run => run.windows.find(w => w.name === window).zones[name]).filter(Boolean);
        if (rows.length !== group.length) continue; // Do not aggregate unequal per-zone trial counts.
        const record = { scene, mode, window, zone: name, runs: rows.length };
        for (const metric of metrics) record[metric] = quantile(rows.map(row => row[metric]).sort((a,b) => a-b), 0.5);
        record.run_median_min_ms = Math.min(...rows.map(row => row.median_ms));
        record.run_median_max_ms = Math.max(...rows.map(row => row.median_ms));
        aggregates.push(record);
      }
    }
  }
}
const loadingAggregates = [];
for (const mode of [...new Set(runs.filter(run => run.scene === 'loading').map(run => run.mode))]) {
  const values = runs.filter(run => run.scene === 'loading' && run.mode === mode).map(run => run.loadingProgress.wallTimeMs).sort((a,b) => a-b);
  loadingAggregates.push({ mode, runs: values.length, median_ms: quantile(values, 0.5), min_ms: values[0], max_ms: values.at(-1) });
}
const summary = { method: { independentUnit: 'one application run', aggregation: 'median of each per-run metric; frame samples are not independent trials',
  quantile: manifest.quantile, timeOrigin: 'Tracy LAB_RUN_START message; CSV ns_since_start and message total_ns are nanoseconds in the same clock',
  censoring: 'only positive-duration spans wholly contained in the specified interval', significance: 'No statistical significance claim; N is small.' },
  manifest: path.basename(manifestPath), runs, aggregates, loadingAggregates };
fs.writeFileSync(path.join(base, 'summary.json'), JSON.stringify(summary, null, 2) + '\n');
function writeCsv(file, records) {
  const keys = Object.keys(records[0] ?? {});
  const quote = value => '"' + String(value ?? '').replaceAll('"', '""') + '"';
  fs.writeFileSync(path.join(base, file), [keys.map(quote).join(','), ...records.map(row => keys.map(key => quote(row[key])).join(','))].join('\n') + '\n');
}
writeCsv('aggregate.csv', aggregates);
writeCsv('per-run.csv', runs.flatMap(run => run.windows.flatMap(window => Object.entries(window.zones).map(([zone, statistics]) => ({ id:run.id, scene:run.scene, mode:run.mode, repeat:run.repeat, workers:run.workers, window:window.name, zone, ...statistics })))));
const fmt = value => value === null || value === undefined ? '—' : value.toFixed(3);
const modeLabel = mode => manifest.compareWorkers ? `${mode === 'before' ? manifest.beforeWorkers : manifest.afterWorkers} workers` : mode;
const report = [
  manifest.compareWorkers ? `# Workers ${manifest.beforeWorkers} vs ${manifest.afterWorkers} — замеры Tracy` : '# ЛР 1 — воспроизводимые замеры Tracy', '',
  `Сборка: ${manifest.configuration}; Tracy ${manifest.tracyVersion}; SHA-256 app: \`${manifest.executableSha256}\`.`,
  `CPU: ${(manifest.hardware?.cpu ?? []).map(cpu=>cpu.model).join('; ') || 'UNKNOWN'}; физических ядер: ${manifest.hardware?.physicalCores ?? 'UNKNOWN'}; логических: ${manifest.hardware?.logicalProcessors ?? 'UNKNOWN'}; RAM: ${manifest.hardware?.ramBytes ? (manifest.hardware.ramBytes / 1024**3).toFixed(1) + ' GiB' : 'UNKNOWN'}.`,
  `ОС: ${manifest.hardware?.os ? `${manifest.hardware.os.name}, ${manifest.hardware.os.version}, build ${manifest.hardware.os.build}` : 'UNKNOWN'}; обнаруженные GPU: ${(manifest.hardware?.detectedGpus ?? []).map(gpu=>`${gpu.name} (driver ${gpu.driverVersion})`).join('; ') || 'UNKNOWN'}. Активный адаптер проверяется по журналу рендера.`,
  `Фактические workers из LAB_RUN_START: ${[...new Set(manifest.runs.map(run=>run.runtimeConfig?.workers).filter(n=>n!==undefined))].join(', ') || 'UNKNOWN'}. Настройки и контекст сборки сохранены в runs.json.`,
  `На режим выполнено ${manifest.runsPerMode} независимых запусков приложения. Порядок A/B чередуется между повторениями.`,
  manifest.compareWorkers ? `Worker-count A/B: before=${manifest.beforeWorkers}, after=${manifest.afterWorkers}. Jobs, async decode и GPU transfer включены в обоих режимах. Меняется только число workers. Asset in-flight limit зафиксирован на ${manifest.workerComparisonInFlight} в обоих режимах (без этого стандартный лимит зависел бы от workers). Публикация: 1 ресурс/кадр, 2 мс; ECS: 4096 объектов. Это отдельная серия, не прежние L1/CPU-vs-GPU upload замеры.`
    : 'В исходном движке уже существовал отдельный пул асинхронной загрузки на 4 потока. Он мигрирован на общую job system. Режим loading/before (sync) — контролируемое отключение async для демонстрации L1, а не утверждение, что исходный движок всегда загружал синхронно или создавал неограниченное число потоков.',
  manifest.compareWorkers ? '' : manifest.compareGpuUpload
    ? 'GPU upload A/B: оба режима используют общий scheduler и async CPU decode. before: GPU upload/GenerateMips в main pump. after: CPU mip generation в jobs + отдельная transfer-очередь Diligent + настоящий GPU fence; main только GPU-side acquire и публикация. В обоих режимах лимит публикаций 1 ресурс/кадр, бюджет 2 мс. Это эффект всей upload pipeline, а не изолированный тест одного GPU copy.'
    : 'Режим loading/after: общий scheduler + ограниченный GPU-памп (1 ресурс/кадр, мягкий бюджет 2 мс); отдельная transfer-очередь отключена. Если включён legacy-pump: тот же новый scheduler с лимитами 65536 ресурсов/кадр и 60000 мс, имитирующий прежнюю неограниченную обработку готовых ресурсов; это изоляция эффекта пампа, а не запуск старого бинарника.',
  'Прогрев: первые 3 с после LAB_RUN_START исключены. Основной диапазон: +3…+14 с; загрузка запускается на +6 с.',
  'Для загрузки дополнительно показано окно LAB_LOAD_START −1…+5 с. Времена отсчитываются по сообщениям Tracy, а не времени запуска процесса или подключения.',
  'Квантили вычислены линейной интерполяцией R-7 отдельно для каждого прогона. В таблице — медиана соответствующей метрики между прогонами.',
  'Число кадров не подменяет число независимых экспериментов. Статистическая значимость не заявляется. Удаляются только незавершённые/невалидные зоны и зоны, пересекающие границу интервала.', '',
  '| Сцена / окно | Зона | Режим | N | Count¹ | Mean, мс | Median, мс | p95, мс | p99, мс | Max, мс |',
  '|---|---|---|---:|---:|---:|---:|---:|---:|---:|',
];
for (const row of aggregates) report.push(`| ${row.scene} / ${row.window} | ${row.zone} | ${modeLabel(row.mode)} | ${row.runs} | ${row.count} | ${fmt(row.mean_ms)} | ${fmt(row.median_ms)} | ${fmt(row.p95_ms)} | ${fmt(row.p99_ms)} | ${fmt(row.max_ms)} |`);
report.push('', '¹ Count — медиана числа полных вызовов/кадров на прогон. Max — медиана максимумов отдельных прогонов; абсолютный максимум каждого запуска сохранён в per-run.csv.', '',
  '## Сравнение целевых зон', '', '| Сцена / окно | Зона | Метрика | До, мс | После, мс | Снижение времени² |', '|---|---|---|---:|---:|---:|');
const comparedZones = ['Main Frame','Render Prepare Transforms','Physics Integrate Bodies','Physics Build Body Proxies','Asset Main Thread Tasks','Asset GPU Finalize'];
if (manifest.compareWorkers) comparedZones.push('Decode Texture','Parse Mesh Data','Upload Build CPU Mips','Upload Transfer Submit','Upload Transfer Record');
for (const before of aggregates.filter(row => row.mode === 'before' && comparedZones.includes(row.zone))) {
  const after = aggregates.find(row => row.mode === 'after' && row.scene === before.scene && row.window === before.window && row.zone === before.zone);
  if (!after) continue;
  const comparedMetrics = before.zone === 'Main Frame' ? ['median_ms','p95_ms','p99_ms','max_ms'] : ['mean_ms','median_ms'];
  for (const metric of comparedMetrics) {
    const reduction = (1 - after[metric] / before[metric]) * 100;
    report.push(`| ${before.scene} / ${before.window} | ${before.zone} | ${metric} | ${fmt(before[metric])} | ${fmt(after[metric])} | ${reduction.toFixed(1)}% |`);
  }
}
report.push('', '## Кадры во время загрузки и длинные кадры', '', '| Прогон | Полных кадров внутри загрузки | Загрузка, мс | Кадров >16,67 мс³ | Кадров >50 мс³ |', '|---|---:|---:|---:|---:|');
for (const run of runs) {
  const frame = run.windows.find(w=>w.name==='steady').zones['Main Frame'];
  report.push(`| ${run.id} | ${run.loadingProgress?.completeFramesDuringLoad ?? '—'} | ${fmt(run.loadingProgress?.wallTimeMs)} | ${frame.count_over_16_67ms} | ${frame.count_over_50ms} |`);
}
report.push('', '³ В основном фиксированном диапазоне. Полные кадры внутри загрузки считаются по двум Tracy-маркерам; не совпадают с числом кадров, пересекающих событие.');
if (loadingAggregates.length) {
  report.push('', '## Полное время загрузки по независимым прогонам', '', '| Режим | N | Median, мс | Min, мс | Max, мс |', '|---|---:|---:|---:|---:|');
  for (const row of loadingAggregates) report.push(`| ${modeLabel(row.mode)} | ${row.runs} | ${fmt(row.median_ms)} | ${fmt(row.min_ms)} | ${fmt(row.max_ms)} |`);
}
report.push('', '² Отрицательное значение означает регрессию. Это описательное сравнение медиан метрик прогонов.', '',
  '## Артефакты и проверка', '',
  '- runs.json: параметры, порядок, SHA-256 executable и исходных ресурсов, коды завершения.',
  '- summary.json / per-run.csv / aggregate.csv: все принятые значения и точные диапазоны в ns_since_start.',
  '- В каждой папке запуска: trace.tracy, полный zones.csv (-u), messages.csv (-m), stdout/stderr приложения и захвата.',
  '- Tracy Compare: открыть before/after одного repeat; диапазоны брать из summary.json для каждого трейса — стартовый timestamp между процессами отличается.',
  '- Worker-зоны показывают параллелизм; сумма времён разных worker-потоков не равна wall time кадра.',
  '- Один разовый hitch может не попасть в p95/p99: поэтому рядом приводится Max каждого прогона.',
  '- Loading использует файловый кэш ОС в обычном состоянии; сброс кэша и холодный диск не заявляются. Порядок AB/BA уменьшает, но не устраняет систематические различия.',
  '- Одновременное воспроизведение видео, сборка проекта и другие фоновые нагрузки искажают сравнение.', '');
fs.writeFileSync(path.join(base, 'measurements.md'), report.join('\n'));
console.log(`Analyzed ${runs.length} runs. Report: ${path.join(base, 'measurements.md')}`);
