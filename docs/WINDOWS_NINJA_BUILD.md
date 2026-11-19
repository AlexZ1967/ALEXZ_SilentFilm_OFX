# Windows: сборка через Ninja при ошибке MSBuild

✅ **2026-11-19 — полный Cadence подтверждён пользователем:** CPU23/23, CUDA31/31 без Skipped, расчёт GUI работает на RTX2070/Resolve Studio21.1.2. После успешной сборки теперь предлагается [установка одной командой](WINDOWS_INSTALLER.md); сам установщик ещё ждёт проверки. Более ранние пометки ниже отражают историю этапа.

✅ **2026-11-19 — Windows CPU OFX подтверждён пользовательским логом:** MSVC19.39.33523.0/x64, configure/generate, сборка40/40, CTest12/12 Passed за1.92с и упаковка. Проверен wrapper из ae751a6 без -Cadence/-Cuda. Полный Cadence, CUDA и запуск в Resolve пока не проверены; прежние пометки «не проверено» ниже относятся к моменту подготовки.

✅ **2026-11-19 — подготовлен дополнительный способ сборки.** Windows-запуск/компиляция этой ветки ещё не проверены. Он предназначен для случая MSB4019 с отсутствующим `Microsoft.VCRedistVersion.default.props`, когда MSVC установлен и работает, а сборка Visual Studio generator не может прочитать side-by-side `.props`.

В пользовательском логе vcpkg успешно собрал FFTW/FFmpeg/OpenCV через MSVC14.39. Поэтому можно попробовать тот же компилятор с генератором Ninja, которому не нужна конфигурация проекта MSBuild. Это обход проблемы сборки, а не восстановление самого Visual Studio Installer. [Microsoft о command-line MSVC](https://learn.microsoft.com/en-us/cpp/build/building-on-the-command-line?view=msvc-170), [генератор CMake Ninja](https://cmake.org/cmake/help/latest/generator/Ninja.html).

## Действия пользователя

1. Открыть новое **обычное Windows PowerShell** через «Пуск». Для этого маршрута не использовать уже открытое окно Developer PowerShell с x86-окружением. Администратор для сборки не нужен.
2. Перейти в проект и скачать обновление:

```powershell
Set-Location C:\work\ALEXZ_SilentFilm_OFX
```

```powershell
git pull --ff-only
```

Если `git pull` завершился ошибкой, не продолжать, прислать её текст. Должен появиться файл `scripts\windows\Build-Windows-Ninja.cmd`.

3. Задать каталог имеющегося SDK (это фактический путь из пользовательской установки):

```powershell
$ofxInclude = 'C:\ProgramData\Blackmagic Design\DaVinci Resolve\Support\Developer\OpenFX\OpenFX-1.4\include'
```

4. Собрать CPU OFX:

```powershell
& .\scripts\windows\Build-Windows-Ninja.cmd -OfxIncludeDir $ofxInclude -VcpkgRoot C:\work\vcpkg
```

CMD wrapper сам вызывает штатный `vcvarsall.bat x64 -vcvars_ver=14.39`. Далее PowerShell проверяет x64/версию компилятора и запускает CMake/Ninja, тесты и упаковку. Для CMD файла отдельное разрешение ExecutionPolicy не требуется; дочерний PowerShell явно использует RemoteSigned. Если скачанные сценарии заблокированы меткой Интернета или политикой организации, это ограничение отдельно не обходится.

Ожидаемый каталог готового OFX:

```text
build-windows-cpu-ninja\stage\ALEXZSilentFilm.ofx.bundle
```

Первый вывод должен содержать инициализацию **x64**, `Ninja compiler:` с `14.39...\bin\Hostx64\x64\cl.exe`, затем определение компилятора **MSVC**. Windows-сборка пока ожидает подтверждения пользователя.

## Полное Cadence и CUDA

После успешной CPU OFX проверки, при уже установленных зависимостях и GTK:

```powershell
& .\scripts\windows\Build-Windows-Ninja.cmd -OfxIncludeDir $ofxInclude -VcpkgRoot C:\work\vcpkg -Cadence
```

Для NVIDIA/CUDA добавить `-Cuda`, каталог Toolkit и архитектуру своей карты. Пример только для RTX3090:

```powershell
& .\scripts\windows\Build-Windows-Ninja.cmd -OfxIncludeDir $ofxInclude -VcpkgRoot C:\work\vcpkg -Cadence -Cuda -CudaRoot 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0' -CudaArchitectures 86
```

Каталоги имеют суффикс `-ninja`: `build-windows-cadence-cpu-ninja` / `build-windows-cadence-cuda-ninja`. Приложение находится в `stage\SilentFilmCadence`. Запуск, установка меню и приёмка — [WINDOWS_CADENCE_APP.md](WINDOWS_CADENCE_APP.md).

Ветви VisualStudio и Ninja используют отдельные build-каталоги: старый `build-windows-cpu` с журналом ошибки сохраняется. Исходники, зависимости, SDK, установленные Visual Studio/Resolve и глобальные настройки PATH не меняются. Новая попытка может выявить следующие ошибки Windows-переноса; фактический результат нужно оценить по выводу команды.

Если Build Tools находится не в стандартном каталоге, перед запуском wrapper указать его корень, например для Community:

```powershell
$env:SILENTFILM_VS_ROOT = 'C:\Program Files\Microsoft Visual Studio\2022\Community'
```

Для текущей пользовательской установки **BuildTools** это не требуется. Если `ninja.exe` отсутствует, сценарий сообщит об этом отдельно — файл из Visual Studio проверяется первым, затем PATH. Не нужно заново собирать уже установленные vcpkg зависимости.
