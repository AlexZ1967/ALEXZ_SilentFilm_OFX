# Проверка Linux CUDA backend

Для Host Probe 0.2 пользователь подтвердил GPU-сборку на Linux (GCC 13.3, CUDA Toolkit 13.0.88, архитектура 86) и CTest 2/2 PASS, включая `silentfilm_cuda_parity`. В `ResolveDebug.txt` обнаружены строки `[ALEXZSilentFilmOFX] CUDA time=...` с temporal mix и offset 1/4: выбор CUDA в Resolve подтверждён. Для версии 0.3 пользователь подтвердил прохождение тестов и правильную работу Negative → Print; затем подтвердил тесты накопленного набора 0.4. Пользователь затем подтвердил тесты перед оптимизацией Camera и сообщил о низкой скорости её CPU-render. В версии 0.7 CUDA descriptor объявлен для Host Probe и Camera; полный Camera render перенесён на GPU. Версия 0.8 дополнительно вводит Playback Mode с проекцией кадров для ускорений/замедлений действия. Новые изменения не собирались и не проверялись агентом. В 0.9 Projector также получил полный CUDA-путь (gate weave, лампа, cuFFT glow); Negative/Print halation и Defects пока выполняются CPU.

## Сборка

Нужен установленный CUDA Toolkit 11.2+ с `nvcc` и поддержкой stream-ordered memory pools на GPU/драйвере; Python, PyTorch и Conda не нужны. На данной машине обнаружен `/usr/local/cuda/bin/nvcc`. Для RTX 3090 используйте архитектуру 86; на другой GPU её нужно выбрать отдельно. Не устанавливайте и не обновляйте драйвер ради этой инструкции автоматически.

```bash
cd /home/alex/Projects/ALEXZ_SilentFilm_OFX
cmake -S . -B build-cuda -DCMAKE_BUILD_TYPE=Release -DSILENTFILM_ENABLE_CUDA=ON -DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc -DCMAKE_CUDA_ARCHITECTURES=86
cmake --build build-cuda --parallel 2
ctest --test-dir build-cuda --output-on-failure
```

Отдельный `build-cuda` сохраняет CPU-сборку в `build`. Если компилятор или CUDA runtime несовместимы с системным toolchain, пришлите первую ошибку конфигурации/сборки; не обходите проверку версии компилятора флагами принудительного разрешения.

В CUDA-сборке 0.9 ожидаются семь тестов; пять прежних: `silentfilm_core`, `silentfilm_sensitometry`, новый `silentfilm_camera_timing`, `silentfilm_cuda_parity` и `silentfilm_camera_cuda_parity`. Timing-тест проверяет реальную переменную скорость в режиме проекции 10–30 FPS, нормировку длительности, повторы при постоянном FPS, границы и независимость от порядка запросов/длины уже построенного расписания. Последний сравнивает Camera с CPU-эталоном: один/девять отсчётов, временную интерполяцию и веса, shake, edge blur с уменьшением разрешения, vignette/global softness/focus, premultiplied alpha, разные bounds, padding/отрицательный stride, render window, изображения 1×1 и узкие кадры, с host stream и без него. Пользователь выполнил Camera CUDA parity успешно (0,41 с), вместе с core/sensitometry и прежним CUDA parity. Весь CTest дал 4/5: timing упал на постоянной частоте. После коррекции границ floor и расширения timing-теста нужен повторный пользовательский прогон; итоговый 5/5 пока не получен. Прежний parity не покрывает ненулевое зерно, расширенный диапазон Shadow Compression, halation, Projector или Defects. Выполняйте CTest только после успешного build, чтобы не получить PASS старых бинарников.

В 0.9 дополнительно подготовлены `silentfilm_projector` и `silentfilm_projector_cuda_parity`; они пока не запускались. Описание сценариев — в `DEFECTS_PROJECTOR.md`.

При отсутствии CUDA GPU CTest может отметить GPU-тест как SKIP (код 77); это не PASS GPU-проверки. Ошибки драйвера и CUDA runtime возвращаются как FAIL.

## Замена bundle

После успешных проверок полностью закройте Resolve и замените собственный bundle:

```bash
sudo cp -a build-cuda/ofx/ALEXZSilentFilm.ofx.bundle /usr/OFX/Plugins/
```

Существующие IDs сохранены; версия 0.5 добавляет `SilentFilm Projector` и `SilentFilm Defects`. Camera и Projector 0.9 имеют CUDA-путь; пространственные эффекты Negative/Print и Defects пока CPU-only. После пользовательской сборки повторно откройте Resolve и проверьте, что все эффекты разрешены в Video Plugins. Системные файлы Resolve и SDK не меняются.

## Подтверждение CUDA в Resolve

Запустите Resolve обычным для вашей установки способом, добавив переменную трассировки. Для прямого запуска:

```bash
ALEXZ_SILENTFILM_OFX_TRACE=1 /opt/resolve/bin/resolve
```

Если используете специальный launcher, добавьте переменную к его запуску. Для CUDA render Resolve должен работать с поддерживаемой NVIDIA GPU в режиме CUDA. Не меняйте настройки GPU в работающем рендере.

Resolve может перенаправлять stderr в свой лог. На текущей установке трассировка оказалась именно в нём, а не в стартовом терминале. Просмотр уже записанных строк:

