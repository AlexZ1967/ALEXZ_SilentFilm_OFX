# Единый цветовой контракт SilentFilm — 0.23

✅ **2026-11-12 — подготовлено согласование заводских настроек цвета.** Новые Negative/Print используют sRGB по умолчанию, как Camera Optics, Projector и кодированные стадии Defects. Ручной Encoding перенесён в закрытую группу **Advanced — Color Encoding**. Агент сборки, тесты и Resolve не запускал; подтверждённого результата 0.23 пока нет.

## Обычная цепочка

`Входной CST → Camera Optics → Negative → Defects негатива → Print → Defects позитива → Projector → Выходной CST`

| Участок | Фактические данные |
|---|---|
| Вход Camera Optics / Negative | RGB: sRGB transfer, примарии Rec.709 |
| Выход Negative, Negative/Printing Defects, вход Print | Физическое пропускание плёнки T, без гамма-декодирования |
| Выход Print, Positive/Projector Defects, Projector | RGB: sRGB transfer, примарии Rec.709 |

Входной CST переводит **фактическое пространство перед цепочкой** в Rec.709/sRGB. Выходной CST после Projector возвращает сигнал в рабочее пространство таймлайна. Например, для Rec.709 Gamma2.4: Gamma2.4 → sRGB перед цепочкой, sRGB → Gamma2.4 после неё. Для DaVinci Wide Gamut/Intermediate нужны преобразования и примарий, и transfer, а не только замена гаммы. В управляемом проекте возврат нужен в рабочее пространство обработки, где затем действует штатный output transform Resolve.

Если реальный сигнал на границах уже sRGB/Rec.709, дополнительные CST не нужны. Пункт sRGB в OFX **интерпретирует** вход; он сам по себе не превращает DWG/Log/Gamma2.4 в sRGB. Наша текущая Comfy-модель — SDR: границу HDR/Log → SDR нужно задать осознанно.

Между Negative и Print не ставь CST для «приведения к цвету таймлайна»: на этом участке идут данные материала. В Negative/Printing Defects поле Encoding выключено и называется **Film transmission — no gamma**. Adapter явно выбирает data/linear-представление независимо от сохранённого пункта. При переключении Material на RGB-стадию ручной Encoding снова доступен в Advanced.

Для Cadence отдельно остаётся Input Transfer фактического исходника/экспортированных кадров. Нативный VideoReader уже переводит поддерживаемый Linear/Gamma2.4 в sRGB перед capture. Это настройка одного входного декодирования, её нельзя автоматически подменять названием таймлайна. Изменений Cadence в 0.23 нет.

## Уже созданные ноды

Сохранённые Encoding и ключи старых проектов не переписываются автоматически. Для Negative, Print, Projector и RGB-стадий Defects выбери:

**Reference Preset → Comfy color only (keep effect) → Apply Reference Preset**.

Профиль меняет только Encoding на sRGB. Экспозиция, grain, seed, Blend, Strength и остальные настройки остаются прежними. Это не полный workflow-пресет и не сброс эффекта. Camera Optics отдельного Encoding не имеет, поскольку её модель уже фиксирована на sRGB. В mapped-пресетах Defects сохранённый RGB override тоже sRGB; на физическом негативе он не применяется.

При самостоятельном использовании Negative/Print/Projector или при намеренно другом входе/выходе можно раскрыть Advanced и оставить ручной override. Автоматического угадывания по имени таймлайна нет. Обычная новая цепочка требует настройки цвета на её границах, а не повторного выбора гаммы в каждой ноде.

## Что с Blend

Сначала нужно исключить несогласованность sRGB/Linear на границах и в старых сохранённых настройках. Пользователь сообщил такую несогласованность; поэтому прежнюю нелинейность Negative T→Print нельзя считать единственной доказанной причиной его сильных полос.

Режимы 0.22 в этом этапе не меняются: **Physical material** — исходная модель маски/плотности, **Perceptual** — видимое смешивание с нейтральной sRGB-калибровкой. Для A/B физической математики Comfy выбирается Physical, для художественного равномерного фейда остаётся Perceptual. Окончательно оценивать нужный ход регуляторов следует на согласованной цепочке. Perceptual не исправляет неверную transfer curve исходных пикселей.

## Автоопределение цвета

OpenFX предусматривает `ColourManagementStyle`, `Colourspace`, `PreferredColourspaces` и Raw для данных материала. В установленном SDK заголовки есть, но передача таких свойств конкретным Resolve21 в runtime ещё не проверена; текущий плагин их не использует. Наличие заголовка не доказывает поддержку хостом. [Официальный ofxColour.h](https://openfx.readthedocs.io/en/latest/Reference/api/file/ofxColour_8h.html).

Будущий Auto должен читать фактический входной clip space и учитывать data-участок Negative→Print. Одного имени Timeline Color Space недостаточно: CST и режим обработки могут менять пиксели перед OFX. В 0.23 Auto не добавлен и фиктивное чтение цвета проекта не используется.

## Проверка пользователем

```bash
cd /home/alex/Projects/ALEXZ_SilentFilm_OFX
cmake --build build-cuda --parallel 2 &&
ctest --test-dir build-cuda --output-on-failure
```

`silentfilm_film_reference`/`silentfilm_film_cuda_parity` дополнены проверкой общего sRGB-контракта, data-стадий, standalone override и color-only профиля без сброса look. Нейтральная Negative/Print пара проверяется на согласованном sRGB и воспроизводит заметное отличие при намеренно неверном Linear на выходе. `silentfilm_workflow_presets` учитывает новый color-only профиль и сохранённый RGB override Defects. Агент эти проверки не запускал.

После успешных тестов закрой Resolve, обнови bundle обычной командой проекта и открой снова. Проверь defaults новых нод, закрытую Advanced, отключённый Encoding у Negative/Printing Defects, сохранность look после Color only, согласованные CST на границах. Затем сравни Scratches в Physical/Perceptual без старого render cache. Полная математическая эквивалентность Comfy Defects всё ещё не заявляется.
