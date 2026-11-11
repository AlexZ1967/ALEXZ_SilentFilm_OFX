# Дополнительные зависимости

## Linux: optical flow в Cadence

Для режима **`blur_mode=flow_integrated`** нужен C++ OpenCV с компонентами `core`, `imgproc`, `video`. В Debian/Ubuntu пакет устанавливает пользователь:

```bash
sudo apt install libopencv-dev
cmake -S . -B build-cuda
cmake --build build-cuda --parallel 2
```

OpenCV линкуется только в `silentfilm_cadence` и отдельный flow-тест. OFX и `silentfilm_core` не получают этой зависимости. Без OpenCV сборка сохраняет режим `simple`; запрос `flow_integrated` останавливается с указанием зависимости, без молчаливой подмены алгоритма. После установки пакета нужно повторить конфигурацию CMake и сборку helper. Python OpenCV/PyTorch/Conda не требуются.

В текущем переносе Farneback и деформация/проверка доверия выполняются на CPU OpenCV, как в CPU-ветке Comfy. При выборе CUDA готовые временные отсчёты загружаются в переиспользуемые GPU-буферы, где выполняются shake, интеграция выдержки и экспозиция; один readback на выходной кадр. Это не полный GPU flow pipeline, скорость ещё не измерена.

## Linux: FFTW3

Для halation и Projector используется FFTW3 single precision: CMake ищет заголовок `fftw3.h` и библиотеку `fftw3f` (`libfftw3f`). В Debian и Ubuntu нужный пакет разработки называется `libfftw3-dev`; он предоставляет файлы разработки для FFTW, включая single precision.

Установка:

```bash
sudo apt update
sudo apt install libfftw3-dev
```

