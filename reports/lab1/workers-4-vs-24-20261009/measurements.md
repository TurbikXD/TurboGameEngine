# Workers 4 vs 24 — замеры Tracy

Сборка: Release; Tracy 0.13.1; SHA-256 app: `7332B29801BA7DD12D31EDA5215FD22247171EC794A12E137BC53A5935D88A1E`.
CPU: AMD Ryzen Threadripper 3960X 24-Core Processor; физических ядер: 24; логических: 48; RAM: 47.9 GiB.
ОС: Майкрософт Windows 11 Pro, 10.0.26200, build 26200; обнаруженные GPU: Parsec Virtual Display Adapter (driver 0.45.0.0); NVIDIA GeForce RTX 5070 (driver 32.0.16.1742). Активный адаптер проверяется по журналу рендера.
Фактические workers из LAB_RUN_START: 4, 24. Настройки и контекст сборки сохранены в runs.json.
На режим выполнено 3 независимых запусков приложения. Порядок A/B чередуется между повторениями.
Worker-count A/B: before=4, after=24. Jobs, async decode и GPU transfer включены в обоих режимах. Меняется только число workers. Asset in-flight limit зафиксирован на 16 в обоих режимах (без этого стандартный лимит зависел бы от workers). Публикация: 1 ресурс/кадр, 2 мс; ECS: 4096 объектов. Это отдельная серия, не прежние L1/CPU-vs-GPU upload замеры.

Прогрев: первые 3 с после LAB_RUN_START исключены. Основной диапазон: +3…+14 с; загрузка запускается на +6 с.
Для загрузки дополнительно показано окно LAB_LOAD_START −1…+5 с. Времена отсчитываются по сообщениям Tracy, а не времени запуска процесса или подключения.
Квантили вычислены линейной интерполяцией R-7 отдельно для каждого прогона. В таблице — медиана соответствующей метрики между прогонами.
Число кадров не подменяет число независимых экспериментов. Статистическая значимость не заявляется. Удаляются только незавершённые/невалидные зоны и зоны, пересекающие границу интервала.

