#include "bearcore.h"

#include <string.h>

#include "font5x7.h"

namespace bear {

// ---------------------------------------------------------------------------
// Frame + primitives

RGB scale(RGB c, int num, int den) {
  return {uint8_t(c.r * num / den), uint8_t(c.g * num / den), uint8_t(c.b * num / den)};
}

void Frame::clear(RGB c) {
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) px[y][x] = c;
}

void Frame::fillRect(int x, int y, int w, int h, RGB c) {
  for (int yy = y; yy < y + h; yy++)
    for (int xx = x; xx < x + w; xx++) set(xx, yy, c);
}

int textWidth(const char* s, int sc) {
  int n = (int)strlen(s);
  return n ? (n * 6 - 1) * sc : 0;
}

void drawText(Frame& f, int x, int y, const char* s, RGB c, int sc) {
  for (; *s; s++, x += 6 * sc) {
    if (x >= W || x + 5 * sc < 0) continue;
    const uint8_t* g = &FONT5X7[(uint8_t)*s * 5];
    for (int col = 0; col < 5; col++)
      for (int row = 0; row < 7; row++)
        if (g[col] & (1 << row)) f.fillRect(x + col * sc, y + row * sc, sc, sc, c);
  }
}

// 7-segment digit, 7 wide x 14 tall, bars 2px thick.
//   aaaaa
//  f     b
//  f     b
//   ggggg
//  e     c
//  e     c
//   ddddd
static const uint8_t SEGMENTS[10] = {
    // bit: a b c d e f g
    0b1111110, 0b0110000, 0b1101101, 0b1111001, 0b0110011,
    0b1011011, 0b1011111, 0b1110000, 0b1111111, 0b1111011,
};

void drawDigit(Frame& f, int x, int y, int d, RGB on, RGB ghost) {
  uint8_t m = (d >= 0 && d <= 9) ? SEGMENTS[d] : 0;
  struct Seg { int x, y, w, h; };
  static const Seg segs[7] = {
      {1, 0, 5, 2},   // a
      {5, 1, 2, 6},   // b
      {5, 7, 2, 6},   // c
      {1, 12, 5, 2},  // d
      {0, 7, 2, 6},   // e
      {0, 1, 2, 6},   // f
      {1, 6, 5, 2},   // g
  };
  // Draw ghosts first so lit segments win where bars overlap.
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < 7; i++) {
      bool lit = m & (1 << (6 - i));
      if (lit != (pass == 1)) continue;
      RGB c = lit ? on : ghost;
      if (!lit && ghost == RGB{0, 0, 0}) continue;
      f.fillRect(x + segs[i].x, y + segs[i].y, segs[i].w, segs[i].h, c);
    }
  }
}

// ---------------------------------------------------------------------------
// Text sanitizing

static int latin1Base(uint32_t cp) {
  // Maps U+00C0..U+00FF to an unaccented ASCII letter (0 = drop).
  static const char* up = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPs";  // C0..DF
  static const char* lo = "aaaaaaaceeeeiiiidnooooo/ouuuuypy";  // E0..FF
  if (cp >= 0xC0 && cp <= 0xDF) return up[cp - 0xC0];
  if (cp >= 0xE0 && cp <= 0xFF) return lo[cp - 0xE0];
  return 0;
}

size_t sanitize(const char* in, char* out) {
  size_t n = 0;
  bool pendingSpace = false;
  auto emit = [&](char c) {
    if (c == ' ') {
      if (n > 0) pendingSpace = true;
      return;
    }
    if (pendingSpace && n < (size_t)MAX_TEXT) out[n++] = ' ';
    pendingSpace = false;
    if (n < (size_t)MAX_TEXT) out[n++] = c;
  };
  const uint8_t* p = (const uint8_t*)in;
  while (*p && n < (size_t)MAX_TEXT) {
    uint32_t cp;
    int len;
    if (*p < 0x80) { cp = *p; len = 1; }
    else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; len = 2; }
    else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; len = 3; }
    else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; len = 4; }
    else { p++; continue; }  // stray continuation byte
    bool bad = false;
    for (int i = 1; i < len; i++) {
      if ((p[i] & 0xC0) != 0x80) { bad = true; len = i; break; }
      cp = (cp << 6) | (p[i] & 0x3F);
    }
    p += len;
    if (bad) continue;

    if (cp == '\t' || cp == '\n' || cp == '\r' || cp == 0xA0) emit(' ');
    else if (cp >= 32 && cp < 127) emit((char)cp);
    else if (cp == 0x2018 || cp == 0x2019 || cp == 0x201B || cp == 0x2032) emit('\'');
    else if (cp == 0x201C || cp == 0x201D || cp == 0x2033) emit('"');
    else if (cp == 0x2013 || cp == 0x2014 || cp == 0x2212) emit('-');
    else if (cp == 0x2026) { emit('.'); emit('.'); emit('.'); }
    else if (int b = latin1Base(cp)) emit((char)b);
    // everything else (emoji, CJK, ...) is dropped
  }
  out[n] = 0;
  return n;
}

