# TamaNest: authors and licences

TamaNest (https://github.com/WavieGotchi/TamaNest) is free software under the GNU General Public License, version 3 (see [LICENSE](LICENSE)).

Copyright (C) 2026 WavieGotchi (https://github.com/WavieGotchi)

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
License as published by the Free Software Foundation, version 3, with the additional terms below. This program is
distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

TamaNest contains no game ROMs. Tamagotchi is a trademark of Bandai; TamaNest is not affiliated with or endorsed by Bandai.

Parts of TamaNest are based on the work of other people. Their copyright and licences stay in force.

## Emulator core (`components/tama_core`)

- **TamaEmu** by maragotchi, https://github.com/maragotchi/tamaemu, GNU GPL version 3
  (`components/tama_core/LICENSE-tamaemu`). The S1C33 core, the LCD (`tc_lcd.c`) and the 4U PN512 stub follow TamaEmu.
- **Paramix Studio** by mameise, https://github.com/mameise/Paramix-Studio, GNU GPL version 3 or later: the embedded C core TamaNest started from (itself derived
  from tamaemu), and the pet-data table used by EZTOOLS and the custom status bars.
- **DLC** (`main/dlc.c`): the store tables, record format and routing follow TamaEmu's `tools/dlc.c`.
- Changes for TamaNest: the generated threaded interpreter (`gen_turbo.py` -> `tc_turbo.c`), native graphics and
  drawing routines (`tc_hle.c`), timing, audio, save and flash-page handling, and the ESP32-S3 host (`main/`).

## Fonts

- **Press Start 2P** by codeman38 (Cody "CodeMan38" Boisclair), SIL Open Font License 1.1 (`main/font_menu.h`).
- **Silkscreen** by Jason Kottke, SIL Open Font License 1.1 (`main/font_small.h`).

## Other components

- **Board support** for the Waveshare ESP32-S3-Touch-AMOLED-2.16, Waveshare, Apache License 2.0 (`bsp/`, with changes listed in `bsp/README.md`).
- **ESP-IDF**, **esp_tinyusb**, **esp_lvgl_adapter** and other Espressif components, Apache License 2.0.
- **LVGL**, MIT License.
- **ESP Web Tools** (installer page, loaded from a CDN), Apache License 2.0.
- **web-serial-polyfill** by Google LLC (installer page on Android, `web/web-serial-polyfill.js`), Apache License 2.0.

The pixel art drawn for TamaNest (menu icons, loading eggs, battery, low-battery character) is part of TamaNest and
under the same licence.

## Additional terms (GNU GPL version 3, section 7)

These terms apply to the parts of TamaNest written for TamaNest (not to the code of TamaEmu or Paramix Studio,
which keep their own licences). Anyone who conveys TamaNest, modified or not, must:

1. **Keep the attributions** (section 7(b)): the "About & Credits" screen, from the TamaNest main menu, and the credits in this file and in the README must
   stay, naming the TamaNest authors (**WavieGotchi**), **TamaEmu by maragotchi** and **Paramix Studio by mameise**, with the link to the
   source code of the version being conveyed.
2. **Mark modified versions** (section 7(c)): a modified version must say clearly, on its About screen and in its
   documentation, that it has been changed from the original TamaNest, and must not present itself as the original.
3. **Not use the name** (section 7(e)): no rights to the name "TamaNest" or its logo are granted. A modified version
   must use a different name; it may say that it is based on TamaNest.

These terms add to the GPL; they do not limit anyone's right under the GPL to copy, change and share the software,
including commercially, as long as the source code is provided.
