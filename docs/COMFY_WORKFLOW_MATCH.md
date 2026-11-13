# Сверка рабочего workflow Comfy — 0.19

✅ **2026-11-14 — подготовлена проявка Defects0.36:** mottling/streaks/mixed, fog и bicubic/temporal CPU/CUDA. Пресеты n15/n18 теперь выбирают нужные Development Pattern, Physical и Fog0.15. [Сравнение с реальным development_field и ограничения RNG](DEFECTS_DEVELOPMENT_0.36.md). Пользователь подтвердил тесты и ручную работу0.36 2026-11-14, без полного лога/численных ошибок; агент проверки не запускал. Прежние строки об отсутствии development относятся к старым срезам.

✅ **2026-11-13 — подготовлен первый блок Defects0.30:** [модели материала, формы и сравнение](DEFECTS_COMFY_0.30.md). Damage Model/Irregularity добавлены в presets; surface/emulsion_loss/dirty и формы отдельных dust/stains/scratches перенесены в общий CPU/CUDA-путь. Старые утверждения ниже об отсутствии damage_type относятся к срезу0.19. Проверки0.30 пока не запускались. Development, mixed/hair и softness окна ещё не перенесены; RNG и ограничение Edge Bias боковыми краями остаются отличиями. Полного совпадения workflow не заявлять.

✅ **2026-11-12 — пользователь подтвердил: «тесты прошли».** Полный лог и число тестов не предоставлены. Независимые `compare_comfy_film.py`/`compare_comfy_optics.py` и визуальная проверка в Resolve пока отдельно не подтверждены; ниже сохранён статус на момент подготовки изменений.

✅ **2026-11-12 — прочитан пользовательский `Видео-в-Хронику-по-этапам_with_defects.json`; подготовлены пресеты и доработка зерна CPU/CUDA.** Референс только читался. Сборки, тесты, сравнения и Resolve агент не запускал. Нового PASS и измеренного ускорения нет.

Источник: `/mnt/comfy/ComfyUI/custom_nodes/ComfyUI_ALEXZ_SilentFilm/workflows/Видео-в-Хронику-по-этапам_with_defects.json`. Снимок параметров и SHA-256 — [comfy_reference_with_defects.json](../presets/comfy_reference_with_defects.json). Путь исходного видео не копируется в снимок; runtime OFX не зависит от Comfy/Python/Torch.

## Настройки для сравнения

Внизу контролов каждого OFX выбери **Reference Preset → Comfy workflow… → Apply Reference Preset**. Выбор списка сам по себе не меняет обработку. Кнопка меняет параметры выбранной ноды одной группой Undo. Существующие значения/анимация не сбрасываются при загрузке проекта; для чистого A/B используй свежие ноды без ключей.

| Этап | Рабочие значения |
|---|---|
| Cadence / Camera, нода 2 | source_fps **24**, capture **20–24**, variation **1**, irregularity **0.5**, shutter **0.5**, motion_blur **0.5**, flow_integrated, samples **9**, shake **0.5**, exposure link **1**, seed **1925** |
| Camera Optics, оптическая часть ноды 2 | vignette **0.6**, edge softness **0.05**, edge start/end **0.6/1.3**, softness **0.15**, focus drift **0.04**, seed **1925** |
| Negative, нода 3 | input **sRGB**, EV **0**, contrast **1**, shadow/highlight/fog **0**, halation **0**, size **0.15**, **grain 0.01 / size 2**, seed **1925** |
| Print, нода 4 | output **sRGB** (`from_negative` наследует sRGB), EV **0**, contrast **1**, shadow/highlight/fog **0**, halation **0**, size **0.15**, **grain 0 / size 2**, neutral_bw, print softness **0**, seed **1925** |
| Projector, нода 5 | **gate weave 2 px**, **flicker 0.035**, **breathing 0.015**, glow **0**, glow size **1**, transfer **sRGB**, seed **1925**, показ **25 fps** |

Cadence остаётся отдельным приложением: для этого A/B выбери source_fps_mode=custom, source_fps=24 и указанные capture-параметры. На таймлайне сравнения должен быть тот же набор кадров при 25 fps. OFX Projector сам не меняет FPS/длительность. Старый объединённый Camera получил пресет capture-контролов, но для нового workflow используй Cadence → Camera Optics.

