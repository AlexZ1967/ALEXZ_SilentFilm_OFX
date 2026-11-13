# Crank Exposure в Color через пресет конкретного клипа

✅ **2026-11-13 — пользователь подтвердил проверки и работу Color-маршрута0.29** с эталоном `presets/crank_exposure_template_2.2.1.drx`. Агент проверки не запускал. После жалобы на скорость исправлено отсутствие CUDA capability Crank Exposure в OFX descriptor; пользователь подтвердил работу GPU и ускорение 2026-11-13.

## Как проверить

✅ **2026-11-13 — пользователь подтвердил тесты и работу уникальных имён ролика/DRX.** Полного лога нет; агент проверки не запускал. Ускорение после CUDA-правки также подтверждено пользователем; численных замеров нет.

Новые расчёты получают имена с ID варианта: например, `Damy_na_skachkah_2_cadence_a84y7i6z.mov` и `Damy_na_skachkah_2_cadence_a84y7i6z.drx`. Оба файла и `exposure.sfexposure` лежат в папке конкретного задания. ID совпадает с восьмисимвольным суффиксом папки. Media Pool/таймлайн получают имя результата с этим ID. Старые файлы и сохранённые привязки не переименовываются.

1. Переоткрой Cadence App. Выбери **Exposure via OFX → Exposure Location: Color** (новое заводское расположение). Выбор сохраняется в настройках/именованных пресетах. Сам Bake остаётся заводским режимом экспозиции.
2. Рассчитай короткий **новый** результат и импортируй привычным способом. Ожидается сообщение **«SilentFilm Crank Exposure настроен в Color через пресет этого клипа»**. В папке задания появляются `<имя результата>.drx` и `exposure_binding.json`; карта sfexposure остаётся уникальной этому результату. Для сравнения имён повтори расчёт того же исходника: суффиксы вариантов должны различаться.
3. На странице Color проверь ноду **SilentFilm Crank Exposure**, автоматически назначенный Exposure Map, Blend1/Frame Offset0; нода располагается перед Camera Optics/Negative/Print/Projector. Эталон содержит нейтральную Corrector-ноду и Crank Exposure, поэтому ожидаются две ноды. Выбранная локальная Color-версия называется `SilentFilm Crank Exposure …`. Fusion-композиция при выборе Color не создаётся.
4. Проверь Blend0/1, первый/последний кадры и близость к Bake. Затем повтори для другого клипа: путь карты должен быть другим. Повторный Import уже настроенного задания не заменяет grade. Старые задания без exposure_location сохраняют Fusion-маршрут; автоматическая миграция существующих Fusion-результатов не выполняется.
5. Если назначения нет, пришли текст из **«Последний импорт»** и `exposure_binding.json`. При отказе Color API не добавляется дополнительная Fusion/OFX-экспозиция. Прежняя Color-версия восстанавливается при ошибке, если API подтверждает восстановление; ошибка rollback также сохраняется в диагностике.

**После исправления скорости нужна пересборка и замена OFX bundle.** Первоначальный Color DRX менял только Python/шаблон, но теперь Crank Exposure0.29 объявляет хосту CUDA/host-stream. Ранее renderer существовал, однако descriptor не включал этот эффект в список CUDA-совместимых: Resolve выбирал CPU.

После сборки/CTest ниже закрой Resolve и скопируй CUDA bundle:

```bash
sudo cp -a /home/alex/Projects/ALEXZ_SilentFilm_OFX/build-cuda/ofx/ALEXZSilentFilm.ofx.bundle /usr/OFX/Plugins/
```

Открой Resolve и сравни скорость на том же участке с одной Crank Exposure, Blend0/1; отключи старый render cache для сравнения. Карта/DRX остаются прежними, пересчитывать Cadence не требуется. GPU renderer — один проход по host GPU images без собственного кадрового CPU-копирования; sRGB/экспозиция/alpha совпадают с прежней математикой. Ускорение подтверждено пользователем 2026-11-13, численных замеров нет. CPU/CUDA parity тест проверяет вычисления, но не выбор backend реальным хостом.

Если замедление остаётся, закрой Resolve и запусти его из терминала с диагностикой:

```bash
cd /home/alex/Projects/ALEXZ_SilentFilm_OFX
ALEXZ_SILENTFILM_OFX_TRACE=1 /opt/resolve/bin/resolve > resolve-crank-trace.log 2>&1
```

После короткого воспроизведения строки `CUDA ... effect=SilentFilm Crank Exposure` подтверждают выбор GPU. Если там CPU, приложи эти строки: это выбор backend хостом, а не отсутствие kernel. Trace включает подробное логирование, поэтому для обычной работы запускать без переменной.

Тесты пользователя без запуска Resolve:

```bash
cd /home/alex/Projects/ALEXZ_SilentFilm_OFX
PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 tests/cadence_color_preset_tests.py
PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 tests/cadence_bridge_tests.py
PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 tests/cadence_preferences_tests.py
```

