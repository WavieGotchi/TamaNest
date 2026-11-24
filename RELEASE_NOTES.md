# TamaNest 1.0.3

- Fixed: Tamagotchi 4U could get stuck in the evening - a green block where the pet should be, a long tone, and
  the game no longer responding. TamaNest's sped-up drawing for the 4U's evening colour tint had a mistake. If this
  happened to you, quick menu > RESTART ROM brings the game back (your pet comes from the game's own last save).
- New: daily save backups. For each game, TamaNest keeps the save from the start of each day in
  `saves/backups/` (the last 7 days). To go back to one, copy it over the game's file in `saves/` (rename it to
  `<game>.sav`) with WI-FI TRANSFER or USB DRIVE.

Update from the device (SETTINGS > UPDATE > CHECK ONLINE), from the SD card, or with the installer page.

---

# TamaNest 1.0.2

## New

- **Wi-Fi transfer** (main menu > FILE TRANSFER > WI-FI TRANSFER): manage the SD card from any web browser on the same
  Wi-Fi - phone, tablet or computer, no app needed. Scan the QR code on TamaNest's screen (or type the address and
  code it shows), then upload ROMs, saves and DLC, download saves, exports and screenshots, make folders and delete
  files. The access code is new every time, and the page only works while that screen is open.
- **Set up Wi-Fi with your phone** (SETTINGS > UPDATE > WI-FI > SET UP WITH A PHONE): TamaNest opens its own small
  Wi-Fi network. Scan the QR code to join it, and a setup page opens where you choose your network and type the
  password on your phone, with a "show password" option.
- **Better keyboard** for typing on TamaNest's screen: SHIFT for capitals, number and symbol keys on the bottom row,
  and an eye key that shows the password so you can check for typos.
- **FILE TRANSFER** in the main menu groups USB DRIVE and WI-FI TRANSFER.

## Fixes

- Long Wi-Fi network names now wrap onto two lines instead of running off the screen.
- File names with accents or Japanese characters now work on the SD card.
- The connecting and Wi-Fi setup screens show the right title (WI-FI TRANSFER, DATE/TIME or UPDATE).

Update from the device (SETTINGS > UPDATE > CHECK ONLINE), from the SD card, or with the installer page.

---

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
