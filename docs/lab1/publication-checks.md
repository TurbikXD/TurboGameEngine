# Проверка перед публикацией — 2 октября 2026

- Release-сборка `app` и всех четырёх test targets через `profile-release`
  завершилась успешно.
- После сборки CTest: **4/4 passed**, 5.65 секунды.
- `node tools/lab1/test-analysis.mjs`: PASS. Проверены offsets маркеров,
  R-7 quantiles, отсечение граничных/незавершённых зон, N=3 и отказ при
  отсутствии обязательного маркера.
- Все **195** ссылок путеводителя разрешаются в существующие файлы;
  ссылки преобразованы в переносимые GitHub relative links с `#L`.
- Проверка 189 текстовых файлов по типовым сигнатурам private keys,
  GitHub/OpenAI/AWS credentials: совпадений нет. Это ограниченная проверка,
  не гарантия обнаружения любого секрета. `.env` не читались и не включались.
- `git diff --check`: без ошибок whitespace.

SHA-256 Release-приложения:
`187B71457DA4B5F41401ED806D7A58A335A8F29F7D742298F8510E95BD59DB20`.
Он совпадает с hash сохранённой измерительной серии.

Новые performance measurements и stress-прогоны при публикации не выполнялись.
Публикуются исходные результаты **24 сентября 2026**, их ограничения
описаны в [results.md](results.md). Включены 15 Tracy captures, исходные CSV,
metadata, итоговые таблицы, исторические validation/stress logs, финальная
презентация и screenshot. CSV/Tracy bytes не нормализуются Git.

Build trees, exe/DLL/PDB, generated fixtures, failed diagnostic captures и
промежуточная презентация исключены. Существующее локальное удаление
`reports/5.pdf` не относится к этой публикации и не включено в коммит.
