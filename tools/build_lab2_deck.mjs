// Editable PPTX/PDF defense deck using the provided Artifact Tool runtime.
import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
const repo=path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const root=path.join(repo,'docs/lab2/defense');
const runtime=process.env.TGE_ARTIFACT_MODULE || path.join(root,'.build/node_modules/@oai/artifact-tool/dist/artifact_tool.mjs');
const skill=process.env.TGE_PRESENTATION_SKILL || path.join(process.env.USERPROFILE,'.codex/plugins/cache/openai-primary-runtime/presentations/26.1013.11914/skills/presentations');
const python=process.env.TGE_ARTIFACT_PYTHON || path.join(process.env.USERPROFILE,'.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe');
process.env.RUNTIME_NODE_MODULES ||= path.resolve(path.dirname(runtime),'../../..');
const {Presentation,PresentationFile}=await import(pathToFileURL(runtime).href);
const {finalizePresentation,applyPresentationChartFont}=await import(pathToFileURL(path.join(skill,'container_tools/artifact_tool_utils.mjs')).href);
const content=JSON.parse(await fs.readFile(path.join(root,'content.json'),'utf8'));
const git=JSON.parse(await fs.readFile(path.join(root,'git-snapshot.json'),'utf8'));
const quality=JSON.parse(await fs.readFile(path.join(root,'verification.json'),'utf8'));
const outputStem=process.env.TGE_DECK_NAME || 'TurboGameEngine-Lab2';
if(!/^[A-Za-z0-9_-]+$/.test(outputStem)) throw new Error('TGE_DECK_NAME must be a plain filename stem');
const p=Presentation.create({slideSize:{width:1280,height:720}});
const C={bg:'#101721',fg:'#F2F5FA',muted:'#A9B7CA',accent:'#66DDB6',line:'#2B3A4E',warn:'#F0BB78'};
function text(s,name,value,x,y,w,h,size=28,bold=false,color=C.fg){
 const a=s.shapes.add({geometry:'textbox',name,position:{left:x,top:y,width:w,height:h},fill:'none',line:{fill:'none',width:0}});
 a.text=value;a.text.style={typeface:'Arial',fontSize:size,bold,color};return a;
}
function base(title,label='ЛР 2 / LUAU + REACT'){
 const s=p.slides.add(),index=p.slides.items.length;s.background.fill=C.bg;
 text(s,'eyebrow',label,64,36,1120,30,16,true,C.accent);
 text(s,'title',title,64,99,1150,124,43,true);
 text(s,'footer','TURBOGAMEENGINE   •   16.10.2026',64,672,1000,24,14,false,C.muted);
 text(s,'page',String(index).padStart(2,'0'),1160,670,64,28,16,false,C.muted);return s;
}
async function photo(s,relative,caption){
 const blob=await fs.readFile(path.join(repo,relative));
 s.images.add({blob:new Uint8Array(blob).buffer,contentType:'image/jpeg',alt:caption,fit:'contain',position:{left:64,top:209,width:1152,height:414}});
 text(s,'caption',caption,64,630,1150,30,18,false,C.muted);
}
function table(s,values,top,height,widths){
 const t=s.tables.add({rows:values.length,columns:values[0].length,left:64,top,width:1150,height,columnWidths:widths,values});
 for(let r=0;r<values.length;r++)for(let c=0;c<values[0].length;c++){
  const cell=t.getCell(r,c);cell.fill=r===0?'#24364B':r%2?'#162230':'#1C2939';
  cell.text.style={typeface:'Arial',fontSize:r===0?21:20,bold:r===0,color:r===0?C.accent:C.fg};
 }
 t.borders.assign({fill:C.line,width:1,style:'solid'});return t;
}
const cover=base('Скриптинг как\nподсистема движка');
text(cover,'engine','TurboGameEngine',64,290,1100,78,64,true);
text(cover,'lead','C++ мир. Luau правила. React UI.',64,398,1100,56,32,false,C.accent);
text(cover,'subtitle','Демонстрация механики, живого обновления\nи авторских инструментов',64,489,1080,90,25,false,C.muted);
cover.speakerNotes.text=content.slides[0].notes.join('\n');
for(const d of content.slides){
 const s=base(d.title);
 if(d.id==='mission'){
  await photo(s,'docs/lab2/defense/evidence/game-overview.jpg','Работающий проект в TurboGameEngine: 8 тем, 3 задания и переход в 3D.');
 }else if(d.id==='boundary'){
  text(s,'lead','Скрипт вызывает host API; движок вызывает поведение.',64,226,1140,60,28,false,C.accent);
  table(s,[['Слой','Ответственность','Граница'],['C++ / ECS','Мир, ввод, ресурсы, GPU, физика','Владеет native-объектами'],['Luau / поведение','Движение, сбор, счёт, таймер, победа','Engine / Input; handle с поколением'],['React Luau / UI','Hooks, события и reconciliation','Native host и retained-дерево']],309,302,[240,480,430]);
 }else if(d.id==='prefab'){
  await photo(s,'docs/lab2/defense/evidence/game-real-ecs.jpg','Три настоящих ECS-префаба: spawn → setPosition → getPosition, затем Luau Update.');
 }else if(d.id==='reload'){
  await photo(s,'docs/lab2/defense/evidence/game-reload-complete.jpg','Реальный disk reload A → B: counter=1, Init=1, OnReload=1. Все 3 ключа получены.');
 }else if(d.id==='editor'){
  await photo(s,'tools/rea/local/investigations/roblox-studio-9b554450/panel-comparison/tge-home-final-font.jpg','Native-редактор: общий реестр команд, пять вкладок ленты, панели сцены и Properties.');
 }else{
  text(s,'lead',d.lead,64,239,1130,95,28,false,C.accent);
  d.bullets.forEach((b,j)=>text(s,`point-${j}`,b,82,368+j*83,1090,72,25));
 }
 s.speakerNotes.text=[d.lead,...d.bullets,...d.notes,'ИСТОЧНИКИ:',...d.sources].join('\n');
}
const memory=base('Память: дефект найден и проверен','ЛР 2 / СТАБИЛЬНОСТЬ');
text(memory,'caption','Windows Release / D3D12 / 1280×720 / vsync • Рост Private Bytes от 60 с, MiB',64,226,1140,45,22,false,C.accent);
const dirs=['memory-windows-run1','memory-windows-run2'];
const samples=[];
for(const dir of dirs){
 const csv=(await fs.readFile(path.join(root,dir,'samples.csv'),'utf8')).trim().split(/\r?\n/).slice(1).map(l=>l.split(',').map(Number));
 const values=Array.from({length:9},(_,i)=>{
  const time=60+i*30;const row=csv.reduce((best,row)=>Math.abs(row[0]-time)<Math.abs(best[0]-time)?row:best,csv[0]);
  return Number((row[1]/1048576).toFixed(2));
 });
 samples.push(values.map(v=>Number((v-values[0]).toFixed(2))));
}
const chart=memory.charts.add('line',{
 position:{left:64,top:286,width:740,height:315},title:'Рост памяти после прогрева',titleTextStyle:{fontSize:18,fill:C.fg},
 categories:Array.from({length:9},(_,i)=>String(60+i*30)),
 series:[{name:'До исправления',values:samples[0],line:{fill:C.warn,width:3,style:'solid'}},{name:'После исправления',values:samples[1],line:{fill:C.accent,width:3,style:'solid'}}],
 chartFill:C.bg,plotAreaFill:C.bg,hasLegend:true,legend:{position:'bottom',textStyle:{fontSize:15,fill:C.fg}},
 xAxis:{title:'Время, с',textStyle:{fontSize:13,fill:C.muted},line:{fill:C.line,width:1},majorGridlines:null},
 yAxis:{title:'MiB',textStyle:{fontSize:13,fill:C.muted},majorGridlines:{fill:C.line,width:1,style:'solid'}},
});
applyPresentationChartFont(chart,{fontFamily:'Arial'});
text(memory,'finding','Потерянный destroy\nвызывал повторный Init\nи рост живого мира.',850,297,355,125,27,true);
text(memory,'verification',quality.memoryShort || 'Результат повторных прогонов: verification.json',850,444,355,141,22,false,C.accent);
text(memory,'limit','Счётчики процесса и headless sanitizer tests не доказывают отсутствие всех native/GPU утечек.',64,622,1150,40,18,false,C.muted);
memory.speakerNotes.text=['Сырые данные: docs/lab2/defense/memory-windows-run1/samples.csv и memory-windows-run2/samples.csv.',
 'Изначальный баг: forgetEntity стирал pendingDestroy. Очистка VM instance отменяла будущий ECS destroy; старые компоненты повторяли Init.',
 'CLion Profile app не доступен для выбранного Windows toolchain; screenshot evidence/clion-profiler-unavailable.jpg.',
 'Sanitizers проверяют Linux CPU scripting/React. См. memory-report.md и исходные неуспешные attempts.'].join('\n');
