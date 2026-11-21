# HakiOS · Xiaomi Redmi 9 (lancelot)

[English](README.md) · Русский

**Открытое ядро Apple Darwin (XNU, xnu-8796.121.2 — ядро iOS 16.5) на Xiaomi Redmi 9** — так же, как Android это Linux на
телефонах: свой загрузчик (`mtkloader`), свои драйверы, поверх — немодифицированный userland iOS 16.5
(launchd → backboardd → SpringBoard → приложения).

Устройство: **Xiaomi Redmi 9 global (lancelot) — MediaTek Helio G80 (MT6769T, family MT6768), Mali-G52, PMIC MT6358,
тач FocalTech FT8719P, Wi-Fi/BT CONNSYS MT6631.** Порт основан на baseline Infinix Hot 20 (X6826B, MT6768) ветки `mtk`;
журналы Infinix в `docs/` описывают тот телефон и **не** всё применимо к lancelot.

Телефон ничего не теряет. `mtkloader` встаёт в `boot` перед ядром Android, один раз запускает XNU, после сеанса телефон
сам возвращается в Android. Образ системы iOS — в отдельном разделе `xnusys` на eMMC.
**Исследовательский стенд, не прошивка на каждый день.**

**Статус и чеклист:** [docs/lancelot-release-checklist.md](docs/lancelot-release-checklist.md) ·
**Установка:** [docs/from-a-clone.md](docs/from-a-clone.md), [docs/mtk-porting.md](docs/mtk-porting.md).

## Что работает на lancelot (проверено на устройстве)

- **Загрузка и возврат** — XNU грузится, SMP 4 ядра, сеанс завершается возвратом в Android без паники; откат на сток — одна прошивка `boot`.
- **Графика** — драйвер Mali в ядре (`redwood_mali.c`), Mesa **panfrost** (OpenGL ES) и **Metal** (`redwoodmetal.m`): SpringBoard, домашний экран, жесты, Пункт управления; чётко на 2× (`fbscale=2`).
- **Настройки** открываются с Metal-драйвером (`metal=1`).
- **Акселерометр / SCP** — сенсорный хаб MT6768 (`redwood_scp.c`, `scp_step=7`), реальные x/y/z 50 Гц.
- **Поворот экрана** — акселерометр → orientation → backboardd, end-to-end.
- **Фонарик** — LED191 на двух GPIO (EN152/SEL153), `redwood_mtktorch.c` (`mtktorch=3`) + ABI iOS.
- **Батарея** — процент, напряжение, ток, температура через MT6358 fuel gauge (`redwood_mtkbatt.c`, `mtkbatt=2`); совпадает с Android.
- **Тач** — FocalTech FT8719P (`redwood_fts.c`), отчёты без ошибок (0 bad/timeout/CRC).
- **Bluetooth** — контроллер отвечает, `bluetoothd` стабилен (краш при скане исправлен), LE-скан принимает advertiser'ов. **Доставка в приложения работает**: приложение `bt_test` с TCC-bypass entitlement (`com.apple.private.tcc.allow`) запускается из SpringBoard, регистрируется в `bluetoothd` и сканирует **без запроса Allow** (`tccRequired : 0`; автотест `btapp` 5/5, `auto_bt=1`).
- **Заставка** — оригинальный знак HakiOS (`mtkloader/splash.h`), без ассетов Apple.

## Известные баги / ещё не готово (честно)