| Сцена / окно | Зона | Режим | N | Count¹ | Mean, мс | Median, мс | p95, мс | p99, мс | Max, мс |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| ecs / steady | Render Gather | 4 workers | 3 | 1367 | 0.803 | 0.660 | 1.569 | 2.496 | 3.974 |
| ecs / steady | Render Prepare Transforms | 4 workers | 3 | 1367 | 0.771 | 0.779 | 1.042 | 1.306 | 2.160 |
| ecs / steady | Job Render Transforms | 4 workers | 3 | 21872 | 0.199 | 0.195 | 0.292 | 0.352 | 1.087 |
| ecs / steady | Main Frame | 4 workers | 3 | 1366 | 7.983 | 7.382 | 12.162 | 16.583 | 26.423 |
| ecs / steady | Render Submit | 4 workers | 3 | 1367 | 5.140 | 4.752 | 6.929 | 8.342 | 12.068 |
| ecs / steady | Job Wait | 4 workers | 3 | 1367 | 0.747 | 0.756 | 0.999 | 1.229 | 2.069 |
| ecs / steady | Asset Main Thread Tasks | 4 workers | 3 | 1367 | 0.001 | 0.001 | 0.003 | 0.010 | 0.020 |
| ecs / steady | Present | 4 workers | 3 | 1367 | 0.423 | 0.334 | 0.827 | 1.477 | 3.781 |
| ecs / steady | Render Gather | 24 workers | 3 | 1424 | 0.839 | 0.703 | 1.580 | 2.676 | 4.556 |
| ecs / steady | Render Prepare Transforms | 24 workers | 3 | 1424 | 0.365 | 0.354 | 0.495 | 0.727 | 1.086 |
| ecs / steady | Render Submit | 24 workers | 3 | 1423 | 5.197 | 4.975 | 7.170 | 8.607 | 10.370 |
| ecs / steady | Main Frame | 24 workers | 3 | 1423 | 7.724 | 7.287 | 12.003 | 15.798 | 26.792 |
| ecs / steady | Job Render Transforms | 24 workers | 3 | 22784 | 0.226 | 0.217 | 0.308 | 0.366 | 0.905 |
| ecs / steady | Job Wait | 24 workers | 3 | 1424 | 0.277 | 0.267 | 0.358 | 0.512 | 0.928 |
| ecs / steady | Asset Main Thread Tasks | 24 workers | 3 | 1424 | 0.001 | 0.001 | 0.002 | 0.008 | 0.022 |
| ecs / steady | Present | 24 workers | 3 | 1424 | 0.382 | 0.335 | 0.644 | 1.413 | 2.642 |
| loading / steady | Render Gather | 4 workers | 3 | 19906 | 0.004 | 0.003 | 0.007 | 0.013 | 5.064 |
| loading / steady | Upload Mesh Publish | 4 workers | 3 | 4 | 0.570 | 0.500 | 0.745 | 0.753 | 0.756 |
| loading / steady | Asset GPU Finalize | 4 workers | 3 | 16 | 0.565 | 0.465 | 0.898 | 1.061 | 1.102 |
| loading / steady | Asset Dispatch | 4 workers | 3 | 16 | 0.011 | 0.006 | 0.030 | 0.035 | 0.036 |
| loading / steady | Job Render Transforms | 4 workers | 3 | 19906 | 0.011 | 0.011 | 0.015 | 0.020 | 0.136 |
| loading / steady | Upload Texture Publish | 4 workers | 3 | 12 | 0.559 | 0.482 | 0.950 | 1.069 | 1.098 |
| loading / steady | Render Prepare Transforms | 4 workers | 3 | 19906 | 0.011 | 0.011 | 0.015 | 0.021 | 0.137 |
| loading / steady | Present | 4 workers | 3 | 19906 | 0.286 | 0.221 | 0.543 | 1.591 | 5.543 |
| loading / steady | Load Texture Transfer Job | 4 workers | 3 | 12 | 195.980 | 195.939 | 209.388 | 211.855 | 212.570 |
| loading / steady | Decode Texture | 4 workers | 3 | 12 | 126.145 | 124.247 | 138.808 | 138.817 | 138.819 |
| loading / steady | Upload Transfer Submit | 4 workers | 3 | 16 | 6.330 | 4.698 | 15.746 | 15.893 | 15.930 |
| loading / steady | Render Submit | 4 workers | 3 | 19906 | 0.036 | 0.032 | 0.064 | 0.123 | 2.248 |
| loading / steady | Main Frame | 4 workers | 3 | 19906 | 0.552 | 0.443 | 1.168 | 2.428 | 16.304 |
| loading / steady | Upload Transfer Record | 4 workers | 3 | 16 | 4.496 | 4.297 | 7.733 | 10.654 | 11.210 |
| loading / steady | Upload Graphics Acquire | 4 workers | 3 | 16 | 0.000 | 0.000 | 0.001 | 0.001 | 0.001 |
| loading / steady | Parse Mesh Data | 4 workers | 3 | 4 | 509.930 | 515.055 | 521.318 | 521.549 | 521.607 |
| loading / steady | Upload Build CPU Mips | 4 workers | 3 | 12 | 63.077 | 62.306 | 68.088 | 70.809 | 71.489 |
| loading / steady | Asset Main Thread Tasks | 4 workers | 3 | 19907 | 0.001 | 0.001 | 0.002 | 0.005 | 1.120 |
| loading / steady | Load Mesh Transfer Job | 4 workers | 3 | 4 | 512.950 | 517.075 | 523.732 | 523.941 | 523.994 |
| loading / steady | Asset Decode Job | 4 workers | 3 | 16 | 275.225 | 198.380 | 521.350 | 523.734 | 523.996 |
| loading / loading_event | Render Gather | 4 workers | 3 | 10366 | 0.004 | 0.003 | 0.008 | 0.014 | 5.064 |
| loading / loading_event | Upload Mesh Publish | 4 workers | 3 | 4 | 0.570 | 0.500 | 0.745 | 0.753 | 0.756 |
| loading / loading_event | Asset GPU Finalize | 4 workers | 3 | 16 | 0.565 | 0.465 | 0.898 | 1.061 | 1.102 |
| loading / loading_event | Asset Dispatch | 4 workers | 3 | 16 | 0.011 | 0.006 | 0.030 | 0.035 | 0.036 |
| loading / loading_event | Job Render Transforms | 4 workers | 3 | 10366 | 0.011 | 0.011 | 0.015 | 0.021 | 0.136 |
| loading / loading_event | Upload Texture Publish | 4 workers | 3 | 12 | 0.559 | 0.482 | 0.950 | 1.069 | 1.098 |
| loading / loading_event | Render Prepare Transforms | 4 workers | 3 | 10366 | 0.011 | 0.011 | 0.015 | 0.021 | 0.137 |
| loading / loading_event | Present | 4 workers | 3 | 10365 | 0.301 | 0.226 | 0.620 | 1.769 | 5.543 |
| loading / loading_event | Load Texture Transfer Job | 4 workers | 3 | 12 | 195.980 | 195.939 | 209.388 | 211.855 | 212.570 |
| loading / loading_event | Decode Texture | 4 workers | 3 | 12 | 126.145 | 124.247 | 138.808 | 138.817 | 138.819 |
| loading / loading_event | Upload Transfer Submit | 4 workers | 3 | 16 | 6.330 | 4.698 | 15.746 | 15.893 | 15.930 |
| loading / loading_event | Render Submit | 4 workers | 3 | 10366 | 0.038 | 0.033 | 0.071 | 0.145 | 2.248 |
| loading / loading_event | Main Frame | 4 workers | 3 | 10365 | 0.578 | 0.452 | 1.282 | 2.665 | 16.304 |
| loading / loading_event | Upload Transfer Record | 4 workers | 3 | 16 | 4.496 | 4.297 | 7.733 | 10.654 | 11.210 |
| loading / loading_event | Upload Graphics Acquire | 4 workers | 3 | 16 | 0.000 | 0.000 | 0.001 | 0.001 | 0.001 |
| loading / loading_event | Parse Mesh Data | 4 workers | 3 | 4 | 509.930 | 515.055 | 521.318 | 521.549 | 521.607 |
| loading / loading_event | Upload Build CPU Mips | 4 workers | 3 | 12 | 63.077 | 62.306 | 68.088 | 70.809 | 71.489 |
| loading / loading_event | Asset Main Thread Tasks | 4 workers | 3 | 10366 | 0.002 | 0.001 | 0.002 | 0.006 | 1.120 |
| loading / loading_event | Load Mesh Transfer Job | 4 workers | 3 | 4 | 512.950 | 517.075 | 523.732 | 523.941 | 523.994 |
| loading / loading_event | Asset Decode Job | 4 workers | 3 | 16 | 275.225 | 198.380 | 521.350 | 523.734 | 523.996 |
| loading / steady | Render Gather | 24 workers | 3 | 20113 | 0.004 | 0.003 | 0.007 | 0.013 | 7.526 |
| loading / steady | Upload Mesh Publish | 24 workers | 3 | 4 | 2.772 | 0.449 | 8.403 | 9.517 | 9.796 |
| loading / steady | Asset GPU Finalize | 24 workers | 3 | 16 | 1.350 | 0.590 | 4.112 | 8.794 | 9.965 |
| loading / steady | Asset Dispatch | 24 workers | 3 | 16 | 0.069 | 0.056 | 0.117 | 0.134 | 0.145 |
| loading / steady | Job Render Transforms | 24 workers | 3 | 20113 | 0.011 | 0.010 | 0.015 | 0.023 | 0.516 |
| loading / steady | Upload Texture Publish | 24 workers | 3 | 12 | 0.871 | 0.628 | 1.859 | 2.090 | 2.148 |
| loading / steady | Render Prepare Transforms | 24 workers | 3 | 20113 | 0.011 | 0.011 | 0.015 | 0.023 | 0.517 |
| loading / steady | Present | 24 workers | 3 | 20113 | 0.310 | 0.238 | 0.622 | 1.503 | 9.670 |
| loading / steady | Load Texture Transfer Job | 24 workers | 3 | 12 | 341.335 | 342.943 | 385.127 | 391.449 | 393.029 |
| loading / steady | Decode Texture | 24 workers | 3 | 12 | 227.409 | 233.584 | 245.625 | 247.008 | 247.354 |
| loading / steady | Upload Transfer Submit | 24 workers | 3 | 16 | 21.975 | 17.705 | 47.119 | 49.301 | 49.847 |
| loading / steady | Render Submit | 24 workers | 3 | 20113 | 0.035 | 0.031 | 0.062 | 0.114 | 2.762 |
| loading / steady | Main Frame | 24 workers | 3 | 20112 | 0.547 | 0.446 | 1.020 | 2.282 | 16.758 |
| loading / steady | Upload Transfer Record | 24 workers | 3 | 16 | 7.261 | 5.185 | 18.310 | 21.044 | 21.727 |
| loading / steady | Upload Graphics Acquire | 24 workers | 3 | 16 | 0.001 | 0.000 | 0.002 | 0.004 | 0.004 |
| loading / steady | Parse Mesh Data | 24 workers | 3 | 4 | 767.720 | 768.006 | 783.577 | 785.625 | 786.136 |
| loading / steady | Upload Build CPU Mips | 24 workers | 3 | 12 | 85.215 | 85.399 | 91.898 | 92.106 | 92.158 |
| loading / steady | Asset Main Thread Tasks | 24 workers | 3 | 20113 | 0.002 | 0.001 | 0.002 | 0.005 | 10.005 |
| loading / steady | Load Mesh Transfer Job | 24 workers | 3 | 4 | 770.163 | 770.331 | 785.782 | 787.825 | 788.336 |
| loading / steady | Asset Decode Job | 24 workers | 3 | 16 | 462.074 | 362.232 | 775.567 | 785.783 | 788.338 |
| loading / loading_event | Render Gather | 24 workers | 3 | 9992 | 0.005 | 0.003 | 0.009 | 0.014 | 7.526 |
| loading / loading_event | Upload Mesh Publish | 24 workers | 3 | 4 | 2.772 | 0.449 | 8.403 | 9.517 | 9.796 |
| loading / loading_event | Asset GPU Finalize | 24 workers | 3 | 16 | 1.350 | 0.590 | 4.112 | 8.794 | 9.965 |
| loading / loading_event | Asset Dispatch | 24 workers | 3 | 16 | 0.069 | 0.056 | 0.117 | 0.134 | 0.145 |
| loading / loading_event | Job Render Transforms | 24 workers | 3 | 9992 | 0.011 | 0.011 | 0.016 | 0.024 | 0.170 |
| loading / loading_event | Upload Texture Publish | 24 workers | 3 | 12 | 0.871 | 0.628 | 1.859 | 2.090 | 2.148 |
| loading / loading_event | Render Prepare Transforms | 24 workers | 3 | 9992 | 0.011 | 0.011 | 0.016 | 0.024 | 0.170 |
| loading / loading_event | Present | 24 workers | 3 | 9991 | 0.342 | 0.247 | 0.743 | 1.900 | 9.670 |
| loading / loading_event | Load Texture Transfer Job | 24 workers | 3 | 12 | 341.335 | 342.943 | 385.127 | 391.449 | 393.029 |
| loading / loading_event | Decode Texture | 24 workers | 3 | 12 | 227.409 | 233.584 | 245.625 | 247.008 | 247.354 |
| loading / loading_event | Upload Transfer Submit | 24 workers | 3 | 16 | 21.975 | 17.705 | 47.119 | 49.301 | 49.847 |
| loading / loading_event | Render Submit | 24 workers | 3 | 9992 | 0.037 | 0.032 | 0.071 | 0.129 | 2.762 |
| loading / loading_event | Main Frame | 24 workers | 3 | 9991 | 0.600 | 0.462 | 1.251 | 2.914 | 16.758 |
| loading / loading_event | Upload Transfer Record | 24 workers | 3 | 16 | 7.261 | 5.185 | 18.310 | 21.044 | 21.727 |
| loading / loading_event | Upload Graphics Acquire | 24 workers | 3 | 16 | 0.001 | 0.000 | 0.002 | 0.004 | 0.004 |
| loading / loading_event | Parse Mesh Data | 24 workers | 3 | 4 | 767.720 | 768.006 | 783.577 | 785.625 | 786.136 |
| loading / loading_event | Upload Build CPU Mips | 24 workers | 3 | 12 | 85.215 | 85.399 | 91.898 | 92.106 | 92.158 |
| loading / loading_event | Asset Main Thread Tasks | 24 workers | 3 | 9992 | 0.003 | 0.001 | 0.002 | 0.006 | 10.005 |
| loading / loading_event | Load Mesh Transfer Job | 24 workers | 3 | 4 | 770.163 | 770.331 | 785.782 | 787.825 | 788.336 |
| loading / loading_event | Asset Decode Job | 24 workers | 3 | 16 | 462.074 | 362.232 | 775.567 | 785.783 | 788.338 |

