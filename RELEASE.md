# Releasing TamaNest

## One-time setup (GitHub)

1. Create a **public** GitHub repository, for example `tamanest`.
2. In `main/main.c`, set `UPDATE_URL_DEFAULT` to `"https://<your-user>.github.io/tamanest"`. The board looks there
   for `manifest.json` and `firmware/tamanest.bin`.
3. In `web/index.html`, replace `SOURCE_LINK` with the repository link. The GPL needs the source to be offered.
4. Push the project. Check `.gitignore` first: it leaves out `CLAUDE.md` and `reference_files/` (notes, captures,
   and mock-ups with Bandai material), backups, the build, ROMs and saves.
5. On GitHub, open Settings > Pages > Source and choose **GitHub Actions**. The workflow in `.github/workflows/pages.yml`
   publishes `web/` whenever it changes.

## Every new version

1. Raise `PROJECT_VER` in `CMakeLists.txt`, for example 0.9.0 to 0.9.1. Boards only offer a version that is newer.
2. Run `idf.py build`, then `sh web/make_release.sh`. This copies the firmware into `web/firmware/` and stamps the
   version into `web/manifest.json`.
3. Commit and push. After a minute or two:
   - The installer page offers the new version.
   - SETTINGS > UPDATE > CHECK ONLINE on the boards finds it.
   - For people without Wi-Fi: they can put `web/firmware/tamanest.bin` on the SD card in `UPDATE/`, then use
     SETTINGS > UPDATE > SD CARD.
4. Optionally also attach `tamanest.bin` to a GitHub Release with release notes.

## Before the first public release

- After making the repo public: check every link on the installer page works (the "latest release" link to
  https://github.com/WavieGotchi/TamaNest/releases, the source-code link) and that the About screen's QR code opens the repo.

- **Status-bar food icons**: `main/pc_bowls.h`, `ps_icons.h`, `idl_icons.h`, `u4_icons.h` were cut from the games' own
  status screens (Bandai art). Replace them with original drawings before publishing, or load them from the
  player's own ROM at run time.
- Add the licence notices (done: `LICENSE`, `NOTICE.md`).

- Set `TL_RAMDUMP` to 0. This hides SNAPSHOT IN 5S/10S.
- Remove or quiet the temporary logs (TRACE, FLASHSAVE, LOC) and the `TL_*` one-off code.
- Rename the binary, the log tag and the folder from TamaLauncher/lvgl_demo_v9 to TamaNest. If you do, change the
  binary name in `web/make_release.sh` to match.
- The flash layout (two app slots) is already final, so later updates never need an erase.

## Update signing (since 2026-11-23)

Devices only install Wi-Fi/SD-card updates signed with the TamaNest key (public half: `main/update_key.h`,
key id `96b11219`). `web/make_release.sh` signs `web/firmware/tamanest.bin` with `web/sign_update.py` and stops if
it cannot.

- The **private key** is `~/Documents (No Cloud)/TamaNest signing key/tamanest-update-key.pem` on the owner's Mac.
  It is never committed (`.gitignore` blocks `*.pem`). **Keep a backup** (e.g. in a password manager as a secure
  note). If it is lost, released devices cannot take new Wi-Fi/SD updates until they are reinstalled once over USB
  with a build containing a new public key.
- Check a file: `python web/sign_update.py --check web/firmware/tamanest.bin`.
- Keep two-factor authentication on the GitHub account.
