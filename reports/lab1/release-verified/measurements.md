# ЛР 1 — воспроизводимые замеры Tracy

Сборка: Release; Tracy 0.13.1; SHA-256 app: `187B71457DA4B5F41401ED806D7A58A335A8F29F7D742298F8510E95BD59DB20`.
CPU: AMD Ryzen Threadripper 3960X 24-Core Processor; физических ядер: UNKNOWN; логических: UNKNOWN; RAM: UNKNOWN.
ОС: UNKNOWN; обнаруженные GPU: UNKNOWN. Активный адаптер проверяется по журналу рендера.
Фактические workers из LAB_RUN_START: 4. Настройки и контекст сборки сохранены в runs.json.
На режим выполнено 3 независимых запусков приложения. Порядок A/B чередуется между повторениями.
В исходном движке уже существовал отдельный пул асинхронной загрузки на 4 потока. Он мигрирован на общую job system. Режим loading/before (sync) — контролируемое отключение async для демонстрации L1, а не утверждение, что исходный движок всегда загружал синхронно или создавал неограниченное число потоков.
Режим loading/after: общий scheduler + ограниченный GPU-памп (1 ресурс/кадр, мягкий бюджет 2 мс). Если включён legacy-pump: тот же новый scheduler с лимитами 65536 ресурсов/кадр и 60000 мс, имитирующий прежнюю неограниченную обработку готовых ресурсов; это изоляция эффекта пампа, а не запуск старого бинарника.
Прогрев: первые 3 с после LAB_RUN_START исключены. Основной диапазон: +3…+14 с; загрузка запускается на +6 с.
Для загрузки дополнительно показано окно LAB_LOAD_START −1…+5 с. Времена отсчитываются по сообщениям Tracy, а не времени запуска процесса или подключения.
Квантили вычислены линейной интерполяцией R-7 отдельно для каждого прогона. В таблице — медиана соответствующей метрики между прогонами.
Число кадров не подменяет число независимых экспериментов. Статистическая значимость не заявляется. Удаляются только незавершённые/невалидные зоны и зоны, пересекающие границу интервала.

