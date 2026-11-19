# Автономный Windows-пакет Cadence и OFX

✅ **2026-11-20 — полное копирование UCRT64 заменено отбором runtime-зависимостей.** Причина — пользователь сообщил о чрезмерно долгой упаковке и лишних файлах. Новый отбор и его PE-тесты подготовлены, но ещё не запускались агентом. Пользователь останавливает старую упаковку; весь stage удалять не нужно.

✅ **2026-11-19 — подготовлена сборка одного Setup.exe.** Новый pipeline упаковки и установка на машине без MSYS2 пока не проверены. Прежние31/31 CUDA-тест и ручной расчёт относятся к обычной Windows-сборке, не подтверждают переносимость нового runtime.

## Что получает вторая машина

Один `ALEXZSilentFilm-Setup-…exe`. Запустить двойным щелчком из обычной учётной записи, закрыв Resolve и Cadence. Мастер распакует пакет, запросит UAC для Visual C++ Runtime/OFX и установит:

- Cadence с Python, GTK4, Pango, Cairo, typelibs, темами, переводами и лицензиями;
- native helper с FFmpeg/OpenCV и CUDA DLL;
- Visual C++ x64 Runtime из подписанного дистрибутива Microsoft;
- OFX bundle и пункт Workspace → Scripts → Utility → SilentFilm Cadence.

MSYS2, Python, Visual Studio, CMake, vcpkg, CUDA Toolkit и Inno Setup **на второй машине не нужны**. Нужны подходящие Windows x64, драйвер NVIDIA для CUDA-пакета и Resolve Studio21.1+ с External scripting → Local. Их установщик не заменяет. Модель GPU должна соответствовать архитектурам пакета; сведения записываются в package-info.json и имя Setup.exe.

Новая версия сохраняет предыдущие компоненты по правилам [общего установщика](WINDOWS_INSTALLER.md). Задания, кэш и пресеты не удаляются. Runtime Cadence находится только в папке приложения; GTK/Python не загружаются в процесс Resolve. Мастер не добавляет системный PATH и не запускает Resolve автоматически.

Мастер Setup.exe — оболочка общего установщика с резервными копиями. Отдельного удаления через «Установленные приложения Windows» сейчас нет. При ошибке см. `%LOCALAPPDATA%\ALEXZSilentFilm\install.log` и временный журнал Setup. Если сначала установлен OFX, а затем не установилось приложение, OFX остаётся обновлённым: общей транзакции для обоих компонентов нет.

## Подготовка машины, на которой собираем пакет

Это дополнительные шаги **только для компьютера сборки**, где уже работают MSYS2 и наша Windows-сборка.

1. Получить исходники одной строкой в обычном PowerShell:

```powershell
git -C C:\work\ALEXZ_SilentFilm_OFX pull --ff-only
```

