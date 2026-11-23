# TamaNest 1.0.1

- The menus now go to sleep after 1 minute without a button press (was 3 minutes).
- Fixed: after waking from menu sleep, the menus never went to sleep again.

Update from the device (SETTINGS > UPDATE > CHECK ONLINE), from the SD card, or with the installer page.

---

# TamaNest 1.0.0

TamaNest turns the Waveshare ESP32-S3-Touch-AMOLED-2.16 into a colour Tamagotchi that plays the firmware of the toys
you own, aiming to be as close to the real thing as it can get. Built on TamaEmu by maragotchi and Paramix Studio by
mameise.

## Supported Firmwares

| Toy | Editions recognised |
|---|---|
| Tamagotchi Plus Color (2008) | Hexagontchi (2009) |
| Tamagotchi iD (2009) | Lovely Melody ver. (2010) |
| Tamagotchi iD L (2011) | 15th Anniversary ver. (2011), Princess Spacy ver. (2012) |
| Tamagotchi P's (2012) | |
| Tamagotchi 4U (2014) | |
| Tamagotchi 4U+ (2015) | |

**4U and 4U+ are playable, but need further optimisation before they are perfect** (see Known issues).

Japanese originals and English translations are both recognised. A version TamaNest has not been tested and optimised
for still runs if an emulation core supports it, but may be slow; TamaNest says so the first time. Please report it on GitHub.

## Highlights

- Full speed on Plus Color, iD, iD L and P's (4U and 4U+ playable), with real-time clock and date, sleep and wake
  like the real thing.
- Automatic saves, plus the game's own saves (items, purchases) kept; export a save as a ROM image for TamaEmu / Paramix.
- Status bars beside the game (hunger and happiness), a shell colour per toy, battery and clock.
- Quick menu while playing: brightness, sound, screen options (size, saturation, LCD look, display mode, buttons),
  extras, game speed, EZTOOLS (Gotchi Points editor, full care), DLC, save & quit, restart, export.
- DLC: install items, food, games and destinations (including English patches delivered as destinations) from the
  SD card's DLC folder, for iD, iD L, P's, 4U and 4U+.
- Install and update from the web page on a computer (Chrome or Edge) or an Android phone or tablet (Chrome).
- USB DRIVE: the SD card appears on your computer, no card reader needed.
- TamaNest Updates over Wi-Fi (SETTINGS > UPDATE > CHECK ONLINE), from the SD card, or from the web installer. A failed update rolls back.
- Battery friendly: about 0.7 % per hour while the pet sleeps; TamaNest menus sleep after 3 minutes.

## Other devices

TamaNest 1.0 runs on the Waveshare ESP32-S3-Touch-AMOLED-2.16. Interested in another ESP32-S3 device? Please open a
request (issue) on GitHub.

## Known issues

- **4U and 4U+ (playable, still being optimised):** pressing buttons quickly through menus can make the game fall briefly behind (up to about a quarter
  of a second), which stretches the button sounds.
- **iD Lovely Melody:** slows down in the rhythm arcade game.
- **English patches** may apply slowly (about 80 seconds for the 4U patch). Leave the game running until it finishes.
- **TamaNest's own status bars** (the hunger and happiness display around the game) are not available yet for iD,
  iD Lovely Melody or Hexagontchi. The games' own status screens work normally. TamaNest status bars for these firmwares is planned.
- **P's Pierces** are not supported yet.

## If something goes wrong

TamaNest cannot permanently damage your device. To reinstall at any time: hold the **BOOT key** (left key when the buttons are on top) while plugging
the device into a computer or Android phone/tablet, open the installer page in Chrome (or Edge) and press INSTALL. Your SD card (ROMs and saves)
is not touched by reinstalling.

## Credits

TamaNest by WavieGotchi is free software (GPL-3.0): https://github.com/WavieGotchi/TamaNest. Emulator based on tamaemu by maragotchi and
Paramix Studio by mameise. Fonts: Press Start 2P and Silkscreen (SIL OFL). Not affiliated with or endorsed by Bandai.
