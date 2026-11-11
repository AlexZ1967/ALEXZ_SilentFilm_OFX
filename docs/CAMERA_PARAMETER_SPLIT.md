# Разделение Comfy Camera между Cadence и Camera Optics — 0.16

Основа сверки — `VideoSilentFilmCamera.INPUT_TYPES()` в `nodes/video_silent_film_pipeline.py` референса. Standalone `VideoSilentFilmCadence` также содержит режимы совместимости, которые Camera намеренно фиксирует. Имена контролов английские, как в Comfy (с сохранением snake_case), подсказки русские. Старые внутренние OFX parameter IDs и request keys сохраняются: смена отображаемого имени не ломает сохранённые значения.

## Приложение Cadence

| Группа | Имя из Comfy / UI | Что реально используется в native helper |
|---|---|---|
| Source Timing | `source_fps_mode` | Дополнительный выбор `timeline` / `source` / `custom`; сохраняется в настройках, пресете и app context |
| Source Timing | `source_fps` | `Settings::sourceFps`: интерпретация индекса кадров, длительность сцены, центры/ширина выдержки, crank phase и число экспозиций |
| Source Timing | In / Out / Frames (информация) | Диапазон автоматически из выделенного монтажного клипа. В request остаются `start_frame`, `frame_count`, ручных контролов нет |
| Hand Crank | `target_fps_min`, `target_fps_max` | Request `fps_min/max` → `HandCrankSettings::minimumFps/maximumFps` |
| Hand Crank | `fps_drift_strength` | `variation` → `rateVariation`, амплитуда циклической скорости |
| Hand Crank | `crank_irregularity` | `irregularity` → `HandCrankSettings::irregularity`, нерегулярность темпа между оборотами |
| Hand Crank | `seed` | Seed расписания ручки и связанного shake |
| Exposure | `blur_mode` | `simple` / `flow_integrated` при подготовке каждого дробного отсчёта |
| Exposure | `shutter_fraction` | `shutter` → `CadenceSettings::shutterFraction`, ширина выдержки |
| Exposure | `motion_blur_strength` | `motion_blur` → `motionBlurStrength`, масштаб временной интеграции |
| Exposure | `blur_samples` | `samples` → `exposureSamples`, 1–17 на CPU/CUDA; default 9 |
| Exposure | `exposure_variation` | `exposure_link` → gain каждой физической экспозиции, запекается в RGB |
| Camera Motion | `camera_shake_strength` | `shake` → преобразование каждого временного отсчёта до усреднения |
| Информация | `timeline_fps` | Частота показа результата всегда из текущего таймлайна; отдельного контрола `output_fps` нет, ключ остаётся в native request |
| Processing | `compute_device` | Request `device`: поддержаны `cpu` и `cuda`; flow пока CPU OpenCV в обоих случаях |
| Storage | `Output Folder` | Корень генераций, сохраняется в настройках и пресетах; внутри проект с ID и папка клипа_cadence_ID |

Диапазон `target_fps_min/max` расширен до 1–60, как в исходной Cadence. Camera defaults — 16/20, `fps_drift_strength=0.7`, `crank_irregularity=0.15`, `shutter_fraction=0.65`, `motion_blur_strength=0.8`, `blur_samples=9`, `camera_shake_strength=0.25`, `exposure_variation=1`, `seed=1925`.

### Три источника source_fps

- `timeline` (по умолчанию): FPS актуального таймлайна, прочитанный bridge перед запуском. Отсутствие таймлайна даёт ошибку вместо угадывания.
- `source`: FPS оригинального исходника выбранного монтажного клипа. Даже если сначала экспортируются обработанные кадры на сетке таймлайна, частота интерпретации берётся из оригинального клипа.
- `custom`: ручной `source_fps`, 1–120; поле доступно для ввода только в этом режиме.

Пресет сохраняет режим и ручное значение `source_fps`. Автоматические режимы заново читают метаданные; показанное автоматическое число не заменяет сохранённое ручное. `target_fps_min/max` и `fps_drift_strength` — параметры съёмки, они также сохраняются. Размеры, In/Out, число кадров и фактический FPS показа в пресеты не входят. Приложение работает только с выделенным клипом таймлайна, FPS результата всегда из этого таймлайна. Output Folder входит в пресеты; организация результатов и две области очистки описаны в [CADENCE_CACHE.md](CADENCE_CACHE.md).

