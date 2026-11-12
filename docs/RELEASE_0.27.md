# Срез 0.27 — камера, плёнка, Defects и экспозиция Cadence

✅ **2026-11-13 — оформлены накопленные изменения0.18–0.27 по запросу пользователя на коммит и публикацию.** Целевая проверка — Linux x86_64 / установленный Resolve Studio21; работа Windows/macOS не подтверждена. Python/PyTorch Comfy и установленные SDK не изменялись.

✅ **2026-11-13 — опубликован основной коммит [08b4b9a](https://github.com/AlexZ1967/ALEXZ_SilentFilm_OFX/commit/08b4b9a) в main.** Успешная отправка подтверждена Git. Это публикация подготовленного к проверке среза, не новый PASS тестов или Resolve.

## Что изменено

| Этап | Подготовленная реализация | Документация |
| --- | --- | --- |
| 0.18 | Camera Optics: виньетка, краевой/общий расфокус и нормализованный гуляющий фокус; уточнение area/round в CPU/CUDA | [Оптика](CAMERA_OPTICS.md), [старый Camera](CAMERA_LEGACY.md) |
| 0.19 | Пресеты заданного Comfy workflow; зерно в плотности Negative/Print; пространственная CUDA-эмульсия и halation | [Соответствие workflow](COMFY_WORKFLOW_MATCH.md) |
| 0.20–0.24 | Defects: один CUDA kernel, общий precompute, Blend/Perceptual/Physical, разброс размера/толщины, боковые края, гуляние царапин и изменение количества во времени | [Defects](DEFECTS_PERFORMANCE.md) |
| 0.23 | Общий sRGB RGB-контракт; физическое пропускание между Negative/Print; Advanced Encoding и Color-only preset | [Цвет](COLOR_PIPELINE.md) |
| 0.25 | Ограниченный FFT-кэш, очистка без нового потока, восстановление CUDA context, PurgeCaches CPU/GPU и диагностика завершения | [Память и выход](OFX_LIFETIME.md) |
| 0.26 | Bake Exposure / Exposure via OFX, точная карта выходных кадров, отдельный SilentFilm Crank Exposure с Blend/Offset/Reload, CPU/CUDA | [Экспозиция Cadence](CADENCE_EXPOSURE_OFX.md) |
| 0.27 | Назначение карты через новую Fusion-композицию результата, readback параметров/соединений, rollback/fallback и повторный Import без нового клипа | [Автопривязка и проверки](CADENCE_EXPOSURE_OFX.md) |

У пыли, царапин и пятен **Edge Bias смещает только к левому/правому краю**, без скопления сверху/снизу. Defects-пресеты остаются mapped: полная математическая эквивалентность всем Comfy damage types не заявляется. Существующие ноды со старым Encoding не перенастраиваются автоматически; Color-only preset не заменяет CST на границе цепочки.

В Exposure via OFX приложение оставляет экспозицию ручки незапечённой и сохраняет `exposure.sfexposure` рядом с результатом. Основной автоматический маршрут0.27 — **MediaIn → SilentFilm Crank Exposure → MediaOut на странице Fusion**, карта этого задания, Blend1/Frame Offset0. Сам OFX/формат карты остаются0.26; изменения0.27 относятся к Python GUI/bridge. При отказе API предусмотрен ручной вариант; частичный новый граф удаляется, пользовательские Fusion-графы не заменяются. Прямой Resolve Effect не получил выдуманного setter параметров. Карты консервативно защищены от автоматической очистки, включая задания без видимого media usage, поскольку все file references OFX недоступны API.

## Подтверждения и открытые проверки

✅ **2026-11-12 — пользователь сообщил об успешных тестах0.19.** Полного нового лога/числа тестов нет; независимое полное совпадение всей цепочки с Comfy не подтверждено.

✅ **2026-11-12 — пользователь подтвердил работу после согласования цвета0.23 («всё работает»).** Это общее ручное подтверждение, а не полный отчёт по каждому последующему изменению.

**Сборки, тесты, синтаксические проверки, Resolve и замеры агент не запускал.** Для0.24–0.27 новых результатов пользователя пока нет. Подготовленные тесты не помечаются PASS. Долгий/аварийный выход с Glow Projector остаётся симптомом для повторной проверки после0.25, его устранение пока не подтверждено. Автоматические Fusion registry/input IDs, реальный цветовой контракт, FPS/trim/move, GPU-путь Fusion и групповое применение карты требуют проверки в установленном Resolve.

## Команды пользователя

```bash
cd /home/alex/Projects/ALEXZ_SilentFilm_OFX
cmake --build build-cuda --parallel 2 &&
ctest --test-dir build-cuda --output-on-failure
```

Для новой автоматики отдельно, без сборки:

```bash
PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 tests/cadence_exposure_tests.py
PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 tests/cadence_bridge_tests.py
```

После успешной сборки, при полностью закрытом Resolve:

```bash
sudo cp -a build-cuda/ofx/ALEXZSilentFilm.ofx.bundle /usr/OFX/Plugins/
```

Новые Python GUI/bridge читаются из `scripts`: переоткрыть Cadence App. Если bundle0.26 уже установлен, только для автоматики0.27 повторное копирование OFX не требуется. Если установлен более старый bundle, нужен новый с нодой Crank Exposure.

Далее: одинаковый подрезанный клип в Bake/OFX, сообщение об автоматической карте, Fusion Blend0/1 и близость к Bake; повторный Import старого OFX-job без дубликата; перенос/подрезка/два разных задания; Defects-контролы; выход Resolve с Glow. Подробные сценарии и логи — в документах соответствующих этапов выше. Текущая точка продолжения — [SESSION_HANDOFF.md](SESSION_HANDOFF.md).