Затем конфигурируйте и собирайте проект как обычно. Пакет и его наличие в репозиториях подтверждены каталогами пакетов [Debian](https://packages.debian.org/source/fftw3) и [Ubuntu](https://packages.ubuntu.com/libfftw3-dev).

## Windows: FFTW3 на будущее

Поддержка Windows для плагина пока не проверялась. Для будущей x64-сборки рекомендуемый путь — установить [vcpkg](https://github.com/microsoft/vcpkg), затем из Developer PowerShell с настроенным `VCPKG_ROOT` выполнить:

```powershell
vcpkg install fftw3:x64-windows
```

Передавайте vcpkg toolchain при конфигурации CMake:

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DCMAKE_BUILD_TYPE=Release
```

Порт vcpkg собирает FFTW для выбранного triplet; текущий CMake ожидает `fftw3.h` и библиотеку single precision `fftw3f`. Для запуска плагина также потребуется, чтобы соответствующая DLL FFTW находилась в доступном Windows пути загрузки или рядом с bundle. Эти команды являются предварительной инструкцией: toolchain, упаковку DLL и загрузку OFX на Windows ещё требуется проверить. Порт и команда установки описаны в [каталоге vcpkg для FFTW3](https://vcpkg.io/en/package/fftw3.html); официальные заметки FFTW по Windows доступны [на сайте FFTW](https://fftw.org/install/windows.html).


## Camera CUDA

Начиная с версии 0.7 Camera использует `cudaMallocAsync`/`cudaFreeAsync` для временных GPU-буферов. Для CUDA-сборки нужен Toolkit 11.2+ и GPU/драйвер с поддержкой stream-ordered memory pools. На рабочей машине уже установлен Toolkit 13.0.88 и RTX 3090; дополнительные Python-модули или CUDA-библиотеки для Camera не нужны. FFTW3 остаётся зависимостью CPU halation/Projector. Новый Camera GPU-путь пока ожидает пользовательской сборки и проверки; Windows CUDA отдельно не проверена.


## Projector CUDA 0.9: cuFFT

Для GPU glow используется cuFFT из установленного CUDA Toolkit; в текущем проекте запрашиваются CMake targets `CUDA::cufft_static`, `CUDA::culibos`, `CUDA::cuda_driver`, а при наличии — `CUDA::nvJitLink_static`. Локальный Toolkit 13.0.88 уже содержит cuFFT static и cuLIBOS; дополнительных Python-модулей для этого нет. CUDA-сборка также подключает системные Threads/dl. FFTW3 остаётся обязательной для CPU-эталона и других плёночных эффектов.

Перенесённый спектральный отклик одинаков, но FFTW3 и cuFFT могут различаться в округлении single precision. GPU-планы связаны с host stream, а рабочая память управляется отдельно; принципы описаны в [официальном руководстве cuFFT 13.0](https://docs.nvidia.com/cuda/archive/13.0.0/cufft/index.html). JIT/LTO-оптимизации Toolkit могут использовать его nvJitLink; при несовместимой/отсутствующей зависимости код cuFFT будет виден в логе. Агент зависимости не устанавливал, сборку не запускал. Windows для этого пути пока не проверена.


Для static cuFFT backend собирается с `CUDA_SEPARABLE_COMPILATION=ON`, а `CUDA_RESOLVE_DEVICE_SYMBOLS=ON` задаётся на конечных целях: OFX и трёх CUDA test executables. Внутри static backend device linking выключен, чтобы регистрационный объект непосредственно входил в конечную линковку и не оставался невыбранным членом архива до cuFFT. Архитектуры и static runtime на конечных целях заданы явно. Свойство описано в [CMake CUDA_RESOLVE_DEVICE_SYMBOLS](https://cmake.org/cmake/help/latest/prop_tgt/CUDA_RESOLVE_DEVICE_SYMBOLS.html).

После изменения RDC/device-link настроек пересоберите с очисткой объектов:

```bash
cmake --build build-cuda --clean-first --parallel 2 && ctest --test-dir build-cuda --output-on-failure
```

Дополнительные пакеты для ошибки `__cudaRegisterLinkedBinary_...` не требуются. Предыдущая попытка разрешения symbols внутри static backend не устранила ошибку. Агент сборку и тесты после переноса device link на конечные цели не запускал; результат пока не подтверждён.


## Нативная подготовка каденса 0.10: FFmpeg SDK

Опция `SILENTFILM_BUILD_CADENCE` по умолчанию ON на базовой Linux-сборке. Для `silentfilm_cadence` нужны pkg-config и development libraries FFmpeg; OFX-модуль с ними не линкуется:

```bash
sudo apt update
sudo apt install pkg-config libavformat-dev libavcodec-dev libavutil-dev libswscale-dev ffmpeg
```

Утилита непосредственно использует libavformat/libavcodec/libavutil/libswscale в своём процессе. `/usr/bin/ffmpeg` нужен также GTK-приложению для потоковой упаковки PNG кадров Resolve в FFV1; `ffprobe` нужен для условного интеграционного CTest. Native helper использует библиотеки FFmpeg напрямую. Кодирование ProRes использует штатный `prores_ks` при его наличии в установленной libavcodec; альтернатива — собственный writer TIFF RGBA16 без дополнительного encoder-пакета. CUDA по-прежнему включается отдельной `SILENTFILM_ENABLE_CUDA`; native CUDA target получает тот же финальный device-link шаг. Для OFX без helper можно задать `-DSILENTFILM_BUILD_CADENCE=OFF`.

Прежний скрипт Resolve написан на Lua, дополнительных Python-модулей/Conda не требует. Его инструкция — `CADENCE_PREPARE.md`.

## Отдельное окно Cadence 0.11

Для нового Linux-окна GTK 4/Pango пользователь устанавливает системный PyGObject:

```bash
sudo apt install python3-gi python3-gi-cairo gir1.2-gtk-4.0
```

GTK/Pango работают в отдельном `/usr/bin/python3`, связь с Resolve — в отдельном `/opt/resolve/bin/ResolvePython` (поставляется с Studio 21.1+). GTK не загружается внутрь Resolve. Pip/Conda не требуются; OFX и native helper не получают Python-зависимости. В Resolve включается External scripting → Local. CMake создаёт `build-cuda/bin/SilentFilm_Cadence_App` и меню-launcher `build-cuda/scripts/SilentFilm Cadence.py`; подробности и ограничения — `CADENCE_APP.md`. Это подготовленный код, runtime-проверок ещё не было.

Команда GTK-пакетов: [официальная инструкция PyGObject для Debian/Ubuntu](https://pygobject.gnome.org/getting_started.html). Pango устанавливается как зависимость GTK; development headers для Python-окна не требуются.
