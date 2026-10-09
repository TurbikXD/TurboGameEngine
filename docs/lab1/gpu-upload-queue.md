# Допфича: GPU upload queue через Diligent Engine

Отдельная **transfer/copy GPU-очередь** для дисковых текстур и мешей. Это не переименование старого main-thread pump и не дополнительный CPU streaming-пул. Используется общая job system enkiTS.

[Фактические проверки, цифры и 6 Tracy-трейсов](gpu-upload-results.md): main finalize 5,228 → 0,549 мс; загрузка всего набора стала медленнее — этот компромисс также зафиксирован.

## Путь ресурса

```text
main: запрос → Loading, placeholder
job: file/decode → CPU mip chain → GPU destinations → transfer copy commands
transfer GPU: copies → COMMON (D3D12) → signal fence N
main: неблокирующий completed >= N → GPU-side acquire → publish handles → Loaded
graphics GPU: штатный переход в vertex/index/shader-read → draw
```

В обычном кадре главный поток **не копирует данные и не вызывает CPU Wait**. Он опрашивает настоящий GPU fence, ставит GPU-side synchronization и публикует готовые handles. Публикация намеренно оставлена на main: workers не меняют поля Mesh/Texture одновременно с чтением renderer/editor. `Loaded` устанавливается только после GPU completion, не после CPU decode или Flush. Незавершённый fence не блокирует другие готовые завершения в pump.

## Файлы и ключевые функции

| Файл | Что смотреть |
|---|---|
| [UploadQueue.h](../../engine/rhi/UploadQueue.h) | Контракты IUploadQueue, IUploadTicket, UploadRequest, UploadSubmission. CPU spans можно освободить после submit. |
| [Device.h](../../engine/rhi/Device.h) | uploadQueue(): очередь или nullptr для fallback. |
| [DiligentDevice.cpp](../../engine/rhi_diligent/DiligentDevice.cpp) | configureUploadContexts: adapter + настоящая transfer queue. DiligentUploadQueue::submit: destinations, UpdateBuffer/UpdateTexture, barriers, EnqueueSignal/Flush/FinishFrame. DiligentUploadTicket: readiness, acquire, lifetime. |
| [image_mips.cpp](../../engine/resources/image_mips.cpp) | buildRgba8MipTail: mip chain RGBA8 UNORM в job, включая нечётные размеры и alpha. Transfer context не умеет GenerateMips. |
| [async_load_queue.cpp](../../engine/resources/async_load_queue.cpp) | submitPrepared: worker возвращает finalize + ticket. pump: completion polling. stop: join producers → wait GPU → cancel. |
| [Renderer.cpp](../../engine/renderer/Renderer.cpp) | requestMeshLoadFromDisk / requestTextureLoadFromDisk: transfer-ветка и main publish; старый путь — fallback. |
| [LabOptions.h](../../engine/core/LabOptions.h) | TGE_GPU_UPLOAD=0/1, по умолчанию включено; setGpuUploadEnabled вызывается до создания device. |
| [GameplayState.cpp](../../engine/game/GameplayState.cpp) | Lab overlay: фактический Upload mode, GPU pending, fence, transfer count и MiB. |
| [AsyncLoadQueueTests.cpp](../../tests/AsyncLoadQueueTests.cpp) | Fake fence: ready/acquire/shutdown, отсутствие head-of-line blocking; CPU mips. |
| [GpuUploadTests.cpp](../../tests/GpuUploadTests.cpp) | 16 concurrent submissions, 32 buffers, все texture mips: GPU→CPU readback и проверка каждого байта, уникальные fence values. |

## Почему это корректно

- Отдельный immediate context создаётся на transfer queue. Diligent context не потокобезопасен: mutex сериализует его запись из jobs. Graphics context workers не используют.
- Destinations имеют ImmediateContextMask с graphics и transfer, в том числе для Vulkan sharing.
- D3D12: transfer переводит фактическое отслеживаемое состояние → COMMON перед передачей graphics. OldState=UNKNOWN означает «взять состояние из Diligent», а не отключить tracking. UpdateBuffer оставляет COPY_DEST, но UpdateTexture временно переводит отдельный mip и восстанавливает предыдущее состояние; поэтому жёстко задавать COPY_DEST нельзя. Штатные graphics bind/draw переводят ресурс в read-состояние.
- Vulkan: CPU observation fence недостаточно для межочередной memory dependency. acquire вызывает DeviceWaitForFence на graphics; это GPU-side wait. Graphics barrier обеспечивает последующий shader/vertex/index read.
- Fence — Diligent FENCE_TYPE_GENERAL, монотонное значение под mutex, signal после copies, затем Flush. Старый frame bookkeeping IFence здесь не используется: он не является GPU fence.
- Worker пишет приватный payload, completion публикуется release/acquire atomic. Shared Mesh/Texture изменяются только на main.
- Ticket удерживает native destinations до GPU completion; Diligent удерживает staging. FinishFrame вызывается для дополнительного context после batch: Present обслуживает основной context.
- Backpressure: одновременно активных CPU/decoded/GPU-completion записей clamp(workers*2, 1, 16). Это лимит числа ресурсов, не строгий byte budget: очень большой одиночный ресурс всё ещё возможен.
- acquire идемпотентен: повторный вызов не потребляет semaphore второй раз. Acquire до completion запрещён.

