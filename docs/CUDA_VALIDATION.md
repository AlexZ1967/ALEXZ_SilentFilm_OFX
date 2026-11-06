# Проверка Linux CUDA backend

Для Host Probe 0.2 пользователь подтвердил GPU-сборку на Linux (GCC 13.3, CUDA Toolkit 13.0.88, архитектура 86) и CTest 2/2 PASS, включая `silentfilm_cuda_parity`. В `ResolveDebug.txt` обнаружены строки `[ALEXZSilentFilmOFX] CUDA time=...` с temporal mix и offset 1/4: выбор CUDA в Resolve подтверждён. Для 0.3 с Negative/Print пользователь сообщил, что тесты прошли, но ручная проверка этих эффектов в Resolve ещё не подтверждена. Появление эффекта в библиотеке само по себе не доказывает использование CUDA: backend выбирает хост на каждый render.

## Сборка

Нужен установленный CUDA Toolkit с `nvcc`; Python, PyTorch и Conda не нужны. На данной машине обнаружен `/usr/local/cuda/bin/nvcc`. Для RTX 3090 используйте архитектуру 86; на другой GPU её нужно выбрать отдельно. Не устанавливайте и не обновляйте драйвер ради этой инструкции автоматически.

```bash
cd /home/alex/Projects/ALEXZ_SilentFilm_OFX
cmake -S . -B build-cuda -DCMAKE_BUILD_TYPE=Release -DSILENTFILM_ENABLE_CUDA=ON -DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc -DCMAKE_CUDA_ARCHITECTURES=86
cmake --build build-cuda --parallel 2
ctest --test-dir build-cuda --output-on-failure
```

Отдельный `build-cuda` сохраняет CPU-сборку в `build`. Если компилятор или CUDA runtime несовместимы с системным toolchain, пришлите первую ошибку конфигурации/сборки; не обходите проверку версии компилятора флагами принудительного разрешения.

В текущей версии ожидаются три теста: `silentfilm_core`, `silentfilm_sensitometry` и `silentfilm_cuda_parity`. Последний проверяет CPU/CUDA-результаты на небольших реальных device-буферах: padding, разные bounds источников, неполный render window, отрицательные strides, отрицательные/HDR RGB у Probe, SDR-модель Negative/Print, alpha, отсутствие и наличие переданного stream. Тест намеренно ждёт собственный stream для чтения результата; OFX render при host stream этого не делает.

При отсутствии CUDA GPU CTest может отметить GPU-тест как SKIP (код 77); это не PASS GPU-проверки. Ошибки драйвера и CUDA runtime возвращаются как FAIL.

## Замена bundle

После успешных проверок полностью закройте Resolve и замените собственный bundle:

```bash
sudo cp -a build-cuda/ofx/ALEXZSilentFilm.ofx.bundle /usr/OFX/Plugins/
```

ID Host Probe сохранён; в версии 0.3 bundle дополнен отдельными Negative/Print, minor version — 0.3. Повторно откройте Resolve и проверьте, что эффекты разрешены в Video Plugins. Системные файлы Resolve и SDK не меняются.

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

Плагин использует stream, переданный хостом. При его наличии не выполняются `cudaDeviceSynchronize` и ожидание завершения kernel в render. Host image handles освобождаются в пределах action согласно SDK; выполнением очереди и сроком жизни GPU-данных управляет хост. Без stream выполняется `cudaStreamSynchronize` для собственного запуска в default stream перед освобождением handles. Новая VRAM в render не выделяется.

## Возврат к CPU-прототипу

Если CUDA-проверка не прошла, закройте Resolve и верните bundle из сохранённой CPU-сборки:

```bash
sudo cp -a build/ofx/ALEXZSilentFilm.ofx.bundle /usr/OFX/Plugins/
```

Не меняйте загруженную библиотеку при работающем Resolve. Для сборки текущих исходников без CUDA можно использовать `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSILENTFILM_ENABLE_CUDA=OFF` и обычный build/CTest.
