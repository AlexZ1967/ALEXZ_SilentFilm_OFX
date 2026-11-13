# Неоднородность проявки Defects — 0.36

✅ **2026-11-14 — пользователь подтвердил успешные тесты и ручную проверку0.36.** Полные логи/численные результаты сравнений и замеры скорости не предоставлены; агент проверки не запускал. Ранние отметки ниже об ожидании относятся к подготовке до этого подтверждения.

✅ **2026-11-14 — подготовлен следующий блок переноса Comfy: development/fog.** Агент не запускал сборку, CTest, сравнения, профилирование или Resolve. Совпадение и скорость0.36 пока не подтверждены.

## Управление

В SilentFilm Defects выбери **Pattern=Development**, затем **Development Pattern**:

- **Legacy0.35** — прежняя проявка, заводской default для сохранности старых проектов.
- **Comfy mottling** — пятнистость плотности.
- **Comfy streaks** — продольные полосы, coarse grid из двух строк.
- **Comfy mixed** —65% mottling +35% streaks.

**Development Fog**: доля добавочной положительной плотности, default0.15, ползунок0–0.3, вручную до1. **Irregularity**:0 фиксирует поле во времени,1 полностью использует меняющиеся сетки, промежуточные значения смешивают их со статическим рисунком. **Persistence** задаёт расстояние между плавно сменяющимися опорными полями; на границе не должен быть скачок/перезапуск рисунка. Amount/поверхностные настройки при Development не используются. Damage Model здесь отключён: новая проявка выбирается отдельным контролом.

Для прямого сравнения с Comfy: **Mix Response=Physical material, Blend=1**. Формула:

```text
density = Strength × (0.35 × field + 0.3 × DevelopmentFog × (field + 1) / 2)
out_linear = clamp(in_linear × 10^(−density), 0, 1)
```

На Negative добавочная плотность осветляет последующий Print; на Positive затемняет экранный позитив. В Perceptual остаётся ранее добавленное управление видимой непрозрачностью: при промежуточном Strength оно отличается от физической формулы Comfy. Blend0/Strength0 полностью отключают эффект, alpha/premultiplication сохраняются.

## Математика

Прочитаны `utils/film_defects.py::development_field` и соответствующая ветка ноды Python. Шаг сетки `max(1,width×Size/100)`, размеры `ceil(dimension/spacing)+1` в пределах2–128. Время — физический номер кадра / Persistence, вес `t²(3−2t)`. Сначала смешиваются две coarse-сетки во времени, затем добавляется stationary grid по Irregularity. Пространственно используется bicubic align_corners=False: полупиксельные координаты, повторение крайних индексов, коэффициентa=−0.75. Каждое поле ограничивается−1..1 **до** mixed65/35. Батч-нормализация не применяется.

Коэффициенты/координаты сверены чтением [первичного исходника PyTorch UpSample.h](https://github.com/pytorch/pytorch/blob/main/aten/src/ATen/native/UpSample.h). Главный численный контроль — скрипт ниже с реально установленным Comfy-reference. RNG остаётся native hash, а не Blake2b/Python Random, поэтому одинаковый seed не обещает одинаковые пятна в Comfy и OFX.

## CPU/GPU

CPU один раз готовит coarse grid, затем использует общий пиксельный helper. CUDA готовит эти узлы непосредственно на GPU отдельным коротким kernel, затем выполняет один проход по изображению с16-tap bicubic для каждого активного слоя. Максимум128×128 +2×128 floats, **65KiB** на кадр. На GPU нет загрузки grid с CPU, полноразмерных масок, readback или CPU-обработки изображения.

Grid выделяется/освобождается через cudaMallocAsync/cudaFreeAsync в host stream, освобождение поставлено после image kernel. С host stream render не ждёт GPU; без него синхронизируется только использованная очередь. CPU/GPU buffers принадлежат одному вызову; между кадрами не удерживаются. Legacy/поверхностные дефекты сохраняют прежний однопроходный путь без дополнительной grid allocation. Это описание реализации, не измерение производительности.

## Пресеты

Workflow n15 теперь выбирает **Comfy mottling**, Fog0.15/Irregularity0.6, Physical. n18 — **Comfy streaks**, те же fog/irregularity; Strength остаётся0 согласно workflow, для просмотра увеличь его вручную. Color only по-прежнему меняет только Encoding.

Существующие экземпляры остаются Legacy. Старые полные My Presets Defects0.31–0.35 загружаются с двумя отсутствующими полями: Development Pattern=Legacy и Fog0.15. Миграция только в памяти, исходный файл не переписывается; прочие несовпадающие/повреждённые схемы по-прежнему отклоняются. Новые Save сохраняют оба поля. Удобные шкалы0.35 и filesystem ABI-исправление сохранены.

## Проверки пользователя

```bash
cd /home/alex/Projects/ALEXZ_SilentFilm_OFX
cmake --build build-cuda --parallel 2 &&
ctest --test-dir build-cuda --output-on-failure
```

Расширены существующие Defects CPU/CUDA tests: ограничение grid, static Irregularity0, переход событий, произвольный порядок, mixed65/35, fog polarity, prepared/procedural consistency, bypass, Positive/Negative, оба Mix Response, alpha, ROI/negative strides и host/default streams. Lifecycle test проверяет defaults миграции старого пресета. Новые сообщения PASS агентом не получены.

Независимое сравнение с **настоящей функцией Comfy development_field**, на Python с доступными torch/numpy:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 tests/compare_comfy_development.py --backend cpu
PYTHONDONTWRITEBYTECODE=1 python3 tests/compare_comfy_development.py --backend cuda
```

Native экспортирует сырые a/b/static grids и metadata, а reference получает их через замену своего random_stream только в отдельном тестовом процессе. Поэтому отдельно проверяются реальные temporal lerp/bicubic/clamp/density, а различие RNG не маскирует ошибки математики. 27 случаев, маленькие/вытянутые кадры/один пиксель, grid limit128, все modes/Irregularity/fog, кадры около24-frame boundary. Native CUDA-режим использует production GPU renderer для конечного RGBA, reference выполняется на Torch CPU; field-диагностика — общий CPU helper. Допуски: field2e−5, RGBA1e−5. Это не полное сравнение цепочки с другим RNG.

После успешных проверок закрой Resolve и обнови bundle:

```bash
sudo cp -a build-cuda/ofx/ALEXZSilentFilm.ofx.bundle /usr/OFX/Plugins/
```

Ручная проверка:

1. Старый проект/пресет: Legacy выбран, вид не поменялся. Сохрани/загрузи новый пресет с Mottling/Fog, проверь восстановление обоих новых параметров.
2. Negative→Defects→Print: Pattern Development, Mottling, Physical, Blend1, Strength0.1, Size8–15, Persistence48. Проверь пятнистость; Streaks даёт полосы, Mixed их сочетание.
3. Irregularity0: поле неподвижно. Irregularity0.6/1: плавно меняется. Перемотка возвращает тот же кадр; на48/96 нет резкой смены.
4. Fog0→0.15→1: на негативе после Print добавляется осветление, на позитиве затемнение. Blend0/Strength0 отключают действие.
5. Сравни скорость одной ноды/полной цепочки и экспорт, проверь первый/последний кадры и сохранение проекта.

Следующий блок — surface mixed с раздельными dust/scratches/stains, волоски и мягкая тень окна Projector. Полный Defects parity до их реализации не заявляется.
