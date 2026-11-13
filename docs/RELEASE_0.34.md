# SilentFilm0.34 — Defects и пользовательские OFX-пресеты

✅ **2026-11-14 — пользователь подтвердил: «всё проверил, всё работает»**, разрешил commit/push. Подтверждение относится к последнему набору предложенных проверок0.34, включая рабочий выбор/сохранение/применение пресетов после устранения крэшей. Полный новый лог/число тестов и отдельные результаты независимого Comfy-сравнения не предоставлены. Агент сборки/тесты/Resolve не запускал.

## Изменения

- Первый блок Defects: Surface/Emulsion loss/Dirty, эллиптические неровные dust/stains, scratch path/gaps, Irregularity, покадровая пыль на плёнке и события Persistence. Общий CPU/CUDA-код, один fused GPU pass; Legacy сохраняет прежний вид. Workflow presets назначают модели и Physical response. [Подробности](DEFECTS_COMFY_0.30.md).
- My Presets во всех художественных OFX: имя, активный Saved Presets, Save/Overwrite, Apply, Refresh, выбор файла. Библиотеки раздельны по типам эффектов и общие между проектами. Snapshot текущего кадра, preflight/Undo/rollback, защита ключей анимации. Crank Exposure сохраняет только Blend, без клиповых карты/offset. [Инструкция](OFX_USER_PRESETS.md).
- Устранён конфликт filesystem-пути пресетов с libProResRAW Resolve: UTF-8 строки и отдельный POSIX storage target вместо std::filesystem в этом пути. Логи показали userPresetRoot→_M_split_cmpts из libProResRAW; пользователь подтвердил сохранение без крэша. [Разбор](OFX_PRESET_CRASH_0.33.md).
- Обновлены тесты Defects CPU/CUDA и workflow checker, добавлены независимое сравнение масок с Comfy, storage/lifecycle/ABI trap regressions. Диагностические логи исключены из Git.

## Что дальше

Продолжить Defects: bicubic development с mottling/streaks/mixed и fog_strength; затем mixed surface layers, волоски и softness тени Projector Gate. RNG остаётся native, Edge Bias намеренно только у боковых краёв. Полное совпадение повреждённой цепочки с Comfy пока не заявляется. Отдельные исторические проверки Bake/OFX и завершения с Glow не считаются автоматически закрытыми этим подтверждением.