Это интерпретация входных индексов, как в Comfy, а не отдельное преобразование количества декодированных кадров. Например, 25 кадров, интерпретированных как 50 FPS, задают сцену длиной 0,5 с. CFR/metadata проверяются отдельно по действительному `encoded_source_fps`; изменение интерпретации не должно выдавать ложную ошибку кодека. `output_fps` определяет длительность показа уже снятых M кадров и не меняет сам план съёмки.

`request.ini` хранит фактический численный `source_fps`, `app_context.json` — выбранный режим. `status.ini`/`result.ini` фиксируют применённые параметры под именами Comfy, включая интерпретированный и закодированный FPS.

### Почему некоторые изменения могут быть незаметны

- При `motion_blur_strength=0` либо `blur_samples=1` интеграция выдержки отсутствует; дробный центр всё ещё может интерполироваться через flow.
- При ненулевом смазе сохранена минимальная ширина выдержки Comfy. Поэтому уменьшение одного `shutter_fraction` до нуля не гарантирует исчезновения смаза.
- Треугольные веса при `blur_samples=3` почти полностью сосредоточены в центре; для сравнения смаза используйте 7–9 отсчётов.
- При равных `target_fps_min/max` или `fps_drift_strength=0` нет пульсации скорости и связанной с ней экспозиции. Seed и сила экспозиции не создают отдельного случайного flicker.
- `output_fps` при размещении на исходном таймлайне соответствует его FPS; менять длительность нужно параметрами съёмки/интерпретации.

## OFX SilentFilm Camera Optics

| Группа | Отображаемое имя Comfy | Сохранённый OFX ID | Действие CPU/CUDA |
|---|---|---|---|
| Vignette and Edge Blur | `vignette_strength` | `vignette_strength` | Затемнение по общей радиальной маске |
| Vignette and Edge Blur | `edge_start` | `edge_start` | Начало нарастания виньетки и краевого размытия |
| Vignette and Edge Blur | `edge_end` | `edge_end` | Радиус полной силы обоих эффектов; строго больше start |
| Vignette and Edge Blur | `edge_softness` | `edge_softness` | Переменный радиус Gaussian по маске, восстановлен в 0.14 |
| Focus and Softness | `softness` | `camera_softness` | Равномерная мягкость всего кадра |
| Focus and Softness | `focus_drift_strength` | `focus_drift` | Случайное воспроизводимое гуляние фокуса по выходным кадрам |
| Focus and Softness | `seed` | `focus_seed` | Независимый seed гуляния фокуса |

Это весь оптический набор Comfy Camera. Его default values сохранены: vignette 0.12, edge softness 0.15, edge start/end 0.6/1, softness 0.1, focus drift 0.03. В раннем упрощении `edge_softness` был не только убран из UI, но и обнулён в settings reader — теперь восстановлены descriptor, handle, чтение анимированного значения и передача в оба backend. Файловых входов/карт экспозиции у Camera Optics нет. Монохромный выход сохранён.

В Comfy общий seed разбивается на независимые потоки эффектов. В раздельных native-этапах seed ручки находится в приложении, seed фокуса — в OFX; дополнительной файловой синхронизации нет. Native RNG уже отличался от Python/Torch; одинаковые числа seed не обещают одинаковый рисунок с Comfy.

## Поля standalone Cadence и служебные поля Comfy

`cadence_model=hand_crank` и физическая проекция одной экспозиции в один выходной кадр фиксированы, как в Comfy Camera. `legacy_random` и `preserve_duration_25fps` — режимы отдельной старой Cadence-ноды, не пропущенные оптические контролы Camera. `crank_frames_per_turn` в референсе устарел и игнорируется: 8 кадров/оборот остаются внутренней константой.

`batch_size` относится к IMAGE-батчам Comfy, его нет в потоковом native helper. `compute_device=auto`/автоматический OOM fallback пока не перенесены; UI честно предлагает только реализованные `cpu` и `cuda`. Эти неработающие настройки не добавляются в панель. `image` заменён входным видеоклипом; `film_json` и `preview` — выходы Comfy, не параметры native-этапов.

## Возврат результата

По умолчанию **Add Above Source**. Выбирается ближайшая существующая видеодорожка выше исходной, включённая, незаблокированная и свободная на всём интервале `[start, start + M)`. Клипам до и после этого интервала разрешено оставаться на дорожке. Если ни одна существующая дорожка не подходит, создаётся ровно одна новая верхняя. Существующие дорожки, клипы и аудио не удаляются. При ошибке вставки удаляется только собственная новая дорожка, если она осталась пустой; backup сохраняется.

Все изменения 0.14 подготовлены в исходниках; сборка, тесты и реальный Resolve-повтор агентом не запускались.
