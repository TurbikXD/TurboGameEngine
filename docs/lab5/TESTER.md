# ЛР 5. Станция «Маяк»

## Запуск

1. Откройте нужный релиз и скачайте из **Assets** файл `TurboGameEngine-Lab5-<buildId>-win64.zip`. [GitHub Releases](https://github.com/TurbikXD/TurboGameEngine/releases)
2. Распакуйте ZIP в пустую папку, доступную для записи. Требуются Windows 10/11 x64, Direct3D 11 и Microsoft Visual C++ Redistributable x64. На машине без подходящего GPU D3D11 переключается на программный WARP. [Visual C++ Redistributable](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist) · [D3D11 WARP](https://learn.microsoft.com/windows/win32/direct3darticles/directx-warp)
3. Двойным щелчком откройте `Play-Lab5.cmd`. Нажмите каналы `1`, `2`, `3` по порядку или выберите их мышью. В сцене меняется конфигурация маяка, а панель показывает результат `3/3`. Клавиша `Esc` и кнопка закрытия окна завершают игру штатно.

## Аварийные пробы

После калибровки выберите пробу и подтвердите действие кнопкой **Вызвать сбой**. Игра завершится с соответствующим кодом Windows.

| Проба | Код | Релиз |
| --- | --- | --- |
| Integer divide by zero | `0xC0000094` | v1 и v2 |
| Access violation | `0xC0000005` | v1 и v2 |
| Assertion failure | `0xC0000420` | v2 |

Отчёты находятся в `%LOCALAPPDATA%\TurboGameEngine\crash-reports`. Откройте этот путь через `Win+R`, выберите новую папку с `reportId` и передайте разработчику её целиком. В ней должны быть `report.json`, `engine.log` и `crash.dmp`. В `report.json` можно сразу прочитать buildId, время UTC, код исключения и последнее действие. Штатный выход завершается без новой папки отчёта.

## Три запуска на защите

1. Распакуйте v2 отдельно. Калибруйте каналы, проверьте доступность Assertion failure и закройте окно клавишей `Esc`. Проверьте папку отчётов.
2. Запустите v1. Калибруйте каналы, подтвердите Integer divide by zero и передайте разработчику новый отчёт. Он находит релиз по buildId и открывает дамп с PDB из того релиза.
3. Повторите пробу в v1. Сравните код, функцию и строку в двух отчётах. Папки отчётов имеют разные `reportId`.
