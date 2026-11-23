import { serial as usbSerial } from "./web-serial-polyfill.js";   /* Android: serial ports over WebUSB */
/* TamaNest installer tweaks for ESP Web Tools 10.1.0 (re-check if the version changes):
 * 1. Safer USB: Chrome on macOS crashed when the device's USB connection dropped while the port was open (the
 *    restart after writing). Now the device is restarted by its own watchdog ~2.5 s after the port is closed, the
 *    port is not reopened afterwards (no Improv: "new_install_improv_wait_time": 0 in manifest.json; TamaNest does
 *    not use Improv), and "Next" shows a finish message instead of a menu that needs the port.
 * 3. Pixel-art icons instead of the emoji on the success and error screens (end of the file).
 * 2. The device's loading egg (SPIN, from main/loading_eggs.h) replaces the install window's
 * spinner. Generated from the firmware's egg frames; colours = the device's menu greens.
 * ESP Web Tools draws its progress view with <ewt-page-progress>; we swap its drawing for <tn-egg-progress>:
 * no percentage yet -> the egg spins in place; a percentage -> the egg rolls along an outlined bar, as on the device. */
const N = 23;
const FRAMES = [
  "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000003333000000000000000003311443300000000000000311111144300000000000003112111443000000000000311552111443000000000003115771114430000000000311127111144430000000003111111111188300000000031111111118663000000000311111111476830000000003411771111444300000000034112274144443000000000034482814444300000000000344464444443000000000000334444443300000000000000033333300000000000000000000000000000000",
  "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000003333000000000000000003311443300000000000000311111144300000000000003111111443000000000000321551111443000000000003215111114430000000000311111111144430000000003111172711144300000000031111222114443000000000311117271414430000000003611111111444300000000036111114144443000000000036441414444300000000000344444444443000000000000334444443300000000000000033333300000000000000000000000000000000",
  "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000003333000000000000000003311443300000000000000311111144300000000000003111111443000000000000311551111443000000000003115111116830000000000311111111146830000000003221111111188300000000032211111114443000000000322111111414430000000003411111111444300000000034111114144443000000000034441414444300000000000344444444443000000000000334444443300000000000000033333300000000000000000000000000000000",
  "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000003333000000000000000003311443300000000000000311111144300000000000003111111443000000000000311551111443000000000003115172714430000000000311111222144430000000003111117271144300000000031111111114443000000000311111111414430000000003411111111444300000000034111114144443000000000034441414444300000000000344444444443000000000000334444443300000000000000033333300000000000000000000000000000000",
  "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000003333000000000000000003311443300000000000000311111144300000000000003111111443000000000000311551111443000000000003775111114430000000000372711111144430000000003771111111144300000000031111111114443000000000311111111874430000000003411111122664300000000034111114188443000000000034441414444300000000000344444444443000000000000334444443300000000000000033333300000000000000000000000000000000",
  "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000003333000000000000000003311443300000000000000311111144300000000000003111111443000000000000311551111443000000000003115111114630000000000311111111144630000000003111111111146300000000031111111114463000000000311727111414430000000003412221111444300000000034172714144443000000000034441414444300000000000344444444443000000000000334444443300000000000000033333300000000000000000000000000000000",
  "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000003333000000000000000003311443300000000000000311111144300000000000003111111443000000000000311551111443000000000003115117714430000000000311111122744430000000003111111227144300000000031111117714443000000000321111111414430000000003611111111444300000000036111114144443000000000034441414444300000000000344444444443000000000000334444443300000000000000033333300000000000000000000000000000000",
  "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000003333000000000000000003311443300000000000000311111144300000000000003111111643000000000000311551172843000000000003125111726430000000000317271111784430000000003122711111144300000000031771111114443000000000311111111414430000000003411111111884300000000034111114186843000000000034441414884300000000000344444444443000000000000334444443300000000000000033333300000000000000000000000000000000"
];
const BOTTOM = [21, 21, 21, 21, 21, 21, 21, 21];
/* 7, 8: corners of the 3x3 spots in a colour between spot and egg (shell / shadow), so they read as round dots
 * on a computer screen (website only; generated by scratchpad ewt/roundspots.py). */
