# Карта референса ComfyUI SilentFilm

Референс находится в `/mnt/comfy/ComfyUI/custom_nodes/ComfyUI_ALEXZ_SilentFilm` и виден в workspace как `SilentFilm_ComfyUI_REFERENCE`. Это отдельный рабочий Python-проект, опубликованный в GitHub; в рамках OFX-задач он доступен только для чтения. Его наличие в workspace не делает его зависимостью нативного плагина.

## Какие файлы читать по задаче

Все пути таблицы указаны относительно корня Python-референса.

| Задача | Первичные файлы и символы |
|---|---|
| Физика Negative/Print | `utils/film_sensitometry.py`: `negative_density`, `print_density`, `transmittance`, `view_positive`, `reference_positive`, `scene_luma` |
| Последовательность этапов и параметры | `nodes/video_silent_film_pipeline.py`: Camera/Negative/Print/Projector, `_capture_exposure_gains`, `_density_grain` |
| Ручка, время и экспозиционное размытие | `nodes/video_silent_film_cadence.py`: `_build_hand_crank_intervals`, `_crank_phase_at_time`, `_apply_capture_shake`, `_temporal_exposure_sample` |
| Форма виньетки и переменный расфокус | `nodes/video_silent_film_finish.py`: `_lens_edge_mask`, `_apply_variable_lens_blur`, `_apply_lens_edges` |
| Grain | `nodes/video_silent_film_finish.py`: `_build_emulsion_grain_field`, `_apply_grain`; pipeline `_density_grain` |
| Glow/halation | `utils/film_optics.py`: `projection_glow`, `halation_exposure`; вызовы в `nodes/video_silent_film_pipeline.py` |
| Самостоятельные повреждения | `nodes/video_silent_film_defects.py`, `utils/film_defects.py`: `random_stream`, `_smooth_random`, `surface_masks`, `development_field` |
| Объяснение настройки | `guides/GUIDE_VIDEO_SILENT_FILM_PIPELINE.md`, `GUIDE_VIDEO_SILENT_FILM_DEFECTS.md`, Cadence/Finish guides |

Не нужно читать всю таблицу при каждой задаче: начинайте с нужной строки и конкретных вызовов.

## Решения, которые нужно сохранить при переносе

- Процесс состоит из съёмки, негатива/проявки, печати позитива и проекции. Повреждения вставляются отдельными эффектами между соответствующими этапами; это не одна общая текстура грязи поверх финала.
- Нейтральная Negative/Print пара калибрована без потери шкалы серого и скрытого автоконтраста. Негатив — пропускание плёнки, а не художественно нормализованная инверсия. Настройки проявки и печати действуют отдельно.
- Скорость ручки циклична, но движение камеры не должно быть явной синусоидой. Внутреннее передаточное отношение — 8 кадров за оборот, без лишнего пользовательского параметра.
- Движение камеры при съёмке связано с фазой ручки; gate weave проектора — отдельный процесс.
- Мерцание съёмки должно следовать реальным длительностям виртуальных экспозиций. Сила эффекта задаётся отдельно. Glow проекции связан с яркостью лампы.
- Halation — возврат света внутри материала при экспонировании Negative/Print; это не тот же этап, что glow проектора. Значение 0 отключает эффект.
- Виньетка круглая с радиальной шкалой относительно ширины, имеет начало и конец нарастания. Сила маски управляет радиусом расфокуса, а не только смешивает резкую картинку с одним готовым blur.
- Зерно и размеры оптических эффектов должны сохранять восприятие при смене разрешения. В OFX дополнительно учитывать полный кадр, render scale/proxy и ROI, не размеры одного render window.
- Общий seed разбивается на независимые потоки эффектов. Grain и дефекты плёнки привязываются к виртуальному кадру, проектор — ко времени показа. Никакой зависимости результата от последовательного продвижения RNG во время render.
- Новая native реализация должна допускать обратную перемотку и произвольный порядок кадров. Расписание ручки можно детерминированно строить и кэшировать отдельно от изображений.

## Что не переносить буквально

- ComfyUI IMAGE-батчи, полный mmap-кэш промежуточных роликов и adapter `execution.CacheSet`: Resolve предоставляет покадровый доступ и собственный медиакэш.
- FFmpeg-загрузчик и сборщик как обязательные узлы: декодирование и экспорт в новом проекте выполняет Resolve.
- `film_json` как универсальный отдельный порт: стандартный OFX-пайплайн не повторяет этот контракт ComfyUI. Для будущей синхронизации нужны явные общие параметры/профиль и при необходимости Fusion controller.
- Предположение, что OFX-фильтр автоматически изменит длину клипа или fps таймлайна. Изменение выборки исходных кадров и монтажный retime нужно проверять раздельно.
- Требования Conda/Python/npm/Make из референса. OFX собирается системным CMake и C++ toolchain.

## Локальный SDK

`/opt/resolve/Developer/OpenFX` — первичный источник поведения установленного Resolve, только чтение. Примеры `GainPlugin`, `TemporalBlurPlugin`, `RandomFrameAccessPlugin`; заголовки `OpenFX-1.4/include`, особенно `ofxImageEffect.h`, `ofxParam.h`, `ofxGPURender.h`.

Общие возможности нового стандарта OpenFX не следует автоматически считать реализованными установленным Resolve. Необязательные metadata могут отсутствовать; ошибка `kOfxPropPluginDescription` уже встречалась и исправлена в adapter.
