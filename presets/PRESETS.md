# Пресеты `tRNS` (tES-DSL v1)

Эта папка — рабочая "библиотека" пресетов для устройства.  

Этим файликом лучше пользоваться как справочником, можно интуитивно на готовых примерах создавать свои пресеты, а если что-то пошло не так, то в файле `errors.log` будет написано, в чем ошибка.

**Практический цикл работы**

1. Редактируем YAML (и создаем луп *.wav если нужен тип WAV).
2. Копируем YAML или YAML+*.wav на флешку.
3. Перезапускаем устройство.
4. Проверяем список пресетов и `errors.log`.
5. Правим YAML и повторяем.

## 1) Какие типы пресетов есть

В `tES-DSL v1` поддерживаются только три типа:

- `CONST` — постоянный ток (`tDCS`- одноканальный, `tDCS-2a`- два независимых анода).
- `SIN` — параметрический синус (`tACS`).
- `WAV` — сигнал из WAV-файла (`hf-tRNS`- tRNS 100-640Гц нормальное распределение, `lf-nGVS` тоже гауссовский, квазибелый в диапазоне 1-100Гц).

## 2) Файлы в этой папке

- `*.yaml` — описание пресета (DSL).
- `*.wav` — данные сигнала для пресетов типа `WAV`.

## 3) Правила имен файлов

- Имя YAML-файла: только ASCII и шаблон `[a-z0-9_-]+`.
- Длина имени (без пути): не более `64` символов.
- Нарушение правила => пресет отбраковывается (см. `errors.log`).

## 4) Общие обязательные поля (для любого типа)

Каждый пресет обязан содержать:

- `dsl_version` (для этой прошивки только `1`)
- `name`
- `type` (`CONST` | `SIN` | `WAV`)
- `channels.mode` (`left` | `both`)
- `feedback.amp_estimation_base` (`MEAN` | `RMS` | `AUTO_RMS`)
- `feedback.amp_estimation_coeff` (число `> 0`) — **обязателен** для `MEAN`/`RMS`, **запрещён** для `AUTO_RMS`
- `params`

Если обязательного поля нет, пресет невалиден (см. `errors.log`).

## 5) Что такое `params`

`params` — это набор параметров, которые показываются/редактируются в UI перед запуском сеанса.

Формат параметра обычно такой:

```yaml
some_param:
  min: ...
  max: ...
  step: ...
  default: ...
```

Состав `params` зависит от `type`:

- `CONST`: `amplitude_mA`, `fade_in_sec`, `fade_out_sec`, `duration_min`.
- `SIN`: `amplitude_mA`, `frequency_hz`, `fade_in_sec`, `fade_out_sec`, `duration_min`.
- `WAV`: `amplitude_mA`, `fade_in_sec`, `fade_out_sec`, `duration_min`.

Единое правило для `duration_min` (во всех пресетах):

- `min: 2`
- `max: 60`
- `step: 1`
- `default: 20`

## 6) Как считается амплитуда в `feedback`

### Три режима `amp_estimation_base`

| Режим | Когда | `base_value` | `amp_estimation_coeff` |
|-------|--------|--------------|-------------------------|
| **MEAN** | `CONST` (постоянный ток) | `\|mean(Vadc)−Voffset\|` | задаётся в YAML (`1.0`) |
| **RMS** | AC, coeff известен вручную | `RMS(Vbip)` | задаётся в YAML |
| **AUTO_RMS** | AC, форма известна из генератора | `RMS(Vbip)` | **crest factor** формы, без YAML |

**Crest factor** (коэффициент пика к RMS) формы сигнала на DAC:

\[
K = \frac{\max|s(t)|}{\mathrm{RMS}(s(t))}
\]

После peak-нормализации WAV/SIN на DAC: `I_peak ≈ I_rms · K`.  
`AUTO_RMS` подставляет `K` автоматически — оценка согласована с тем, что реально воспроизводится.

Разрешение `AUTO_RMS` при загрузке пресетов (`scanAll`):