После очередной конфигурации CMake доступен CTest **silentfilm_cadence_color_preset**. Для обычного общего цикла:

```bash
cmake --build build-cuda --parallel 2 &&
ctest --test-dir build-cuda --output-on-failure
```

## Генерация и ограничения

Реальный DRX — XML с Qt-style именами тегов; clip Body содержит префикс0x81 и Zstandard-сжатое protobuf-сообщение. Поддержана **наблюдаемая структура экспорта Resolve21.1.2**, не произвольный публичный контракт DRX. Модуль `scripts/cadence_color_preset.py` читает только нужные вложенные поля, проверяет единственный активный Crank Exposure и отсутствие посторонней коррекции, меняет только UTF-8 значение exposure_map и пересчитывает вложенные varint-длины. Остальные поля и внешний XML сохраняются; Body сжимается заново. Заводские Blend1/Offset0 в исходном экспорте не сериализованы как overrides; шаблон с другими overrides отклоняется.

Для сжатия используется системная **libzstd** через Python ctypes; модуль `zstandard`/pip не нужен. В текущей системе библиотека доступна при чтении экспорта. Debian/Ubuntu runtime package: `libzstd1` (при необходимости установить самостоятельно `sudo apt install libzstd1`). Внешний Python helper по-прежнему отделён от процесса OFX; C++/CUDA математика не менялась.

Каждый `crank_exposure.drx` создаётся атомарно в своей папке задания, с путём только его карты. Подтверждается повторное чтение сгенерированного Body; hash исходного шаблона записывается в diagnostics. Путь с Unicode/кавычками кодируется как protobuf string, не вставляется в XML/код/командную строку. В исходном DRX есть служебные thumbnail/metadata, они сохраняются для совместимости.

Тесты подготовлены для реального эталона, Unicode-путей/изменения varint-длин, обратного восстановления всех остальных Body-данных, разных карт двух заданий, повреждённого Zstd/protobuf и другого plugin ID; mocks проверяют local version, инвалидированный Graph, rollback, fresh-only guard и отсутствие Fusion-дублирования. Это не подтверждает чтение DRX реальным Resolve или render/гамму/тайминг. Результаты запусков не заявляются.

Встроенные Reference Preset меняют собственные параметры экземпляра через OFX paramSetValue. Применение пресета Resolve — другой вход: установленный SDK содержит `TimelineItem.GetNodeGraph()` / `Graph.ApplyGradeFromDRX()`. Поэтому отсутствие setter прямого Effect не исключает назначение параметров через DRX.

## Маршрут применения

1. Одна эталонная Color-нода SilentFilm Crank Exposure сохраняется из Resolve как DRX с валидным Exposure Map, Blend1/Frame Offset0.
2. После расчёта Cadence приложение создаёт отдельный DRX в папке задания, содержащий привязку к его уникальному неизменяемому `exposure.sfexposure`. Этот DRX не применяется к другим клипам. Сам sfexposure остаётся таблицей коэффициентов; DRX хранит настройки Color-графа/OFX.
3. Пресет применяется сразу к **только что импортированному результату**, перед Camera Optics/Negative. Fusion в этом маршруте не добавляется. Карта и Blend сохраняются как параметры OFX в Color.
4. После ApplyGradeFromDRX объект Graph запрашивается заново: SDK предупреждает, что прежний handle больше не действителен. Проверяются состав графа и отсутствие второй экспозиции.

**Ограничение SDK:** ApplyGradeFromDRX заменяет Color-граф. Поэтому применять его автоматически к уже обработанному результату или исходному монтажному клипу нельзя. Проверки числа нод недостаточно для определения нейтральности. Модуль допускает только новый результат сразу после AppendToTimeline и создаёт для него отдельную **локальную Color-версию**, проверяя её выбор до применения. Это также отделяет новый grade от возможных remote/shared исходных версий. При отказе восстанавливается прежняя версия и удаляется незавершённая новая, если API подтверждает операции. Прежний Graph после успешного ApplyGrade больше не используется; запрашивается новый, проверяются состав/Crank Exposure и сохранность позиции/длины клипа. Прямого readback значения OFX из Color API нет: проверены файл пресета и ответ/граф API, а фактический path/render должен подтвердить пользователь.

## Эталон

Используется предоставленный пользователем `presets/crank_exposure_template_2.2.1.drx`. Поиск предпочитает точное имя `crank_exposure_template.drx`, если такой файл появится; иначе нужен ровно один `crank_exposure_template*.drx`. Неоднозначность или неизвестная структура останавливают Color-подготовку с объяснением. Исходный экспорт не изменён. Следующий шаг — тесты пользователя и реальное применение сгенерированного DRX.

Справка: `/opt/resolve/Developer/Scripting/DaVinciResolveScript.pyi`, `README.md` → Graph information; [Blackmagic: экспорт только DRX](https://documents.blackmagicdesign.com/SupportNotes/DaVinci_Resolve_18.6_New_Features_Guide.pdf?_v=1695106810000). Сборки, тесты, экспорт и Resolve выполняет пользователь.