const inventory=base('Весь результат, включая локальные изменения','ЛР 2 / СОСТОЯНИЕ РЕПОЗИТОРИЯ');
const rows=[['Область','Что входит в результат'],['Runtime / игра','VM, bindings, события, jobs reload, prefab, поля, Collectathon'],['React / визуальный host','Upstream React, Instance facade, layout, images, TweenService, CanvasGroup'],['Авторские инструменты','Script Workspace, StyLua, Output/Command/State/Errors, optional Codex'],['Проекты / редактор','Launcher, .tgeproj, Play, scene deltas, Undo/Redo, команды и Studio-style UI'],['Надёжность / основа','AtomicFile, guards, culling, GPU lifetime, input, crash diagnostics, тесты, REA']];
table(inventory,rows,242,340,[275,875]);
text(inventory,'git',`${git.branch.name} • ${git.counts.staged_paths} staged • ahead=${git.branch.ahead} (локальные refs) • изменения ещё не закоммичены`,64,592,1145,35,21,false,C.accent);
text(inventory,'scope','Подсистемы и каждый файл: implementation-evidence.json / git-snapshot.json. Без fetch, commit и push.',64,636,1145,26,17,false,C.muted);
inventory.speakerNotes.text='Полный Git snapshot: docs/lab2/defense/git-snapshot.json. Ignored local research/fonts/settings выделены отдельно. Committed foundation:76c2386,c8e3967,9782dea; не приписывается целиком новой ЛР2.';
const demo=base('Три минуты живой демонстрации','ЛР 2 / СЦЕНАРИЙ ЗАЩИТЫ');
content.appendix.demo.forEach((d,i)=>text(demo,`step-${i}`,d,64,245+i*73,1140,62,24));
demo.speakerNotes.text='Основной показ: demos/lab2-collectathon. ДоPlay: изменить поля и Save Scene; послеPlay: изменить формулу начисления очков в Luau. Подробный сценарий defense-notes.md.';
const limits=base('Границы реализации','ЛР 2 / ЧЕСТНЫЕ ОГРАНИЧЕНИЯ');
content.appendix.limits.forEach((d,i)=>text(limits,`limit-${i}`,d,64,247+i*89,1130,79,26));
limits.speakerNotes.text='Подробная матрица: docs/lab2/defense/implementation-evidence.json. Нет full VMdebugger,savegame,fullRobloxruntime,object-reference picker. Check - compiler,StepTick - simulation step.';
const build=path.join(root,'.build/final-deck');await fs.mkdir(path.join(build,'renders'),{recursive:true});
for(const [i,s]of p.slides.items.entries()){
 const blob=await p.export({slide:s,format:'png',scale:1});
 await fs.writeFile(path.join(build,'renders',`slide-${String(i+1).padStart(2,'0')}.png`),new Uint8Array(await blob.arrayBuffer()));
}
const candidate=path.join(build,'candidate.pptx');await(await PresentationFile.exportPptx(p)).save(candidate);
// The bundled native PDF exporter has an unresolved #standard-fonts alias on
// this Windows runtime. The editable PPTX is finalized here; ReportLab creates
// the companion slide PDF from the inspected PNGs without patching the runtime.
const result=await finalizePresentation({workspaceDir:root,candidatePath:candidate,finalPath:path.join(root,`output/${outputStem}.pptx`),
 pythonExecutable:python,integrityValidatorPath:path.join(skill,'container_tools/inspect_presentation_package_integrity.py'),
 layoutValidatorPath:path.join(skill,'container_tools/inspect_presentation_layout_geometry.py'),
 layoutArgs:['--expected-slide-size-emu','12192000,6858000','--validate-bullet-geometry','--validate-heading-fit'],
 fontPolicy:{basis:'design',families:['Arial']},verifyArtifactToolImport:true,
 materializeLiteralChartWorkbooks:true,
 receiptPath:path.join(build,outputStem+'.validation.json')});
console.log(JSON.stringify({slides:p.slides.items.length,result}));
