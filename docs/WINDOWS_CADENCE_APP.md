# Cadence GTK/Pango для Resolve на Windows: сборка, запуск и расчёт

✅ **2026-11-18 — подготовлен полноценный Windows-путь приложения:** native FFmpeg/OpenCV helper, существующие CPU/CUDA алгоритмы, запрос обработанных кадров через loopback TCP, launcher и установка команды в меню Resolve. **Сборка и работа на Windows ещё не проверены.** Агент не запускал установку, тесты или Resolve; подтверждение возможно после прохождения шагов ниже пользователем.

Ранее здесь описывался только пробный запуск GUI. Эта редакция заменяет его: теперь собираем настоящий `silentfilm_cadence.exe` и полный комплект приложения. Одного OFX bundle для этого недостаточно.

## 1. Что потребуется

- Windows x64 и **DaVinci Resolve Studio 21.1+** с встроенным ResolvePython. Первичная цель — функциональность API, используемая текущим Linux-приложением, включая TimelineItem.ExportFrame.
- Инструменты из [подробного Windows-гайда](WINDOWS_USER_GUIDE_RU.md): VS2022/MSVC14.39, Git, CMake4.1+, Runtime x64, vcpkg зафиксированной там ревизии и OpenFX SDK.
- Для GPU — NVIDIA, совместимый драйвер и CUDA Toolkit13.0 Update1. Без NVIDIA можно собрать CPU-вариант.
- Для окна — **MSYS2 UCRT64 Python + GTK4/Pango**, устанавливаются следующим шагом.

Роли разделены: GTK работает в Python MSYS2, Resolve API — в отдельном встроенном ResolvePython, расчёт — в нативном `.exe`. DLL GTK/FFmpeg/OpenCV не встраиваются в OFX-модуль.

Пример путей: проект `C:\work\ALEXZ_SilentFilm_OFX`, vcpkg `C:\work\vcpkg`, MSYS2 `C:\msys64`. Windows-сценарии должны уже находиться в вашей копии проекта. Локальные изменения становятся доступны через GitHub после отдельного commit/push.

## 2. Установить GTK/Pango

