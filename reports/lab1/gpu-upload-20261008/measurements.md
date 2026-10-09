# ЛР 1 — воспроизводимые замеры Tracy

Сборка: Release; Tracy 0.13.1; SHA-256 app: `AA602C8EB0743294FA0ECCE24C39FA788669703F82D10015EC3EB49EA5C2733E`.
CPU: AMD Ryzen Threadripper 3960X 24-Core Processor; физических ядер: 24; логических: 48; RAM: 47.9 GiB.
ОС: Майкрософт Windows 11 Pro, 10.0.26200, build 26200; обнаруженные GPU: Parsec Virtual Display Adapter (driver 0.45.0.0); NVIDIA GeForce RTX 5070 (driver 32.0.16.1742). Активный адаптер проверяется по журналу рендера.
Фактические workers из LAB_RUN_START: 4. Настройки и контекст сборки сохранены в runs.json.
На режим выполнено 3 независимых запусков приложения. Порядок A/B чередуется между повторениями.
В исходном движке уже существовал отдельный пул асинхронной загрузки на 4 потока. Он мигрирован на общую job system. Режим loading/before (sync) — контролируемое отключение async для демонстрации L1, а не утверждение, что исходный движок всегда загружал синхронно или создавал неограниченное число потоков.
GPU upload A/B: оба режима используют общий scheduler и async CPU decode. before: GPU upload/GenerateMips в main pump. after: CPU mip generation в jobs + отдельная transfer-очередь Diligent + настоящий GPU fence; main только GPU-side acquire и публикация. В обоих режимах лимит публикаций 1 ресурс/кадр, бюджет 2 мс. Это эффект всей upload pipeline, а не изолированный тест одного GPU copy.
Прогрев: первые 3 с после LAB_RUN_START исключены. Основной диапазон: +3…+14 с; загрузка запускается на +6 с.
Для загрузки дополнительно показано окно LAB_LOAD_START −1…+5 с. Времена отсчитываются по сообщениям Tracy, а не времени запуска процесса или подключения.
Квантили вычислены линейной интерполяцией R-7 отдельно для каждого прогона. В таблице — медиана соответствующей метрики между прогонами.
Число кадров не подменяет число независимых экспериментов. Статистическая значимость не заявляется. Удаляются только незавершённые/невалидные зоны и зоны, пересекающие границу интервала.

