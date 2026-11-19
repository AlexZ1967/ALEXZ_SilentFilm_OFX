# Первый полный Windows Cadence build: исправления 2026-11-19

✅ **2026-11-19 — пользователь подтвердил компиляцию CPU Cadence через Ninja:** MSVC19.39/x64, сборка50/50. Из23проверок прошли17, упали6; упаковка остановилась, запуск приложения и Resolve ещё не проверены.

Причины падений:

- Color preset, Exposure binding и Bridge: временная папка Windows задана коротким путём `DDB98~1.ANT`, а приложение правильно разворачивает его в `d.antropov`. Исправлены корни временных папок тестов через `Path.resolve()`. Проверки точных путей, Unicode и привязок сохранены.
- Frame provider, TCP provider и Cadence export: `Unknown input format: lavfi` при создании тестовых кадров. К зависимости FFmpeg добавлен `avdevice`; ранняя проверка возможностей FFmpeg выполняется при Windows-конфигурации с тестами.

`lavfi` относится к [libavdevice](https://ffmpeg.org/ffmpeg-devices.html#lavfi). Для изменения features уже установленного пакета требуется его пересборка; скрипт передаёт [vcpkg install --recurse](https://learn.microsoft.com/en-us/vcpkg/commands/install#--recurse). Это может занять время для FFmpeg; удалять downloads, binary cache и каталог сборки проекта не нужно.

## Повторный запуск

В обычном PowerShell выполняйте по одной строке. Если вы уже в нужной папке и политика текущего окна разрешает скрипты, соответствующие строки можно пропустить.

```powershell
Set-Location C:\work\ALEXZ_SilentFilm_OFX
git pull --ff-only
Set-ExecutionPolicy -Scope Process RemoteSigned
$env:VCPKG_ROOT = 'C:\work\vcpkg'
$env:VCPKG_SSL_REVOKE_BEST_EFFORT = '1'
& .\scripts\windows\Prepare-Cadence-Dependencies.ps1 -VcpkgRoot C:\work\vcpkg
```

После сообщения `Cadence dependencies prepared`:

```powershell
$ofxInclude = 'C:\ProgramData\Blackmagic Design\DaVinci Resolve\Support\Developer\OpenFX\OpenFX-1.4\include'
& .\scripts\windows\Build-Windows-Ninja.cmd -OfxIncludeDir $ofxInclude -VcpkgRoot C:\work\vcpkg -Cadence
```

Ожидается23/23 без ошибок, затем stage/ZIP. **Это ожидаемый результат, повторный прогон после исправлений пока не выполнен.** Агент сборки и тесты не запускал. Следующие этапы — запуск GTK-приложения, Windows CUDA и ручная приёмка в Resolve.
