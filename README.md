# ALEXZ SilentFilm OFX

Отдельный проект нативных OpenFX-эффектов для DaVinci Resolve. Первая целевая платформа — **Linux x86_64**, локальная база SDK — **Resolve Studio 20.2**.

Проект расположен в `/home/alex/Projects/ALEXZ_SilentFilm_OFX`. Рабочий ComfyUI-пакет SilentFilm не изменяется и используется как справочная реализация. Его Python/PyTorch-инфраструктура в процесс Resolve не встраивается.

## Что подготовлено

- CMake-проект C++17 с отдельно выделенным вычислительным ядром и OpenFX adapter.
- Диагностический эффект **SilentFilm Host Probe**, ID `com.alexz.silentfilm.hostprobe`.
- В версии 0.4 появился **SilentFilm Camera** (`com.alexz.silentfilm.camera`) с воспроизводимой статической виньеткой; это пока оптическая часть Camera, без каденса, движения и расфокуса.
- В версии 0.3 добавлены отдельные **SilentFilm Negative** и **SilentFilm Print** с общими CPU/CUDA-функциями сенситометрии. Пользователь подтвердил, что все тесты прошли, а пара Negative → Print правильно работает в Resolve. В исходниках версии 0.4 добавлено настраиваемое зерно Negative/Print; его проверка пользователем ещё ожидается.
- CPU-обработка RGBA float32 с учётом bounds, render window и знакового row stride; подготовлен опциональный CUDA backend с тем же контрактом.
- Нейтральный `IsIdentity`, регулировка RGB gain и запрос соседних кадров через `GetFramesNeeded`/`clipGetImage`.
- Параметры вычисляются на запрошенное время; общего накопительного состояния кадров нет. Полученные изображения освобождаются до возврата из render; отмена проверяется между строками.
- Небольшие тесты ядра: нейтральность, значения выше 1, альфа, временное смешивание и повторяемость.
- Для CUDA подготовлен отдельный тест сравнения с CPU: раздельные bounds входов, padding, отрицательный stride, render window, HDR/alpha, host stream и режим без него.

**Проверенный базовый прототип 0.2:** пользователь подтвердил Release-сборку на Linux системным GCC 13.3, CUDA Toolkit 13.0.88 (архитектура 86), CTest 2/2 PASS, работу Host Probe в Resolve, соседние кадры, изменение offset, повторяемость после перемотки и границы клипа. В `ResolveDebug.txt` подтверждён CUDA render. **Для версии 0.3 пользователь подтвердил, что все тесты прошли и пара Negative → Print правильно работает в Resolve.** Полный лог тестового прогона не предоставлен; отдельные результаты отмены и экспорта не фиксировались. Зерно и статическая виньетка Camera версии 0.4 реализованы, но ещё не проверены; halation, расфокус, каденс ручки и отдельные повреждения не реализованы. CPU-путь не означает, что сам Resolve может работать без поддерживаемого GPU.

## Структура

```text
include/silentfilm/     публичные структуры вычислительного ядра
src/core/              математика без зависимости от OpenFX и ОС
src/ofx/               взаимодействие с хостом
tests/                 небольшие независимые тесты ядра
docs/                  план и ручная проверка
```

OpenFX подключается из установленного SDK. Заголовки и Support Library не копируются в проект; SDK-библиотеки и OpenGL не линкуются. Используется C API OpenFX V1, примеры Resolve служат справкой. Обычная CPU-сборка не зависит от CUDA; при `SILENTFILM_ENABLE_CUDA=ON` добавляется статический CUDA runtime.

## Сборка на текущей Linux-машине

Нужны CMake 3.20+, компилятор C++17, CTest и установленные заголовки OpenFX. Команды запускает пользователь:

