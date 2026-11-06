// Kloudputer — interrupt-free TCA8418 keyboard reader for the Cardputer ADV.
//
// M5Cardputer's stock TCA8418KeyboardReader only reads the chip when its GPIO11 interrupt fires,
// and is only selected when M5GFX autodetects the board as ADV. This reader polls the chip's event
// FIFO instead, so typing works even if detection or the interrupt line misbehaves.
#pragma once
#include <M5Cardputer.h>
#include <algorithm>
#include <memory>
#include "utility/Adafruit_TCA8418/Adafruit_TCA8418.h"
#include "utility/Keyboard/KeyboardReader/KeyboardReader.h"

class PollingTCA8418Reader : public KeyboardReader {
 public:
  void begin() override {
    tca_ = std::make_unique<Adafruit_TCA8418>();  // 0x34 on M5's internal I2C bus
    ok_ = tca_->begin();
    if (!ok_) return;
    tca_->matrix(7, 8);
    tca_->flush();
  }

  // One event per call, like the stock reader: a press and its release land in separate
  // updates, so the keyboard layer sees the key go down before it comes back up.
  void update() override {
    if (!ok_ || tca_->available() == 0) return;
    handle(tca_->getEvent());
    ++events_;
  }

  bool ok() const { return ok_; }
  uint32_t events() const { return events_; }

 private:
  // Same decode + remap as M5Cardputer's TCA8418KeyboardReader, so coordinates match its key map.
  void handle(uint8_t raw) {
    const bool pressed = raw & 0x80;
    uint16_t code = raw & 0x7F;
    if (code == 0) return;
    code--;
    const int row = code / 10;
    const int col = code % 10;
    Point2D_t p;
    p.x = row * 2 + (col > 3 ? 1 : 0);
    p.y = (col + 4) % 4;
    auto it = std::find(_key_list.begin(), _key_list.end(), p);
    if (pressed) {
      if (it == _key_list.end()) _key_list.push_back(p);
    } else if (it != _key_list.end()) {
      _key_list.erase(it);
    }
  }

  std::unique_ptr<Adafruit_TCA8418> tca_;
  bool ok_ = false;
  uint32_t events_ = 0;
};
