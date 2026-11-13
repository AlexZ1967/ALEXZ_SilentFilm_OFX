# Крэши My Presets: диагностика и исправление0.33

✅ **2026-11-13 — пользователь скопировал логи Resolve в `diagnostics/resolve-crash/`.** Они прочитаны локально. Файлы диагностик исключены из Git и не отправлялись вовне.

## Что показывает стек

В актуальном ResolveDebug.txt: Resolve Studio21.1.2, Signal11 после Save на Camera Optics. Верхняя прикладная цепочка:

```text
libProResRAW.so: std::filesystem::__cxx11::path::_M_split_cmpts()
ALEXZSilentFilm.ofx +0x33ea13: silentfilm::userPresetRoot()
ALEXZSilentFilm.ofx +0x3178fa: userPresetChanged(...)
ALEXZSilentFilm.ofx +0x3195c4: changed(...)
ALEXZSilentFilm.ofx +0x31ec00: entry<CameraOptics>(...)
```

Имена функций установлены read-only разбором текущего `build-cuda/.../ALEXZSilentFilm.ofx` через addr2line. Release binary не содержит точных source lines. ELF также показывает импорт функций std::filesystem из C++ runtime. В ранних LogArchive/ResolveDebug_C1/C2/C3 — тот же вызов `_M_split_cmpts` из libProResRAW. Это объясняет общий подозрительный участок как для CreateInstance0.31, так и для Save0.32: оба обращались к userPresetRoot.

**Стек показывает межбиблиотечное разрешение C++ filesystem-символа в поставляемую Resolve libProResRAW.** Это указывает на ABI/symbol-interposition конфликт. Крэш происходит при построении пути библиотеки, до записи файла, поэтому отсутствие папки ожидаемо. Причина не установлена в Unicode-имени; оно не участвует в этом конструкторе пути. Прежнее предположение о перестройке Choice как причине именно этих падений стеком не подтвердилось. Защитная0.32 не устраняла filesystem-вызов Save.

## Что изменено

- `std::filesystem` удалён из adapter и пути хранения пользовательских OFX-пресетов: пути передаются UTF-8 строками, операции — POSIX mkdir/stat/opendir/readdir/open/read/write/rename/unlink. Формат `.sfpreset` v1 не менялся; Unicode и atomic overwrite сохранены.
- Файловый storage вынесен в `src/platform/user_presets.cpp`/отдельный target `silentfilm_preset_storage`. Вычислительное ядро не получает POSIX-зависимость. Это текущая Linux-реализация, не заявление о проверке Windows/macOS.
- UI не изменяет ChoiceOption после Describe. Выбор существующего пресета — стандартный **Preset File** с кнопкой обзора; **Available Presets** — текстовый каталог по Refresh. Имя можно вводить вручную. Скрытый статический `user_preset` оставлен только для совместимости ID0.31–0.32. Save не сканирует библиотеку и не перестраивает UI-список; после записи назначает путь/статус.
- Save читает значения по известной типизированной схеме без host parameter property introspection. Apply сохраняет preflight/range/key checks; parameter properties запрашиваются явно с проверкой null. Это упрощение, а не доказанная причина крэша из логов.
- Добавлены flush-traces стадий UI-сохранения в stderr/ResolveDebug, без кадрового логирования Render. CMake и художественные OFX0.33.

## Подготовленные проверки

`silentfilm_user_presets`: storage, UTF-8, типы, точность, повреждённые файлы/чужой эффект, symlinks, file-picker path. `silentfilm_ofx_preset_lifecycle`: настоящий adapter с host double, полный Save Camera Optics с русским именем, Apply/Undo-group/key rejection, reentrant callbacks, отсутствие runtime Choice mutation и host metadata reads на Save. `silentfilm_preset_abi`: Linux executable намеренно экспортирует тот же mangled filesystem-символ, но аварийно завершится при его вызове; ASCII/Unicode Save/Load/List должны обходиться без него.

**Агент не запускал ни эти тесты, ни сборку, ни Resolve.** Выполнены только чтение исходников/логов и символикация уже существующего ELF. Нового PASS и подтверждения устранения крэша0.33 нет.

```bash
cd /home/alex/Projects/ALEXZ_SilentFilm_OFX
cmake --build build-cuda --parallel 2 &&
ctest --test-dir build-cuda --output-on-failure
```

После успешных проверок при закрытом Resolve:

```bash
sudo cp -a build-cuda/ofx/ALEXZSilentFilm.ofx.bundle /usr/OFX/Plugins/
```

Открой прежний проект. На Camera Optics задай имя `Моя оптика`, Save: ожидаются файл и путь в Preset File, статус «Сохранён». Измени настройки/Apply, затем отдельно Refresh и выбор файла через кнопку обзора. Проверь Undo и повторное открытие проекта. Если падение повторится — скопируй свежие логи в ту же diagnostics-папку, не вызывая повторный крэш специально.