const COL = [null, "#a8d47c", "#80a862", "#1c2a14", "#8eba68", "#d2ecb0", "#709656", "#94be6f", "#7fa85f"];
const INK = "#1c2a14", W = 112, H = 30;           /* virtual pixels like the device's LCD */
const BX = 8, BW = 96, BH = 7, BY = H - BH - 1;
/* Always a WHOLE number of screen pixels per egg pixel (3x like the device, 2x if the window is narrow): a canvas
 * scaled by an odd factor drops or doubles pixel rows, which made the round spots look like squares. */
let S = 3;

class TnEggProgress extends HTMLElement {
  constructor() {
    super();
    const r = this.attachShadow({ mode: "open" });
    r.innerHTML = `<style>
      :host { display: flex; flex-direction: column; align-items: center; gap: 10px; text-align: center; }
      canvas { image-rendering: pixelated; }
      .pct { font: 0.8rem/1.2 var(--pixel, monospace); }
    </style><canvas></canvas><div class="pct"></div><div class="label"></div>`;
    this._cv = r.querySelector("canvas");
    this._c = this._cv.getContext("2d");
    this._s = 0;
    this._pct = r.querySelector(".pct");
    this._label = r.querySelector(".label");
    this._spin = 0;
  }
  set progress(v) { this._progress = v; this._draw(); }
  set label(v) { this._label.textContent = v ?? ""; }
  _fit() {                                         /* whole-number scale that fits the window, sharp on Retina too */
    const avail = this.clientWidth || W * 3, s = Math.max(1, Math.min(3, Math.floor(avail / W)));
    const dpr = Math.max(1, Math.round(window.devicePixelRatio || 1));
    if (s * dpr === this._s) return;
    this._s = s * dpr; S = this._s;
    this._cv.width = W * S; this._cv.height = H * S;
    this._cv.style.width = W * s + "px"; this._cv.style.height = H * s + "px";
  }
  connectedCallback() {
    this._ro = new ResizeObserver(() => { this._fit(); this._draw(); });
    this._ro.observe(this);
    this._timer = setInterval(() => { if (this._progress === undefined) { this._spin = (this._spin + 1) & 7; this._draw(); } }, 220);
    this._draw();
  }
  disconnectedCallback() { clearInterval(this._timer); if (this._ro) this._ro.disconnect(); }
  _px(x, y, w, h, c) { this._c.fillStyle = c; this._c.fillRect(x * S, y * S, w * S, h * S); }
  _egg(f, xc, bottomY) {                           /* bottomY = the row the egg's lowest pixel sits on */
    const fr = FRAMES[f], ex = xc - (N >> 1), ey = bottomY - BOTTOM[f];
    for (let r = 0; r < N; r++)
      for (let c = 0; c < N; c++) {
        const k = fr.charCodeAt(r * N + c) - 48;
        if (k) this._px(ex + c, ey + r, 1, 1, COL[k]);
      }
  }
  _draw() {
    this._fit(); S = this._s;
    this._c.clearRect(0, 0, W * S, H * S);
    const p = this._progress;
    if (p === undefined) {                         /* waiting: spin in place */
      this._pct.textContent = "";
      this._egg(this._spin, W >> 1, H - 4);
      return;
    }
    const pc = Math.max(0, Math.min(100, p | 0));
    const fill = Math.floor((BW - 4) * pc / 100);
    this._px(BX, BY, BW, 1, INK); this._px(BX, BY + BH - 1, BW, 1, INK);      /* outlined bar */
    this._px(BX, BY, 1, BH, INK); this._px(BX + BW - 1, BY, 1, BH, INK);
    if (fill > 0) this._px(BX + 2, BY + 2, fill, BH - 4, INK);
    this._egg((fill / 5 | 0) & 7, BX + 2 + fill, BY - 3);                    /* rolls with the distance */
    this._pct.textContent = pc + "%";
  }
}
customElements.define("tn-egg-progress", TnEggProgress);

