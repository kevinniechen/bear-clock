// Platform-independent core of the Bear clock: countdown model, message queue,
// text sanitizing, and rendering into a 64x32 RGB framebuffer.
// Compiled both for the ESP32 (firmware) and natively (sim/) so visuals and
// logic can be tested without hardware. No Arduino headers allowed in here.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace bear {

constexpr int W = 64;
constexpr int H = 32;

struct RGB {
  uint8_t r, g, b;
  bool operator==(const RGB& o) const { return r == o.r && g == o.g && b == o.b; }
  bool operator!=(const RGB& o) const { return !(*this == o); }
};

RGB scale(RGB c, int num, int den);

struct Frame {
  RGB px[H][W];
  void clear(RGB c = {0, 0, 0});
  void set(int x, int y, RGB c) {
    if (x >= 0 && x < W && y >= 0 && y < H) px[y][x] = c;
  }
  void fillRect(int x, int y, int w, int h, RGB c);
};

constexpr int MAX_TEXT = 120;     // max chars of any displayed message
constexpr int MAX_QUEUE = 8;      // pending guest messages
constexpr int MAX_NAME = 16;      // guest name length
constexpr int MAX_HISTORY = 24;   // past guest messages kept for replay
constexpr int MAX_PHRASES = 12;   // rotating Bear phrases
constexpr uint32_t MAX_SEC = 9999u * 3600;  // longest settable countdown

struct Settings {
  uint32_t durationSec = 3 * 3600;  // what "reset" goes back to
  uint8_t brightness = 90;          // panel brightness 0-255
  RGB color = {30, 110, 255};       // main digit color (Bear blue)
  RGB textColor = {150, 210, 255};  // ticker text color
  bool guestMessages = true;        // accept messages from the QR page
  bool filter = true;               // drop hateful guest messages
  bool replay = true;               // mix past guest messages into the ticker
  uint16_t flashEverySec = 40;      // full-screen phrase flash period (0=off)
  bool ghostSegments = true;        // dim unlit 7-segment bars
  uint8_t guestRepeats = 2;         // times each guest message scrolls
  char zeroText[24] = "TIME'S UP";  // shown when the countdown hits zero
};

// Text helpers ---------------------------------------------------------------

// Converts arbitrary UTF-8 into printable ASCII (smart quotes/dashes mapped,
// emoji and other glyphs dropped), collapses whitespace, trims, and caps at
// MAX_TEXT chars. Returns length written to out (out must hold MAX_TEXT+1).
size_t sanitize(const char* in, char* out);
// Loose filter: true for slurs/hate (n-word, f-slur, ...). Ordinary swearing passes.
bool isHateful(const char* s);
int textWidth(const char* s, int scale = 1);  // pixels, 6px per char at scale 1

// Clock ----------------------------------------------------------------------

enum class Kind : uint8_t { Phrase, Guest, Replay };

struct Message {
  char text[MAX_TEXT + 1];
  char from[MAX_NAME + 1];  // "" = anonymous
  Kind kind;
};

class BearClock {
 public:
  Settings settings;

  BearClock();

  // Countdown. All times are a monotonically increasing ms clock (millis()).
  void setDuration(uint32_t sec);           // sets duration and resets timer
  void setRemaining(uint32_t sec, uint32_t nowMs);  // keeps running state
  void start(uint32_t nowMs);
  void pause(uint32_t nowMs);
  void toggle(uint32_t nowMs) { running_ ? pause(nowMs) : start(nowMs); }
  void reset(uint32_t nowMs);                // back to duration, paused
  void addSeconds(int32_t s, uint32_t nowMs);
  // 64-bit: 1193+ hour durations overflow 32-bit ms. (Elapsed time still uses
  // the 32-bit ms clock, so a single uninterrupted run must be < 49 days.)
  uint64_t remainingMs(uint32_t nowMs) const;
  // Marks the timer running without the start() "restart from zero" rule.
  void resume(uint32_t nowMs) { anchorMs_ = nowMs; running_ = true; }
  bool running() const { return running_; }

  // Messages.
  // false if dropped (guests off, queue full, empty, or hateful).
  bool pushGuest(const char* rawText, const char* rawFrom, uint32_t nowMs);
  void flashNow(const char* rawText, uint32_t nowMs);
  void skip(uint32_t nowMs);  // a skipped guest message is also dropped from replay
  void clearQueue(uint32_t nowMs);
  int queueLength() const { return qCount_; }

  // Replay history (newest last). historyVersion() changes on every edit so the
  // firmware knows when to persist it.
  int historyCount() const { return hCount_; }
  const Message& history(int i) const { return history_[(hHead_ + i) % MAX_HISTORY]; }
  void restoreHistory(const Message* msgs, int n);
  void clearHistory();
  uint32_t historyVersion() const { return histVer_; }

  // Phrases (rotating ticker + periodic flashes).
  void setPhrases(const char* const* list, int n);
  int phraseCount() const { return nPhrases_; }
  const char* phrase(int i) const { return phrases_[i]; }

  // Draws the full frame for time nowMs.
  void render(uint32_t nowMs, Frame& f);

  // For status reporting.
  const char* currentText() const { return cur_.text; }
  const char* currentFrom() const { return cur_.from; }

 private:
  // timer
  bool running_ = false;
  uint32_t anchorMs_ = 0;      // millis() when remAtAnchor_ was measured
  uint64_t remAtAnchor_ = 0;   // remaining ms at anchorMs_

  // ticker
  Message queue_[MAX_QUEUE];
  int qHead_ = 0, qCount_ = 0;
  Message cur_;                // what the ticker is showing
  bool haveCur_ = false;
  uint32_t curStartMs_ = 0;
  int curPasses_ = 0;          // passes left for cur_
  int nextPhrase_ = 0;
  bool replayTurn_ = false;    // idle ticker alternates phrase / replay
  int nextReplay_ = 0;

  Message history_[MAX_HISTORY];
  int hHead_ = 0, hCount_ = 0;
  uint32_t histVer_ = 0;
  void addHistory(const Message& m);
  void removeHistory(const Message& m);

  // takeover (full-screen flash)
  bool takeover_ = false;
  char takeText_[MAX_TEXT + 1];
  uint32_t takeStartMs_ = 0, takeLenMs_ = 0;
  uint32_t lastFlashMs_ = 0;
  int nextFlashPhrase_ = 0;

  char phrases_[MAX_PHRASES][MAX_TEXT + 1];
  int nPhrases_ = 0;

  void nextTicker(uint32_t nowMs);
  bool tickerDone(uint32_t nowMs) const;
  void startTakeover(const char* text, uint32_t nowMs, uint32_t lenMs);
  void drawClock(uint32_t nowMs, Frame& f);
  void drawTicker(uint32_t nowMs, Frame& f);
  void drawTakeover(uint32_t nowMs, Frame& f);
  void drawZero(uint32_t nowMs, Frame& f);
};

// Drawing primitives (exposed for the sim/tests).
void drawText(Frame& f, int x, int y, const char* s, RGB c, int scale = 1);
void drawDigit(Frame& f, int x, int y, int d, RGB on, RGB ghost);

}  // namespace bear
