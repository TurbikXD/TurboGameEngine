# Материалы защиты TurboGameEngine, ЛР 2

Основной вариант - [интерактивная презентация-игра](../../../demos/lab2-defense/README.md) в собственном движке. Проект: `demos/lab2-defense/Lab2Defense.tgeproj`; открыть через Projects и нажать Play. Для живого показа механики подготовлен отдельный [Collectathon](../../../demos/lab2-collectathon/README.md).

Резерв: [13 слайдов PPTX](output/TurboGameEngine-Lab2.pptx), [слайды PDF](output/TurboGameEngine-Lab2-slides.pdf), [технический отчёт PDF](output/TurboGameEngine-Lab2-report.pdf). В PPTX редактируются текст, таблицы и график с вложенной таблицей данных; speaker notes содержат источники. PDF слайдов собран из проверенных рендеров, PDF отчёта содержит выделяемый текст.

## Что читать

- [Доклад, демонстрация на 3 минуты и ответы](defense-notes.md).
- [Архитектура ЛР 2: файлы, вызовы и потоки данных](../architecture.md).
- [Технический отчёт](technical-report.md): требования, архитектура и все затронутые подсистемы.
- [Память и стабильность](memory-report.md): исходный дефект, повторные прогоны и пределы выводов.
- [Матрица фактов](implementation-evidence.json): source paths, строки, тесты и ограничения по каждому пункту ТЗ.
- [Полный Git-реестр](git-snapshot.json): tracked/untracked файлы, изменения, удаления, HEAD/upstream и локальные неотправленные коммиты.
- [Итог проверок](verification.json), сырые CTest/build/runtime логи и `evidence/`.

`content.json` - общий текст тем для игры и слайдов. После его изменения выполнить `python demos/lab2-defense/sync_content.py`. Статусы в старых логах относятся к их времени и бинарнику; финальная сводка указывает актуальные артефакты.

Генераторы: `tools/build_lab2_report.py`, `tools/build_lab2_deck.mjs`, `tools/summarize_lab2_verification.py`. JS использует предоставленный Artifact Tool runtime; PDF отчёта - ReportLab. Локальные пути зависимостей задаются через `TGE_ARTIFACT_MODULE`, `TGE_PRESENTATION_SKILL` и `TGE_ARTIFACT_PYTHON`. Для повторного экспорта PPTX установить новое имя `TGE_DECK_NAME` (без расширения), сохранив проверенную версию.

## Граница утверждений

Технические функции подтверждены рабочим деревом на момент подготовки материалов, включая тогда ещё незакоммиченные файлы. Наличие теста в исходниках не означает его успешный запуск: результаты запусков указаны отдельно. `git-snapshot.json`, отчёт и слайды сохраняют историческое состояние перед публикацией: сравнение с `origin/main` в том снимке использовало локальные tracking refs без fetch. Последующие commit/push не переписывают этот снимок; актуальное состояние репозитория проверяется через Git. Отдельный `commit-validation.log` содержит повторный CTest перед коммитом.

Ignored local artifacts перечислены как отдельная категория: REA-исследования, локальные reference fonts, личные CMake/IDE-настройки. Временные зависимости/рендеры `.build/` и копии нагрузочных fixtures исключены из Git-инвентаря. Предсуществующее удаление `reports/5.pdf` сохранено.

## Повторение проверок

Пользоваться CMake из существующего cache/CLion и не запускать параллельные configure одного binary dir. Выбирать конфигурацию явно.

```powershell
cmake --build C:/tge/clion-vs2022b-rel --config Debug --target engine_scripting_tests engine_react_runtime_tests engine_gameplay_state_tests engine_script_editor_tests --parallel 4
ctest --test-dir C:/tge/clion-vs2022b-rel -C Debug --output-on-failure
python demos/lab2-defense/check.py --luau build/react-check/luau/luau.exe
python tools/run_lab2_stability.py --app C:/tge/profile/Release/app.exe --output docs/lab2/defense/memory-windows-new-run --seconds 300
```

Последняя команда создаёт собственный новый fixture и не меняет пользовательские проекты. Windows process counters не являются detector утечек. Инструментированный Linux headless-проект и воспроизводимые команды находятся в `tools/verify_lab2_memory/`; он проверяет scripting/React CPU, а не D3D12.

## Источники о CLion

Установленная версия и фактическая GUI-ошибка записаны локально. Документация JetBrains: [CPU Profiler](https://www.jetbrains.com/help/clion/cpu-profiler.html), [Valgrind Memcheck](https://www.jetbrains.com/help/clion/memory-profiling-with-valgrind.html). Онлайн-справка может относиться к более новой версии; вывод о текущем toolchain основан на реальном запуске в установленной IDE.