В этом workflow оптика проектора выключена (softness/vignette/edge_softness=0). Print tone нейтральный и print_softness=0. Эти нулевые параметры соответствуют текущему native-пути; дополнительные ненулевые оптические параметры проектора/тонировка/мягкость печати ещё не перенесены в эти OFX. `development_variation` Negative сохранён как 0, но вход подключён к пустому Reroute n45: из JSON нельзя подтвердить отличающееся runtime-значение. Пресет соответствует сохранённому нулю; отдельного такого контрола в OFX пока нет.

## Зерно: исправленная математика

Прежний native emulsion grain был плавно интерполируемым hash-шумом без нормализации и с шагом ниже одного пикселя. Это отличалось от `_density_grain` и `_build_emulsion_grain_field` рабочего Comfy.

Новый рабочий CPU/CUDA-путь Negative/Print повторяет структуру reference:

1. `effective_size = grain_size × width / 1920`; амплитуда плотности = `grain_strength × min(1, effective_size)`.
2. Размер структуры `max(1, effective_size)`. Четыре независимых Gaussian-решётки с масштабами **1, 2, 4, 6**. Размеры решёток округляются nearest-even; увеличение — bilinear, `align_corners=False`, с продлением границы.
3. Каждое увеличенное поле центрируется и нормализуется на population std с нижней границей **1e-6**.
4. `clump = 0.78 + 0.48 × sigmoid(envelope × 1.3)`; смесь `fine × clump + mid × 0.42 + coarse × 0.24` снова нормализуется; выход `tanh(field × 1.25)`.
5. Зерно добавляется **в оптическую плотность** после проявки, до пропускания/показа. Negative и Print имеют независимые потоки. Номер кадра — целый индекс относительно Source range, а не float-биты абсолютного OFX time.

Повышение плотности негатива уменьшает его пропускание, поэтому через печать проявляется иначе, чем повышение плотности позитива. Это не одинаковое RGB-наложение шума поверх двух этапов. При нулевом зерне Print зерно негатива сохраняется в отпечатке.

При ширине 640 и размере 2 амплитуда составляет примерно 0.667 от указанной силы — именно так в Comfy. Зерно такого размера не следует искусственно усиливать на маленьком разрешении для «видимости».

На CPU Gaussian-решётки генерируются один раз, затем используются плоские буферы и проходы нормализации. На CUDA каждая точка решётки тоже генерируется один раз, увеличение/нормализация/смешивание и плотность рассчитываются в VRAM. Редукции фиксированы по блокам с double-накоплением; нет копирования кадров/статистик через CPU и нет продвижения общего RNG по порядку render. Нативный Box–Muller/Gaussian RNG отличается от Torch, а stage/frame seed mixing — от Comfy BLAKE2b/Torch. **Одинаковый seed не обещает одинаковую текстуру**, сравниваются структура/амплитуда и статистика.

Negative/Print теперь объявляют CUDA в OFX: полный spatial-путь включает halation через существующий кэш cuFFT без ограничения промежуточного линейного света перед проявкой. При выключенных зерне/halation это один GPU-проход без кадрового scratch. С зерном нужны четыре поля и одна повторно используемая решётка (около 20 байт/пиксель плюс небольшие редукции); halation дополнительно использует кадровые/FFT-буферы. Host stream соблюдается, host image pointers не кэшируются между action. Автоматическое снижение качества или CPU-fallback для GPU-указателей не добавлены. Старый пиксельный `emulsionPixel`/диагностический CUDA-тест сохраняют прежний упрощённый шум; реальный OFX использует новый spatial-путь и отдельный parity-тест.

## Проектор

Транспорт сдвигает позитив **вместе с зерном/повреждениями и alpha**, bilinear с border sampling. Направление совпадает с Comfy `affine_grid/grid_sample`: положительная shift выбирает более правую/верхнюю координату источника. Оси x/y независимы, амплитуда y = **0.8 × gate_weave_px**.