- **Тач залипает/вязкий** — рендер ~30 fps (не 60); чип исправен, лаг от кадр-рейта/доставки HID.
- **Настройки падают без Metal** — открываются только с `metal=1`.
- **Картинка шакальная на 1×** — 375×812 растягивается на 1080×2340; чётко на 2×/нативе.
- **Интернет в demo не работает** — ядро начинает join, но `wifid` отказывает в ассоциации/DHCP; в отдельных прогонах Wi-Fi трафик шёл.
- **Bluetooth не доставляет в приложения/Настройки** — TCC не строит identity приложения, промпт Allow не поднимается.
- **Звук (динамик) — DMA работает.** Вся цепочка DL1→ADDA DAC→MT6358 LOL→AW87519→динамик запущена и проверена автотестом `audio4` (12/12): клоки APLL1, кодек, LOL (−40 dB), профиль усилителя, memif DL1, IRQ0, маршрут DL1→ADDA_DL, **DMA читает кольцо** (CUR движется, ADDA принимает данные). Тон 1 кГц звучит (`mtkafe=4`), громкость регулируется (`mtkafe_gain`, `audio4g` 12/12); sink для звука iOS зарегистрирован (`mtkafe=5`, HAL-плагин Speaker подтверждён). **Микрофон: DMA записи работает** (`mtkmic=1`, `mic` 4/4): MT6358 ADC1→ADDA UL→VUL12→моно-кольцо `/dev/redwood-mic`, ~48k кадр/с. Физическое прослушивание/запись и звук в приложениях — при интерактивной проверке. Детали: [docs/lancelot-audio-backend.md](docs/lancelot-audio-backend.md).
- Экран может гаснуть (auto-lock); натив 1080×2340 + UIKit-scale не сведены с тачем.
- Камера, вибрация, GPS — в очереди ([docs/lancelot-peripherals.md](docs/lancelot-peripherals.md)).
- Онлайн-сервисы Apple (активация, Apple ID, iCloud, App Store) — **не поддерживаются, не цель** (нет Secure Enclave).

## Что приносишь ты, и чего здесь нет

Ничего из Apple и вендорских прошивок в репозитории нет. Userland раскладываешь сам из своего IPSW (`ios165/fetch.sh`);
прошивки MediaTek (CONNSYS, SCP) — со своего телефона. Ничего не качается за тебя, не расшифровывается и не обходится.
Ни одного файла iOS, прошивки телефона, скриншота iOS, серийника или радио-идентификатора в репо нет.
См. [docs/lancelot-release-audit.md](docs/lancelot-release-audit.md).

## Сборка

Ядро: внешнее дерево XNU + `xnu/redwood.patch` (`xnu-build.sh`). Userland: `userland/build.sh` (`GL=1 METAL=1` для графики).
Образ системы: `tools/system-flash.sh` → `xnusys`. Загрузчик + сеанс: `tools/xnu-payload.sh` + `tools/xnu-smoke.sh`.
Все команды по порядку: [docs/from-a-clone.md](docs/from-a-clone.md).

> **Важно:** образ системы iOS (`system.apfs` для `xnusys`) **в репозитории не распространяется** — это проприетарный
> userland Apple, его собираешь сам из своего IPSW (`ios165/fetch.sh`). Без него XNU загрузится, но launchd не найдёт
> `libSystem.B.dylib`/`launchd` (ошибка «отсутствует libSystem.B.dylib»). Готового «boot.img с iOS» нет и быть не может:
> `boot.img` несёт только загрузчик + ядро, корень iOS живёт в `xnusys` и является проприетарным.

## Автотесты

Автоматический стенд: одна загрузка → пробы → парсинг pstore → отчёт PASS/FAIL.
`HAKIOS_SERIAL=<serial> ./tools/autotest.sh <profile>` (под WSL). Профили: `hwprobe`
(акселерометр, фонарик, кодек, усилитель, батарея — 7 проверок), `audio3`/`audio4` (звук).
Отчёты в `logs/autotest/<ts>-<profile>/report.{json,md}`. Описание: [docs/lancelot-autotest.md](docs/lancelot-autotest.md).

## Лицензия и правовое

Патч XNU и производные — Apple Public Source License 2.0; остальное — MIT, как указано в заголовках файлов. Не юридическая консультация.

HakiOS — независимый исследовательский проект, **не связан с Apple и не одобрен Apple**; «Darwin», «iPhone», «XNU», «iOS»,
«Apple» названы только чтобы описать, с чем работает код. Если считаешь, что файл нарушает чьи-то права — открой issue.

## Происхождение

Основано на baseline HakiOS `mtk` (Infinix Hot 20 / X6826B). Upstream:
[hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel). Этот репозиторий — порт на Redmi 9 (lancelot).