// Roots matched anywhere inside a word (after leetspeak normalizing).
static const char* const BAD_SUBSTR[] = {
    "fuck", "fuk", "fck", "cunt", "shit", "nigg", "niga", "fagg", "retard",
    "whore", "slut", "bitch", "kike", "chink", "tranny", "twat", "pussy",
    "asshole", "rape",
};
// Words matched only exactly (they're common inside innocent words).
static const char* const BAD_EXACT[] = {
    "fag", "fags", "spic", "spics", "cock", "cocks", "dick", "dicks", "cum",
    "tits", "ass", "nazi",
};

static char leet(char c) {
  switch (c) {
    case '0': return 'o';
    case '1': case '!': return 'i';
    case '3': return 'e';
    case '4': case '@': return 'a';
    case '5': case '$': return 's';
    case '7': return 't';
    default: return (c >= 'A' && c <= 'Z') ? c + 32 : c;
  }
}

static bool wordChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         c == '@' || c == '$' || c == '!' || c == '*';
}

bool maskProfanity(char* s) {
  bool any = false;
  for (char* p = s; *p;) {
    if (!wordChar(*p)) { p++; continue; }
    char* start = p;
    char norm[MAX_TEXT + 1];
    int n = 0;
    while (*p && wordChar(*p)) {
      norm[n++] = leet(*p);
      p++;
    }
    // Trailing '!' is punctuation, not a leet 'i' ("HEARD!").
    while (n > 0 && start[n - 1] == '!') n--;
    norm[n] = 0;
    bool bad = false;
    for (const char* w : BAD_SUBSTR) bad |= strstr(norm, w) != nullptr;
    for (const char* w : BAD_EXACT) bad |= strcmp(norm, w) == 0;
    if (bad) {
      any = true;
      for (int i = 1; i < n; i++) start[i] = '*';
    }
  }
  return any;
}

// ---------------------------------------------------------------------------
// Clock

static const char* const DEFAULT_PHRASES[] = {
    "EVERY SECOND COUNTS",
    "YES CHEF",
    "HEARD",
    "LET IT RIP",
    "BEHIND!",
    "CORNER!",
    "HANDS!",
    "WE'RE RUNNING OUT OF TIME",
    "COUSIN!",
    "WHEN THIS HITS ZERO WE'RE DONE",
};

static const RGB BLACK = {0, 0, 0};
static const RGB WHITE = {235, 235, 235};
static const RGB GUEST = {255, 255, 255};

BearClock::BearClock() {
  setPhrases(DEFAULT_PHRASES, sizeof(DEFAULT_PHRASES) / sizeof(DEFAULT_PHRASES[0]));
  setDuration(settings.durationSec);
  cur_.text[0] = 0;
  takeText_[0] = 0;
}

void BearClock::setDuration(uint32_t sec) {
  settings.durationSec = sec > MAX_SEC ? MAX_SEC : sec;
  running_ = false;
  remAtAnchor_ = (uint64_t)(sec > MAX_SEC ? MAX_SEC : sec) * 1000u;
}

void BearClock::setRemaining(uint32_t sec, uint32_t nowMs) {
  anchorMs_ = nowMs;
  remAtAnchor_ = (uint64_t)(sec > MAX_SEC ? MAX_SEC : sec) * 1000u;
}

uint64_t BearClock::remainingMs(uint32_t nowMs) const {
  if (!running_) return remAtAnchor_;
  uint32_t el = nowMs - anchorMs_;
  return el >= remAtAnchor_ ? 0 : remAtAnchor_ - el;
}

void BearClock::start(uint32_t nowMs) {
  if (running_) return;
  if (remAtAnchor_ == 0) remAtAnchor_ = (uint64_t)settings.durationSec * 1000u;  // restart
  anchorMs_ = nowMs;
  running_ = true;
}