| Сцена / окно | Зона | Режим | N | Count¹ | Mean, мс | Median, мс | p95, мс | p99, мс | Max, мс |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| loading / steady | Render Gather | before | 3 | 21690 | 0.003 | 0.003 | 0.004 | 0.008 | 3.714 |
| loading / steady | Upload Texture GPU | before | 3 | 12 | 5.092 | 4.489 | 8.532 | 10.910 | 11.505 |
| loading / steady | Asset Dispatch | before | 3 | 16 | 0.006 | 0.003 | 0.015 | 0.019 | 0.020 |
| loading / steady | Job Render Transforms | before | 3 | 21690 | 0.009 | 0.009 | 0.011 | 0.013 | 0.188 |
| loading / steady | Parse Mesh Data | before | 3 | 4 | 366.647 | 363.056 | 384.633 | 387.338 | 388.014 |
| loading / steady | Render Prepare Transforms | before | 3 | 21690 | 0.009 | 0.009 | 0.011 | 0.013 | 0.188 |
| loading / steady | Present | before | 3 | 21691 | 0.320 | 0.246 | 0.394 | 2.778 | 4.972 |
| loading / steady | Load Texture CPU | before | 3 | 12 | 109.944 | 109.549 | 115.199 | 116.220 | 116.475 |
| loading / steady | Decode Texture | before | 3 | 12 | 109.932 | 109.542 | 115.171 | 116.206 | 116.465 |
| loading / steady | Render Submit | before | 3 | 21690 | 0.028 | 0.026 | 0.034 | 0.054 | 0.260 |
| loading / steady | Asset GPU Finalize | before | 3 | 16 | 5.228 | 4.488 | 12.096 | 13.510 | 13.864 |
| loading / steady | Main Frame | before | 3 | 21690 | 0.507 | 0.407 | 0.711 | 2.982 | 15.969 |
| loading / steady | Load Mesh CPU | before | 3 | 4 | 366.653 | 363.061 | 384.636 | 387.341 | 388.017 |
| loading / steady | Upload Mesh GPU | before | 3 | 4 | 6.192 | 4.050 | 12.469 | 13.583 | 13.862 |
| loading / steady | Asset Main Thread Tasks | before | 3 | 21690 | 0.004 | 0.001 | 0.001 | 0.002 | 13.877 |
| loading / steady | Asset Decode Job | before | 3 | 16 | 174.126 | 111.117 | 371.115 | 384.639 | 388.020 |
| loading / loading_event | Render Gather | before | 3 | 11748 | 0.003 | 0.003 | 0.004 | 0.009 | 3.714 |
| loading / loading_event | Upload Texture GPU | before | 3 | 12 | 5.092 | 4.489 | 8.532 | 10.910 | 11.505 |
| loading / loading_event | Asset Dispatch | before | 3 | 16 | 0.006 | 0.003 | 0.015 | 0.019 | 0.020 |
| loading / loading_event | Job Render Transforms | before | 3 | 11748 | 0.009 | 0.009 | 0.011 | 0.013 | 0.142 |
| loading / loading_event | Parse Mesh Data | before | 3 | 4 | 366.647 | 363.056 | 384.633 | 387.338 | 388.014 |
| loading / loading_event | Render Prepare Transforms | before | 3 | 11748 | 0.009 | 0.009 | 0.011 | 0.014 | 0.143 |
| loading / loading_event | Present | before | 3 | 11747 | 0.319 | 0.246 | 0.387 | 2.748 | 4.972 |
| loading / loading_event | Load Texture CPU | before | 3 | 12 | 109.944 | 109.549 | 115.199 | 116.220 | 116.475 |
| loading / loading_event | Decode Texture | before | 3 | 12 | 109.932 | 109.542 | 115.171 | 116.206 | 116.465 |
| loading / loading_event | Render Submit | before | 3 | 11748 | 0.028 | 0.027 | 0.034 | 0.054 | 0.260 |
| loading / loading_event | Asset GPU Finalize | before | 3 | 16 | 5.228 | 4.488 | 12.096 | 13.510 | 13.864 |
| loading / loading_event | Main Frame | before | 3 | 11747 | 0.510 | 0.407 | 0.699 | 2.974 | 15.969 |
| loading / loading_event | Load Mesh CPU | before | 3 | 4 | 366.653 | 363.061 | 384.636 | 387.341 | 388.017 |
| loading / loading_event | Upload Mesh GPU | before | 3 | 4 | 6.192 | 4.050 | 12.469 | 13.583 | 13.862 |
| loading / loading_event | Asset Main Thread Tasks | before | 3 | 11748 | 0.007 | 0.001 | 0.001 | 0.002 | 13.877 |
| loading / loading_event | Asset Decode Job | before | 3 | 16 | 174.126 | 111.117 | 371.115 | 384.639 | 388.020 |
| loading / steady | Render Gather | after | 3 | 25862 | 0.003 | 0.003 | 0.004 | 0.008 | 4.014 |
| loading / steady | Upload Mesh Publish | after | 3 | 4 | 0.486 | 0.472 | 0.738 | 0.781 | 0.792 |
| loading / steady | Asset GPU Finalize | after | 3 | 16 | 0.549 | 0.486 | 0.823 | 0.891 | 0.908 |
| loading / steady | Asset Dispatch | after | 3 | 16 | 0.006 | 0.003 | 0.021 | 0.022 | 0.023 |
| loading / steady | Asset Decode Job | after | 3 | 16 | 249.786 | 185.305 | 484.486 | 486.939 | 487.552 |
| loading / steady | Upload Texture Publish | after | 3 | 12 | 0.518 | 0.489 | 0.816 | 0.889 | 0.907 |
| loading / steady | Upload Build CPU Mips | after | 3 | 12 | 59.776 | 60.000 | 70.008 | 70.106 | 70.130 |
| loading / steady | Load Texture Transfer Job | after | 3 | 12 | 179.438 | 182.622 | 195.185 | 199.002 | 199.956 |
| loading / steady | Render Prepare Transforms | after | 3 | 25862 | 0.010 | 0.009 | 0.012 | 0.016 | 0.162 |
| loading / steady | Job Render Transforms | after | 3 | 25862 | 0.010 | 0.009 | 0.012 | 0.016 | 0.162 |
| loading / steady | Decode Texture | after | 3 | 12 | 115.597 | 116.727 | 123.600 | 125.194 | 125.593 |
| loading / steady | Present | after | 3 | 25861 | 0.235 | 0.204 | 0.308 | 0.594 | 3.393 |
| loading / steady | Main Frame | after | 3 | 25861 | 0.425 | 0.382 | 0.558 | 0.925 | 15.801 |
| loading / steady | Upload Graphics Acquire | after | 3 | 16 | 0.000 | 0.000 | 0.001 | 0.001 | 0.001 |
| loading / steady | Parse Mesh Data | after | 3 | 4 | 460.269 | 471.727 | 485.110 | 485.601 | 485.724 |
| loading / steady | Render Submit | after | 3 | 25862 | 0.030 | 0.028 | 0.038 | 0.062 | 1.290 |
| loading / steady | Asset Main Thread Tasks | after | 3 | 25862 | 0.001 | 0.001 | 0.001 | 0.002 | 0.919 |
| loading / steady | Load Mesh Transfer Job | after | 3 | 4 | 462.326 | 473.664 | 486.937 | 487.427 | 487.550 |
| loading / steady | Upload Transfer Submit | after | 3 | 16 | 4.205 | 3.757 | 8.758 | 9.778 | 10.033 |
| loading / steady | Upload Transfer Record | after | 3 | 16 | 3.702 | 3.676 | 5.791 | 8.488 | 8.941 |
| loading / loading_event | Render Gather | after | 3 | 14187 | 0.003 | 0.003 | 0.004 | 0.008 | 4.014 |
| loading / loading_event | Upload Mesh Publish | after | 3 | 4 | 0.486 | 0.472 | 0.738 | 0.781 | 0.792 |
| loading / loading_event | Asset GPU Finalize | after | 3 | 16 | 0.549 | 0.486 | 0.823 | 0.891 | 0.908 |
| loading / loading_event | Asset Dispatch | after | 3 | 16 | 0.006 | 0.003 | 0.021 | 0.022 | 0.023 |
| loading / loading_event | Asset Decode Job | after | 3 | 16 | 249.786 | 185.305 | 484.486 | 486.939 | 487.552 |
| loading / loading_event | Upload Texture Publish | after | 3 | 12 | 0.518 | 0.489 | 0.816 | 0.889 | 0.907 |
| loading / loading_event | Upload Build CPU Mips | after | 3 | 12 | 59.776 | 60.000 | 70.008 | 70.106 | 70.130 |
| loading / loading_event | Load Texture Transfer Job | after | 3 | 12 | 179.438 | 182.622 | 195.185 | 199.002 | 199.956 |
| loading / loading_event | Render Prepare Transforms | after | 3 | 14187 | 0.010 | 0.009 | 0.013 | 0.016 | 0.107 |
| loading / loading_event | Job Render Transforms | after | 3 | 14187 | 0.010 | 0.009 | 0.012 | 0.016 | 0.107 |
| loading / loading_event | Decode Texture | after | 3 | 12 | 115.597 | 116.727 | 123.600 | 125.194 | 125.593 |
| loading / loading_event | Present | after | 3 | 14187 | 0.233 | 0.205 | 0.304 | 0.594 | 3.393 |
| loading / loading_event | Main Frame | after | 3 | 14186 | 0.423 | 0.384 | 0.545 | 0.954 | 14.160 |
| loading / loading_event | Upload Graphics Acquire | after | 3 | 16 | 0.000 | 0.000 | 0.001 | 0.001 | 0.001 |
| loading / loading_event | Parse Mesh Data | after | 3 | 4 | 460.269 | 471.727 | 485.110 | 485.601 | 485.724 |
| loading / loading_event | Render Submit | after | 3 | 14187 | 0.031 | 0.028 | 0.039 | 0.064 | 1.290 |
| loading / loading_event | Asset Main Thread Tasks | after | 3 | 14187 | 0.001 | 0.001 | 0.001 | 0.002 | 0.919 |
| loading / loading_event | Load Mesh Transfer Job | after | 3 | 4 | 462.326 | 473.664 | 486.937 | 487.427 | 487.550 |
| loading / loading_event | Upload Transfer Submit | after | 3 | 16 | 4.205 | 3.757 | 8.758 | 9.778 | 10.033 |
| loading / loading_event | Upload Transfer Record | after | 3 | 16 | 3.702 | 3.676 | 5.791 | 8.488 | 8.941 |

