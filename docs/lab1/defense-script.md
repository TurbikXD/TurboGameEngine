# ЛР 1: сценарий защиты на три минуты

Для допфичи выделить 30–45 секунд: [GPU upload showcase](gpu-upload-queue.md) (`Launch-Lab1Demo -GpuUpload`). Показать Upload=transfer queue, fence/bytes; Tracy workers Transfer Record против прежнего main Upload Texture GPU; в коде EnqueueSignal после copy и Loaded только после completed fence. Отдельный hardware readback-тест проверяет реальный GPU результат.

До выступления: собрать `profile-release`, выполнить тесты, `New-BenchmarkAssets.ps1`, три прогона каждого режима и stress. Открыть `measurements.md` из текущего каталога результатов и соответствующие Tracy-трейсы. Версия GUI — 0.13.1, как в клиенте. Команды и параметры: [инструкция запуска](../../tools/lab1/README.md).

| Время | Показать | Короткое объяснение |
|---|---|---|
| 0:00–0:20 | Схема и `Application::init/shutdown` | «Один enkiTS scheduler принадлежит Application. На него перенесены loader и расчёт render-матриц; бывший asset-пул удалён». |
| 0:20–0:55 | Живая сцена `loading`, событие на шестой секунде | «12 PNG и четыре OBJ читаются штатными декодерами. До готовности — placeholder. В UI видны CPU/Uploads/Ready/Failed, кадры продолжаются». |
| 0:55–1:25 | Tracy: `LAB_LOAD_START`, Main Frame, workers, pump | «Синхронный режим — ablation. Исходный loader уже имел отдельный пул. Здесь сравниваем стоимость IO на main и наш L1; отдельно снят неограниченный pump». Показать фактический худший кадр и p95/p99 из отчёта. |
| 1:25–1:55 | Сцена `ecs`, 4096 объектов; `Render Prepare Transforms` | «Gather на main, затем const World и непересекающиеся выходные диапазоны; join; GPU submit на main». В Compare показать одну целевую зону и её измеренное изменение. |
| 1:55–2:20 | Stress-результаты + короткий выход с загрузкой | «Непоставленные запросы отменяются, активные CPU-задачи завершаются до уничтожения renderer». Показать `LAB_SHUTDOWN pending_cpu/pending_uploads`, нормальный exit code. |
| 2:20–3:00 | Таблица трёх прогонов, затем код при необходимости | «Release, одна сцена/камера, прогрев 3 с, одинаковые диапазоны Tracy. Отдельно median/p95/p99 кадра и время целевой зоны. Ограничение pump действует между upload; один драйверный вызов не прерывается». |

## Подготовка живого окна

В отдельном PowerShell, работающем из каталога приложения:

```powershell
Set-Location C:\tge\profile\Release
$env:TGE_LAB_SCENE = 'loading'
$env:TGE_BENCHMARK_ASSET_DIR = 'C:\tge\lab1-assets'
$env:TGE_JOBS = '1'
$env:TGE_ASYNC_LOADING = '1'
$env:TGE_WAIT_FOR_TRACY = '0'
$env:TGE_LOAD_AT_SECONDS = '6'
$env:TGE_DEMO_SECONDS = '18'
$env:TGE_UPLOADS_PER_FRAME = '1'
$env:TGE_UPLOAD_BUDGET_MS = '2'
.\app.exe
```

Для второй сцены изменить только `$env:TGE_LAB_SCENE = 'ecs'`; задать `$env:TGE_ECS_ENTITIES = '4096'`. Для выхода с живой загрузкой — `loading`, `TGE_LOAD_AT_SECONDS=1`, `TGE_DEMO_SECONDS=1.01`. Наличие живых задач проверяется журналом, а не предположением о длительности.

Для отсутствующего ресурса задать `TGE_MISSING_ASSET=1`; ошибка видна в `Failed` и логе, кадр продолжает рисоваться. Перед сравнительными прогонами убрать этот флаг. Все эти переменные принадлежат только текущему процессу PowerShell.

## Работа с Tracy

Открыть нужный `trace.tracy`; в Messages найти `LAB_RUN_START` и `LAB_LOAD_START`. Основной range: `LAB_RUN_START +3…+14 с`; дополнительные загрузочные метрики: `LAB_LOAD_START −1…+5 с`. В Find zone и Statistics использовать тот же Limit range. По диапазону искать `Main Frame`, `Asset Synchronous Load`, `Load Texture CPU`, `Parse Mesh Data`, `Asset Main Thread Tasks`, `Render Prepare Transforms`.

В Compare открыть второй trace. Каждый файл имеет своё начало записи: диапазон выравнивать относительно его маркера, а не механически копировать абсолютные timestamp. Не складывать времена параллельных worker-зон и не называть эту сумму длительностью кадра.

Цифры произносить только из полученного отчёта. Если целевая зона ускорилась, а общий кадр нет, так и сказать: следующая граница — оставшийся serial/GPU submit. Если захват не содержит нужного диапазона или завершённой загрузки, такой прогон не использовать.
