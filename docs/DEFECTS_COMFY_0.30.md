# Defects0.30: модели материала и формы отдельных следов

✅ **2026-11-13 — подготовлен первый блок переноса Defects из Comfy.** Агент не запускал сборки, тесты, сравнения или Resolve. Совпадение и скорость нового кода пока не подтверждены.

## Что изменилось

**Damage Model**: Legacy0.29 / Comfy surface / Comfy emulsion loss / Comfy dirty. По умолчанию Legacy: сохранённые экземпляры не переключаются сами. Применение Reference Preset заново назначает новую модель для поверхностных дефектов; Development остаётся прежним.

- Surface: пропускание умножается на `10^(-0.65 × coverage × Strength)`.
- Dirty: `10^(-1.5 × coverage × Strength)`.
- Emulsion loss: царапины/пятна приближают пропускание к clear stock `10^-0.1` на Negative, к1 на Positive. Пыль остаётся непрозрачной: множитель `1 − coverage × Strength`.
- Capture/Printing/Projector — окна аппаратов: во всех Comfy-моделях множитель `1 − coverage × Strength`. Снятие эмульсии к этим стадиям не применяется.

Для прямого математического сравнения **Mix Response=Physical material, Blend=1**. Пресеты поверхностных дефектов ставят эти значения. Perceptual остаётся альтернативой для удобного видимого смешивания Negative→Print, но при промежуточной Strength не воспроизводит закон плотности Comfy. Blend0/Strength0 сохраняют bypass. Alpha и premultiplication обрабатываются общим путём.

Пыль/пятна получили поворот, эллиптичность, неровность контура `0.16 sin(3θ+phase) + 0.09 cos(5θ−phase)`, субпиксельное ослабление и случайную непрозрачность0.55–1. Пыль имеет антиалиасинг в пикселях, пятна — экспоненциальный профиль и конечную область `_patch`, как в Comfy. Царапины получили линейную траекторию между frame/frame+1, антиалиасинг, модуляцию прерывистости и субпиксельной толщины. Irregularity управляет этими процессами. Scratch Wander — дополнительное расширение native; пресеты ставят0, чтобы не дублировать встроенный ход Comfy.

При Spread35 воспроизводятся диапазоны геометрии референса: диаметр0.7–1.4; у царапины отдельный множитель радиуса0.65–1.3. Другой Spread — расширение native. Для каждой частицы отдельно сдвинуто событие Persistence. Пыль на Negative/Positive меняется по физическим кадрам; пыль окна держится событием и шевелится по плавному сигналу frame/8. Выполнение не зависит от порядка кадров.

**Edge Bias намеренно действует только слева/справа**, по прежнему требованию пользователя. В исходном Comfy есть все четыре стороны; это сознательное отличие, а не заявленное полное совпадение. RNG остаётся native hash, не Blake2b/Python Random референса. Один seed не даёт тождественное расположение частиц между приложениями.

## GPU и границы этапа

Сохранён один fused CUDA kernel. Геометрия до64 следов готовится один раз на CPU и передаётся параметрами запуска; ограничение CUDA launch payload проверяется static_assert. Кадровых масок, GPU allocation, readback и собственных потоков в этом пути не добавлено. Sparse-отсечение перед atan2/sin/cos/exp уменьшает работу вне следов, монохром обрабатывается одной цветовой веткой. Это описание устройства кода, не замер ускорения0.30.

Ещё не перенесены: волоски, mixed с отдельными слоями dust/scratches/stains, bicubic development (mottling/streaks/mixed), fog_strength проявки, softness тени Projector Gate и точный RNG Comfy. Поэтому полная цепочка с повреждениями пока не объявляется эквивалентной workflow. Дополнительные параметры оптики/печати — другой этап. Входные значения подчиняются существующему SDR/color-контракту; Log/HDR не добавлены.

## Проверки пользователя

```bash
cd /home/alex/Projects/ALEXZ_SilentFilm_OFX
cmake --build build-cuda --parallel 2 &&
ctest --test-dir build-cuda --output-on-failure
```

Расширены существующие `silentfilm_defects` и `silentfilm_defects_cuda_parity`: независимые уравнения материала, legacy-регрессии, новая геометрия/модели, оба Mix Response, premultiplication, ROI, origins/negative strides, host stream/default stream, повторный произвольный порядок и покадровая пыль на плёнке. Сверка параметров workflow обновлена для Damage Model/Irregularity/Physical/Scratch Wander0.

Независимое сравнение с реальными `_particle`/`_scratch` Comfy, на Python с уже доступными torch/numpy:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 tests/compare_comfy_defects.py \
  --native-test build-cuda/silentfilm_defects_tests
```

Скрипт читает reference только как модуль в отдельном тестовом процессе. Два следа на маску проверяют перекрытие, субпиксельные размеры и clipping. Геометрия обеим сторонам задана явно: сравнение масок не скрывается различием RNG. Затем проверяются45 комбинаций material/damage/pattern при Strength0.37, Blend1, Linear и неизменной alpha. Допуск5e-6. CUDA проверяется отдельно production renderer parity тестом; этот Python script сам по себе не доказывает native RNG/end-to-end/CUDA-сходство с Comfy.

После успешных проверок закрой Resolve и обнови bundle:

```bash
sudo cp -a build-cuda/ofx/ALEXZSilentFilm.ofx.bundle /usr/OFX/Plugins/
```

В Resolve:

1. Убедись, что старые экземпляры остались в Legacy и не изменились. На копии выбери актуальный Reference Preset n16/n19/n28/n32/n33 или Comfy-модель вручную. Для сравнения Physical, Blend1; кодирование общей цепочки согласовано.
2. На Negative→Defects→Print сравни Surface/Dirty/Emulsion loss. Dirty сильнее Surface затемняет само пропускание (после Print полярность меняется). Снятая эмульсия осветляет Negative T, но даёт тёмный след после Print. Пыль в Emulsion loss остаётся непрозрачной, не становится снятой эмульсией.
3. На Dust/Stains/Scratches проверь Irregularity0/0.6/1, Strength0/0.1/0.5/1, Blend0/0.5/1. На паузе повтори кадр после перемотки; у Negative dust сравни соседние физические кадры. Edge Bias1 — только боковые края.
4. Проверь playback с одной нодой/полной цепочкой и экспорт. Новые тригонометрические маски требуют измерения производительности, особенно при больших Size/Amount.

Следующий блок после этих проверок — development с fog и bicubic полями; затем отдельные mixed/hair и мягкая тень окна проектора.
