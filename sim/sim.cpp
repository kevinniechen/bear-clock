// Native simulator + tests for bearcore. Build/run via sim/run.sh.
//   ./sim test            run logic tests
//   ./sim frames out.rgb  write a scripted demo as raw 64x32 RGB frames @20fps
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "bearcore.h"

using namespace bear;

static int failures = 0;
#define CHECK(cond)                                                  \
  do {                                                               \
    if (!(cond)) {                                                   \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                    \
    }                                                                \
  } while (0)

static void checkSanitize(const char* in, const char* want) {
  char out[MAX_TEXT + 1];
  sanitize(in, out);
  if (strcmp(out, want)) {
    fprintf(stderr, "FAIL sanitize(\"%s\") = \"%s\", want \"%s\"\n", in, out, want);
    failures++;
  }
}

static void checkMask(const char* in, const char* want) {
  char s[MAX_TEXT + 1];
  sanitize(in, s);
  maskProfanity(s);
  if (strcmp(s, want)) {
    fprintf(stderr, "FAIL mask(\"%s\") = \"%s\", want \"%s\"\n", in, s, want);
    failures++;
  }
}

static int litPixels(const Frame& f) {
  int n = 0;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) n += f.px[y][x] != RGB{0, 0, 0};
  return n;
}

static int runTests() {
  // sanitize
  checkSanitize("  hello   world  ", "hello world");
  checkSanitize("Yes, Chef\xE2\x80\x99s \xF0\x9F\x94\xA5 fire", "Yes, Chef's fire");
  checkSanitize("caf\xC3\xA9 \xE2\x80\x94 ok\xE2\x80\xA6", "cafe - ok...");
  checkSanitize("\xF0\x9F\x90\xBB", "");
  checkSanitize("a\nb\tc", "a b c");
  checkSanitize("\xE2\x80", "");  // truncated sequence
  {
    char longIn[400], out[MAX_TEXT + 1];
    memset(longIn, 'x', 399);
    longIn[399] = 0;
    CHECK(sanitize(longIn, out) == (size_t)MAX_TEXT);
    CHECK(strlen(out) == (size_t)MAX_TEXT);
  }

  // profanity
  checkMask("what the fuck", "what the f***");
  checkMask("SH1T happens", "S*** happens");
  checkMask("cocktail hour", "cocktail hour");
  checkMask("spicy food", "spicy food");
  checkMask("HEARD!", "HEARD!");
  checkMask("Assume nothing, class", "Assume nothing, class");
  checkMask("dick", "d***");

  // timer
  BearClock c;
  c.setDuration(3 * 3600);
  CHECK(c.remainingMs(0) == 3 * 3600 * 1000u);
  c.start(1000);
  CHECK(c.remainingMs(2000) == 3 * 3600 * 1000u - 1000);
  c.pause(5000);
  CHECK(c.remainingMs(99999) == 3 * 3600 * 1000u - 4000);
  c.start(100000);
  CHECK(c.remainingMs(101000) == 3 * 3600 * 1000u - 5000);
  c.addSeconds(-5 * 3600, 101000);
  CHECK(c.remainingMs(101000) == 0);
  c.addSeconds(60, 200000);
  CHECK(c.remainingMs(230000) == 30000);
  CHECK(c.remainingMs(300000) == 0);
  c.reset(0);
  CHECK(!c.running() && c.remainingMs(0) == 3 * 3600 * 1000u);
  // millis() wraparound
  c.start(0xFFFFF000u);
  CHECK(c.remainingMs(0x00001000u) == 3 * 3600 * 1000u - 0x2000);

  // Restored at zero after a reboot: stays at zero instead of restarting.
  BearClock z;
  z.setDuration(60);
  z.setRemaining(0, 0);
  z.resume(5000);
  CHECK(z.running() && z.remainingMs(9000) == 0);

  // guest queue
  BearClock q;
  q.settings.flashEverySec = 0;
  Frame f;
  q.render(0, f);
  for (int i = 0; i < MAX_QUEUE; i++) CHECK(q.pushGuest("hi", 10));
  CHECK(!q.pushGuest("overflow", 10));
  CHECK(!q.pushGuest("\xF0\x9F\x90\xBB", 10));  // empty after sanitize
  q.clearQueue(10);
  CHECK(q.queueLength() == 0);
  q.settings.guestMessages = false;
  CHECK(!q.pushGuest("off", 10));

  // Guest message becomes current after the NEW TICKET card.
  BearClock g;
  g.settings.flashEverySec = 0;
  g.render(0, f);
  g.pushGuest("hello bear", 100);
  g.render(200, f);
  g.render(2000, f);
  CHECK(strcmp(g.currentText(), "hello bear") == 0);

  // Every render path lights something and never crashes, across long runs.
  BearClock r;
  r.settings.flashEverySec = 5;
  r.setDuration(90);
  r.start(0);
  for (uint32_t t = 0; t < 200000; t += 37) {
    if (t == 3000) r.pushGuest("a much longer guest message that has to scroll by", t);
    if (t == 9000) r.flashNow("LET IT RIP", t);
    if (t == 11000) r.flashNow("this flash text is far too long to fit on a card", t);
    r.render(t, f);
  }
  BearClock big;
  big.setDuration(1440 * 3600);  // the show's 1440 hours (overflowed 32-bit ms)
  CHECK(big.remainingMs(0) == 1440ull * 3600 * 1000);
  big.start(0);
  CHECK(big.remainingMs(1000) == 1440ull * 3600 * 1000 - 1000);
  big.addSeconds(3600, 1000);
  CHECK(big.remainingMs(1000) == 1441ull * 3600 * 1000 - 1000);
  big.setDuration(20000 * 3600);  // clamped
  CHECK(big.remainingMs(0) == (uint64_t)MAX_SEC * 1000);
  big.setDuration(1440 * 3600);
  big.render(0, f);
  CHECK(litPixels(f) > 50);
  big.setDuration(5000 * 3600);  // 4-digit hours -> small-text fallback
  big.render(0, f);
  CHECK(litPixels(f) > 20);

  if (failures) fprintf(stderr, "%d failure(s)\n", failures);
  else printf("all tests passed\n");
  return failures ? 1 : 0;
}

static int writeFrames(const char* path) {
  FILE* out = fopen(path, "wb");
  if (!out) return 1;
  BearClock c;
  c.settings.flashEverySec = 8;
  c.setDuration(3 * 3600);
  Frame f;
  const uint32_t step = 50;  // 20 fps
  auto run = [&](uint32_t from, uint32_t to) {
    for (uint32_t t = from; t < to; t += step) {
      if (t == 1000) c.start(t);
      if (t == 12000) c.pushGuest("Happy Halloween Cousin! \xF0\x9F\x8E\x83", t);
      if (t == 24000) c.setRemaining(65, t);  // jump to the final minute
      if (t == 30000) c.setRemaining(3, t);   // and to zero
      c.render(t, f);
      fwrite(f.px, sizeof(f.px), 1, out);
    }
  };
  run(0, 42000);
  fclose(out);
  return 0;
}

int main(int argc, char** argv) {
  if (argc >= 2 && !strcmp(argv[1], "test")) return runTests();
  if (argc >= 3 && !strcmp(argv[1], "frames")) return writeFrames(argv[2]);
  fprintf(stderr, "usage: sim test | sim frames out.rgb\n");
  return 2;
}
