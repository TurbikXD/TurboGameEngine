# GPU upload queue: проверка и замеры 08.10.2026

Реализация и команды защиты: [gpu-upload-queue.md](gpu-upload-queue.md).

## Что проверено

- Release + Tracy 0.13.1: приложение и аппаратный readback-тест собраны.
- CTest: 4/4. Дополнены async loading tests: GPU fence readiness, graphics acquire только на owner, отсутствие CPU wait в pump, нет head-of-line blocking, shutdown, synchronous prepared upload, backpressure незавершённых GPU uploads и CPU mips.
- Hardware readback **D3D12 и Vulkan**: 16 submissions из четырёх jobs, 32 vertex/index buffers и все mip-уровни 16 NPOT-текстур. CPU source освобождён до readback; каждый GPU байт проверен, fence values уникальны 1..16. [D3D12 лог](../../reports/lab1/gpu-upload-validation-20261008/d3d12.log), [Vulkan лог](../../reports/lab1/gpu-upload-validation-20261008/vulkan.log). Vulkan validation layer не установлен: проверка этим слоем **не заявляется**.
- Stress 6/6 с включённой transfer queue: missing resources, выход с 16 pending CPU requests, 30-секундная сессия, три повторных старта. Все процессы завершились штатно. [Результаты и логи](../../reports/lab1/gpu-upload-stress-20261008/stress-results.json).
- Проверки анализатора: marker offsets, квантили R-7, censoring, N=3, missing markers и обозначение отдельного GPU A/B.

Это проверки конкретных сценариев, не формальное доказательство отсутствия всех гонок; ThreadSanitizer не запускался.

## Регрессионные исправления 09.10.2026

- Исправлен D3D12 assertion UNDEFINED != COPY_DEST: release-барьеры используют OldState=UNKNOWN, NewState=COMMON, UPDATE_STATE. UpdateTexture восстанавливает прежнее состояние отдельных mip-уровней, поэтому COPY_DEST нельзя предполагать. Debug-проверки движка не отключены.
- Добавлен ASCII PPM P3 decoder: реальные arena_floor/arena_wall/sandstone, comments/CRLF, maxval до 65535 с переводом в RGBA8. Проверяются размеры, переполнение, количество и диапазон samples; невалидные данные не публикуются. PNG/P6 остаются на stb_image.
- CTest **5/5 в Debug и Release + Tracy**. Новый TextureLoaderTests проверяет P3/P6/PNG, 17 повреждённых P3, отсутствующий файл и 30 jobs с настоящими текстурами сцены. [Debug](../../reports/lab1/upload-fix-20261009/ctest-debug.log), [Release](../../reports/lab1/upload-fix-20261009/ctest-release.log).
- Hardware readback повторён на **D3D12 и Vulkan, в Debug и Release**: в каждом запуске 16 batches, 32 buffers, все mip-уровни, unique fences=16, Diligent validation_errors=0. Для D3D12 дополнительно проверено tracked COMMON до graphics copy. Ошибки и assertions теперь завершают тест с ненулевым exit code, а не позволяют вывести PASS. [D3D12 Debug](../../reports/lab1/upload-fix-20261009/gpu-d3d12-debug.log), [Vulkan Debug](../../reports/lab1/upload-fix-20261009/gpu-vk-debug.log), [D3D12 Release](../../reports/lab1/upload-fix-20261009/gpu-d3d12-release.log), [Vulkan Release](../../reports/lab1/upload-fix-20261009/gpu-vk-release.log). Khronos Vulkan validation layer по-прежнему отсутствует; проверка этим слоем не заявляется.
- Обычный **app Debug**, без lab scene: exit=0, workers=4, parallel_ecs=true, async_loading=true, graphics_context=0/transfer_context=1. arena_floor.ppm и arena_wall.ppm загружены через GPU transfer как 8×8; DGLogo.png — 512×512. Assertions, ошибок декодирования и Diligent ERROR нет. [Лог приложения](../../reports/lab1/upload-fix-20261009/app-debug.log), [Diligent stderr](../../reports/lab1/upload-fix-20261009/app-debug-stderr.log). Layout редактора после smoke-теста не изменился.

Числа A/B ниже относятся к бинарнику от 08.10.2026: старые трейсы и результаты не перезаписаны. Это проверка корректности исправлений, не новая серия измерений производительности.

## Условия сравнения

