// Kloudputer — telling speech from background by loudness, for talk mode. Fed the level of each 100 ms of
// microphone audio, it says when someone started talking and when they have stopped.
#pragma once
#include <algorithm>
#include <cstdint>

namespace vad {

struct Tuning {
  float minLevel = 60;        // speech is never assumed below this (a silent room reads under 20)
  float ratio = 3.0f;         // speech has to be this many times the background level
  float maxThreshold = 400;   // a loud room can't raise the bar out of a voice's reach
  float endRatio = 0.6f;      // below this fraction of the bar counts as quiet again
  int settleFrames = 3;       // the first 300 ms only measures the room
  int startFrames = 2;        // 200 ms of sound in a row starts it: a click or a knock doesn't
  int endFrames = 12;         // 1.2 s of quiet ends it: a breath between sentences doesn't
};

enum class Event : uint8_t { None, Started, Ended };

class Detector {
 public:
  explicit Detector(const Tuning& tuning = Tuning()) : t_(tuning) {}

  // One frame's level (RMS around its mean) in; whether this frame started or ended the speech.
  Event feed(float level) {
    ++frames_;
    if (speaking_) {
      if (level >= threshold() * t_.endRatio) {
        quiet_ = 0;
        return Event::None;
      }
      if (++quiet_ < t_.endFrames) return Event::None;
      speaking_ = false;
      return Event::Ended;
    }
    // The background level: the quietest of the settling frames, then falling fast and rising slowly, and
    // never learning from a frame loud enough to be speech.
    if (frames_ == 1) floor_ = level;
    else if (level < floor_) floor_ = frames_ <= t_.settleFrames ? level : (floor_ + level) / 2;
    else if (level < threshold()) floor_ += (level - floor_) * 0.01f;  // about 10 s to follow a louder room
    if (frames_ <= t_.settleFrames) return Event::None;
    if (level < threshold()) {
      loud_ = 0;
      return Event::None;
    }
    if (++loud_ < t_.startFrames) return Event::None;
    loud_ = 0;
    quiet_ = 0;
    speaking_ = true;
    return Event::Started;
  }

  bool speaking() const { return speaking_; }
  float threshold() const { return std::min(std::max(floor_ * t_.ratio, t_.minLevel), t_.maxThreshold); }
  float background() const { return floor_; }

 private:
  Tuning t_;
  float floor_ = 0;
  int frames_ = 0, loud_ = 0, quiet_ = 0;
  bool speaking_ = false;
};

}  // namespace vad
