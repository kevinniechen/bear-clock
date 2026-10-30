# Bear clock

Wearable countdown clock from *The Bear* S4: blue 7-segment countdown on a 64x32 HUB75
panel, kitchen phrases, full-screen flashes, and guests can scan a QR on the apron to
"fire a ticket" (a message) onto it.

- **Board:** Seengreat/WatangTech "RGB Matrix HUB75 S3" (ESP32-S3-WROOM-1-N16R8).
  Pinout from https://seengreat.com/wiki/214/rgb-matrix-hub75-s3, in `firmware/include/config.h`.
- **Panel:** 64x32 P2.5 HUB75.

## How it works

```
guest phone ──QR──> kevinniechen.github.io/bear-clock/ ──POST──> ntfy.sh/<guest topic>
your phone ──> .../admin.html#<admin topic> ──POST──> ntfy.sh/<admin topic>
                                                         │ (one HTTPS stream)
board (on your phone's hotspot) <────────────────────────┘
board ──status every 2 min + after each command──> ntfy.sh/<guest topic>-status ──> live clock on both pages
```

Fallback with no internet: the board always runs an open hotspot **THE-BEAR-CLOCK**.
Joining it pops a captive portal with the same guest page. `http://192.168.4.1/admin` is
the admin page there (PIN = `ADMIN_PIN` in `secrets.h`).

## Tomorrow checklist (~1 hour)

1. **Wire it.** Ribbon cable from the panel's *input* HUB75 header (arrow points away
   from it) to the board's box header. Panel power cable into the board's VH-4P
   connector. One USB-C into the board's main port (not "Power"). That port powers
   both. Use a power bank or charger rated 2 A+. Full-screen flashes are the peak draw.
2. **Wi-Fi.** Edit `firmware/include/secrets.h` → `WIFI_NETWORKS` with your phone hotspot.
   iPhone: Settings → Personal Hotspot → **Maximize Compatibility ON** (ESP32 is 2.4 GHz only).
   The SSID must match exactly, including the curly apostrophe in `Kevin’s iPhone`.
3. **Flash.** `cd firmware && pio run -t upload -t monitor`. If upload can't find the
   port: hold BOOT, tap EN, release BOOT, retry.
4. **Smoke test** (serial monitor): type `hello` + Enter → it should scroll.
   Type `{"cmd":"set","m":2,"start":true}` → a 2-minute countdown runs.
   `status` prints the state JSON.
5. **Panel fixes, if needed** (`config.h`, re-flash):
   - wrong colors → `PANEL_RGB_ORDER` (e.g. blue looks green → try `"RBG"`/`"BGR"`)
   - garbled/ghosted/blank → `PANEL_FM6126A 1`, and/or `PANEL_CLKPHASE false`
   - upside down → `PANEL_ROTATE_180 1`
6. **Phones.** Scan `qr/guest-qr.png` and fire a message. Open the admin link (below) and
   press Start. The "seen Xs ago" dot goes green once the board has reported in.
7. Set the real duration on the admin page (default 3:00:00) → **Set & start**.

Admin link (keep private, it's the key):
`https://kevinniechen.github.io/bear-clock/admin.html#<NTFY_ADMIN_TOPIC from secrets.h>`

## Controls

- **BOOT button:** tap = start/pause, hold 2 s = reset.
- **Thumb wheel** (PCA9557, unverified): K1 like BOOT, K2 = skip message, K3 = flash a phrase.
  Serial prints `button N short/long` so you can learn which is which.
- **Admin commands** (JSON over ntfy admin topic, `/api/cmd?pin=`, or serial):
  `start` `pause` `toggle` `reset` `set{h,m,s,start}` `remaining{sec}` `add{sec}`
  `flash{text}` `msg{text}` `skip` `clear` `guests{on}` `filter{on}` `brightness{v}`
  `color{hex}` `textcolor{hex}` `flashevery{sec}` `repeats{n}` `ghost{on}` `zero{text}`
  `phrases{list}` `status` `restart`.

Settings, phrases, and the countdown (saved every 10 s) persist across power loss.

## Layout

- `firmware/lib/bearcore/`: platform-independent clock, queue, text sanitizing,
  profanity masking, rendering. **All visuals live here.**
- `firmware/src/main.cpp`: panel driver, Wi-Fi/ntfy, hotspot + captive portal, buttons, NVS.
- `web/index.html`: guest page (QR target). `web/admin.html`: controls. Both are embedded
  into the firmware at build time for hotspot mode.
- `sim/run.sh`: compiles bearcore natively, runs logic tests under ASan/UBSan, and
  renders `sim/demo.gif`. Change visuals, run this, look at the GIF, *then* flash.
- `qr/`: printable QR codes (normal + mirrored for iron-on transfer).

The guest topic is public by design (it's in the page). If someone spams it, change
`NTFY_GUEST_TOPIC` in `config.h` **and** `GUEST_TOPIC` in both web pages, push, and re-flash.
The QR points at the page, not the topic, so no reprint is needed. Or just hit Guests OFF.
Spam can't control the clock: only the admin topic takes commands.