Новая серия, **6 .tracy файлов**, N=3 на режим, порядок AB/BA/AB. Оба режима — общая job system (4 workers), async decode, одинаковые placeholders и лимит публикации 1 ресурс/кадр / 2 мс. Before: TGE_GPU_UPLOAD=0, GPU upload/GenerateMips на main. After: TGE_GPU_UPLOAD=1, CPU mip chain в jobs и отдельная transfer queue с fence. Меняется upload pipeline целиком, не только GPU queue.

12 PNG 2048² + четыре OBJ по 131072 треугольника, одна сцена/камера/1280×720, vsync off. Захват 15 с, прогрев 3 с, событие на +6 с; одинаковое окно загрузки LAB_LOAD_START −1…+5 с. Начало/завершение и worker IDs подтверждены в trace/log, отдельная transfer queue и >=16 фактических uploads подтверждены runner. Все app/capture exit codes 0.

Threadripper 3960X (24C/48T), RTX 5070, NVIDIA 32.0.16.1742, Windows 11 Pro build 26200, RAM 47,88 GiB. Это рабочий ПК, фоновые приложения не закрывались; три прогона — описательные результаты, не статистическое доказательство универсального выигрыша. Hidden-mode CPU Main Frame не является display latency/FPS.

Измеренный бинарник: SHA-256 AA602C8EB0743294FA0ECCE24C39FA788669703F82D10015EC3EB49EA5C2733E. Старые замеры ЛР 1 не изменены; использовать их числа для этой допфичи нельзя.

## Измеримый эффект

Все значения ниже — **медиана соответствующей метрики трёх независимых запусков**. Frame samples не выдаются за независимые испытания. Для CPU finalize по 16 вызовов на каждый запуск.

| Метрика, окно загрузки | Before | After |
|---|---:|---:|
| Asset GPU Finalize, mean per call | 5,228 мс | 0,549 мс |
| Asset GPU Finalize, median per call | 4,488 мс | 0,486 мс |
| Asset Main Thread Tasks, max одного прогона¹ | 13,877 мс | 0,919 мс |
| Main Frame, median | 0,407 мс | 0,384 мс |
| Main Frame, p95 | 0,699 мс | 0,545 мс |
| Main Frame, p99 | 2,974 мс | 0,954 мс |
| Main Frame, max одного прогона¹ | 15,969 мс | 14,160 мс |
| Вся загрузка: LAB_LOAD_START → COMPLETE | 720,783 мс | 1053,091 мс |

¹ Это медиана трёх максимумов, не абсолютный максимум серии. В after-03 был Asset GPU Finalize **9,504 мс**; абсолютный max Main Frame всех трёх after — **15,801 мс**. Нулевые stalls/фризы не заявляются. Полные метрики каждого прогона сохранены.

Средняя main-thread финализация уменьшилась на **89,5%**, p99 CPU кадра в загрузочном окне — на **67,9%**. Практическая польза: копирование и создание ресурсов не конкурируют с кадром в main upload path.

**Компромисс:** полная загрузка медленнее на **46,1%** (0,721 → 1,053 с). CPU mip generation заняла в среднем около 59,8 мс/текстуру (медиана средних по прогонам); это дополнительная работа CPU. Также присутствуют serialization transfer context, staging и backpressure. Не утверждается, что mip generation — единственная причина регрессии. Это улучшение отзывчивости/хвостов кадра, не улучшение всех метрик.

`Upload Transfer Record` — около 3,70 мс CPU записи/batch; `Upload Graphics Acquire` — около 0,000375 мс. Эти значения не являются GPU copy duration. GPU timestamps не добавлялись.

## Исходные данные и просмотр

[Полный отчёт](../../reports/lab1/gpu-upload-20261008/measurements.md), [manifest и SHA](../../reports/lab1/gpu-upload-20261008/runs.json), [точные диапазоны/метрики](../../reports/lab1/gpu-upload-20261008/summary.json).

Для первого A/B открыть [before-01](../../reports/lab1/gpu-upload-20261008/loading-before-01/trace.tracy) и Compare → [after-01](../../reports/lab1/gpu-upload-20261008/loading-after-01/trace.tracy). Limit range каждого trace брать из summary.json: абсолютные timestamps процессов различаются. Ищите Asset GPU Finalize на main, Upload Transfer Record на workers и Upload Texture/Mesh Publish.