¹ Count — медиана числа полных вызовов/кадров на прогон. Max — медиана максимумов отдельных прогонов; абсолютный максимум каждого запуска сохранён в per-run.csv.

## Сравнение целевых зон

| Сцена / окно | Зона | Метрика | До, мс | После, мс | Снижение времени² |
|---|---|---|---:|---:|---:|
| ecs / steady | Render Prepare Transforms | mean_ms | 0.771 | 0.365 | 52.7% |
| ecs / steady | Render Prepare Transforms | median_ms | 0.779 | 0.354 | 54.6% |
| ecs / steady | Main Frame | median_ms | 7.382 | 7.287 | 1.3% |
| ecs / steady | Main Frame | p95_ms | 12.162 | 12.003 | 1.3% |
| ecs / steady | Main Frame | p99_ms | 16.583 | 15.798 | 4.7% |
| ecs / steady | Main Frame | max_ms | 26.423 | 26.792 | -1.4% |
| ecs / steady | Asset Main Thread Tasks | mean_ms | 0.001 | 0.001 | 23.8% |
| ecs / steady | Asset Main Thread Tasks | median_ms | 0.001 | 0.001 | -9.9% |
| loading / steady | Asset GPU Finalize | mean_ms | 0.565 | 1.350 | -139.1% |
| loading / steady | Asset GPU Finalize | median_ms | 0.465 | 0.590 | -27.1% |
| loading / steady | Render Prepare Transforms | mean_ms | 0.011 | 0.011 | 3.3% |
| loading / steady | Render Prepare Transforms | median_ms | 0.011 | 0.011 | 5.7% |
| loading / steady | Decode Texture | mean_ms | 126.145 | 227.409 | -80.3% |
| loading / steady | Decode Texture | median_ms | 124.247 | 233.584 | -88.0% |
| loading / steady | Upload Transfer Submit | mean_ms | 6.330 | 21.975 | -247.1% |
| loading / steady | Upload Transfer Submit | median_ms | 4.698 | 17.705 | -276.9% |
| loading / steady | Main Frame | median_ms | 0.443 | 0.446 | -0.7% |
| loading / steady | Main Frame | p95_ms | 1.168 | 1.020 | 12.7% |
| loading / steady | Main Frame | p99_ms | 2.428 | 2.282 | 6.0% |
| loading / steady | Main Frame | max_ms | 16.304 | 16.758 | -2.8% |
| loading / steady | Upload Transfer Record | mean_ms | 4.496 | 7.261 | -61.5% |
| loading / steady | Upload Transfer Record | median_ms | 4.297 | 5.185 | -20.7% |
| loading / steady | Parse Mesh Data | mean_ms | 509.930 | 767.720 | -50.6% |
| loading / steady | Parse Mesh Data | median_ms | 515.055 | 768.006 | -49.1% |
| loading / steady | Upload Build CPU Mips | mean_ms | 63.077 | 85.215 | -35.1% |
| loading / steady | Upload Build CPU Mips | median_ms | 62.306 | 85.399 | -37.1% |
| loading / steady | Asset Main Thread Tasks | mean_ms | 0.001 | 0.002 | -43.9% |
| loading / steady | Asset Main Thread Tasks | median_ms | 0.001 | 0.001 | 1.7% |
| loading / loading_event | Asset GPU Finalize | mean_ms | 0.565 | 1.350 | -139.1% |
| loading / loading_event | Asset GPU Finalize | median_ms | 0.465 | 0.590 | -27.1% |
| loading / loading_event | Render Prepare Transforms | mean_ms | 0.011 | 0.011 | 1.9% |
| loading / loading_event | Render Prepare Transforms | median_ms | 0.011 | 0.011 | 2.9% |
| loading / loading_event | Decode Texture | mean_ms | 126.145 | 227.409 | -80.3% |
| loading / loading_event | Decode Texture | median_ms | 124.247 | 233.584 | -88.0% |
| loading / loading_event | Upload Transfer Submit | mean_ms | 6.330 | 21.975 | -247.1% |
| loading / loading_event | Upload Transfer Submit | median_ms | 4.698 | 17.705 | -276.9% |
| loading / loading_event | Main Frame | median_ms | 0.452 | 0.462 | -2.2% |
| loading / loading_event | Main Frame | p95_ms | 1.282 | 1.251 | 2.4% |
| loading / loading_event | Main Frame | p99_ms | 2.665 | 2.914 | -9.3% |
| loading / loading_event | Main Frame | max_ms | 16.304 | 16.758 | -2.8% |
| loading / loading_event | Upload Transfer Record | mean_ms | 4.496 | 7.261 | -61.5% |
| loading / loading_event | Upload Transfer Record | median_ms | 4.297 | 5.185 | -20.7% |
| loading / loading_event | Parse Mesh Data | mean_ms | 509.930 | 767.720 | -50.6% |
| loading / loading_event | Parse Mesh Data | median_ms | 515.055 | 768.006 | -49.1% |
| loading / loading_event | Upload Build CPU Mips | mean_ms | 63.077 | 85.215 | -35.1% |
| loading / loading_event | Upload Build CPU Mips | median_ms | 62.306 | 85.399 | -37.1% |
| loading / loading_event | Asset Main Thread Tasks | mean_ms | 0.002 | 0.003 | -68.1% |
| loading / loading_event | Asset Main Thread Tasks | median_ms | 0.001 | 0.001 | 3.2% |