customElements.whenDefined("ewt-page-progress").then(() => {
  const C = customElements.get("ewt-page-progress");
  C.prototype.render = function () {
    if (!this._tn) this._tn = document.createElement("tn-egg-progress");
    this._tn.progress = this.progress;
    return [this._tn, this.label];                 /* the label can be formatted text: ESP Web Tools draws it */
  };
});

/* 0. Only offer the right kind of device (owner 2026-11-23: Chrome's list also showed Bluetooth and other serial
 *    ports). The Waveshare board connects through the ESP32-S3's own USB port, which always has Espressif's USB
 *    vendor id 0x303A (also in download mode), so other devices are not listed and cannot be written to.
 *    Android (2026-11-23, owner's tablet): Chrome's Web Serial there does not list USB serial ports, but WebUSB
 *    reaches the board; Google's web-serial-polyfill (web/web-serial-polyfill.js, Apache-2.0) provides the same
 *    port interface over WebUSB. This file loads before ESP Web Tools so its "is Web Serial there?" check passes. */
const FILTERS = [{ usbVendorId: 0x303a }];
/* Chrome on Android TABLETS asks for desktop sites by default, so its user agent says "Linux x86_64"-like text
 * without "Android". The processor still shows: Android devices are ARM. A desktop Linux PC (Intel/AMD) keeps the
 * normal route - there the system's own serial driver owns the port, so WebUSB could not use it anyway.
 * "?usb" in the address forces the WebUSB route (link "Try alternate USB mode" under the install button). */
const PLAT = ((navigator.userAgentData && navigator.userAgentData.platform) || "") + " " + (navigator.platform || "");
const ANDROID = /Android/i.test(navigator.userAgent) || /Android/i.test(PLAT) ||
                (/Linux/i.test(PLAT) && /arm|aarch64/i.test(navigator.platform || "")) ||
                new URLSearchParams(location.search).has("usb");
if (ANDROID && navigator.usb) {
  const requestUsbPort = (opts) => usbSerial.requestPort({ ...(opts || {}), filters: FILTERS });
  if (navigator.serial) navigator.serial.requestPort = requestUsbPort;
  else Object.defineProperty(navigator, "serial", { value: { requestPort: requestUsbPort }, configurable: true });
} else if (navigator.serial && navigator.serial.requestPort) {
  const requestPort = navigator.serial.requestPort.bind(navigator.serial);
  navigator.serial.requestPort = (opts) => requestPort({ ...(opts || {}), filters: FILTERS });
}

