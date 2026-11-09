// Kloudputer — screen dimming and sleep. After the chosen idle time the screen dims, after twice that it
// turns off. Any key brings it back; the key that wakes a screen that was off is not typed, so a blind
// press doesn't put a stray letter in the prompt.
#pragma once
#include <cstdint>

namespace power {

struct Option {
  uint32_t ms;  // 0 = never
  const char* name;
};

inline constexpr Option kOptions[] = {{0, "Never"}, {30000, "30 s"}, {60000, "1 min"}, {120000, "2 min"}, {300000, "5 min"}};
inline constexpr int kOptionCount = sizeof(kOptions) / sizeof(kOptions[0]);
inline constexpr int kDefaultOption = 2;

// A saved value that is no longer an option falls back to the default.
inline int optionFor(uint32_t ms) {
  for (int i = 0; i < kOptionCount; ++i) {
    if (kOptions[i].ms == ms) return i;
  }
  return kDefaultOption;
}

inline int stepOption(int index, int dir) { return ((index + dir) % kOptionCount + kOptionCount) % kOptionCount; }

enum class Level { Awake, Dim, Off };

struct Step {
  Level level;
  bool changed;  // the backlight needs to be set
  bool swallow;  // the key that did this must be ignored
};

class ScreenSleep {
 public:
  // Call often. `activity` is a key or button press this instant.
  Step tick(uint32_t now, uint32_t timeoutMs, bool activity) {
    const Level before = level_;
    bool swallow = false;
    if (activity) {
      swallow = before == Level::Off;
      last_ = now;
      level_ = Level::Awake;
    } else if (timeoutMs == 0) {
      level_ = Level::Awake;
    } else {
      const uint32_t idle = now - last_;  // wraps correctly when millis() does
      const Level due = idle >= 2 * timeoutMs ? Level::Off : idle >= timeoutMs ? Level::Dim : Level::Awake;
      level_ = due;
    }
    return {level_, level_ != before, swallow};
  }

  // Something happened on screen without a key (a reply arrived): wake and restart the clock.
  Step poke(uint32_t now, uint32_t timeoutMs) {
    const Step s = tick(now, timeoutMs, true);
    return {s.level, s.changed, false};
  }

  Level level() const { return level_; }

 private:
  Level level_ = Level::Awake;
  uint32_t last_ = 0;
};

}  // namespace power
