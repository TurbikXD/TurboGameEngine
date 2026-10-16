# ЛР 2: отдельный Collectathon

В Release `app.exe` выберите **Projects → Open Existing Project →
Lab2Collectathon.tgeproj**. Проект открывается в **Edit** с выделенным
`Collectathon Controller`, Explorer и Properties. Собственная сцена и ресурсы
не изменяют Engine Demo, презентацию или пользовательские проекты.

Камера `[0,17,-17]` и Ground `[0,-2.5,8]` взяты из
`tools/run_lab2_stability.py`. Здесь нет SoakDriver и автоматического рестарта.
Скрипты и четыре префаба — копии настоящего примера из `assets/`.

Короткий сценарий защиты:

1. **До Play** поменяйте в Properties `pointsPerToken`, `goal` или `duration`;
   нажмите **Save Scene**. Для спокойного показа заданы `duration=180`, `goal=20`.
   Сцена сохраняет поля; исходный prefab controller сохраняет штатные defaults.
2. **Play in Editor** или **Play Window**. Во Viewport управляйте синей сферой
   **WASD / стрелками**, собирайте золотые кубы. **R** начинает новую сессию.
   React HUD показывает настоящий счёт, таймер, цель, результат и useState-кнопку.
3. Во время Play откройте `scripts/Collectathon.luau`. В строке начисления очков
   замените `self.state.score += self.fields.pointsPerToken` на
   `self.state.score += self.fields.pointsPerToken * 2`, затем **Save**.
   Вернитесь из Script Workspace во Viewport и соберите следующий предмет:
   прибавка удвоится; текущий счёт и таймер сохранятся. **Stop не нужен**.
4. Чтобы повторить исходный сценарий, верните множитель и сохраните файл.
   **Stop** восстанавливает сцену до Play; изменения файлов Luau остаются на диске.

`duration` применяется к новой сессии: после его изменения в Play нажмите **R**.
Игра обрабатывает ввод, когда Viewport принимает клавиши; поля кода/Properties
не должны иметь текстовый фокус. В Play Window редактируйте файл в основном
редакторе и возвращайтесь в игровое окно. **Esc** закрывает отдельный Play.

Проверка комплекта без запуска GPU:

```powershell
python demos/lab2-collectathon/verify.py --compile build/react-check/luau/luau-compile.exe
```

Это проверяет JSON, собственные пути, сцены/префабы, ресурсы и компиляцию трёх
Luau-файлов. GUI, поведение native ECS и реальный hot reload проверяются отдельно
в Release; этот проект не содержит заявлений о FPS или отсутствии утечек.