2. Установить [Inno Setup 7 либо 6.3+ с официальной страницы](https://jrsoftware.org/inno-setup/downloads). Оставить стандартную папку установки. Компилятор ISCC.exe найдётся автоматически; нестандартный путь передаётся параметром `-InnoCompiler`.

3. Для включения Visual C++ Runtime упаковщик сначала ищет `vc_redist.x64.exe` в установленном Visual Studio Build Tools. Если файл не найден, скачать [x64-дистрибутив Microsoft](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist) в `C:\work\vc_redist.x64.exe`. Можно одной строкой:

```powershell
curl.exe --fail --location --ssl-revoke-best-effort --output C:\work\vc_redist.x64.exe https://aka.ms/vc14/vc_redist.x64.exe
```

Подпись Microsoft проверяется перед упаковкой и перед установкой. Если проверка подписи не прошла, не отключать её: прислать текст ошибки.

## Сборка и упаковка

Закрыть Cadence и Resolve. Пример для текущей **RTX2070, архитектура75, CUDA13.0**, весь вызов — одна строка:

```powershell
& 'C:\work\ALEXZ_SilentFilm_OFX\scripts\windows\Build-Windows-Ninja.cmd' -OfxIncludeDir 'C:\ProgramData\Blackmagic Design\DaVinci Resolve\Support\Developer\OpenFX\OpenFX-1.4\include' -VcpkgRoot 'C:\work\vcpkg' -Cadence -Cuda -CudaRoot 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0' -CudaArchitectures 75 -Standalone -VcRedistPath 'C:\work\vc_redist.x64.exe' -SkipInstallPrompt
```

Если redist находится автоматически, `-VcRedistPath ...` можно убрать. `-Standalone` добавляет упаковку после успешных build/CTest/stage и создаёт только Setup.exe, без промежуточного сжатия ZIP (в том числе старого большого runtime). Ошибка проверки runtime или компилятора Inno останавливает создание нового установщика. Обычная команда без -Standalone сохраняет прежнюю ZIP-упаковку; для переноса автономного комплекта нужен именно Setup.exe.

Для нескольких GPU можно передать, например, `-CudaArchitectures '75;86;89'`. Сборка будет дольше; работа других GPU не подтверждается тестами на RTX2070. Сначала уточнить модель GPU и драйвер второй машины.

Ожидаемый результат для этого примера:

```text
C:\work\ALEXZ_SilentFilm_OFX\build-windows-cadence-cuda-ninja\ALEXZSilentFilm-Setup-build-windows-cadence-cuda-ninja-sm75.exe
```

## Упаковка ранее собранного stage

Этот путь не запускает повторную компиляцию/CTest. Используйте после успешных проверок той версии исходников, которую упаковываете:

```powershell
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File 'C:\work\ALEXZ_SilentFilm_OFX\scripts\windows\Package-Standalone.ps1' -StageDirectory 'C:\work\ALEXZ_SilentFilm_OFX\build-windows-cadence-cuda-ninja\stage' -VcRedistPath 'C:\work\vc_redist.x64.exe'
```

Упаковщик обновляет scripts/launcher из текущих исходников и добавляет runtime в stage. Исходный MSYS2 не изменяет. Сохранённые ZIP и старый Setup.exe автоматически не объявляются проверенными или актуальными при ошибке нового запуска.

## Как проверяется переносимость

`bundle_runtime.py` отбирает Python stdlib без тестов/Tk/IDLE/pip и только GUI-пакеты gi/cairo. Метаданные GIRepository задают набор typelibs GTK/Pango и их зависимостей. DLL собираются по обычным и delay-load PE imports, а также по динамическим именам библиотек в typelibs. Добавляются PNG/SVG/JPEG загрузчики, темы/значки, схемы, fontconfig, русский перевод и лицензии. Компиляторы, SDK, include, статические библиотеки и посторонние site-packages не копируются. Системные Windows DLL не переносятся с компьютера сборки.

Перед копированием выводятся количество файлов, общий размер и число PE-библиотек. В `runtime/runtime-files.json` записаны файлы, размеры и причины включения; список pacman отражает исходную установку, а не содержимое всего пакета. Inno Setup показывает ход сжатия без длинного списка файлов. Исторический вариант полного prefix из9686fa0 больше не используется.

Упаковщик сначала выполняет тесты PE-отбора (обычные/отложенные импорты, циклы DLL, отсутствующие зависимости, исключение файлов разработки). После этого остаётся реальная проверка перенесённого GTK/Python. Это не доказательство абсолютно минимального размера: Python stdlib и необходимые ресурсы интерфейса всё ещё содержат много небольших файлов.

Launcher предпочитает `runtime/ucrt64`, запускает Python с `-I -X utf8`, задаёт локальные пути GTK и ограничивает GUI PATH каталогом runtime и системными каталогами Windows. Путь MSYS2 с компьютера сборки не нужен. Кэш gdk-pixbuf генерируется заново для фактического пути запуска; fontconfig использует конфигурацию из пакета и системные шрифты Windows. Дочерние native/ResolvePython-процессы не наследуют GTK/fontconfig/gdk-pixbuf overrides.

Перед компиляцией Setup.exe выполняется реальная проверка скопированного runtime: Python prefix/import paths, загрузка GTK/Pango, наличие шрифтов, PNG-декодирование и создание/удаление GTK Window без показа. Та же проверка выполняется установщиком на целевой машине до замены прежнего Cadence. Это не заменяет полноценную приёмку в Resolve.

## Если предыдущая упаковка прервана

Не удаляйте весь stage: bin/scripts/presets и OFX уже собраны. После Ctrl+C можно удалить только временные `.runtime-<GUID>` внутри stage/SilentFilmCadence, если они остались. Рабочий `runtime` новый упаковщик заменяет после проверки нового комплекта. Папки `.runtime-*`/`.previous-runtime-*` не включаются в Setup.exe: payload перечислен явно. Для повторной попытки достаточно git pull и команды Package-Standalone из раздела выше, без компиляции C++ и без повторного скачивания FFmpeg.

## Приёмка на второй машине

1. На машине без MSYS2/Python/Toolkit запустить только Setup.exe; разрешить UAC.
2. Открыть Resolve Studio21.1+, меню Cadence. Проверить шрифты/русский текст/диалоги/пресеты.
3. Короткий клип: Raw Source и с обработками, In/Out, CUDA, результат в том же таймлайне.
4. Проверить OFX на Color. Затем повторить установку и убедиться, что backup создан вне Plugins.
5. Проверить путь пользователя с пробелами/кириллицей и запуск без доступа к компьютеру сборки/интернету. При установке с отказом UAC должен быть показан отказ, а не успешное завершение.

Эти проверки ещё не выполнялись агентом. Пакет пока нельзя объявлять проверенным на чистой Windows.