## Ошибки и shutdown

stop запрещает новые запросы, отменяет неотправленный backlog, join-ит отправленные CPU jobs, ждёт tickets перед освобождением payload, сообщает cancellation. Renderer затем drain-ит transfer context и уничтожает GPU resources/device. JobSystem живёт дольше.

Ошибка decode/GPU allocation переводит asset в Failed на main. Частично записанный batch drain-ится в catch до освобождения destinations. CPU wait допустим в shutdown/error path, не в обычном кадре. stb/Assimp посреди вызова не прерываются.

## Поддержка и границы

Diligent D3D12/Vulkan требуют adapter с отдельной transfer queue. D3D11/OpenGL/WebGPU и adapters без неё используют bounded main pump. Лог явно показывает GPU_UPLOAD_QUEUE enabled и context IDs либо disabled/fallback. UI показывает фактический путь, не только requested flag.

Обычный editor тоже использует очередь для async disk mesh/texture. Procedural/placeholder при init, shaders/PSO и прямые createGpuBuffer/createGpuImage остаются на прежнем пути. Нет texture streaming по mip, отдельного L2 pool, fibers или parallel graphics submission.

CPU mip filtering — box filter RGBA8 UNORM: 2×2 с округлением для power-of-two, целые покрытые области для NPOT. Побитовое совпадение с прежним GPU GenerateMips для NPOT не обещается. sRGB в текущем texture path нет. A/B сравнивает всю upload pipeline, включая перенос mip generation на CPU, не изолированную стоимость одного GPU copy.

## Повторить и показать

```powershell
cmake --build --preset profile-release --target app engine_gpu_upload_tests
ctest --test-dir C:/tge/profile -C Release --output-on-failure
& C:/tge/profile/Release/engine_gpu_upload_tests.exe d3d12
# Если Vulkan доступен:
& C:/tge/profile/Release/engine_gpu_upload_tests.exe vk

# Регрессии: обязательна проверка Debug (assertions), не только Release.
cmake --build --preset clion-debug --target app engine_gpu_upload_tests engine_texture_loader_tests
ctest --test-dir C:/tge/clion-vs2022b-dbg -C Debug --output-on-failure
& C:/tge/clion-vs2022b-dbg/Debug/engine_gpu_upload_tests.exe d3d12

# CPU decode async в обоих режимах; отличается upload pipeline:
.\tools\lab1\Launch-Lab1Demo.ps1 -Scene loading -Mode before -GpuUpload -Seconds 30
.\tools\lab1\Launch-Lab1Demo.ps1 -Scene loading -Mode after -GpuUpload -Seconds 30 -Tracy
.\tools\lab1\Invoke-Lab1Measurements.ps1 -Scene loading -CompareGpuUpload -Runs 3
.\tools\lab1\Invoke-Lab1Stress.ps1 -GpuUpload
```

На шестой секунде запрашиваются 12 PNG 2048² и четыре OBJ. Смотреть смену placeholder, Upload mode, transfer count/bytes/fence. GPU pending часто ноль: GPU успевает завершить copy между двумя опросами. Это не признак отсутствия очереди.

Обычные arena_floor/arena_wall/sandstone.ppm используют ASCII P3: loaders.cpp декодирует его самостоятельно, stb_image остаётся для PNG и бинарного PPM P6. TextureLoaderTests проверяет реальные P3-текстуры в jobs, comments/CRLF, масштабирование до RGBA8, повреждённые данные и границы размеров. Hardware-тест считает Diligent ERROR/assertion ошибкой теста, проверяет COMMON перед graphics-потреблением и побитовое совпадение readback всех mip-уровней.

В Tracy на workers: Load Texture Transfer Job, Upload Build CPU Mips, Upload Transfer Submit/Record. На main: Upload Graphics Acquire, Upload Texture/Mesh Publish. До правки main содержит Upload Texture/Mesh GPU, после — только публикацию. Сравнивать Asset GPU Finalize, Asset Main Thread Tasks, Main Frame, median/p95/p99/max и полное время загрузки с одинаковым Limit range. Transfer Record — CPU время записи, не длительность GPU copy; GPU timestamp-зоны не добавлены.

Исторические L1-трейсы не перезаписываются. Новый runner -CompareGpuUpload требует enabled transfer path и >=16 сообщений via GPU transfer, иначе серия не принимается.
