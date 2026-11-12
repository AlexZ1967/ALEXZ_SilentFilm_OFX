# Прямые кадры Resolve — 2026-11-12

✅ Подготовлен путь обработанного входа без промежуточного ролика. Пользовательская сборка и runtime нового пути ещё не подтверждены.

## Поток данных

Cadence определяет соседние timeline frames для отсчётов выдержки. VideoReader запрашивает отсутствующие кадры у отдельного Resolve Python worker через локальный Unix socket. Worker вызывает TimelineItem.ExportFrame и возвращает PNG по памяти (файл — только fallback SDK). Libavcodec декодирует PNG в RAM, затем действующий CUDA sampler получает кадры и поля flow. Full clip PNG sequence и FFV1 не создаются; конечный ProRes/TIFF сохраняется как прежде. Raw-путь остаётся прямым native чтением файла с временной сеткой таймлайна.

Базовая реализация только Linux. Abstract socket не зависит от длины имени каталога проекта. Metadata frame_source.ini содержит адрес и одноразовый токен (права0600). В GPU напрямую передаётся уже декодированное изображение; Scripting API по-прежнему не отдаёт CUDA-буфер.

## Память и жизненный цикл

Worker кэширует максимум8 PNG и64МиБ, native reader использует свой ограниченный кэш декодированных кадров, GPU — кэши исходников/полей и буферы экспозиции. Первый кадр0 дополнительно запрашивается при открытии для проверки формата. Остальные — по потребности cadence; промежуточные skipped input frames экспортировать не требуется.

GUI запускает provider после Calculate, затем native подключается и делает запросы. Отмена — cancel.request; закрытие native соединения завершает provider. Ожидание подключения и ожидание следующего запроса ограничены5минутами. После истечения срока/ошибки нужно новое задание. Во время расчёта не меняй обработки выбранного клипа: hash всех OFX/Fusion/grade параметров SDK не предоставляет. Автообновление metadata не доказывает неизменность изображения.

Очистка кэша не удаляет job, пока provider_active=true, даже если native уже завершился ошибкой. Старые FFV1 задания поддерживаются историческим кодом, новый Calculate его не вызывает.

## Что измеряется

В result.ini: resolve_frame_requests, resolve_wait_seconds (получение PNG от worker), png_decode_seconds (PNG декодирование и сохранение raw кадра), native_seconds (весь native), flow_cpu_estimation_seconds (Farneback), render_fps. Эти интервалы не следует безусловно складывать: CPU encoder и CUDA могут работать с перекрытием. Преобразование decoded кадра в float отдельно пока не измеряется.

В bake_state.json: exported_frames, served_requests, cache_hits, transferred_bytes, export_seconds, provider_seconds, provider_active. provider_seconds включает ожидания, а не только обработку API.

## Проверка пользователем

```bash
cmake --build build-cuda --parallel 2 && ctest --test-dir build-cuda --output-on-failure
```

Новый CTest silentfilm_frame_provider запускает реальный native decoder против локального mock provider без Resolve. Проверяет4 физических экспозиции из100 timeline frames, выборочные запросы, отсутствие промежуточного видео и передачу ошибок. Отдельная регрессия cache защищает активного provider.

Далее переоткрыть GTK, отключить Use Raw Source и проверить короткий клип с заметным OFX/grade: цвет, In/Out, одинаковый count при прежних параметрах и время работы. Ни агентом сборки/тесты/Resolve, ни бенчмарки этой редакции не запускались.


✅ **2026-11-12 — подготовлена регрессия целостности изображения:** каждый отдельный PNG передаётся decoder с AV_PKT_FLAG_KEY, чтобы исключить delta-сложение с предшествующим кадром. CTest расширен чередующимися RGB48 PNG и точным сравнением float-пикселей до GPU. Пользовательский повторный прогон после исправления ещё ожидается.