void BearClock::pause(uint32_t nowMs) {
  if (!running_) return;
  remAtAnchor_ = remainingMs(nowMs);
  running_ = false;
}

void BearClock::reset(uint32_t nowMs) {
  (void)nowMs;
  running_ = false;
  remAtAnchor_ = (uint64_t)settings.durationSec * 1000u;
}

void BearClock::addSeconds(int32_t s, uint32_t nowMs) {
  int64_t rem = (int64_t)remainingMs(nowMs) + (int64_t)s * 1000;
  if (rem < 0) rem = 0;
  if (rem > (int64_t)MAX_SEC * 1000) rem = (int64_t)MAX_SEC * 1000;
  anchorMs_ = nowMs;
  remAtAnchor_ = (uint64_t)rem;
}

void BearClock::setPhrases(const char* const* list, int n) {
  nPhrases_ = 0;
  for (int i = 0; i < n && nPhrases_ < MAX_PHRASES; i++) {
    if (sanitize(list[i], phrases_[nPhrases_]) > 0) nPhrases_++;
  }
  nextPhrase_ = nextFlashPhrase_ = 0;
}

bool BearClock::pushGuest(const char* raw, uint32_t nowMs) {
  if (!settings.guestMessages || qCount_ >= MAX_QUEUE) return false;
  Message& m = queue_[(qHead_ + qCount_) % MAX_QUEUE];
  if (sanitize(raw, m.text) == 0) return false;
  if (settings.filter) maskProfanity(m.text);
  m.kind = Kind::Guest;
  qCount_++;
  // Interrupt a phrase so guests see their message right away.
  if (!haveCur_ || cur_.kind != Kind::Guest) {
    startTakeover("NEW TICKET", nowMs, 1300);
    haveCur_ = false;
  }
  return true;
}

void BearClock::flashNow(const char* raw, uint32_t nowMs) {
  char t[MAX_TEXT + 1];
  if (sanitize(raw, t) == 0) return;
  startTakeover(t, nowMs, 4000);
}

void BearClock::skip(uint32_t nowMs) {
  takeover_ = false;
  nextTicker(nowMs);
}

void BearClock::clearQueue(uint32_t nowMs) {
  qCount_ = 0;
  if (haveCur_ && cur_.kind == Kind::Guest) nextTicker(nowMs);
}

void BearClock::startTakeover(const char* text, uint32_t nowMs, uint32_t lenMs) {
  strncpy(takeText_, text, MAX_TEXT);
  takeText_[MAX_TEXT] = 0;
  takeover_ = true;
  takeStartMs_ = nowMs;
  takeLenMs_ = lenMs;
  lastFlashMs_ = nowMs;
}

// --- ticker geometry
static const int TICKER_Y = 23;
static const int SCROLL_PX_PER_S = 30;
static const uint32_t HOLD_MS = 3500;

static uint32_t passMs(const char* t) {
  int tw = textWidth(t);
  if (tw <= W) return HOLD_MS;
  return (uint32_t)(W + tw) * 1000u / SCROLL_PX_PER_S;
}

void BearClock::nextTicker(uint32_t nowMs) {
  curStartMs_ = nowMs;
  if (qCount_ > 0) {
    cur_ = queue_[qHead_];
    qHead_ = (qHead_ + 1) % MAX_QUEUE;
    qCount_--;
    curPasses_ = settings.guestRepeats ? settings.guestRepeats : 1;
  } else if (nPhrases_ > 0) {
    nextPhrase_ %= nPhrases_;
    strcpy(cur_.text, phrases_[nextPhrase_++]);
    cur_.kind = Kind::Phrase;
    curPasses_ = 1;
  } else {
    cur_.text[0] = 0;
    cur_.kind = Kind::Phrase;
    curPasses_ = 1;
  }
  haveCur_ = true;
}

bool BearClock::tickerDone(uint32_t nowMs) const {
  return nowMs - curStartMs_ >= passMs(cur_.text) * (uint32_t)curPasses_;
}

// --- takeover word wrap
struct Wrapped {
  char lines[4][11];
  int n = 0, scale = 1;
};

