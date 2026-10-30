// Hardware + behavior config. Things to tweak tomorrow live here.
#pragma once

// ---- Panel: 64x32 P2.5, 1/16 scan ----------------------------------------
#define PANEL_W 64
#define PANEL_H 32
// If colors are wrong (e.g. blue shows as green), change the order the
// framebuffer's R,G,B are sent to the panel: "RGB", "RBG", "GRB", "GBR", "BRG", "BGR".
#define PANEL_RGB_ORDER "RGB"
// Mount the panel upside down? Set to 1.
#define PANEL_ROTATE_180 0
// If the image is garbled/ghosted, try 1 (FM6126A driver chip) and/or flip clkphase.
#define PANEL_FM6126A 0
#define PANEL_CLKPHASE true

// HUB75 -> ESP32 pins, from the Seengreat wiki pin table.
#define PIN_R1 5
#define PIN_G1 4
#define PIN_B1 6
#define PIN_R2 15
#define PIN_G2 7
#define PIN_B2 17
#define PIN_A 8
#define PIN_B 18
#define PIN_C 10
#define PIN_D 9
#define PIN_E 16
#define PIN_CLK 12
#define PIN_LAT 11
#define PIN_OE 13

// Other board pins.
#define PIN_I2C_SDA 1
#define PIN_I2C_SCL 2
#define PIN_SPK_EN 3   // NS4150 speaker amp enable; kept low (off)
#define PIN_BOOT 0     // BOOT button: tap = start/pause, hold 2s = reset

// ---- Network ----------------------------------------------------------------
// The board always runs its own open hotspot with a captive portal, so phones
// can join it directly if there's no internet. 192.168.4.1 = guest page,
// 192.168.4.1/admin = controls.
#define AP_SSID "THE-BEAR-CLOCK"
#define AP_PASS ""  // "" = open network; or 8+ chars

// Guest messages arrive through ntfy.sh (free pub/sub relay). This topic is
// public by design (it's baked into the QR page) and must match web/index.html.
#define NTFY_HOST "ntfy.sh"
#define NTFY_GUEST_TOPIC "bearclock-a2039d53"