| Сцена / окно | Зона | Режим | N | Count¹ | Mean, мс | Median, мс | p95, мс | p99, мс | Max, мс |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| ecs / steady | Render Gather | before | 3 | 1089 | 0.880 | 0.731 | 1.588 | 2.060 | 2.993 |
| ecs / steady | Render Prepare Transforms | before | 3 | 1089 | 2.790 | 2.686 | 3.645 | 4.312 | 4.897 |
| ecs / steady | Present | before | 3 | 1089 | 0.373 | 0.350 | 0.629 | 0.922 | 2.043 |
| ecs / steady | Main Frame | before | 3 | 1088 | 10.099 | 9.851 | 12.903 | 14.400 | 21.740 |
| ecs / steady | Asset Main Thread Tasks | before | 3 | 1089 | 0.001 | 0.001 | 0.002 | 0.004 | 0.036 |
| ecs / steady | Job Render Transforms | before | 3 | 1089 | 2.790 | 2.685 | 3.645 | 4.312 | 4.897 |
| ecs / steady | Render Submit | before | 3 | 1088 | 5.109 | 4.973 | 6.195 | 6.731 | 7.134 |
| ecs / steady | Render Gather | after | 3 | 1180 | 1.072 | 0.958 | 1.887 | 2.465 | 3.098 |
| ecs / steady | Render Prepare Transforms | after | 3 | 1180 | 0.925 | 0.916 | 1.117 | 1.216 | 1.606 |
| ecs / steady | Job Render Transforms | after | 3 | 18880 | 0.238 | 0.233 | 0.323 | 0.378 | 1.364 |
| ecs / steady | Render Submit | after | 3 | 1179 | 5.497 | 5.433 | 6.659 | 7.395 | 9.070 |
| ecs / steady | Main Frame | after | 3 | 1179 | 9.321 | 9.184 | 11.817 | 13.654 | 20.552 |
| ecs / steady | Present | after | 3 | 1180 | 0.487 | 0.446 | 0.885 | 1.229 | 1.521 |
| ecs / steady | Asset Main Thread Tasks | after | 3 | 1180 | 0.002 | 0.001 | 0.003 | 0.006 | 0.026 |
| ecs / steady | Job Wait | after | 3 | 1180 | 0.890 | 0.883 | 1.060 | 1.167 | 1.576 |
| loading / steady | Render Gather | before | 3 | 9898 | 0.341 | 0.003 | 0.011 | 0.014 | 3408.610 |
| loading / steady | Parse Mesh Data | before | 3 | 4 | 432.172 | 422.218 | 460.318 | 465.298 | 466.542 |
| loading / steady | Asset Dispatch | before | 3 | 16 | 212.749 | 121.838 | 447.834 | 475.837 | 482.837 |
| loading / steady | Decode Texture | before | 3 | 12 | 107.900 | 108.167 | 111.928 | 112.681 | 112.869 |
| loading / steady | Load Texture CPU | before | 3 | 12 | 107.909 | 108.177 | 111.938 | 112.690 | 112.878 |
| loading / steady | Job Render Transforms | before | 3 | 9898 | 0.012 | 0.011 | 0.018 | 0.024 | 0.210 |
| loading / steady | Render Submit | before | 3 | 9898 | 0.038 | 0.032 | 0.078 | 0.113 | 0.675 |
| loading / steady | Asset GPU Finalize | before | 3 | 16 | 12.691 | 12.581 | 16.889 | 17.306 | 17.411 |
| loading / steady | Main Frame | before | 3 | 9897 | 1.110 | 0.453 | 2.515 | 5.953 | 3410.838 |
| loading / steady | Upload Mesh GPU | before | 3 | 4 | 12.428 | 12.338 | 17.152 | 17.358 | 17.409 |
| loading / steady | Upload Texture GPU | before | 3 | 12 | 12.777 | 12.580 | 16.035 | 16.578 | 16.714 |
| loading / steady | Present | before | 3 | 9897 | 0.513 | 0.226 | 2.118 | 5.563 | 18.761 |
| loading / steady | Asset Main Thread Tasks | before | 3 | 9898 | 0.001 | 0.000 | 0.002 | 0.004 | 0.033 |
| loading / steady | Asset Synchronous Load | before | 3 | 16 | 212.748 | 121.837 | 447.834 | 475.836 | 482.836 |
| loading / steady | Load Mesh CPU | before | 3 | 4 | 432.178 | 422.221 | 460.330 | 465.310 | 466.555 |
| loading / steady | Render Prepare Transforms | before | 3 | 9898 | 0.012 | 0.011 | 0.019 | 0.024 | 0.211 |
| loading / loading_event | Render Gather | before | 3 | 3376 | 0.929 | 0.003 | 0.011 | 0.014 | 3408.610 |
| loading / loading_event | Parse Mesh Data | before | 3 | 4 | 432.172 | 422.218 | 460.318 | 465.298 | 466.542 |
| loading / loading_event | Asset Dispatch | before | 3 | 16 | 212.749 | 121.838 | 447.834 | 475.837 | 482.837 |
| loading / loading_event | Decode Texture | before | 3 | 12 | 107.900 | 108.167 | 111.928 | 112.681 | 112.869 |
| loading / loading_event | Load Texture CPU | before | 3 | 12 | 107.909 | 108.177 | 111.938 | 112.690 | 112.878 |
| loading / loading_event | Job Render Transforms | before | 3 | 3376 | 0.012 | 0.011 | 0.018 | 0.024 | 0.210 |
| loading / loading_event | Render Submit | before | 3 | 3376 | 0.038 | 0.032 | 0.078 | 0.111 | 0.253 |
| loading / loading_event | Asset GPU Finalize | before | 3 | 16 | 12.691 | 12.581 | 16.889 | 17.306 | 17.411 |
| loading / loading_event | Main Frame | before | 3 | 3374 | 1.778 | 0.462 | 3.495 | 5.849 | 3410.838 |
| loading / loading_event | Upload Mesh GPU | before | 3 | 4 | 12.428 | 12.338 | 17.152 | 17.358 | 17.409 |
| loading / loading_event | Upload Texture GPU | before | 3 | 12 | 12.777 | 12.580 | 16.035 | 16.578 | 16.714 |
| loading / loading_event | Present | before | 3 | 3375 | 0.555 | 0.230 | 2.944 | 5.458 | 18.761 |
| loading / loading_event | Asset Main Thread Tasks | before | 3 | 3375 | 0.001 | 0.000 | 0.003 | 0.004 | 0.009 |
| loading / loading_event | Asset Synchronous Load | before | 3 | 16 | 212.748 | 121.837 | 447.834 | 475.836 | 482.836 |
| loading / loading_event | Load Mesh CPU | before | 3 | 4 | 432.178 | 422.221 | 460.330 | 465.310 | 466.555 |
| loading / loading_event | Render Prepare Transforms | before | 3 | 3376 | 0.012 | 0.011 | 0.019 | 0.024 | 0.211 |
| loading / steady | Render Gather | after | 3 | 13070 | 0.005 | 0.003 | 0.011 | 0.014 | 6.051 |
| loading / steady | Upload Texture GPU | after | 3 | 12 | 9.033 | 7.440 | 17.654 | 20.823 | 21.322 |
| loading / steady | Asset Dispatch | after | 3 | 16 | 0.011 | 0.006 | 0.029 | 0.032 | 0.034 |
| loading / steady | Job Render Transforms | after | 3 | 13070 | 0.011 | 0.011 | 0.016 | 0.020 | 0.233 |
| loading / steady | Parse Mesh Data | after | 3 | 4 | 593.536 | 595.892 | 605.759 | 607.171 | 607.524 |
| loading / steady | Render Prepare Transforms | after | 3 | 13070 | 0.011 | 0.011 | 0.017 | 0.021 | 0.234 |
| loading / steady | Decode Texture | after | 3 | 12 | 138.825 | 136.650 | 152.096 | 157.507 | 158.860 |
| loading / steady | Asset Decode Job | after | 3 | 16 | 269.535 | 142.218 | 603.760 | 605.768 | 607.533 |
| loading / steady | Present | after | 3 | 13069 | 0.543 | 0.239 | 2.262 | 6.513 | 13.253 |
| loading / steady | Load Texture CPU | after | 3 | 12 | 138.841 | 136.663 | 152.114 | 157.537 | 158.893 |
| loading / steady | Render Submit | after | 3 | 13070 | 0.038 | 0.033 | 0.074 | 0.110 | 0.524 |
| loading / steady | Asset GPU Finalize | after | 3 | 16 | 8.561 | 7.373 | 16.147 | 20.646 | 21.325 |
| loading / steady | Main Frame | after | 3 | 13069 | 0.841 | 0.464 | 2.734 | 7.160 | 30.003 |
| loading / steady | Load Mesh CPU | after | 3 | 4 | 593.540 | 595.894 | 605.764 | 607.176 | 607.528 |
| loading / steady | Upload Mesh GPU | after | 3 | 4 | 9.714 | 9.920 | 12.205 | 12.261 | 12.275 |
| loading / steady | Asset Main Thread Tasks | after | 3 | 13070 | 0.011 | 0.001 | 0.003 | 0.005 | 21.408 |
| loading / loading_event | Render Gather | after | 3 | 6506 | 0.006 | 0.003 | 0.011 | 0.014 | 6.051 |
| loading / loading_event | Upload Texture GPU | after | 3 | 12 | 9.033 | 7.440 | 17.654 | 20.823 | 21.322 |
| loading / loading_event | Asset Dispatch | after | 3 | 16 | 0.011 | 0.006 | 0.029 | 0.032 | 0.034 |
| loading / loading_event | Job Render Transforms | after | 3 | 6506 | 0.012 | 0.011 | 0.017 | 0.021 | 0.233 |
| loading / loading_event | Parse Mesh Data | after | 3 | 4 | 593.536 | 595.892 | 605.759 | 607.171 | 607.524 |
| loading / loading_event | Render Prepare Transforms | after | 3 | 6506 | 0.012 | 0.011 | 0.017 | 0.021 | 0.234 |
| loading / loading_event | Decode Texture | after | 3 | 12 | 138.825 | 136.650 | 152.096 | 157.507 | 158.860 |
| loading / loading_event | Asset Decode Job | after | 3 | 16 | 269.535 | 142.218 | 603.760 | 605.768 | 607.533 |
| loading / loading_event | Present | after | 3 | 6505 | 0.598 | 0.247 | 2.451 | 6.593 | 13.253 |
| loading / loading_event | Load Texture CPU | after | 3 | 12 | 138.841 | 136.663 | 152.114 | 157.537 | 158.893 |
| loading / loading_event | Render Submit | after | 3 | 6506 | 0.040 | 0.033 | 0.078 | 0.114 | 0.487 |
| loading / loading_event | Asset GPU Finalize | after | 3 | 16 | 8.561 | 7.373 | 16.147 | 20.646 | 21.325 |
| loading / loading_event | Main Frame | after | 3 | 6505 | 0.922 | 0.486 | 3.115 | 7.326 | 30.003 |
| loading / loading_event | Load Mesh CPU | after | 3 | 4 | 593.540 | 595.894 | 605.764 | 607.176 | 607.528 |
| loading / loading_event | Upload Mesh GPU | after | 3 | 4 | 9.714 | 9.920 | 12.205 | 12.261 | 12.275 |
| loading / loading_event | Asset Main Thread Tasks | after | 3 | 6506 | 0.021 | 0.001 | 0.003 | 0.006 | 21.408 |
| loading / steady | Render Gather | legacy-pump | 3 | 13221 | 0.005 | 0.003 | 0.011 | 0.015 | 6.580 |
| loading / steady | Upload Texture GPU | legacy-pump | 3 | 12 | 11.972 | 10.985 | 19.098 | 20.089 | 20.337 |
| loading / steady | Asset Dispatch | legacy-pump | 3 | 16 | 0.012 | 0.006 | 0.033 | 0.043 | 0.046 |
| loading / steady | Asset Decode Job | legacy-pump | 3 | 16 | 266.219 | 145.228 | 649.264 | 651.518 | 652.082 |
| loading / steady | Parse Mesh Data | legacy-pump | 3 | 4 | 634.196 | 640.521 | 651.501 | 651.950 | 652.063 |
| loading / steady | Render Prepare Transforms | legacy-pump | 3 | 13221 | 0.012 | 0.011 | 0.019 | 0.024 | 0.415 |
| loading / steady | Present | legacy-pump | 3 | 13221 | 0.533 | 0.229 | 2.412 | 6.051 | 12.905 |
| loading / steady | Load Texture CPU | legacy-pump | 3 | 12 | 143.553 | 141.900 | 155.304 | 157.050 | 157.487 |
| loading / steady | Asset GPU Finalize | legacy-pump | 3 | 16 | 11.635 | 10.986 | 18.856 | 20.000 | 20.338 |
| loading / steady | Main Frame | legacy-pump | 3 | 13221 | 0.831 | 0.459 | 2.972 | 6.473 | 72.152 |
| loading / steady | Load Mesh CPU | legacy-pump | 3 | 4 | 634.202 | 640.526 | 651.511 | 651.962 | 652.074 |
| loading / steady | Decode Texture | legacy-pump | 3 | 12 | 143.539 | 141.886 | 155.289 | 157.038 | 157.475 |
| loading / steady | Upload Mesh GPU | legacy-pump | 3 | 4 | 10.618 | 10.784 | 12.615 | 12.668 | 12.682 |
| loading / steady | Asset Main Thread Tasks | legacy-pump | 3 | 13222 | 0.015 | 0.000 | 0.003 | 0.005 | 70.734 |
| loading / steady | Job Render Transforms | legacy-pump | 3 | 13221 | 0.012 | 0.011 | 0.019 | 0.024 | 0.415 |
| loading / steady | Render Submit | legacy-pump | 3 | 13221 | 0.038 | 0.032 | 0.079 | 0.118 | 0.912 |
| loading / loading_event | Render Gather | legacy-pump | 3 | 6839 | 0.005 | 0.003 | 0.012 | 0.015 | 6.580 |
| loading / loading_event | Upload Texture GPU | legacy-pump | 3 | 12 | 11.972 | 10.985 | 19.098 | 20.089 | 20.337 |
| loading / loading_event | Asset Dispatch | legacy-pump | 3 | 16 | 0.012 | 0.006 | 0.033 | 0.043 | 0.046 |
| loading / loading_event | Asset Decode Job | legacy-pump | 3 | 16 | 266.219 | 145.228 | 649.264 | 651.518 | 652.082 |
| loading / loading_event | Parse Mesh Data | legacy-pump | 3 | 4 | 634.196 | 640.521 | 651.501 | 651.950 | 652.063 |
| loading / loading_event | Render Prepare Transforms | legacy-pump | 3 | 6839 | 0.012 | 0.011 | 0.019 | 0.025 | 0.415 |
| loading / loading_event | Present | legacy-pump | 3 | 6838 | 0.561 | 0.229 | 2.938 | 6.203 | 12.853 |
| loading / loading_event | Load Texture CPU | legacy-pump | 3 | 12 | 143.553 | 141.900 | 155.304 | 157.050 | 157.487 |
| loading / loading_event | Asset GPU Finalize | legacy-pump | 3 | 16 | 11.635 | 10.986 | 18.856 | 20.000 | 20.338 |
| loading / loading_event | Main Frame | legacy-pump | 3 | 6838 | 0.877 | 0.460 | 3.429 | 6.652 | 72.152 |
| loading / loading_event | Load Mesh CPU | legacy-pump | 3 | 4 | 634.202 | 640.526 | 651.511 | 651.962 | 652.074 |
| loading / loading_event | Decode Texture | legacy-pump | 3 | 12 | 143.539 | 141.886 | 155.289 | 157.038 | 157.475 |
| loading / loading_event | Upload Mesh GPU | legacy-pump | 3 | 4 | 10.618 | 10.784 | 12.615 | 12.668 | 12.682 |
| loading / loading_event | Asset Main Thread Tasks | legacy-pump | 3 | 6839 | 0.028 | 0.000 | 0.003 | 0.006 | 70.734 |
| loading / loading_event | Job Render Transforms | legacy-pump | 3 | 6839 | 0.012 | 0.011 | 0.019 | 0.024 | 0.415 |
| loading / loading_event | Render Submit | legacy-pump | 3 | 6839 | 0.039 | 0.032 | 0.083 | 0.123 | 0.535 |

