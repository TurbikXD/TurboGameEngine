# ЛР 1: комплект и честная матрица приёмки

Выбор задачи №2: **распараллеливание внутри ECS RenderSystem: snapshot entity ID → dispatch диапазонов world-matrix preparation → join → main-thread GPU submit**. Задача соответствует каталогу и существующей архитектуре на hash-таблицах. Она не зависит от сохранения собственной физики в ЛР 4.

## Согласование MM

Требование ТЗ «по согласованию в чате MM» не заменяется готовым кодом. Внешнее согласование пока не подтверждено. Готовый текст для самостоятельной отправки:

> Для ЛР 1 выбираем задачу №2 «распараллеливание внутри одной ECS-системы». В TurboGameEngine переносим существующую подготовку мировых матриц RenderSystem: snapshot видимых entity ID, read-only Transform/Hierarchy, независимые выходные диапазоны, join перед GPU submit. Фиксированная сцена — 4096 объектов. Обязательная задача №1 — миграция штатной async-загрузки текстур/моделей на общий enkiTS JobSystem с main-thread GPU pump, placeholder и корректным shutdown. Просим согласовать выбор.

## По коду

| Требование | Реализация / доказательство | Статус |
|---|---|---|
| Допустимая job system | enkiTS, единый lifecycle Application, общая постановка | Release собран; CTest 4/4 |
| L1: фоновые file+decode | stb_image / Assimp через `Priority::Low`; прежний asset-пул удалён | Реализовано |
| L1: GPU pump | Один вызов за кадр, count=1 и nominal 2 мс | Реализовано; отдельный upload может превысить бюджет |
| L1: placeholder/error | Placeholder mesh, white texture, fallback shader; Failed + lastError | Видимое демо: 18 Ready / 0 Failed; missing-resource stress пройден |
| L1: выход с живыми задачами | Cancel backlog, join CPU, cancel uploads, затем GPU destruction | Unit + live stress пройдены; при выходе зафиксировано 16 pending CPU |
| Реальная задача №2 | World matrices существующего render path, 4096-сцена | Реализовано; не синтетический sleep/повтор цикла |
| Зависимости доступа | Таблица фаз read/write в [architecture.md](architecture.md) | Описано; runtime conflict detector не заявляется |
| Замеры Release, прогрев, N≥3 | Runner, alternating mode order, одинаковые ranges/fixtures | 15/15 запусков, app/capture exit code 0; по 3 на каждый режим |
| Median/p95/p99 + целевые зоны | CSV-анализ; диапазоны относительно Tracy messages | [Результаты и ограничения](results.md), исходные данные сохранены |
| Артефакты before/after | `.tracy`, полные CSV, run manifest, таблица | [Комплект release-verified](../../reports/lab1/release-verified/measurements.md) |
| Live demo + презентация | [Сценарий 3 минуты](defense-script.md), [ответы](questions.md) | Видимое окно проверено; [7 слайдов PPTX](../../reports/lab1-defense/TurboGameEngine-Lab1-final.pptx) подготовлены и проверены |

## Допфичи без завышения

Реализованы общий work-stealing scheduler (библиотека enkiTS), приоритеты, зависимости `dispatchAfter`, наблюдаемость Tracy (workers/dispatch/wait/queue plots), backpressure loader и несколько потребителей задач. Наличие функций можно показать в коде; баллы определяет преподаватель.

Не заявляются собственные lock-free/fibers, L2 выделенный streaming-пул, текстурный streaming по mip, L3 copy queue/fence и runtime-анализ read/write. Физические stages сохранены как дополнение, но их прежние цифры не подменяют новый эксперимент по выбранной задаче №2.

## Реальные проверки и результаты

Проверено 24 сентября 2026:

- Сборка приложения `profile-release` завершена успешно.
- Повторный CTest: **4/4**, 5,78 с; physics, job system, async loading и transform batches. [Лог CTest](../../reports/lab1/validation/ctest-release.log).
- Stress: **6/6**. Отсутствующие ресурсы, выход с живой загрузкой, 60-секундная сессия, три повторных старта. При раннем выходе журнал подтвердил `pending_cpu=16`, `pending_uploads=0`; процесс завершился штатно с кодом 0. [JSON и ссылки на сценарии](../../reports/lab1/stress-final/stress-results.json).
- Все **15** измерительных запусков завершены с кодами app/capture 0: ECS before/after и loading sync/async/legacy-pump, N=3 каждый. [Результаты](results.md), [manifest](../../reports/lab1/release-verified/runs.json), [полная таблица](../../reports/lab1/release-verified/measurements.md). Серия `release-final` содержит диагностическую ошибку доступа до исправления ёмкости D3D12 descriptor heap и не является итоговой.
- Проверено отдельное видимое loading-окно: [18 Ready / 0 Failed](../../reports/lab1-defense/live-loading.png). Измерительные процессы работали в одинаковом hidden-режиме; их CPU frame time не выдаётся за display latency/FPS.
- [Итоговый журнал дополнительной проверки](../../reports/lab1/validation/verification.md) фиксирует CLI и видимое демо; завершённая ручная проверка Tracy GUI не заявляется.
- [Финальная презентация](../../reports/lab1-defense/TurboGameEngine-Lab1-final.pptx): 7 слайдов, 5 редактируемых таблиц; пройдены структурная проверка, layout, повторный импорт и визуальная проверка каждого слайда. Проверка в нативном PowerPoint не заявляется.

В измерениях ECS mean целевой зоны уменьшился **2,790 → 0,925 мс**. Во время async-загрузки между маркерами продолжались кадры; секундная sync-блокировка устранена. При этом async p99 вырос **5,849 → 7,326 мс**, один кадр достиг **50,570 мс**. Поэтому не заявляется отсутствие всех фризов. Методические ограничения и полные числа — в [results.md](results.md).

Тесты и stress подтверждают выполненные сценарии, но не являются формальным доказательством отсутствия всех гонок; ThreadSanitizer не запускался. Единственное оставшееся внешнее действие — согласовать выбор задачи №2 в MM и договориться о приёмке.

Команды повторения и формат исходных данных: [tools/lab1/README.md](../../tools/lab1/README.md). Старые одиночные физические замеры из предыдущего этапа не удовлетворяют новому полному ТЗ и не используются как итоговое доказательство.