static bool wrap(const char* t, int maxChars, int maxLines, Wrapped& w) {
  w.n = 0;
  char line[MAX_TEXT + 1] = {0};
  int len = 0;
  auto flush = [&]() -> bool {
    if (len == 0) return true;
    if (w.n >= maxLines) return false;
    memcpy(w.lines[w.n], line, len);
    w.lines[w.n][len] = 0;
    w.n++;
    len = 0;
    return true;
  };
  while (*t) {
    while (*t == ' ') t++;
    const char* ws = t;
    while (*t && *t != ' ') t++;
    int wl = (int)(t - ws);
    if (wl == 0) break;
    if (wl > maxChars) return false;
    if (len && len + 1 + wl > maxChars && !flush()) return false;
    if (len) line[len++] = ' ';
    memcpy(line + len, ws, wl);
    len += wl;
  }
  return flush() && w.n > 0;
}

static bool layoutTakeover(const char* t, Wrapped& w) {
  if (wrap(t, 5, 2, w)) { w.scale = 2; return true; }
  if (wrap(t, 10, 4, w)) { w.scale = 1; return true; }
  return false;
}

// --- render

void BearClock::render(uint32_t nowMs, Frame& f) {
  f.clear();
  uint64_t rem = remainingMs(nowMs);

  if (!haveCur_ || tickerDone(nowMs)) nextTicker(nowMs);

  if (takeover_ && nowMs - takeStartMs_ >= takeLenMs_) takeover_ = false;

  // Periodic phrase flash, only when no guest message is up or waiting.
  if (!takeover_ && settings.flashEverySec && nPhrases_ && rem > 0 &&
      cur_.kind != Kind::Guest && qCount_ == 0 &&
      nowMs - lastFlashMs_ >= settings.flashEverySec * 1000u) {
    Wrapped w;
    for (int tries = 0; tries < nPhrases_; tries++) {
      const char* p = phrases_[nextFlashPhrase_++ % nPhrases_];
      if (layoutTakeover(p, w)) { startTakeover(p, nowMs, 2600); break; }
    }
    lastFlashMs_ = nowMs;  // even if nothing fit, wait another period
  }

  if (takeover_) return drawTakeover(nowMs, f);
  if (rem == 0 && running_) return drawZero(nowMs, f);
  drawClock(nowMs, f);
  drawTicker(nowMs, f);
}

void BearClock::drawClock(uint32_t nowMs, Frame& f) {
  uint64_t rem = remainingMs(nowMs);
  uint32_t secs = (uint32_t)((rem + 999) / 1000);  // ceil: shows 3:00:00 until 1s elapsed
  uint32_t h = secs / 3600, m = (secs / 60) % 60, s = secs % 60;

  RGB on = settings.color;
  if (running_ && secs > 0 && secs <= 60) {
    // Final minute: pulse brightness once per second.
    int phase = (int)(rem % 1000);
    on = scale(on, 600 + phase * 400 / 1000, 1000);
  }
  bool started = rem != (uint64_t)settings.durationSec * 1000u;
  if (!running_ && started && (nowMs / 600) % 2) on = scale(on, 1, 4);  // paused
  RGB ghost = settings.ghostSegments ? scale(settings.color, 1, 14) : BLACK;

  // Groups of digits: hours (2+ digits), minutes, seconds.
  int digits[3][4];
  int counts[3] = {0, 2, 2};
  uint32_t hh = h;
  do { digits[0][counts[0]++] = hh % 10; hh /= 10; } while (hh && counts[0] < 4);
  if (counts[0] < 2) digits[0][counts[0]++] = 0;
  // reverse hours (they were collected least-significant first)
  for (int i = 0; i < counts[0] / 2; i++) {
    int t = digits[0][i];
    digits[0][i] = digits[0][counts[0] - 1 - i];
    digits[0][counts[0] - 1 - i] = t;
  }
  digits[1][0] = m / 10; digits[1][1] = m % 10;
  digits[2][0] = s / 10; digits[2][1] = s % 10;

  int nd = counts[0] + 4;
  // Try roomy spacing first, then tight.
  int gap = 2, cpad = 2;
  auto width = [&](int g, int c) { return nd * 7 + (nd - 3) * g + 2 * (2 + 2 * c); };
  if (width(gap, cpad) > W) { gap = 1; cpad = 1; }
  if (width(gap, cpad) > W) {
    // 1000+ hours: fall back to small text.
    char buf[16];
    int n = 0;
    for (int i = 0; i < counts[0]; i++) buf[n++] = '0' + digits[0][i];
    buf[n++] = ':'; buf[n++] = '0' + digits[1][0]; buf[n++] = '0' + digits[1][1];
    buf[n++] = ':'; buf[n++] = '0' + digits[2][0]; buf[n++] = '0' + digits[2][1];
    buf[n] = 0;
    drawText(f, (W - textWidth(buf)) / 2, 5, buf, on);
  } else {
    const int y = 2;
    int x = (W - width(gap, cpad)) / 2;
    for (int g = 0; g < 3; g++) {
      for (int i = 0; i < counts[g]; i++) {
        drawDigit(f, x, y, digits[g][i], on, ghost);
        x += 7 + (i + 1 < counts[g] ? gap : 0);
      }
      if (g < 2) {
        x += cpad;
        bool colonOn = running_ || !started || (nowMs / 600) % 2 == 0;
        RGB cc = colonOn ? on : ghost;
        f.fillRect(x, y + 3, 2, 2, cc);
        f.fillRect(x, y + 9, 2, 2, cc);
        x += 2 + cpad;
      }
    }
  }

  // Progress bar: time left out of the full duration.
  const int bx = 2, bw = W - 4, by = 18;
  uint64_t dur = (uint64_t)settings.durationSec * 1000u;
  int fill = dur ? (int)((uint64_t)(rem > dur ? dur : rem) * bw / dur) : 0;
  f.fillRect(bx, by, bw, 1, scale(settings.color, 1, 10));
  f.fillRect(bx, by, fill, 1, scale(settings.color, 2, 3));
  if (fill > 0 && fill < bw) f.set(bx + fill - 1, by, WHITE);
}