¹ Count — медиана числа полных вызовов/кадров на прогон. Max — медиана максимумов отдельных прогонов; абсолютный максимум каждого запуска сохранён в per-run.csv.

## Сравнение целевых зон

| Сцена / окно | Зона | Метрика | До, мс | После, мс | Снижение времени² |
|---|---|---|---:|---:|---:|
| loading / steady | Render Prepare Transforms | mean_ms | 0.009 | 0.010 | -8.3% |
| loading / steady | Render Prepare Transforms | median_ms | 0.009 | 0.009 | -1.5% |
| loading / steady | Asset GPU Finalize | mean_ms | 5.228 | 0.549 | 89.5% |
| loading / steady | Asset GPU Finalize | median_ms | 4.488 | 0.486 | 89.2% |
| loading / steady | Main Frame | median_ms | 0.407 | 0.382 | 6.3% |
| loading / steady | Main Frame | p95_ms | 0.711 | 0.558 | 21.5% |
| loading / steady | Main Frame | p99_ms | 2.982 | 0.925 | 69.0% |
| loading / steady | Main Frame | max_ms | 15.969 | 15.801 | 1.1% |
| loading / steady | Asset Main Thread Tasks | mean_ms | 0.004 | 0.001 | 76.5% |
| loading / steady | Asset Main Thread Tasks | median_ms | 0.001 | 0.001 | -7.8% |
| loading / loading_event | Render Prepare Transforms | mean_ms | 0.009 | 0.010 | -8.3% |
| loading / loading_event | Render Prepare Transforms | median_ms | 0.009 | 0.009 | -1.2% |
| loading / loading_event | Asset GPU Finalize | mean_ms | 5.228 | 0.549 | 89.5% |
| loading / loading_event | Asset GPU Finalize | median_ms | 4.488 | 0.486 | 89.2% |
| loading / loading_event | Main Frame | median_ms | 0.407 | 0.384 | 5.7% |
| loading / loading_event | Main Frame | p95_ms | 0.699 | 0.545 | 22.0% |
| loading / loading_event | Main Frame | p99_ms | 2.974 | 0.954 | 67.9% |
| loading / loading_event | Main Frame | max_ms | 15.969 | 14.160 | 11.3% |
| loading / loading_event | Asset Main Thread Tasks | mean_ms | 0.007 | 0.001 | 82.2% |
| loading / loading_event | Asset Main Thread Tasks | median_ms | 0.001 | 0.001 | -5.9% |

