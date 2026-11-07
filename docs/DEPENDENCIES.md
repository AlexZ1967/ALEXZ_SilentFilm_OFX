# Дополнительные зависимости

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

Утилита непосредственно использует libavformat/libavcodec/libavutil/libswscale в своём процессе. `ffmpeg`/`ffprobe` как исполняемые файлы нужны для условного интеграционного CTest, но production helper не вызывает их через shell. Кодирование ProRes использует штатный `prores_ks` при его наличии в установленной libavcodec; альтернатива — собственный writer TIFF RGBA16 без дополнительного encoder-пакета. CUDA по-прежнему включается отдельной `SILENTFILM_ENABLE_CUDA`; native CUDA target получает тот же финальный device-link шаг. Для OFX без helper можно задать `-DSILENTFILM_BUILD_CADENCE=OFF`.

Скрипт Resolve написан на Lua, дополнительных Python-модулей/Conda не требует. Первая версия подготовлена только под Linux; установка пакетов, сборка и runtime-проверки агентом не выполнялись. Полная инструкция — `CADENCE_PREPARE.md`.