void BearClock::drawTicker(uint32_t nowMs, Frame& f) {
  const char* t = cur_.text;
  if (!*t) return;
  RGB c = cur_.kind == Kind::Guest ? GUEST : settings.textColor;
  int tw = textWidth(t);
  uint32_t el = nowMs - curStartMs_;
  if (tw <= W) {
    drawText(f, (W - tw) / 2, TICKER_Y, t, c);
  } else {
    uint32_t pm = passMs(t);
    uint32_t inPass = el % pm;
    int x = W - (int)(inPass * SCROLL_PX_PER_S / 1000);
    drawText(f, x, TICKER_Y, t, c);
  }
}

void BearClock::drawTakeover(uint32_t nowMs, Frame& f) {
  Wrapped w;
  bool fits = layoutTakeover(takeText_, w);
  uint32_t el = nowMs - takeStartMs_;
  // Strobe inverted for the first second, then hold steady.
  bool inverted = el < 1000 && (el / 125) % 2 == 0;
  RGB bg = inverted ? scale(settings.color, 1, 2) : BLACK;  // half: limits peak current
  RGB fg = inverted ? BLACK : WHITE;
  f.clear(bg);
  if (!fits) {
    // Too long for a full-screen card: big scroll across the middle.
    int tw = textWidth(takeText_, 2);
    int x = W - (int)(el * 45 / 1000) % (W + tw);
    drawText(f, x, 9, takeText_, fg, 2);
    return;
  }
  int lh = 8 * w.scale;
  int total = w.n * lh - w.scale;
  int y = (H - total) / 2;
  for (int i = 0; i < w.n; i++, y += lh)
    drawText(f, (W - textWidth(w.lines[i], w.scale)) / 2, y, w.lines[i], fg, w.scale);
}

void BearClock::drawZero(uint32_t nowMs, Frame& f) {
  // Alternate blinking 00:00:00 with a full-screen card.
  uint32_t ph = nowMs % 4000;
  if (ph < 2500) {
    if ((ph / 400) % 2 == 0) drawClock(nowMs, f);
    drawText(f, (W - textWidth(settings.zeroText)) / 2, TICKER_Y, settings.zeroText, WHITE);
  } else {
    bool inv = (ph / 150) % 2 == 0;
    f.clear(inv ? scale(settings.color, 1, 2) : BLACK);
    Wrapped w;
    if (layoutTakeover(settings.zeroText, w)) {
      int lh = 8 * w.scale, total = w.n * lh - w.scale, y = (H - total) / 2;
      for (int i = 0; i < w.n; i++, y += lh)
        drawText(f, (W - textWidth(w.lines[i], w.scale)) / 2, y, w.lines[i],
                 inv ? BLACK : WHITE, w.scale);
    }
  }
}

}  // namespace bear