- **`CONST`** → `MEAN`, `coeff = 1.0` (синоним `MEAN × 1.0`)
- **`SIN`** → `RMS`, `coeff = 1.414` (`√2`)
- **`WAV`** → `RMS`, `coeff = K` по **левому** каналу лупа (один проход: peak + sum of squares)

### DSL и формула на экране

```yaml
# tDCS — постоянный ток
feedback:
  amp_estimation_base: "MEAN"
  amp_estimation_coeff: 1.0

# tACS / tRNS / nGVS — авто crest factor
feedback:
  amp_estimation_base: "AUTO_RMS"
```

- `base_value` — `RMS(Vbip)` в вольтах или `|mean(Vadc)−Voffset|` для MEAN (окно ~1 с).
- `estimated_mA = base_value · V_TO_MA · amp_estimation_coeff`

Strict-правила:

- `AUTO_RMS` + `amp_estimation_coeff` в YAML → **fatal**
- `MEAN`/`RMS` без `amp_estimation_coeff` → **fatal**
- `type: WAV`: левый канал лупа с `peak = 0` или `RMS ≈ 0` → **fatal** (нули только через `CONST`, напр. `tDCS`)

## 7) Поля по типам пресета

### 7.1 `CONST`

Обязательные блоки/поля:

- общий набор (см. выше);
- `sample_rate_hz.value` (фиксированная частота дискретизации параметрического генератора).
- в `params` обязательно `duration_min`.
- для `feedback` рекомендуется:
  - `amp_estimation_base: MEAN` (или `AUTO_RMS` — то же самое)
  - `amp_estimation_coeff: 1.0` (только для `MEAN`, не для `AUTO_RMS`)

Не используется:

- `scope.sync_mode` (для `CONST` это запрещено).
- `wave_file`.

### 7.2 `SIN`

Обязательные блоки/поля:

- общий набор;
- `sample_rate_hz.value`;
- в `params` обязательно `frequency_hz` и `duration_min`.
- для `feedback` рекомендуется:
  - `amp_estimation_base: AUTO_RMS` (или `RMS` + `amp_estimation_coeff: 1.414`)

Рекомендуется:

- `scope.sync_mode: two_periods`.

Не используется:

- `wave_file`.

### 7.3 `WAV`

Обязательные блоки/поля:

- общий набор;
- `wave_file` (имя WAV-файла на флешке).
- в `params` обязательно `duration_min`.
- для `feedback` рекомендуется:
  - `amp_estimation_base: AUTO_RMS` (crest factor из `wave_file`, левый канал)

Рекомендуется:

- `scope.sync_mode: one_period` или `no_sync`.

Не используется:

- `sample_rate_hz` (частота берется из WAV/пайплайна воспроизведения).

Канальный маппинг WAV:

- `mono + left`: сигнал только в левый канал.
- `mono + both`: один и тот же сигнал копируется в L/R.
- `stereo + left`: используется только левый канал WAV.
- `stereo + both`: используется L/R как в WAV.

Нормализация амплитуды WAV выполняется поканально (независимо для L и R).

## 8) Терминология fade/ramp

Для пользовательского DSL фиксируем названия:

- `fade_in_sec`
- `fade_out_sec`

Это более понятные UX-термины.  
Слово `ramp` можно использовать только как внутренний инженерный термин в коде.

## 9) Комментарии в YAML

Да, комментарии в YAML поддерживаются и приветствуются:

```yaml
# Это комментарий
name: "tDCS"
```

Русские комментарии в пресетах допустимы и полезны как "живая документация".

## 10) Ошибки и отбраковка

- Любая ошибка пресета считается `fatal` для этого файла.
- Невалидный пресет пропускается, валидные продолжают работать.
- Причины пишутся в `errors.log`.

## 11) Runtime-параметры (NVS)

- Значения сохраняются в NVS по ключам вида `preset_file_name + param_name`.
- Отдельного флага "инициализировано" нет: наличие ключа уже означает инициализацию.
- `default` из YAML применяется только при первом создании ключа.