```bash
cd /home/alex/Projects/ALEXZ_SilentFilm_OFX
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DOFX_SDK_ROOT=/opt/resolve/Developer/OpenFX
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

Ожидаемое место результата после успешной сборки:

```text
build/ofx/ALEXZSilentFilm.ofx.bundle/Contents/Linux-x86-64/ALEXZSilentFilm.ofx
```

Для разработки используются системные CMake, CTest и GCC/Clang с поддержкой C++17. Conda и Python для сборки и работы плагина не нужны. Если в терминале активировано Conda environment, перед конфигурацией проекта выйдите из него через `conda deactivate`, чтобы CMake выбрал системный toolchain. Другой SDK задаётся через `OFX_SDK_ROOT`; для стороннего OpenFX SDK можно явно указать `OFX_INCLUDE_DIR`.

CMake содержит расположения бинарников Windows/macOS для дальнейшего развития. Работоспособность на этих ОС пока не заявляется; упаковка, архитектуры и GPU требуют отдельных проверок.

## CUDA-сборка на текущей машине

CUDA включается явно в отдельном каталоге сборки, чтобы сохранить прежний CPU bundle. Для найденного Toolkit `/usr/local/cuda` и установленной NVIDIA RTX 3090 (архитектура 86):

```bash
cmake -S . -B build-cuda -DCMAKE_BUILD_TYPE=Release -DSILENTFILM_ENABLE_CUDA=ON -DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc -DCMAKE_CUDA_ARCHITECTURES=86
cmake --build build-cuda --parallel 2
ctest --test-dir build-cuda --output-on-failure
```

Подробности, установка и проверка фактического выбора CUDA в Resolve: [docs/CUDA_VALIDATION.md](docs/CUDA_VALIDATION.md). Эти команды агент не запускал.

Подтверждённый пользователем набор версии 0.3 включал core, sensitometry и CUDA parity. Для версии 0.4 тесты запускает пользователь; существующий CUDA parity тест пока не проверяет ненулевое зерно, расширенный диапазон теней или виньетку Camera.

CPU и CUDA используют одну функцию пиксельной математики. CUDA-путь работает непосредственно с указателями хоста, без копирования всего изображения в RAM и без собственных видеобатчей. Когда Resolve передаёт stream, kernel ставится в него без `cudaDeviceSynchronize` и без ожидания завершения в render. Если stream не передан, ожидается только завершение собственного запуска в default stream. CPU-путь выбирается, когда OFX render передаёт CPU-буферы; ошибки CUDA не маскируются попыткой прочитать GPU-указатель на CPU.

## Установка для проверки в Resolve

Только после успешной сборки и тестов закройте Resolve. Стандартный каталог Linux указан в README установленного SDK: `/usr/OFX/Plugins`.

Если раньше этого bundle ещё не было, скопируйте его вручную:

```bash
sudo mkdir -p /usr/OFX/Plugins
sudo cp -a build/ofx/ALEXZSilentFilm.ofx.bundle /usr/OFX/Plugins/
```

Для последующих замен также закрывайте Resolve. Копируется только собственный bundle, SDK и файлы Resolve не изменяются. После открытия Resolve найдите **SilentFilm Host Probe** в OpenFX/Effects. При необходимости проверьте список разрешённых Video Plugins в настройках Resolve.

Пошаговая ручная проверка: [docs/LINUX_VALIDATION.md](docs/LINUX_VALIDATION.md).

## Параметры прототипа

| Параметр | Действие |
|---|---|
| `Diagnostic Gain (EV)` | 0 — нейтрально, +1 умножает RGB на 2, −1 делит на 2. Альфа не умножается. Это диагностика буфера, без преобразования Log/sRGB и физики плёнки |
| `Temporal Mix` | 0 — текущий кадр, 1 — 50% текущего и по 25% двух соседних. Промежуточные значения плавно включают смешивание. Это проверка temporal access, не optical flow |
| `Neighbor Offset (frames)` | Расстояние до соседей в единицах времени OpenFX (кадры), от 1 до 8; запрос ограничивается диапазоном Source, объявленным хостом |

## Negative → Print

В bundle зарегистрированы четыре эффекта: Host Probe, Camera и два самостоятельных плёночных этапа. Для пары используйте **SilentFilm Negative → SilentFilm Print** с одинаковым input/output encoding, экспозициями 0, контрастами 1, компрессией теней/светов и вуалью 0. `View Positive` у Negative должен быть выключен при передаче данных в Print. В исходниках 0.4 добавлены `Emulsion Grain`, `Grain Size`, `Grain Seed`, расширенная шкала `Shadow Compression` до 5 и подробные русские подсказки для параметров нод. Зерно выключено при силе 0; значения Shadow Compression выше 1 усиливают кривую без потери монотонности. Изменения ожидают сборки и проверки.

## SilentFilm Camera

Текущая Camera-нода реализует только статическую круговую виньетку; она не меняет каденс, движение или фокус. `Vignette Strength` умножает RGB на радиальную маску, `Edge Start` и `Edge End` задают её границы относительно половины ширины кадра. Альфа сохраняется; маска привязана к полному image bounds, поэтому render window не двигает её центр. Для установки в цепь поставьте Camera до Negative. Подробности и проверочный сценарий: [docs/CAMERA_OPTICS.md](docs/CAMERA_OPTICS.md).

Пара сохраняет шкалу серого SDR-исходника в пределах точности float32; цветной исходник становится монохромным по линейной яркости Rec.709. Это не HDR/Log-преобразователь и не автоматическое определение рабочего пространства Resolve. Модель ограничивает линейную яркость и позитив диапазоном 0–1. Native view для Gamma 2.4 добавлен явно; Linear/sRGB и кривые плотностей основаны на Python-модели `alexz.neutral_bw_v1`.

Подробные параметры, CST, контроль негатива и сценарий проверки: [docs/NEGATIVE_PRINT.md](docs/NEGATIVE_PRINT.md).

При нулевом gain и mix хост может пропустить render через `IsIdentity`. Если хост не сообщает temporal support, gain остаётся доступным, а попытка включить temporal mix возвращает ошибку поддержки. Молчаливой замены соседних кадров текущим при ошибке доступа нет; на штатной границе диапазона используется крайний кадр.

Для трассировки запросов можно запустить Resolve из своего терминала с переменной `ALEXZ_SILENTFILM_OFX_TRACE=1`. Плагин пишет backend (`CPU` или `CUDA`), время и выбранных соседей в stderr. Не оставляйте трассировку включённой при измерении скорости; при нейтральном эффекте `IsIdentity` может исключить render и строк трассы не будет.

Resolve на текущей Linux-установке перенаправляет stderr в `~/.local/share/DaVinciResolve/logs/ResolveDebug.txt`; отсутствие строк в стартовом терминале не означает отсутствия render.

## План

[docs/PLAN_LINUX_FIRST.md](docs/PLAN_LINUX_FIRST.md) описывает переход от Host Probe к полноценному SilentFilm, включая синхронизацию, цвет и GPU.

CPU- и CUDA-сборки базового Host Probe и CTest выполнил пользователь; подтверждены временной доступ и CUDA render в Resolve. Для 0.3 пользователь подтвердил прохождение всех тестов и правильную работу Negative → Print в Resolve. В версии 0.4 добавлены воспроизводимое зерно, расширенная Shadow Compression, русские подсказки и статическая виньетка Camera; проверку сборки, тестов и Resolve выполняет пользователь пакетом. Отдельные результаты отмены, экспорта и сравнения контекстов Color/Fusion не фиксировались.

## Отдельный workspace и новый диалог

Локальный файл `ALEXZ_SilentFilm_Resolve.code-workspace` открывает рабочий OFX-проект и два read-only референса: ComfyUI SilentFilm и OpenFX SDK. В нём есть пути этой машины, поэтому он исключён из Git. Открытие в новом окне:

```bash
code --new-window /home/alex/Projects/ALEXZ_SilentFilm_OFX/ALEXZ_SilentFilm_Resolve.code-workspace
```

Для нового чата начните с `AGENTS.md` и [состояния проекта](docs/SESSION_HANDOFF.md). Карта перенесённых решений и точечных Python-референсов — [docs/REFERENCE_MAP.md](docs/REFERENCE_MAP.md). Это позволяет продолжать работу без повторного чтения всей переписки и всего референсного проекта. Ветвление разговора само по себе не меняет рабочую папку: проверьте, что задача относится к OFX-корню.
