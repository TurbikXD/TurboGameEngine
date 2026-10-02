// Synthetic input tests for analysis math only. These are NOT benchmark evidence.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'tge-analysis-selfcheck-'));
const script = path.join(path.dirname(fileURLToPath(import.meta.url)), 'analyze-traces.mjs');
const manifest = { schema:1, runsPerMode:3, configuration:'SYNTHETIC SELF-CHECK ONLY', tracyVersion:'0.13.1', executableSha256:'NOT_A_BENCHMARK', windowSeconds:[3,14], quantile:'R-7', runs:[] };
for (const mode of ['before','after']) for (let i=1;i<=3;++i) {
  const id=`${mode}-${i}`, origin=123456789+i*100000000;
  const rows=['name,src_file,src_line,ns_since_start,exec_time_ns,thread,value'];
  // Coverage events are outside the analysis interval, not included in percentiles.
  rows.push(`Main Frame,test.cpp,1,${origin+2e9},1000000,1,`);
  rows.push(`Main Frame,test.cpp,1,${origin+15e9},1000000,1,`);
  for(let j=1;j<=50;++j) {
    rows.push(`Main Frame,test.cpp,1,${origin+3e9+j*1e8},${j*1e6},1,`);
    rows.push(`Render Prepare Transforms,test.cpp,1,${origin+3e9+j*1e8},100000,1,`);
  }
  rows.push(`Main Frame,test.cpp,1,${origin+3e9-10000000},20000000,1,`); // crosses start
  rows.push(`Main Frame,test.cpp,1,${origin+14e9-10000000},20000000,1,`); // crosses end
  rows.push(`Main Frame,test.cpp,1,${origin+7e9},-1,1,`); // unfinished
  fs.writeFileSync(path.join(temp,`${id}.csv`),rows.join('\n')+'\n');
  fs.writeFileSync(path.join(temp,`${id}-messages.csv`),`MessageName,total_ns\nLAB_RUN_START,${origin}\n`);
  manifest.runs.push({id,scene:'ecs',mode,repeat:i,status:'complete',zones:`${id}.csv`,messages:`${id}-messages.csv`});
}
fs.writeFileSync(path.join(temp,'runs.json'),JSON.stringify(manifest));
const result=spawnSync(process.execPath,[script,path.join(temp,'runs.json')],{encoding:'utf8'});
assert.equal(result.status,0,result.stderr);
const summary=JSON.parse(fs.readFileSync(path.join(temp,'summary.json'),'utf8'));
for(const run of summary.runs) {
  const frame=run.windows[0].zones['Main Frame'];
  assert.equal(frame.count,50); assert.equal(frame.median_ms,25.5);
  assert.equal(frame.p95_ms,47.55); assert.equal(frame.p99_ms,49.51); assert.equal(frame.max_ms,50);
  assert.equal(run.exclusions.invalidOrIncomplete,1); assert.equal(run.exclusions.boundaryCrossing.steady,2);
}
assert.equal(summary.aggregates.find(x=>x.zone==='Main Frame').runs,3);
const loadingManifest={...manifest,loadAtSeconds:6,loadingWindowRelativeToEventSeconds:[-1,5],runs:manifest.runs.map(run=>({...run,scene:'loading'}))};
for (const run of loadingManifest.runs) {
  const origin=123456789+run.repeat*100000000;
  fs.writeFileSync(path.join(temp,run.messages),`MessageName,total_ns\nLAB_RUN_START,${origin}\nLAB_LOAD_START,${origin+6e9}\nLAB_LOAD_COMPLETE,${origin+7.5e9}\n`);
}
fs.writeFileSync(path.join(temp,'loading-runs.json'),JSON.stringify(loadingManifest));
const loading=spawnSync(process.execPath,[script,path.join(temp,'loading-runs.json')],{encoding:'utf8'});
assert.equal(loading.status,0,loading.stderr);
const loadingSummary=JSON.parse(fs.readFileSync(path.join(temp,'summary.json'),'utf8'));
for(const run of loadingSummary.runs) {
  assert.equal(run.loadingProgress.completeFramesDuringLoad,15);
  assert.equal(run.loadingProgress.wallTimeMs,1500);
}
fs.writeFileSync(path.join(temp,'before-1-messages.csv'),'MessageName,total_ns\n');
const missing=spawnSync(process.execPath,[script,path.join(temp,'runs.json')],{encoding:'utf8'});
assert.notEqual(missing.status,0); assert.match(missing.stderr,/LAB_RUN_START missing/);
if (process.argv[2]) {
  const exporter=process.argv[3] ?? 'C:/tge/tracy-0.13.1/tracy-csvexport.exe';
  for (const [flag,name] of [['-u','before-1.csv'],['-m','before-1-messages.csv']]) {
    const exported=spawnSync(exporter,[flag,path.resolve(process.argv[2])],{maxBuffer:128*1024*1024});
    assert.equal(exported.status,0,exported.stderr?.toString());
    fs.writeFileSync(path.join(temp,name),exported.stdout);
  }
  const real=spawnSync(process.execPath,[script,path.join(temp,'runs.json')],{encoding:'utf8'});
  assert.notEqual(real.status,0); assert.match(real.stderr,/LAB_RUN_START missing/);
  console.log('PASS: real legacy trace without marker is rejected instead of silently misaligning the range.');
}
console.log(`PASS: marker offsets, R-7 quantiles, boundary censoring, incomplete spans, N=3 independent runs, missing-marker rejection. Synthetic files: ${temp}`);
