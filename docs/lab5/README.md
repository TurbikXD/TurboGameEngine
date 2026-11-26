# ЛР 5. Упаковка релиза и разбор сбоев

[Инструкция испытателя](TESTER.md) входит в игровой ZIP. [Сцена «Маяк»](../../demos/lab5-beacon/Lab5Beacon.tgeproj) открывается в редакторе. Релизный запуск через `Play-Lab5.cmd` включает подтверждаемые аварийные пробы после калибровки.

## Сборка и публикация

Workflow на push собирает `app` в Windows x64 `RelWithDebInfo`, формирует игровой ZIP и отдельный ZIP с PDB. Конфигурация поставки читает шейдеры, шрифты и React Luau из пакета. Код движка и Diligent слинкован статически, поэтому финальный `app.pdb` содержит символы поставляемого кода. В обоих ZIP размещается одинаковый `build.json` с идентификатором `GITHUB_RUN_ID.GITHUB_RUN_ATTEMPT`, SHA проверенного коммита и ревизией игры. Исполняемый файл возвращает ту же запись по `--build-info`. [Переменные GitHub Actions](https://docs.github.com/en/actions/reference/workflows-and-actions/variables)

На теге `lab5-v*` workflow публикует проверенные ZIP в GitHub Release, связанный с этим тегом. Описание содержит buildId, SHA и ссылку на конкретную попытку Actions. Повторная сборка получает новый buildId. Существующий релиз сохраняет исходные бинарники и символы. Ревизия 1 распространяется под тегом `lab5-v1.0.0`, ревизия 2 под `lab5-v2.0.0`. Между ревизиями меняется компилируемая версия `Lab5Session`, которая открывает третью пробу.

При первом выпуске сначала отправьте рабочую ветку с workflow. Когда Actions покажет запуск сборки ветки, отправьте тег выпуска.

С `lab5-v2.1.0` официальный Win64 ZIP включает HTTPS-адрес для **Send Report**. Эта сборка сохраняет игровую ревизию 2 с тремя аварийными пробами. Отчёты доступны разработчику в приватном хранилище по `reportId`. При своей упаковке адрес можно задать параметром `-CrashReportUrl` или переменной Actions `TGE_CRASH_REPORT_URL`. [Настройка приёмника и отправки](../crash-diagnostics.md#send-report)

Локальный прогон из чистого коммита выполняется в PowerShell 7 с установленными Visual Studio 2022 и Windows SDK. Для следующего прогона выберите новый buildId и новый каталог вывода.

```powershell
git submodule update --init --recursive
$sha = (git rev-parse HEAD).Trim()
$id = 'local-' + [DateTime]::UtcNow.ToString('yyyyMMddHHmmss')
$build = "C:/tge/lab5-$id"
cmake -S . -B $build -G 'Visual Studio 17 2022' -A x64 `
  -DENGINE_PACKAGED_RUNTIME=ON -DENGINE_BUILD_DILIGENT_SAMPLES=OFF -DENGINE_ENABLE_TRACY=OFF `
  -DDILIGENT_NO_DIRECT3D12=ON -DDILIGENT_NO_OPENGL=ON -DDILIGENT_NO_VULKAN=ON `
  "-DENGINE_BUILD_ID=$id" "-DENGINE_BUILD_COMMIT=$sha"
cmake --build $build --config RelWithDebInfo --target app --parallel 4
$package = ./tools/lab5/New-Lab5Package.ps1 -BuildDirectory $build -OutputDirectory "out/$id"
./tools/lab5/Test-Lab5Package.ps1 -GameZip $package.GameZip -SymbolsZip $package.SymbolsZip `
  -EvidenceDirectory "out/$id"
```

В `verification.json` записываются три откалиброванных канала, штатный выход без отчёта, идентификаторы и коды аварийных запусков, а также проверки отсутствующего ресурса и дампа. Интеграционный тест распаковывает оба ZIP во временный каталог, запускает реальные процессы и читает потоки исключения и аварийного потока из каждого minidump. `-CheckStack` запускает также проверку стека, когда установлен CDB.

## Поиск исходников и проверка стека

1. Прочитайте `buildId` и `commitSha` из `report.json`. Найдите релиз с **этим** buildId, указанным в описании и именах ZIP. Для сборки Actions первая часть buildId указывает на номер прогона в разделе **Actions**, вторая на его попытку.
2. Скачайте и отдельно распакуйте оба ZIP этого релиза. Откройте исходники по полному SHA коммита из `report.json`. Путь в PDB может указывать на папку сборочной машины. Файл `app/Lab5Session.cpp` в нужном коммите содержит указанную отладчиком строку.
3. Установите компонент **Debugging Tools for Windows** из Windows SDK или укажите `-Cdb` с путём к x64 `cdb.exe`. Запустите проверку в PowerShell 7. Для системных модулей с другой версии Windows можно передать `-SystemSymbolCache C:/tge/symbol-cache`, чтобы CDB загрузил их с сервера Microsoft. [Debugging Tools for Windows](https://learn.microsoft.com/windows-hardware/drivers/debugger/debugger-download-tools)

```powershell
./tools/lab5/Test-Lab5Stack.ps1 `
  -ReportDirectory 'C:/Lab5/report-v1' `
  -GameDirectory 'C:/Lab5/game-v1' `
  -SymbolsDirectory 'C:/Lab5/symbols-v1'
```

`LAB5_STACK_OK` содержит код исключения, ID потока, функцию и исходную строку. CDB устанавливает контекст исключения командой `.ecxr` и выводит стек с включёнными номерами строк. Подстановка `-SymbolsDirectory 'C:/Lab5/symbols-v2'` при том же отчёте и игровом пакете завершает проверку ошибкой. [Контекст исключения в WinDbg и CDB](https://learn.microsoft.com/windows-hardware/drivers/debuggercmds/-ecxr--display-exception-context-record-)

Для ручного разбора откройте `crash.dmp` в WinDbg. Укажите распакованный игровой каталог командой `.exepath`, каталог PDB командой `.sympath`, затем выполните `.reload`, `.exr -1`, `.ecxr` и `kn`. Первая пара проб использует `RaiseException` с фиксированными кодами Windows. Третья сначала пишет сообщение об assertion в лог. Все три проходят обычный обработчик необработанного исключения движка.

## Разбор отчёта

Движок сохраняет `crash.dmp` через DbgHelp с регистрами и ID аварийного потока. `engine.log` относится к конкретному запуску, включая параллельные окна Play. `report.json` появляется после попыток записать лог и дамп. Если какой-либо файл недоступен, результат виден в `dumpWritten`, `dumpError` и `logWritten`. Переменная `TGE_CRASH_REPORT_DIR` переопределяет каталог при указании абсолютного пути. Отчёты сохраняются локально. [MiniDumpWriteDump](https://learn.microsoft.com/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump)

На Windows native SEH передаёт исходный контекст исключения. Обработчики CRT abort и необработанных C++ исключений создают диагностический SEH-контекст при записи. Завершение процесса извне и некоторые низкоуровневые отказы могут произойти до работы обработчика. Минидамп и лог могут содержать локальные пути и фрагменты памяти процесса. Передавайте отчёт разработчику напрямую.
