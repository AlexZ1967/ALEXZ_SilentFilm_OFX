# Windows x64: подготовка, сборка и установка OFX

✅ **2026-11-19 — пользователь подтвердил Windows CPU Cadence23/23, CUDA31/31 без Skipped и расчёт из GUI на RTX2070/Resolve Studio21.1.2.** Установщик и OFX внутри Windows Resolve ещё ждут ручной проверки. Ниже сохранены исходные ограничения этапа подготовки.

**Установка одной командой:** [единый Windows-установщик](WINDOWS_INSTALLER.md) копирует OFX с запросом UAC и Cadence с меню для текущего пользователя. После успешной сборки предлагается установка; готовую сборку можно установить отдельно без компиляции.

**MSB4019 / отсутствует Microsoft.VCRedistVersion.default.props:** подготовлен [вариант сборки через Ninja](WINDOWS_NINJA_BUILD.md) с тем же MSVC14.39, без зависимости от проектов MSBuild. Windows-запуск пока не подтверждён.

**Если собираете программу впервые:** начните с [простого пошагового гайда](WINDOWS_USER_GUIDE_RU.md). Там объяснены окна установщиков, ввод команд, поиск SDK, признаки успеха и исправление типовых ошибок. Этот документ сохраняет технические детали pipeline.

✅ **2026-11-18 — подготовлены исходники и локальный pipeline Windows.** Сборка, CTest и запуск в Windows/Resolve **ещё не выполнялись**. Это инструкция для первой проверки, а не объявление проверенной поддержки.

## Границы этапа

Готовим нативный **ALEXZSilentFilm OFX**: Camera Optics, Negative, Print, Defects, Projector, Crank Exposure и остальные эффекты существующего bundle. CPU и CUDA собираются отдельно. Математика эффектов не заменяется Windows-аналогами.

Для **Cadence GTK/Pango** теперь подготовлен отдельный полный Windows-путь: FFmpeg/OpenCV helper, CPU/CUDA, loopback TCP provider, упаковка и меню Resolve. Он включается параметром `-Cadence` нашего PowerShell-сценария; OFX без этого параметра не получает FFmpeg/OpenCV/Python-зависимости. Подробные команды — [WINDOWS_CADENCE_APP.md](WINDOWS_CADENCE_APP.md). Реальная Windows-сборка и работа обоих комплектов ещё требуют пользовательской проверки.

## Что поставить на компьютер

Начальная конфигурация — Windows 11 x64, DaVinci Resolve Studio, для GPU-ветки NVIDIA. Windows ARM64, AMD/Intel GPU и OpenCL в этом этапе не проверяются. CPU-сборка не требует NVIDIA или CUDA.