## Кадры во время загрузки и длинные кадры

| Прогон | Полных кадров внутри загрузки | Загрузка, мс | Кадров >16,67 мс³ | Кадров >50 мс³ |
|---|---:|---:|---:|---:|
| ecs-before-01 | — | — | 16 | 0 |
| ecs-after-01 | — | — | 12 | 0 |
| ecs-after-02 | — | — | 11 | 0 |
| ecs-before-02 | — | — | 12 | 0 |
| ecs-before-03 | — | — | 14 | 0 |
| ecs-after-03 | — | — | 3 | 0 |
| loading-before-01 | 1978 | 1197.417 | 0 | 0 |
| loading-after-01 | 862 | 608.908 | 1 | 0 |
| loading-after-02 | 632 | 796.248 | 0 | 0 |
| loading-before-02 | 1720 | 1135.345 | 1 | 0 |
| loading-before-03 | 1948 | 1084.320 | 0 | 0 |
| loading-after-03 | 701 | 860.117 | 1 | 0 |

³ В основном фиксированном диапазоне. Полные кадры внутри загрузки считаются по двум Tracy-маркерам; не совпадают с числом кадров, пересекающих событие.

## Полное время загрузки по независимым прогонам

| Режим | N | Median, мс | Min, мс | Max, мс |
|---|---:|---:|---:|---:|
| 4 workers | 3 | 1135.345 | 1084.320 | 1197.417 |
| 24 workers | 3 | 796.248 | 608.908 | 860.117 |

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