1. Скачать x86_64 installer с [официального сайта MSYS2](https://www.msys2.org/).
2. Установить, например, в `C:\msys64`.
3. Через «Пуск» открыть **MSYS2 UCRT64**.
4. Обновить окружение:

```bash
pacman -Syu
```

Если обновление закрыло терминал, снова открыть UCRT64 и повторить команду. MSYS2 поддерживает [полное обновление](https://www.msys2.org/docs/updating/).

В том же UCRT64-терминале установить:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gtk4 mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-gobject
```

Pango/Cairo приходят с зависимостями. Это [официальный путь PyGObject для Windows](https://pygobject.gnome.org/getting_started.html#windows). Отдельный `pip install pygobject` в Python из python.org не нужен.

Проверить, выполняя команды самостоятельно:

```bash
which python
python --version
gtk4-demo
```

Ожидается `/ucrt64/bin/python` и окно демонстрации GTK. После проверки его закрыть. Не добавлять MSYS2 в глобальный PATH Windows/Resolve: launcher настраивает окружение только своего процесса.

## 3. Подготовить библиотеки native helper

Закрыть UCRT64-терминал или оставить его отдельно. Следующие команды вводить в **Developer PowerShell for VS 2022**, а не в MSYS2 и не в Console Resolve.

```powershell
Set-Location C:\work\ALEXZ_SilentFilm_OFX
Set-ExecutionPolicy -Scope Process RemoteSigned
& .\scripts\windows\Prepare-Cadence-Dependencies.ps1 -VcpkgRoot C:\work\vcpkg
```

vcpkg должен быть скачан и bootstrap выполнен по основному гайду. Скрипт устанавливает:

- FFTW `fftw3:x64-silentfilm` — статическая библиотека для ядра;
- FFmpeg с avcodec/avformat/avfilter/swscale и zlib для PNG, программами ffmpeg/ffprobe;
- OpenCV4 core/imgproc/video;
- Zstd DLL для индивидуального Color DRX.

Медиабиблиотеки используют отдельный triplet **x64-silentfilm-media**: MSVC14.39, динамический CRT и DLL. Пакеты собираются из [портов Microsoft vcpkg](https://github.com/microsoft/vcpkg/tree/de36db04550a8625e3154b1a758c60035901f244/ports). MinGW-библиотеки MSYS2 в native helper не линкуются.

Это может занять значительное время. Дождаться **Cadence dependencies prepared**. При ошибке остановиться и сохранить лог; не переходить к сборке. Скрипт не меняет установленный Resolve.

## 4. Собрать CPU-вариант

Нужен каталог, в котором рядом лежат `ofxImageEffect.h` и `ofxGPURender.h`. Найти его по разделу9 [основного гайда](WINDOWS_USER_GUIDE_RU.md). Если переменная `$ofxInclude` в текущем PowerShell ещё задана, использовать её. Иначе задать реальный путь заново, например:

```powershell
$ofxInclude = 'C:\work\ofx-sdk\OpenFX-1.4\include'
```

Путь — пример; заменить фактическим. Затем:

```powershell
& .\scripts\windows\Build-Windows.ps1 -OfxIncludeDir $ofxInclude -VcpkgRoot C:\work\vcpkg -Cadence
```

`-Cadence` включает настоящий helper и требует установленные медиабиблиотеки/GTK Python. Сценарий выполняет конфигурацию → сборку Release → CTest → упаковку. При ошибке не продолжает упаковку.

Ожидаемая папка готового приложения:

```text
C:\work\ALEXZ_SilentFilm_OFX\build-windows-cadence-cpu\stage\SilentFilmCadence
```

В ней должны быть:

```text
SilentFilmCadence\
  Start-Cadence.cmd
  Start-Cadence.ps1
  Install-Cadence-Menu.ps1
  cadence_runtime.json
  bin\silentfilm_cadence.exe
  bin\ffmpeg.exe, ffprobe.exe, zstd.dll и другие DLL
  scripts\cadence_*.py
  presets\crank_exposure_template*.drx
```

Ожидаемый архив: `build-windows-cadence-cpu\SilentFilmCadence-windows-x64-cadence-cpu.zip`. Отдельный OFX bundle также собирается и упаковывается. GTK runtime в архив не включён: он берётся из установленного MSYS2.

## 5. Собрать GPU-вариант

После успешной CPU-сборки. Для **RTX3090** архитектура86; для другой карты выбрать значение по [таблице NVIDIA](https://developer.nvidia.com/cuda-gpus), как в основном гайде.

```powershell
& .\scripts\windows\Build-Windows.ps1 -OfxIncludeDir $ofxInclude -VcpkgRoot C:\work\vcpkg -Cadence -Cuda -CudaRoot 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0' -CudaArchitectures 86
```

Готовое приложение:

```text
C:\work\ALEXZ_SilentFilm_OFX\build-windows-cadence-cuda\stage\SilentFilmCadence
```

Архив: `build-windows-cadence-cuda\SilentFilmCadence-windows-x64-cadence-cuda.zip`. В комплект добавляются cuFFT/nvJitLink DLL выбранного Toolkit. Драйверную `nvcuda.dll` не копировать.

Пропущенный CUDA-тест (`Skipped`) не подтверждает GPU. В существующем алгоритме Farneback рассчитывает CPU OpenCV, а CUDA выполняет деформацию flow, confidence/blend, интеграцию выдержки и движение/экспозицию через существующий VRAM-кэш. Перенос не заменяет математику и не превращает CPU Farneback в другой GPU-алгоритм.

Если MSYS2 установлен иначе, добавить к сборке `-MsysRoot 'D:\msys64'`. Это параметр нашего PowerShell-сценария.

## 6. Разместить приложение и настроить Resolve

После успешной сборки можно оставить приложение в `stage` либо **скопировать всю папку SilentFilmCadence** в постоянное место, например `C:\work\SilentFilmCadence`. Не переносить только `.exe` или `.cmd`. Для обновления копировать полный комплект при закрытом приложении, без активных расчётов.

В Resolve открыть **Preferences → System → General → External scripting → Local**. Нужен Studio и открытый проект/таймлайн. Настройка GPU Resolve для OFX отдельно описана в основном гайде.

Стандартный interpreter по установленному SDK Resolve:

```text
C:\Program Files\Blackmagic Design\DaVinci Resolve\ResolvePython\ResolvePython.exe
```

Если Resolve установлен иначе, передать путь launcher-параметром `-ResolvePython`. Проверить API можно из обычного PowerShell:

```powershell
@'
import DaVinciResolveScript as d
print(d.scriptapp("Resolve") is not None)
'@ | & 'C:\Program Files\Blackmagic Design\DaVinci Resolve\ResolvePython\ResolvePython.exe' -
```

При работающем Resolve ожидается `True`. Если `False` или ошибка импорта — сначала восстановить подключение, затем переходить к приложению.

## 7. Запустить приложение

В Проводнике открыть постоянную папку приложения и дважды щёлкнуть **Start-Cadence.cmd**. Для CUDA-сборки автоматически выбран backend `cuda`, для CPU — `cpu`. В самом приложении режим можно менять в пределах возможностей собранного helper.

Альтернативно из PowerShell:

```powershell
& 'C:\work\SilentFilmCadence\Start-Cadence.ps1'
```

Если папки установки отличаются:

```powershell
& 'C:\work\SilentFilmCadence\Start-Cadence.ps1' -MsysRoot 'D:\msys64' -ResolvePython 'D:\DaVinci Resolve\ResolvePython\ResolvePython.exe'
```

Launcher проверяет существование реального `.exe`, GUI и interpreter; отсутствующий helper больше не является допустимым «пробным» запуском. Сценарий настраивает GTK-пути локально и восстанавливает окружение после завершения. Для native/ResolvePython дочерних процессов пути GTK удаляются; отдельные глобальные изменения Windows не требуются.

## 8. Добавить запуск в меню Resolve

В обычном PowerShell выполнить из постоянного места приложения:

```powershell
Set-ExecutionPolicy -Scope Process RemoteSigned
& 'C:\work\SilentFilmCadence\Install-Cadence-Menu.ps1'
```

При нестандартных путях добавить те же `-MsysRoot`/`-ResolvePython`. Администратор не нужен: устанавливается только пользовательский menu-script.

Сценарий создаёт:

```text
%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\Fusion\Scripts\Utility\SilentFilm Cadence.py
```

Предыдущий menu-script сохраняется вне папки скриптов. Затем вручную перезапустить Resolve и выбрать **Workspace → Scripts → Utility → SilentFilm Cadence**. Меню запускает отдельное GTK-окно, не загружая GTK в Resolve.

При переносе папки приложения menu-script установить заново: он запоминает её абсолютный путь. Пункт контекстного меню по правой кнопке клипа не добавляется.

## 9. Выполнить первый полноценный расчёт

1. Создать тестовый проект. Добавить короткий обычный видеоклип, подрезать начало/конец и оставить рядом следующий клип для проверки позиций.
2. Выделить нужный клип на таймлайне.
3. Запустить Cadence через меню. Убедиться, что показаны выбранный клип и диапазон.
4. Для первого расчёта выбрать **Use Raw Source**, **Bake Exposure**, **Add Above Source**. Задать Output Folder в короткий доступный путь, например `C:\work\CadenceResults`.
5. Нажать **Calculate**. Ожидать прогресс и завершение; при включённом Auto Import результат должен появиться в Media Pool и над исходником в том же таймлайне.
6. Проверить начало/конец, новую длительность и позиции остальных клипов. Дрожание/неравномерность оценивать на движущемся материале, не на стоп-кадре.
7. Добавить к исходнику хорошо заметную цветокоррекцию/эффект. Выключить **Use Raw Source** и выполнить новый расчёт. Обработки должны сохраниться в результате.
8. Отдельно проверить **Replace Source**, **Exposure via OFX**, несколько выделенных клипов, отмену и очистку кэша.

Исходный In/Out и FPS берутся из монтажа в обоих режимах. Raw читает исходник напрямую; Processed запрашивает нужные обработанные PNG через Resolve API. Транспорт Windows использует **только 127.0.0.1, случайный порт и токен**, не Render Queue и не промежуточный фильм. PNG декодируется CPU FFmpeg, затем кадры поступают в существующий CPU/CUDA pipeline. Прямого GPU image API Resolve здесь нет.

Остальные монтажные позиции сохраняются; сокращение при Replace оставляет промежуток. Add Above использует подходящую дорожку выше, включая отключённую, по существующему алгоритму. Возможности API конкретной Windows-версии Resolve требуют ручной проверки.

Индивидуальные exposure map/DRX лежат рядом с результатом в папке задания. Для автоматической ноды Crank Exposure нужен установленный OFX bundle этой же версии. Проверка Windows Color/Fusion и схемы DRX остаётся частью приёмки; Bake Exposure не зависит от назначения такой ноды.

## 10. Тесты и диагностика

Сценарий сборки сам запускает CTest. Для повторного запуска:

```powershell
ctest --test-dir C:\work\ALEXZ_SilentFilm_OFX\build-windows-cadence-cuda -C Release --output-on-failure
```

Новые ключевые проверки: `silentfilm_cadence_platform`, `silentfilm_frame_provider_tcp`, прежние export/flow/bridge/preferences/cache/Color и CUDA parity. TCP integration использует production C++ reader, проверяет реальные PNG-пиксели, Unicode job/output path, длительность и передачу ошибок. На Linux дополнительно сохраняется прежний abstract socket и тестируется TCP для регрессии общего протокола.

Для Linux-регрессии пользователь выполняет:

```bash
cmake -S . -B build-cuda
cmake --build build-cuda --parallel 2
ctest --test-dir build-cuda --output-on-failure
```

Ни одна из этих команд агентом не выполнялась.

Настройки/пресеты Cadence: `%LOCALAPPDATA%\ALEXZ\SilentFilm\cadence_settings.json`. Корень результатов по умолчанию: `%LOCALAPPDATA%\ALEXZSilentFilm\cadence\renders`; Output Folder можно изменить в GUI. Это отдельные хранилища от OFX-presets.

Если запускали через меню и окна нет, открыть `%LOCALAPPDATA%\ALEXZSilentFilm\cadence\app_start.log`. Для ошибки расчёта — **Show Folder**, затем `render.log`, `resolve_frames.log`, `resolve_bridge.log`, `status.ini`. Не удалять папку неудачного задания до разбора ошибки.

| Ошибка | Что проверить |
| --- | --- |
| Required file not found | Весь комплект приложения, MSYS2 и реальный путь ResolvePython |
| Нет GTK/Pango/typelib | Пакеты именно UCRT64, запуск нашим launcher, не произвольным Python |
| CMake не находит FFmpeg/OpenCV | Prepare-Cadence-Dependencies; отдельный triplet media; полный лог ошибки |
| Программа не запускается из-за DLL | Скопирована вся папка bin, установлен Runtime x64; не подменять случайными DLL |
| Не найден zstd.dll | Полная упаковка приложения и установленная dependency zstd |
| Нет подключения Resolve | Studio21.1+, External scripting Local, проект/таймлайн открыты |
| Нет TimelineItem.ExportFrame/selected clips API | Проверить фактическую версию Resolve и его SDK, прислать сообщение; Render Queue fallback не включается |
| TCP provider unavailable | Создать новое задание; проверить resolve_frames.log и политики локального IPC; не открывать порт наружу |
| Длинный/недоступный путь результата | Выбрать короткую доступную Output Folder и новое задание |

## Подтверждённая Linux-регрессия

✅ **2026-11-18 — по прямому запросу пользователя агент выполнил configure/build и CTest в `build-cuda`: Release, CUDA86, Cadence ON. 32/32 Passed, без пропусков, 9.27с.** Включая TCP и abstract provider, реальные PNG, Unicode job/output, platform lifecycle и CUDA parity. Исправления не потребовались. Это не подтверждает исполнение WinSock/MSVC/PowerShell или работу в Windows Resolve; прежние пометки о незапущенных проверках относятся к подготовке, до этого прогона.

## Приёмка Windows

- [x] ✅ **2026-11-18 — подготовлены исходники полного Windows Cadence и команды сборки/упаковки/запуска/меню.**
- [ ] CPU build/CTest, включая TCP и реальные PNG-пиксели.
- [ ] CUDA build/CTest, фактические выполненные parity-тесты.
- [ ] Запуск GUI через .cmd и меню Resolve.
- [ ] Raw и Processed: правильные In/Out/FPS, обработки, длительность, Unicode имена.
- [ ] Replace/Add Above без сдвига остального монтажа, отключённая дорожка, несколько клипов.
- [ ] Bake/OFX, карта/DRX и Crank Exposure.
- [ ] Отмена/ошибка/закрытие GUI/повторное открытие задания, кэш сохраняет используемые клипы.
- [ ] Замер скорости и VRAM на реальном клипе, нормальное завершение Resolve.

До этих проверок статус — **реализация подготовлена, Windows runtime не подтверждён**.