¹ Count — медиана числа полных вызовов/кадров на прогон. Max — медиана максимумов отдельных прогонов; абсолютный максимум каждого запуска сохранён в per-run.csv.

## Сравнение целевых зон

| Сцена / окно | Зона | Метрика | До, мс | После, мс | Снижение времени² |
|---|---|---|---:|---:|---:|
| ecs / steady | Render Prepare Transforms | mean_ms | 2.790 | 0.925 | 66.8% |
| ecs / steady | Render Prepare Transforms | median_ms | 2.686 | 0.916 | 65.9% |
| ecs / steady | Main Frame | median_ms | 9.851 | 9.184 | 6.8% |
| ecs / steady | Main Frame | p95_ms | 12.903 | 11.817 | 8.4% |
| ecs / steady | Main Frame | p99_ms | 14.400 | 13.654 | 5.2% |
| ecs / steady | Main Frame | max_ms | 21.740 | 20.552 | 5.5% |
| loading / steady | Main Frame | median_ms | 0.453 | 0.464 | -2.3% |
| loading / steady | Main Frame | p95_ms | 2.515 | 2.734 | -8.7% |
| loading / steady | Main Frame | p99_ms | 5.953 | 7.160 | -20.3% |
| loading / steady | Main Frame | max_ms | 3410.838 | 30.003 | 99.1% |
| loading / steady | Render Prepare Transforms | mean_ms | 0.012 | 0.011 | 4.0% |
| loading / steady | Render Prepare Transforms | median_ms | 0.011 | 0.011 | -2.4% |
| loading / loading_event | Main Frame | median_ms | 0.462 | 0.486 | -5.2% |
| loading / loading_event | Main Frame | p95_ms | 3.495 | 3.115 | 10.9% |
| loading / loading_event | Main Frame | p99_ms | 5.849 | 7.326 | -25.2% |
| loading / loading_event | Main Frame | max_ms | 3410.838 | 30.003 | 99.1% |
| loading / loading_event | Render Prepare Transforms | mean_ms | 0.012 | 0.012 | 0.7% |
| loading / loading_event | Render Prepare Transforms | median_ms | 0.011 | 0.011 | -3.1% |

## Кадры во время загрузки и длинные кадры

| Прогон | Полных кадров внутри загрузки | Загрузка, мс | Кадров >16,67 мс³ | Кадров >50 мс³ |
|---|---:|---:|---:|---:|
| ecs-before-01 | — | — | 6 | 0 |
| ecs-after-01 | — | — | 7 | 0 |
| ecs-after-02 | — | — | 4 | 0 |
| ecs-before-02 | — | — | 5 | 0 |
| ecs-before-03 | — | — | 4 | 0 |
| ecs-after-03 | — | — | 7 | 0 |
| loading-before-01 | 0 | 3445.825 | 2 | 1 |
| loading-after-01 | 852 | 1042.894 | 3 | 0 |
| loading-legacy-pump-01 | 710 | 2003.423 | 16 | 4 |
| loading-after-02 | 672 | 1127.987 | 6 | 0 |
| loading-legacy-pump-02 | 767 | 1105.976 | 5 | 1 |
| loading-before-02 | 0 | 3131.336 | 2 | 1 |
| loading-legacy-pump-03 | 860 | 1104.846 | 5 | 1 |
| loading-before-03 | 0 | 3409.461 | 2 | 1 |
| loading-after-03 | 738 | 1109.204 | 6 | 1 |

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