Четыре AR(1)-сигнала нормализуются по всему клипу: smoothing **0.55 / 0.60 / 0.18 / 0.92** для x/y/flicker/breathing. Лампа = `max(0, 1 + flicker + breathing)`. Сначала движение, затем decode sRGB → умножение на лампу → спектральный glow → SDR clamp → encode. Лампа одинаково освещает резкую и рассеянную части. На одном кадре центрирование выключает эти колебания, как reference. Семейство формул уже было перенесено; здесь добавлены точный workflow-пресет, проверки амплитуд и независимая сверка транспорта/света/glow.

## Дефекты и границы совпадения

Сохранённый workflow содержит **13 отдельных defect-нод**, включая несколько последовательных NegativeDamage и PositiveDamage с разными подключёнными PrimitiveInt seed. Многим нодам записан seed=1925 в widgets, но активные связи задают другое число: n15=1924, n16=1923, n32=1922, n33=1921, n19=1930, n28=2022, n39=1919, n40=1918, n41=1917. В пресетах учтены подключённые значения.

OFX Defects получил отдельные пресеты **mapped** для этих нод: material/pattern/strength/size/amount/persistence/seed/encoding в пределах существующей модели. У development нет amount в Comfy, native amount не используется этой веткой. **Это не полный перенос Defects:** отсутствуют точные damage_type surface/emulsion_loss/dirty, Comfy-формы/когерентность/edge bias/fog поля и projector dirt softness. Их нельзя считать совпадающими по математике. Полная цепочка с повреждениями пока не эквивалентна пользовательскому workflow, даже если численные параметры выставлены.

Для проверки именно зерна/лампы сначала bypass Defects в обеих сравниваемых цепочках. Затем дефекты нужно переносить и проверять отдельно по типам и физическим стадиям. Зерно и пятна проявки не следует сводить к одному виду финального шума.

## Проверки выполняет пользователь

```bash
cd /home/alex/Projects/ALEXZ_SilentFilm_OFX
cmake --build build-cuda --parallel 2 &&
ctest --test-dir build-cuda --output-on-failure
```

Новые CTest: `silentfilm_film_reference` (структура/плотность/наследование зерна), `silentfilm_film_cuda_parity` (рабочие CPU/CUDA с зерном, halation и alpha), `silentfilm_workflow_presets` (реальные значения пресетов против сохранённого JSON-снимка). Projector-тест дополнен проверкой нормализации четырёх сигналов. Предыдущие Camera Optics тесты 0.18 тоже остаются непроверенными агентом.

Независимая сверка из существующего Python Comfy с Torch/NumPy; читать reference можно, запускать Comfy не требуется:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 tests/compare_comfy_film.py --backend cpu
PYTHONDONTWRITEBYTECODE=1 python3 tests/compare_comfy_film.py --backend cuda
```

Скрипт сверяет пресеты с исходным workflow (включая подключённые seed). Четыре grain-fixture используют одинаковые Gaussian-решётки настоящего Comfy для CPU production-builder; шесть projector-fixture используют одинаковые реальные значения его weave/lamp, с glow 0/0.3, для выбранного native CPU/CUDA. Таким образом RNG намеренно исключён из численного сравнения; GPU-структура зерна проверяется дополнительно CPU/CUDA production parity. Это не end-to-end проверка всего workflow. Допуски grain max1e-4/mean1e-5, projector max2e-4/mean2e-5. Пока вывода PASS нет.

После успешных проверок закрой Resolve, обнови bundle и открой проект:

```bash
sudo cp -a build-cuda/ofx/ALEXZSilentFilm.ofx.bundle /usr/OFX/Plugins/
```

Для визуальной проверки: Negative workflow + Print workflow + Projector neutral показывает отпечатанное зерно негатива; Negative neutral + Print Grain only показывает собственное зерно позитива. Затем оба зерна выключить и включить Projector workflow — проверяется только лампа/дрожание. Сравнивай один файл, один диапазон, одинаковое число кадров/разрешение/показ, одну transfer curve, одинаковые временные настройки и отсутствие старого render cache. Нельзя ставить CST/LUT между физическим выходом Negative и входом Print.
