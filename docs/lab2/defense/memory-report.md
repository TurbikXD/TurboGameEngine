# Память и стабильность: что проверено

Дата: 16.10.2026. Все исходные отрицательные результаты сохранены. Итоговая сводка: verification.json.

## Обнаруженный дефект и исправление

В первом Release/D3D12 прогоне примерно через 149 секунд возник OOM Luau. Очистка behavior в forgetEntity стирала pendingDestroy. Когда ранний Update запрашивал удаление позднего экземпляра, его VM cleanup отменял ECS destroy. На следующем шаге живой ScriptComponent снова получал Init и создавал новые объекты. Это runaway lifetime bug, а не доказанная утечка native/GPU allocator.

Fix: освобождение instance не снимает запрос уничтожения. flushDestroy применяет его на безопасной границе. Добавлен regression с настоящими Collectathon/HUD; без fix сценарий нарушает bounded world/root counts. Принудительный GC не используется как способ скрыть ошибку.

## Итоги

Повторный Windows CTest после исправления: 19/19 PASS за 35.12 с. Новый regression использует настоящие prefab и React HUD, проверяет число сущностей/UI roots и невалидность старого handle. Оптимизированный app пересобран; SHA-256 указан в verification.json.

Windows после fix: 303.1 с, exit=0; ошибок Luau 0, применённых reload batches 30. Диапазон Luau по секундным samples [3.14, 7.65] MiB; диапазон сущностей [5, 18]. После прогрева Private Bytes: 628.54-630.03 MiB; линейный тренд 0.219 MiB/min. Это наблюдения сценария, не универсальный тест отсутствия утечек.

Linux ASan/LSan/UBSan: 11/11 успешных процессов: scripting 5 раз, React 5 раз и same-VM lifecycle с двумя фазами по 120 циклов. Диагностик sanitizer нет, suppressions нет. В normal-GC фазе live world/root counts ограничены; перед принудительным GC 6,814,692 bytes, после 3,527,060 bytes при baseline 3,476,060 bytes. После Stop/reset roots=0, events=0; после shutdown VM=0 в обеих фазах.

Для инструментированной React test-copy общий batch watchdog увеличен до 1000 ms из-за sanitizer overhead; оригинальный production guard не менялся, отдельный тест 5 ms сохранён. Byte-identical assets скопированы на Linux FS, чтобы WSL NTFS I/O не расходовало watchdog. Исходные неуспешные попытки с неизменённым timing сохранены в memory-linux/ и local-assets/.

Native-презентация прошла квиз, реальный prefab/Transform и disk reload с counter=1, Init=1, OnReload=1; скриншоты в evidence/. Первоначальная ошибка require без .luau исправлена, headless loader теперь проверяет точные имена. Файлы правил возвращены к A после проверки.

CLion Profile app был действительно запущен, но capture недоступен для текущего Windows/MSVC toolchain. Это точный блокер запрошенного CLion-профиля; альтернативные прогоны не называются результатом CLion. Настройки IDE и системные пакеты не менялись.

## Сравнение Windows

| Метрика | До fix | После fix |
| --- | --- | --- |
| Длительность | 304.2 с | 303.1 с |
| Ошибки Luau / OOM | 9371 / 9340 | 0 / 0 |
| Private MiB/min после прогрева | 12.679 | 0.219 |
| Handles после прогрева | [410, 766] | [734, 751] |

Одинаковая основа сценария: создание/уничтожение Collectathon+HUD каждые 3 с, disk reload driver каждые 10 с; Release/D3D12, 1280x720, vsync, 300 с. Во втором запуске включены дополнительные frame diagnostics и два status counters в собственном driver. Сравнение FPS/времени CPU не заявляется. Секундная выборка может пропустить краткие состояния между кадрами.

В raw summary первого прогона print-счётчики равны 0: print остаётся в editor Output и не попадает в stdout. Это не число выполненных циклов. Интерпретация исправлена в verification.json; оригинальный summary не переписан. Exit=0 первого запуска также не означает PASS, поскольку runtime содержал ошибки.

## Файлы и повторение

- Windows: memory-windows-run1/ и run2/: runtime.log, samples.csv, summary.json; app SHA-256 связывает результат с бинарником.
- Linux: memory-linux/post-fix/: summary.json, lifecycle.json, instrumentation.json и каждый test log.
- CLion: evidence/clion-profiler-unavailable.jpg; profiling-capabilities.json.
- Команды: tools/run_lab2_stability.py и tools/verify_lab2_memory/README.md.

## Пределы вывода

- CPU headless Linux sanitizers do not instrument the Windows D3D12 binary or GPU/driver.
- Finite exercised scenarios do not prove all engine paths free of leaks or races.
- Reachable retention during a session is checked separately from LSan exit-time leaks.
- Windows counters are process observations, not heap allocation stacks.
- No CLion CPU/Valgrind capture was produced; the installed Windows toolchain is unsupported.

Корректный вывод: найденный дефект устранён; перечисленные тесты и ограниченные прогоны завершились без обнаруженных ошибок. Утверждение «в движке вообще нет утечек» этими данными не обосновано.