## Кадры во время загрузки и длинные кадры

| Прогон | Полных кадров внутри загрузки | Загрузка, мс | Кадров >16,67 мс³ | Кадров >50 мс³ |
|---|---:|---:|---:|---:|
| loading-before-01 | 1420 | 865.404 | 0 | 0 |
| loading-after-01 | 1905 | 1053.091 | 0 | 0 |
| loading-after-02 | 1990 | 1072.207 | 4 | 1 |
| loading-before-02 | 1072 | 720.783 | 0 | 0 |
| loading-before-03 | 920 | 702.618 | 1 | 0 |
| loading-after-03 | 2264 | 1030.067 | 0 | 0 |

³ В основном фиксированном диапазоне. Полные кадры внутри загрузки считаются по двум Tracy-маркерам; не совпадают с числом кадров, пересекающих событие.

² Отрицательное значение означает регрессию. Это описательное сравнение медиан метрик прогонов.

## Артефакты и проверка

- runs.json: параметры, порядок, SHA-256 executable и исходных ресурсов, коды завершения.
- summary.json / per-run.csv / aggregate.csv: все принятые значения и точные диапазоны в ns_since_start.
- В каждой папке запуска: trace.tracy, полный zones.csv (-u), messages.csv (-m), stdout/stderr приложения и захвата.
- Tracy Compare: открыть before/after одного repeat; диапазоны брать из summary.json для каждого трейса — стартовый timestamp между процессами отличается.
- Worker-зоны показывают параллелизм; сумма времён разных worker-потоков не равна wall time кадра.
- Один разовый hitch может не попасть в p95/p99: поэтому рядом приводится Max каждого прогона.
- Loading использует файловый кэш ОС в обычном состоянии; сброс кэша и холодный диск не заявляются. Порядок AB/BA уменьшает, но не устраняет систематические различия.
- Одновременное воспроизведение видео, сборка проекта и другие фоновые нагрузки искажают сравнение.