/* 1. Safer USB (see the top of this file). */
async function armWatchdog() {                     /* ESP32-S3 registers from esptool targets/esp32s3.py */
  const ld = window.esploader;                     /* set by ESP Web Tools while it flashes (stub running) */
  if (!ld || !ld.writeReg) return false;
  const RTC = 0x60008000, OPTION1 = 0x6000812c, WDTCONFIG0 = RTC + 0x98, WDTCONFIG1 = RTC + 0x9c,
        WDTWPROTECT = RTC + 0xb0, WKEY = 0x50d83aa1;
  try {
    await ld.writeReg(OPTION1, 0, 0x1);            /* clear "forced download boot", so it starts TamaNest */
    await ld.writeReg(WDTWPROTECT, WKEY);          /* unlock */
    await ld.writeReg(WDTCONFIG1, 340000);         /* stage 0 timeout, RTC slow clock (~136 kHz): ~2.5 s */
    await ld.writeReg(WDTCONFIG0, ((1 << 31) | (5 << 28) | (1 << 8) | 2) >>> 0);   /* on, stage 0 = system reset */
    await ld.writeReg(WDTWPROTECT, 0);             /* lock */
    console.log("TamaNest: watchdog armed - the device restarts after the port is closed");
    return true;
  } catch (e) {
    console.warn("TamaNest: watchdog restart not possible, using the normal reset", e);
    return false;
  }
}
customElements.whenDefined("ewt-install-dialog").then(() => {
  const P = customElements.get("ewt-install-dialog").prototype;
  const finished = (d) => d._installState && d._installState.state === "finished";
  const confirm = P._confirmInstall, init = P._initialize, noImprov = P._renderDashboardNoImprov;
  if (!confirm || !init || !noImprov) { console.warn("TamaNest: installer tweaks not applied (ESP Web Tools changed)"); return; }
  P._confirmInstall = function () {
    const port = this.port;
    if (port && !port._tnOpen) {                   /* the flasher still opens it; only the reopen after "finished" is skipped */
      const open = port.open.bind(port), setSignals = port.setSignals.bind(port);
      port._tnOpen = true;
      port.open = (o) => (finished(this) ? Promise.resolve() : open(o));
      /* Restart after writing: ESP Web Tools resets the device with the RTS line while the port is still open, so
       * the device's USB connection drops under an open port - that crashed Chrome on macOS (same crash site every
       * time, 2026-11-23). Instead arm the chip's RTC watchdog (as esptool's watchdog_reset for the ESP32-S3) to
       * restart it ~2.5 s later, skip the RTS pulse, and let the installer close the port first. */
      let wdt = 0;                                 /* 0 not tried, 1 armed, -1 failed -> normal RTS reset */
      port.setSignals = async (sig) => {
        const st = this._installState;
        const atEnd = st && st.state === "writing" && st.details && st.details.percentage === 100;
        if (atEnd && wdt === 0) wdt = (await armWatchdog()) ? 1 : -1;
        if (atEnd && wdt === 1) return;            /* skip both edges of the RTS reset */
        return setSignals(sig);
      };
    }
    return confirm.apply(this, arguments);
  };
  P._initialize = function (afterInstall) {
    if (afterInstall && finished(this)) { this._client = null; this._tnDone = true; return Promise.resolve(); }
    return init.apply(this, arguments);
  };
  P._renderDashboardNoImprov = function () {      /* "Next" after the install: a finish message instead of a menu */
    if (!this._tnDone) return noImprov.apply(this, arguments);   /* whose options need the (now closed) port */
    if (!this._tnEnd) {
      const msg = document.createElement("div");
      msg.slot = "content";
      msg.innerHTML = "TamaNest is installed. Your device will restart into TamaNest by itself.<br><br>" +
                      "You can unplug it now and close this window.";
      const act = document.createElement("div");
      act.slot = "actions";
      const btn = document.createElement("ew-text-button");
      btn.textContent = "Close";
      btn.addEventListener("click", () => this._closeDialog());
      act.appendChild(btn);
      this._tnEnd = [msg, act];
    }
    return ["All done", this._tnEnd, false];
  };
});

/* 3. Our pixel art instead of the install window's emoji (owner's picks 2026-11-23, mock-ups in
 *    reference_files/mockups/installer_icons_round2.png; drawn by scratchpad icons/icons3.py from the loading egg):
 *    success = S1 baby peeking out of the shell (provisional, round 3: reference_files/mockups/installer_success_round3*.png), error = egg with "!" (W3). 24x24 pixels, shown 4x. */
