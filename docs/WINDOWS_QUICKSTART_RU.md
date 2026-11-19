# Windows: от компиляции до установки SilentFilm

**2026-11-20.** Обычный Windows-комплект проверен пользователем на RTX2070/Resolve Studio21.1.2. Автономный runtime и создание Setup.exe + files проверены; **повторная установка после последнего исправления UAC ещё ожидает подтверждения**. Ниже приведён текущий порядок работы, а не объявление всех Windows-конфигураций проверенными.

## 1. Подготовить компьютер сборки

Если собираете впервые, выполните [подробный гайд подготовки Windows](WINDOWS_USER_GUIDE_RU.md). Нужны Git, CMake, Visual Studio Build Tools2022/MSVC14.39 x64, OpenFX SDK, vcpkg-зависимости FFTW/FFmpeg/OpenCV/Zstd, MSYS2 UCRT64 с Python/GTK/Pango. Для CUDA установите Toolkit; для Setup — Inno Setup6.3+/7 и подписанный vc_redist.x64.exe. Подготовка этих компонентов нужна только на компьютере сборки.

Текущая проверенная база компиляции — MSVC14.39 и CUDA13.0 через Ninja. Этот маршрут обходит обнаруженную проблему MSBuild. [Подробнее о Ninja](WINDOWS_NINJA_BUILD.md). Заголовки SDK и установленные Resolve/ComfyUI не изменяются.

## 2. Обновить исходники

Все команды — **по одной строке в обычном PowerShell**. Администратор для сборки не нужен.

```powershell
git -C C:\work\ALEXZ_SilentFilm_OFX pull --ff-only
```

Если pull завершился ошибкой, сначала устранить её; не продолжать со старым установщиком.

## 3. Скомпилировать, проверить и создать Setup

Закройте Resolve и Cadence. Команда для текущих двух машин: RTX2070 (`75`) и RTX4070Ti (`89`), CUDA Toolkit13.0, OpenFX SDK по проверенному пользователем пути:

```powershell
& 'C:\work\ALEXZ_SilentFilm_OFX\scripts\windows\Build-Windows-Ninja.cmd' -OfxIncludeDir 'C:\ProgramData\Blackmagic Design\DaVinci Resolve\Support\Developer\OpenFX\OpenFX-1.4\include' -VcpkgRoot 'C:\work\vcpkg' -Cadence -Cuda -CudaRoot 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0' -CudaArchitectures '75;89' -Standalone -VcRedistPath 'C:\work\vc_redist.x64.exe' -SkipInstallPrompt
```

Скрипт последовательно выполняет configure/build/CTest, подготавливает stage, проверяет переносимый GTK/Python и создаёт комплект **без сжатия payload**. При ошибке тестов к подготовке установщика не переходит. CUDA-тесты должны иметь Passed, а не Skipped. Запуск на RTX2070 не подтверждает RTX4070Ti: её проверяют отдельно.

Результат этой команды:

```text
C:\work\ALEXZ_SilentFilm_OFX\build-windows-cadence-cuda-ninja\
  ALEXZSilentFilm-Setup-build-windows-cadence-cuda-ninja-sm75-89-folder\
    Setup.exe
    files\
      ALEXZSilentFilm.ofx.bundle\
      SilentFilmCadence\
      prerequisites\
      Install-Windows.ps1
```

**Setup.exe и files должны оставаться рядом.** Весь stage удалять не нужно. Подробности состава runtime, кэширования и необязательного режима одного сжатого EXE — в [инструкции по автономному комплекту](WINDOWS_STANDALONE.md).

## 4. Обновить только скрипты или установщик без C++-сборки

Если уже собраны нужные архитектуры и менялись только Python/PowerShell/Setup-файлы, после git pull достаточно:

```powershell
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File 'C:\work\ALEXZ_SilentFilm_OFX\scripts\windows\Package-Standalone.ps1' -StageDirectory 'C:\work\ALEXZ_SilentFilm_OFX\build-windows-cadence-cuda-ninja\stage' -VcRedistPath 'C:\work\vc_redist.x64.exe'
```

Команда не меняет CUDA-архитектуры скомпилированного stage. Чтобы перейти с75 на75;89 или изменить C++/CUDA-код, нужна команда из шага3. Неизменные runtime/Setup переиспользуются, files обновляются выборочно; повторной компрессии большого архива нет.

## 5. Установить на другой Windows-компьютер

1. Убедитесь, что там Windows x64, подходящий драйвер NVIDIA и Resolve Studio21.1+ для интеграции Cadence. CUDA Toolkit, MSYS2, отдельные Python/GTK, CMake, Visual Studio и vcpkg там не нужны.
2. Скопируйте **всю папку …-sm75-89-folder** на локальный диск второй машины.
3. Закройте Resolve/Cadence. Запустите Setup.exe обычным двойным щелчком от своей учётной записи и подтвердите UAC. Только системные компоненты требуют повышения; приложение и меню относятся к исходному пользователю.
4. Дождитесь успешного завершения. При ошибке сохраните `%LOCALAPPDATA%\ALEXZSilentFilm\install.log`; журнал повышенной части находится в `%PROGRAMDATA%\ALEXZSilentFilm\install-logs`. Не считать ошибочный запуск успешной установкой.
5. Откройте Resolve вручную. Для Cadence включите Preferences → System → General → External scripting → Local. Найдите приложение в Workspace → Scripts → Utility → SilentFilm Cadence, OFX — на Color → Effects по слову SilentFilm.
6. Проверьте короткий подрезанный клип с Raw Source и с обработками, импорт в текущий таймлайн и работу OFX. CUDA выбрана по умолчанию у CUDA-пакета.

Предыдущие версии сохраняются вне OFX Plugins. Папки установки, резервные копии и ручное восстановление описаны в [инструкции установщика](WINDOWS_INSTALLER.md). Задания, пользовательские пресеты и кэш установщик не очищает.

Проверки, компиляцию и установку выполняет пользователь. Для последнего исправления UAC повторный Windows-прогон пока не подтверждён.