| Компонент | Начальная конфигурация |
| --- | --- |
| Resolve | Установить Studio и записать точную версию; сначала отдельный тестовый проект |
| Git | [Git for Windows](https://git-scm.com/download/win) |
| CMake | **4.1 или новее**, добавить в PATH; [официальный установщик](https://cmake.org/download/) |
| Visual Studio | **Visual Studio 2022 / Build Tools 2022**, workload **Desktop development with C++** |
| MSVC | В Individual components выбрать **v143 x64/x86 v14.39 (17.9)**; установить также Windows SDK |
| CRT | [Microsoft Visual C++ Redistributable x64](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170), не старее выбранных Build Tools |
| CUDA, только GPU | **CUDA Toolkit 13.0 Update 1**, с Visual Studio Integration; сначала установить VS, затем Toolkit |
| NVIDIA driver | Совместимый с Toolkit и Resolve драйвер с [официального сайта NVIDIA](https://www.nvidia.com/drivers/); Toolkit не заменяет проверку драйвера |
| FFTW | Сборка vcpkg `fftw3:x64-silentfilm`, см. ниже |
| OpenFX headers | Имеющийся SDK Resolve; передать каталог с `ofxImageEffect.h` |

Для начала фиксируем именно MSVC **19.39 / toolset 14.39**, не произвольную последнюю версию Visual Studio. NVIDIA указывает MSVC 193x / VS2022 в [таблице CUDA 13.0 Update 1](https://docs.nvidia.com/cuda/archive/13.0.1/cuda-installation-guide-microsoft-windows/index.html). Язык проекта остаётся **C++17**. Исходное Linux-правило GCC/Clang относится к Linux; подготовленный Windows pipeline использует MSVC/NVCC.

Открыть **Developer PowerShell for VS 2022**, без Conda. Команды ниже пользователь выполняет сам:

```powershell
git --version
cmake --version
cl
nvidia-smi
& 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0\bin\nvcc.exe' --version
```

Последние две команды нужны только для CUDA. `nvidia-smi` показывает возможности драйвера; версию установленного Toolkit показывает `nvcc`. Если Developer PowerShell активировал другой MSVC, сборочный сценарий всё равно выбирает установленный 14.39 через CMake `-T`.

## Исходники и FFTW

Пример путей первой сборки — `C:\work\ALEXZ_SilentFilm_OFX` и `C:\work\vcpkg`. Linux-пути не переносить буквально. На первой проверке использовать короткий путь без кириллицы для build/dependencies; русский пресет и пользовательскую папку проверить отдельно.

```powershell
New-Item -ItemType Directory -Path C:\work -Force
Set-Location C:\work
git clone https://github.com/AlexZ1967/ALEXZ_SilentFilm_OFX.git
git clone https://github.com/microsoft/vcpkg.git
git -C C:\work\vcpkg checkout de36db04550a8625e3154b1a758c60035901f244
& C:\work\vcpkg\bootstrap-vcpkg.bat -disableMetrics
if ($LASTEXITCODE -ne 0) { throw 'vcpkg bootstrap failed' }
& C:\work\vcpkg\vcpkg.exe install fftw3:x64-silentfilm --overlay-triplets=C:\work\ALEXZ_SilentFilm_OFX\cmake\triplets
if ($LASTEXITCODE -ne 0) { throw 'FFTW installation failed' }
```

Нужна ревизия нашего проекта, содержащая этот Windows-этап; подготовленные локально файлы становятся доступны через clone после отдельного commit/push. Ревизия vcpkg зафиксирована при подготовке инструкции; её работа с этой конфигурацией ещё требует первой сборки. Не обновлять зависимости одновременно с первым поиском ошибок переноса.

Triplet `cmake/triplets/x64-silentfilm.cmake` задаёт x64, статические библиотеки, динамический CRT `/MD`, toolset 14.39. Так `fftw3f` включается в наш модуль без дополнительной FFTW DLL. Сам CRT устанавливается через Redistributable. [Порт Microsoft](https://github.com/microsoft/vcpkg/tree/de36db04550a8625e3154b1a758c60035901f244/ports/fftw3) собирает также single precision; двойная точность не заменяет нужную библиотеку `fftw3f`.

Не использовать для этой инструкции `x64-windows` или MinGW-библиотеку: получится другая схема линковки. Не копировать случайные DLL в каталог Resolve.

Заголовки OpenFX брать из имеющегося SDK: найти `ofxImageEffect.h` в Developer/OpenFX и далее передать **сам каталог заголовков**. Расположение SDK зависит от установки; сценарий не угадывает его и не копирует SDK в репозиторий.

## Pipeline CPU → CUDA → архив

Начать с CPU: это отделяет ошибки MSVC/FFTW/пресетов от CUDA. `C:\SDK\OpenFX\include` ниже — пример, заменить реальным каталогом.

```powershell
Set-Location C:\work\ALEXZ_SilentFilm_OFX
& .\scripts\windows\Build-Windows.ps1 `
  -OfxIncludeDir 'C:\SDK\OpenFX\include' `
  -VcpkgRoot C:\work\vcpkg
```

Сценарий выполняет configure → Release build → CTest → staging → ZIP. При ошибке следующий этап не запускается. Команда **не устанавливает OFX в Resolve**. Если PowerShell блокирует локальные скрипты, можно разрешить их только для текущего окна: `Set-ExecutionPolicy -Scope Process RemoteSigned`.

После успешной CPU-проверки — CUDA. Значение **86 соответствует RTX 3090**, это пример с текущей Linux-машины. Для другой видеокарты выбрать её compute capability по [таблице NVIDIA](https://developer.nvidia.com/cuda-gpus), например `8.6 → 86`. Список архитектур должен поддерживаться выбранным Toolkit.

```powershell
& .\scripts\windows\Build-Windows.ps1 `
  -OfxIncludeDir 'C:\SDK\OpenFX\include' `
  -VcpkgRoot C:\work\vcpkg `
  -Cuda `
  -CudaRoot 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0' `
  -CudaArchitectures 86
```

Результаты:

```text
build-windows-cpu/stage/ALEXZSilentFilm.ofx.bundle/Contents/Win64/ALEXZSilentFilm.ofx
build-windows-cpu/ALEXZSilentFilm-windows-x64-cpu.zip
build-windows-cuda/stage/ALEXZSilentFilm.ofx.bundle/Contents/Win64/ALEXZSilentFilm.ofx
build-windows-cuda/ALEXZSilentFilm-windows-x64-cuda.zip
```

CUDA bundle также содержит cuFFT DLL и найденную рядом nvJitLink DLL выбранного Toolkit. На Windows [статическая cuFFT недоступна](https://docs.nvidia.com/cuda/archive/13.0.0/cufft/index.html). Delay-load hook загружает cuFFT из папки самого OFX по абсолютному пути, без изменения общего DLL-поиска Resolve. CPU FFTW и CUDA Runtime линкуются статически. Драйверную `nvcuda.dll` не копировать: её устанавливает NVIDIA driver.

Если CMake не находит ровно одну cuFFT DLL, проверить каталог выбранного Toolkit и переменную `SILENTFILM_CUDA_RUNTIME_DIR` в CMake cache. Не смешивать import library и DLL разных Toolkit. Вариант Linux/static cuFFT сохранён.

Повторная сборка и тесты вручную:

```powershell
cmake --build build-windows-cpu --config Release --parallel 2
ctest --test-dir build-windows-cpu -C Release --output-on-failure
cmake --build build-windows-cuda --config Release --parallel 2
ctest --test-dir build-windows-cuda -C Release --output-on-failure
```

`-C Release` обязателен для Visual Studio generator. CUDA тесты с результатом **Skipped** не подтверждают GPU. Проверка symlink в preset-тесте может отдельно пропускаться без Windows Developer Mode/права создания symlink; остальной тест продолжает работать. Linux ABI trap тест в Windows не регистрируется.

## Установка и настройка Resolve

Закрыть Resolve. Открыть PowerShell **от имени администратора**, перейти в проект и установить выбранный **один** bundle:

```powershell
Set-Location C:\work\ALEXZ_SilentFilm_OFX
& .\scripts\windows\Install-OFX.ps1 `
  -Bundle .\build-windows-cuda\stage\ALEXZSilentFilm.ofx.bundle
```

Для CPU заменить `build-windows-cuda` на `build-windows-cpu`. Каталог установки:

```text
C:\Program Files\Common Files\OFX\Plugins\ALEXZSilentFilm.ofx.bundle\Contents\Win64\
```

Это [стандартный каталог OpenFX](https://openfx.readthedocs.io/en/latest/Reference/ofxPackaging.html). Нужна папка bundle целиком. CPU и CUDA имеют одинаковые plugin IDs и не устанавливаются одновременно под разными именами. Старый bundle сценарий сохраняет вне сканируемой папки: `%LOCALAPPDATA%\ALEXZSilentFilm\ofx-backups`. Для отката при закрытом Resolve убрать текущий bundle и вернуть нужную резервную копию с именем `ALEXZSilentFilm.ofx.bundle`.

Запустить Resolve вручную. В **Preferences → System → Memory and GPU** для CUDA-сборки выбрать CUDA и нужную NVIDIA GPU, сохранить настройки; если Resolve просит перезапуск — выполнить его. В Effects/Open FX искать **SilentFilm**; на Color добавлять OFX в ноду. Точный вид меню сверить с установленной версией Resolve.

Пользовательские пресеты Windows хранятся в:

```text
%LOCALAPPDATA%\ALEXZSilentFilm\ofx-presets\<plugin ID>\<имя>.sfpreset
```

Новая Windows-ветка использует Win32 Unicode API, атомарную замену файла и ту же схему пресета. Linux XDG/HOME-путь сохраняется. Русские имена и пробелы предусмотрены, но ещё требуют проверки в настоящем Resolve. У Crank Exposure перенос Linux-пути карты не автоматический: назначить существующий Windows-путь через Exposure Map.

## Приёмка первой Windows-сборки

- [ ] Записать Windows/Resolve/GPU/driver, MSVC, CMake, NVCC, commit проекта и vcpkg.
- [ ] CPU configure/build/CTest без ошибок; отдельно сохранить вывод.
- [ ] CUDA configure/build/CTest; parity-тесты реально выполнились, не Skipped.
- [ ] Resolve увидел все эффекты bundle; в журнале нет Failed to load / missing DLL.
- [ ] Camera Optics: общий/краевой расфокус, гуляющий фокус, виньетка; сравнить с Linux на одинаковом кадре.
- [ ] Negative → Print: монохром, зерно, halation; Projector: flicker, weave и **Glow > 0**.
- [ ] Defects: пыль/царапины/проявка, Blend 0/0.5/1, последовательный и произвольный порядок кадров.
- [ ] Save → Refresh → выбор → Apply с русским именем; Factory → свой пресет; сохранить/открыть проект.
- [ ] Crank Exposure: карта назначена, Blend 0/1, CPU/CUDA, первый/последний кадры.
- [ ] Несколько экземпляров, playback/render; нормальное закрытие Resolve после Projector Glow.
- [ ] Повторная установка и откат bundle при закрытом Resolve.

Если модуль не загрузился: сначала проверить x64, содержимое `Contents/Win64`, CRT x64 и наличие cuFFT/nvJitLink из той же установки Toolkit. Из Developer PowerShell можно выполнить `dumpbin /exports ...\ALEXZSilentFilm.ofx` (нужны `OfxGetNumberOfPlugins`, `OfxGetPlugin`) и `dumpbin /dependents ...\ALEXZSilentFilm.ofx`. Не удалять весь кэш/настройки Resolve при первой ошибке. Для сообщения об ошибке нужны полный лог сборки/CTest либо Resolve diagnostic log и точные версии.

## Дальнейшие этапы

- [x] ✅ **2026-11-18 — подготовлен OFX pipeline:** Windows preset storage, cuFFT DLL packaging/loading, PowerShell build/install, инструкция компьютера.
- [ ] Первая реальная сборка MSVC CPU, затем CUDA; исправления по логу.
- [ ] Ручная приёмка в Windows Resolve; только после этого подтвердить поддержку проверенной конфигурации.
- [x] ✅ **2026-11-18 — подготовлен полный перенос Cadence App:** Windows transport, native FFmpeg/OpenCV, GTK/Pango launcher/меню. [Команды](WINDOWS_CADENCE_APP.md).
- [ ] Windows Cadence: пользовательская сборка/тесты и end-to-end в Resolve.
- [ ] Автоматизация Windows CI после подтверждения локального toolchain; SDK и GPU checks определить отдельно.
