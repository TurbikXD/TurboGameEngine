-- Questions describe the actual host boundary, not unsupported Roblox services.
return {
    { prompt = "Кто владеет ECS, GPU-ресурсами и жизненным циклом VM?", answers = { "C++ ядро", "Luau поведение", "HTML браузер" }, correct = 1 },
    { prompt = "Где находятся правила этой игры, квиз и счёт?", answers = { "В шейдере", "В Luau поведении", "В Roblox DataModel" }, correct = 2 },
    { prompt = "Что реально предоставляет RobloxUI в этом движке?", answers = { "Все сервисы Roblox", "Браузерный DOM", "Явное подмножество UI Instance/Enum" }, correct = 3 },
}
