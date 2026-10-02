# ЛР 1 — запуск и комплект защиты

Для знакомства с исходниками сначала откройте [путеводитель по проекту](../project-code-guide.md): карта файлов, путь кадра и ресурса, объяснение ECS/JobSystem и ключевые функции с прямыми ссылками.

Выбрана задача №2 **подготовка world-матриц внутри ECS RenderSystem**: она уже выполняется движком и не зависит от будущего физического middleware. Обязательная задача №1 — штатная загрузка PNG/OBJ через общий enkiTS JobSystem с GPU-пампом на главном потоке.

Комплект проверен: Release собран, CTest **4/4**, stress **6/6**, сохранены **15 Tracy-прогонов**. [Цифры и ограничения](results.md), [готовая презентация](../../reports/lab1-defense/TurboGameEngine-Lab1-final.pptx).

## Собрать и проверить

Из корня проекта, Windows PowerShell; нужны Visual Studio 2022, CMake ≥3.24 и Node.js:

```powershell
cmake --preset profile-release
cmake --build --preset profile-release --target app engine_job_system_tests engine_async_load_tests engine_transform_batch_tests engine_physics_tests
ctest --test-dir C:/tge/profile -C Release --output-on-failure
.\tools\lab1\New-BenchmarkAssets.ps1
```

Приложение: `C:\tge\profile\Release\app.exe`. Это Release с оптимизациями и debug symbols; `profile`/RelWithDebInfo остаётся для прежних материалов. Fixture: `C:\tge\lab1-assets`, 12 PNG 2048² и четыре OBJ. Не менять fixture между режимами.

## Показать живое демо

Из корня проекта одной командой:

```powershell
.\tools\lab1\Launch-Lab1Demo.ps1 -Scene loading -Mode after
```

На шестой секунде запрашиваются реальные файлы, placeholder сменяется загруженными ресурсами; счётчики и frame graph остаются в окне. Окно автоматически закрывается через 60 с; длительность задаётся `-Seconds 18`. После его завершения запустить задачу №2: `Launch-Lab1Demo.ps1 -Scene ecs -Mode after`. Режим `-Mode before` включает соответствующий последовательный вариант; `legacy-pump` доступен только для loading.

Для живого Tracy открыть GUI **0.13.1**, добавить к команде `-Tracy` и подключиться к локальному клиенту в течение 10 секунд после инициализации. Launcher восстанавливает переменные родительского PowerShell. Для итоговых данных использовать runner: он сохраняет стартовый marker и фиксирует диапазон.

## Записать доказательства

Из корня проекта:

```powershell
.\tools\lab1\Invoke-Lab1Measurements.ps1 -Scene all -Runs 3 -IncludeUnboundedPump
.\tools\lab1\Invoke-Lab1Stress.ps1
```

Каждый запуск пишет новый каталог `reports/lab1/<timestamp>`. Основной диапазон Tracy: `LAB_RUN_START +3…+14 с`; загрузка: `LAB_LOAD_START −1…+5 с`. В GUI выставить такой же **Limit range** и сравнивать `Main Frame`, `Render Prepare Transforms`, `Asset Main Thread Tasks`. Нельзя сравнивать разные сцены или суммировать worker durations как длительность кадра.

У исходного loader уже был свой пул. Поэтому синхронный before — управляемый эксперимент, а `legacy-pump` — отдельное снятие лимита финализаций на новом scheduler. Полная методика и значения параметров: [tools/lab1/README.md](../../tools/lab1/README.md).

## Материалы

- [Проверка перед публикацией](publication-checks.md).
- [Архитектура, lifetime и read/write-анализ](architecture.md).
- [Сценарий на три минуты](defense-script.md).
- [Вопросы и ответы для обоих участников](questions.md).
- [Приёмка: выполненное, ограничения и согласование MM](acceptance.md).
- [Краткие результаты с честными ограничениями](results.md).
- [Финальный отчёт: 15 запусков / N=3 на режим](../../reports/lab1/release-verified/measurements.md).
- [Stress: 6/6](../../reports/lab1/stress-final/stress-results.json), [CTest: 4/4](../../reports/lab1/validation/ctest-release.log).
- [Итоговый журнал проверки CLI и видимого демо](../../reports/lab1/validation/verification.md).
- [Презентация: 7 слайдов PPTX](../../reports/lab1-defense/TurboGameEngine-Lab1-final.pptx), [проверенное видимое демо](../../reports/lab1-defense/live-loading.png).

В комплекте сохранены `.tracy` всех прогонов, исходные CSV, `runs.json` с SHA-256 приложения, fixture manifest, таблица median/p95/p99 и целевых зон, stress-логи и презентация. [Матрица приёмки](acceptance.md) фиксирует проверку и ограничения. Внешнее согласование выбора задачи в MM остаётся за командой; готовый текст есть в матрице.

`reports/lab1/release-final` — сохранённая диагностическая серия с ошибкой доступа до исправления D3D12 descriptor capacity. Её не использовать как итоговые замеры; [причина и одинаковая настройка для A/B](architecture.md#готовность-d3d12-к-большой-сцене) описаны отдельно.