const ICONS = {
  "\u{1F389}": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABgAAAAYCAYAAADgdz34AAAA/ElEQVR4nGNgGAX0BjJaIv+R+Uz4JMk1HKs5IEEYpoYlJEvKkGk5IyEFMlBDtx2aA+Z72aWA6SfX3jBSbAHIcJjB6ABkETGWMJFjOAiA5IgJMpwWUAswEaPICxruuPgUW7ANLajwBR3JFhy6tJ8kcZIsAAWFnZ4jnI0MOnKWEp1UMQAoday4UgPP2TJY+H27MuAZD19qwvABSHHPqgwwGxddNCGEQUpKBEwTSqoYXgRpgBmADzx79gbO7itYgzO4SMoHz6CGIhtOCGC1FeYLGAD5BmQojEYG+FxPtA+ekeFyGMBpMyzyipB8ggxALgcBQkmV6OIaHZCdB6gNAPnMgu+kAF2RAAAAAElFTkSuQmCC",
  "\u26A0\uFE0F": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABgAAAAYCAYAAADgdz34AAAAx0lEQVR4nGNgGAWjYNADRmIUyWiJ/EfmP7n2hpEqFsAM7lmVAeY/e/YGTPcVrCHaIkZ8hsMMRgfIFhGyhImBxoCRkOu1JEzAtJddCgOu4MLnC6J8cO3FGdKdjssC9LC/RsDwogkhGKkMrwXEAikpEaLUkW3BM2gc0MSCZ8/eEO0DFmIN6EGKF5Ac1X1QEjYDjGEOIBZgWABK06C0DTIEhpEBOp9QPsAaROjg2bM34ORIDsBbFoFoXAYTW+ARLA1xZSJSimyaAgBhI2WDYuJmrgAAAABJRU5ErkJggg==",
};
customElements.whenDefined("ewt-page-message").then(() => {
  const P = customElements.get("ewt-page-message").prototype, render = P.render;
  P.render = function () {
    const src = ICONS[this.icon];
    if (!src || (this.label != null && typeof this.label !== "string")) return render.apply(this, arguments);
    if (!this._tnImg) {
      const img = document.createElement("img");
      img.width = 96; img.height = 96; img.alt = "";
      img.style.cssText = "image-rendering: pixelated; display: block; margin: 0 auto 12px;";
      this._tnImg = img;
      this._tnText = document.createElement("div");
    }
    this._tnImg.src = src;
    this._tnText.textContent = this.label || "";
    /* "Installation complete!" reads as the window's title, in the pixel font; error texts stay plain */
    this._tnText.style.cssText = this.icon === "\u{1F389}" ? "font: 1rem/1.6 var(--pixel, monospace);" : "";
    return [this._tnImg, this._tnText];
  };
});

/* 4. The tips window shown when the device list is closed without choosing (owner 2026-11-23): our own steps -
 *    no driver list (the board needs none: native USB) and no "tiny light" - plus the BOOT-key tip. */
customElements.whenDefined("ewt-no-port-picked-dialog").then(() => {
  const P = customElements.get("ewt-no-port-picked-dialog").prototype;
  P.render = function () {
    if (this._tnDlg) return this._tnDlg;
    const linux = /Linux/.test(navigator.platform) && !/Android/.test(navigator.userAgent);
    const d = document.createElement("ew-dialog");
    d.setAttribute("open", "");
    d.addEventListener("closed", () => this._handleClose());
    d.innerHTML = `
      <div slot="headline">No device chosen</div>
      <div slot="content">
        <div>If your device was not in the list, try this:</div>
        <ol style="padding-left: 1.5em; margin-bottom: 0;">
          <li>Connect the device to this computer with a USB-C cable that can carry data (some cables only charge).</li>
          <li style="margin-top: 8px;">Make sure the device is switched on.</li>
          <li style="margin-top: 8px;">Still not listed? Unplug it, hold its <b>BOOT key</b> (left key when the buttons are on top) while plugging it back in,
            then try again.</li>
          ${linux ? `<li style="margin-top: 8px;">On Linux, your user needs to be in the <code>dialout</code> group:
            <code style="display: block; margin: 0.5em 0;">sudo usermod -a -G dialout YourUserName</code>
            then log out and back in.</li>` : ""}
        </ol>
      </div>
      <div slot="actions"></div>`;
    const act = d.querySelector('[slot="actions"]');
    const button = (text, fn) => {
      const b = document.createElement("ew-text-button");
      b.textContent = text;
      b.addEventListener("click", fn);
      act.appendChild(b);
    };
    if (this.doTryAgain) { button("Cancel", () => this.close()); button("Try again", () => this.tryAgain()); }
    else button("Close", () => this.close());
    this._tnDlg = d;
    return d;
  };
});