```bash
rg 'ALEXZSilentFilmOFX' ~/.local/share/DaVinciResolve/logs/ResolveDebug.txt | tail -20
```

1. На коротком клипе включите `Diagnostic Gain=1` — нейтральные параметры могут включить identity без render. Найдите в stderr/логе строку `[ALEXZSilentFilmOFX] CUDA time=...`. Строка `CPU` означает, что хост предоставил CPU-буферы: это не подтверждение GPU-пути.
2. Верните gain в 0, включите `Temporal Mix=1`, сначала offset=1, затем offset=4. Сверьте с уже проверенным CPU-поведением, включая края клипа и перемотку.
3. Проверьте короткую последовательность в Color, затем отдельно в Fusion. Фиксируйте backend для каждого контекста: выбор хоста может различаться.
4. Сохраните 8–16 кадров средствами Resolve и сравните просмотр/экспорт. Проверьте отмену и повторный запуск.
5. Отключите трассировку для обычной работы. Скорость на этом лёгком диагностическом kernel не характеризует будущие blur/flow-эффекты.

Плагин использует stream, переданный хостом. При его наличии не выполняются `cudaDeviceSynchronize` и ожидание завершения kernel в render. Host image handles освобождаются в пределах action согласно SDK; выполнением очереди и сроком жизни GPU-данных управляет хост. Без stream выполняется `cudaStreamSynchronize` для собственного запуска в default stream перед освобождением handles. Probe не выделяет scratch VRAM. Camera при активном пространственном blur использует один stream-ordered блок для 5–6 RGBA float-плоскостей и маски (около 84–100 байт на пиксель, без исходных изображений хоста); буферы переиспользуются между уровнями и освобождаются через `cudaFreeAsync` в том же stream. Без spatial blur используется один kernel без scratch allocation. Пиксельных readback/upload в production Camera нет; CPU вычисляет только расписание, веса и аффинные матрицы. `cudaMallocAsync` может потребовать первоначального расширения пула; первый кадр не подходит для замера устойчивой скорости.

## Camera 0.8: backend, каденс и скорость

1. На одном и том же коротком клипе сравните Camera отдельно и всю цепочку, сохранив разрешение, параметры, кодек экспорта и настройки качества. Исключите ранее рассчитанный Render Cache; для замера выключите трассировку. Сравнивайте время готового экспорта, а не время отправки kernel в очередь. Запишите разрешение, длительность, Exposure Samples и время.
2. Для подтверждения backend включите трассировку и найдите `CUDA camera` в `ResolveDebug.txt`. Поля `samples` и `images` показывают число временных отсчётов и уникальных Source images. Несколько отсчётов могут использовать одну пару кадров. `CPU camera` не подтверждает GPU-путь.
3. Проверьте `Exposure Samples=1/5/9`, shake 0/0,25, выключенный blur и затем всю оптику. Сравните экспорт с CPU-сборкой на одинаковых настройках; отдельный Camera parity проверяет ядро, но не temporal access самого Resolve.
4. После исправления перевода кадров OFX в секунды ручки обязательно повторите визуальную проверку cadence, связи экспозиции/толчков/фокуса, границ клипа, обратной перемотки и отмены. При сохранении прежнего seed результат старого прототипа может измениться из-за исправленной временной шкалы и перехода к sampler Comfy.
5. Отдельно проверьте Color и Fusion. Выбор GPU делает хост. Оставшиеся CPU-эффекты Negative/Print/Defects и декодирование могут ограничивать скорость полной цепочки, даже если Camera уже работает на GPU.

Фактический прирост скорости ещё не измерен. CUDA ошибки возвращаются хосту с диагностикой; попытки обработать GPU pointers через CPU fallback нет.

## Projector 0.9

После пользовательской сборки создайте новую Projector после Print. Сначала отключите glow, проверьте weave и лампу отдельно, затем включите glow=0,1. В трассе ожидается `CUDA projector` с shift/lamp/glow. Без glow используется один kernel без scratch; с glow — batched cuFFT и кэш планов на context/stream/размер. В production нет CPU readback. Не включайте первый кадр с инициализацией/JIT планов в оценку устойчивой скорости. Проверьте Color/Fusion, proxy/full, alpha, перемотку, экспорт, отмену и удаление ноды. CPU и CUDA Projector должны сохранять одинаковую модель при одинаковых seed/диапазоне Source/параметрах. Смена backend или CPU parity не подтверждает качество в Resolve автоматически.

Системные CMake/CTest и команды сборки остаются прежними. Новый CUDA Projector линкуется с cuFFT static из Toolkit, Threads и CUDA driver; SDK/Resolve не изменяются. Скорость 0.9 пока не измерена. Подробная ручная проверка и отличие RNG от Comfy — `DEFECTS_PROJECTOR.md`.

## Возврат к CPU-прототипу

Если CUDA-проверка не прошла, закройте Resolve и верните bundle из сохранённой CPU-сборки:

```bash
sudo cp -a build/ofx/ALEXZSilentFilm.ofx.bundle /usr/OFX/Plugins/
```

Не меняйте загруженную библиотеку при работающем Resolve. Для сборки текущих исходников без CUDA можно использовать `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSILENTFILM_ENABLE_CUDA=OFF` и обычный build/CTest.
