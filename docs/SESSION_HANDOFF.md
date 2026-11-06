# Состояние проекта для нового диалога

## Где работаем

Редактируем только `/home/alex/Projects/ALEXZ_SilentFilm_OFX`. Это самостоятельный нативный C++17/OpenFX-проект для Resolve. Существующий ComfyUI SilentFilm используется только как эталон алгоритмов и не изменяется. Не нужен Python, PyTorch, Conda или ComfyUI внутри процесса Resolve.

Рабочий язык — русский; правила разработки — в корневом `AGENTS.md`. Сборки, тесты и проверки в Resolve запускает пользователь; агент передаёт команды и анализирует результаты. Агент не устанавливает плагин в системный каталог и не перезапускает Resolve без прямой просьбы.

## Подтверждённая среда

- Linux x86_64, Resolve Studio 20.2.0.0013.
- Системный GCC 13.3; CUDA Toolkit 13.0.88, `/usr/local/cuda/bin/nvcc`.
- NVIDIA RTX 3090, CUDA architecture 86.
- SDK: `/opt/resolve/Developer/OpenFX`, только чтение.
- Bundle: `build-cuda/ofx/ALEXZSilentFilm.ofx.bundle/Contents/Linux-x86-64/ALEXZSilentFilm.ofx`.
- Установка пользователем: `/usr/OFX/Plugins/ALEXZSilentFilm.ofx.bundle`.
- Лог: `~/.local/share/DaVinciResolve/logs/ResolveDebug.txt`. Resolve перенаправляет туда stderr плагина; отсутствие трассы в стартовом терминале не является ошибкой.

## Что сделано и проверено

1. Версия 0.2: Host Probe с gain, temporal mix и offset. CPU/CUDA, bounds, signed row stride, render window, identity, несколько экземпляров без накопительного состояния.
2. Пользователь подтвердил сборку и CTest 2/2 PASS для 0.2. В Resolve подтвердил загрузку, работу, соседние кадры ±1/±4, границы и повторяемость при перемотке. В логе прочитана трасса `CUDA time=...`.
3. Исправлена ошибка регистрации: отсутствие необязательного `kOfxPropPluginDescription` давало OFX status 3 и прерывало Describe. Теперь неизвестные optional metadata пропускаются; остальные ошибки не скрываются.
4. Текущие исходники 0.3: три отдельных эффекта — `SilentFilm Host Probe`, `SilentFilm Negative`, `SilentFilm Print`. Новый общий CPU/CUDA файл математики — `include/silentfilm/sensitometry.hpp`.
5. Пользователь подтвердил, что все тесты 0.3 прошли и Negative → Print правильно работает в Resolve. Состав ручной проверки и полный лог тестового прогона не предоставлены; детали экспорта, отмены и alpha отдельно не фиксировались.

**Следующий шаг:** следующий этап плана — оптика, grain и отдельные дефекты. Перед началом открыть соответствующий раздел `docs/PLAN_LINUX_FIRST.md`; не считать подготовленный код Camera/Cadence или оптики уже реализованным — их пока нет.

## Важные решения

- Один OFX bundle, несколько самостоятельных эффектов; общий adapter, независимое от хоста ядро.
- Native IDs: `com.alexz.silentfilm.hostprobe`, `com.alexz.silentfilm.negative`, `com.alexz.silentfilm.print`; текущая версия 0.3.
- GPU-путь согласуется через `CudaEnabled`/`CudaStream`. Не разыменовывать GPU pointers на CPU, не менять device/context хоста, не использовать `cudaDeviceSynchronize` в render.
- При host stream kernel ставится в эту очередь без ожидания; без stream завершается собственный default-stream запуск. Host image handles освобождаются в пределах action.
- Negative выдаёт физическое пропускание, Print принимает его без декодирования как sRGB. Экспозиция Negative со знаком плюс осветляет итоговую копию; экспозиция Print со знаком плюс затемняет.
- Encoding явный: Linear Rec.709 / sRGB / Rec.709 Gamma 2.4. Никакого угадывания Log/DWG/ACES. Первая модель SDR 0–1, не HDR.
- У Negative `View Positive` меняет выход для контрольного просмотра; перед Print его выключают. Нет скрытого восстановления исходника и автоконтраста.
- Premultiplication читается из host image properties; alpha сохраняется и не служит метаданными.
- Материалы ComfyUI и смысл исходных параметров: `docs/REFERENCE_MAP.md`. Общий план: `docs/PLAN_LINUX_FIRST.md`.

## Команды пользователя

Для уже настроенного CUDA build:

```bash
cd /home/alex/Projects/ALEXZ_SilentFilm_OFX
cmake --build build-cuda --parallel 2
ctest --test-dir build-cuda --output-on-failure
```

В 0.3 ожидаются core, sensitometry и CUDA parity. При передаче следующей команды не требовать Conda. Установка после закрытия Resolve: `sudo cp -a build-cuda/ofx/ALEXZSilentFilm.ofx.bundle /usr/OFX/Plugins/` — её выполняет пользователь.

Git инициализирован на `main`, но первого коммита/remote ещё нет; исходники пока не закоммичены. Локальный `.code-workspace` с путями этой машины исключён из Git.
