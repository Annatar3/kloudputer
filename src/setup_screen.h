// Kloudputer — the Cardputer's side of phone setup: a QR code to join its Wi-Fi, then one that opens
// the setup page, each with the same thing spelled out for typing.
#pragma once
#include <deque>
#include <vector>
#include "ui.h"

namespace ui {
class SetupScreen {
 public:
  explicit SetupScreen(Header& header) : header_(header) {}
  void begin(Gfx& d) { d_ = &d; }

  void busy(const String& text) {
    clear();
    cloud(*d_, 6, 56, 36, 26, 24, Cloud::Filled, ORANGE, BG);
    line(text, 46, 66, SOFT);
  }

  // Step 1: the camera joins from the standard Wi-Fi QR; the name and password are there for typing.
  void join(const String& qr, const String& ssid, const String& pass) {
    clear();
    auto& d = *d_;
    code(qr);
    heading("1", "Join its Wi-Fi");
    line("Point your camera", kTextX, 38, SOFT);
    line("at the code, or", kTextX, 48, SOFT);
    line("join by hand:", kTextX, 58, SOFT);
    line("Wi-Fi", kTextX, 74, DIM);
    line(ssid, kTextX, 84, TEXT);
    line("Password", kTextX, 98, DIM);
    d.setFont(&fonts::FreeMonoBold9pt7b);
    d.setTextColor(ORANGE, BG);
    d.drawString(pass.c_str(), kTextX - 1, 107);
    hint();
  }

  // Step 2 only: what the web server has seen, so a photo of the screen says where setup got stuck.
  void status(const String& summary, const String& detail) {
    d_->fillRect(kTextX, kStatusY, W - kTextX, 19, BG);
    line(summary.substring(0, (W - kTextX) / CHAR_W), kTextX, kStatusY, DIM);
    line(detail.substring(0, (W - kTextX) / CHAR_W), kTextX, kStatusY + 10, DIM);
  }

  // Step 2: a phone is on; most open the page by themselves, the QR and address cover the rest.
  void open(const String& url, const String& host) {
    clear();
    auto& d = *d_;
    code(url);
    heading("2", "Open setup");
    line("It opens by itself", kTextX, 38, SOFT);
    line("on most phones.", kTextX, 48, SOFT);
    line("If not, scan this", kTextX, 64, SOFT);
    line("code or go to", kTextX, 74, SOFT);
    d.setFont(&fonts::DejaVu12);
    d.setTextColor(ORANGE, BG);
    d.drawString(host.c_str(), kTextX, 86);
    hint();
  }

  // The setup log (L): newest event at the bottom, long ones wrapped under their timestamp. Rows are
  // padded and drawn opaque, so a new event redraws without a flash.
  void log(const std::deque<String>& events) {
    if (!logShown_) {
      header_.show("Setup log");
      d_->fillRect(0, HEAD_H, W, H - HEAD_H, BG);
      logShown_ = true;
    }
    constexpr int kCols = W / CHAR_W, kIndent = 5, kRows = (H - kLogY) / 10;
    std::vector<std::pair<String, uint16_t>> rows;
    for (const String& e : events) {
      const String body = e.substring(kIndent);
      const uint16_t color = body.startsWith("dns ") ? DIM : body.startsWith("join") || body.startsWith("leave") ? SYS : TEXT;
      rows.push_back({e.substring(0, kCols), color});
      for (size_t at = kCols; at < e.length(); at += kCols - kIndent) {
        rows.push_back({"     " + e.substring(at, at + kCols - kIndent), color});
      }
    }
    const int first = std::max(0, static_cast<int>(rows.size()) - kRows);
    for (int r = 0; r < kRows; ++r) {
      const int i = first + r;
      const bool has = i < static_cast<int>(rows.size());
      d_->setFont(&fonts::Font0);
      d_->setTextColor(has ? rows[i].second : BG, BG);
      d_->drawString(fit(has ? rows[i].first : String(), kCols).c_str(), 0, kLogY + r * 10);
    }
  }

 private:
  static constexpr int kLogY = HEAD_H + 3;
  static constexpr int kQrX = 4, kQrY = HEAD_H + 5, kQrW = 111;
  static constexpr int kTextX = kQrX + kQrW + 9;
  static constexpr int kStatusY = 101;

  void clear() {
    logShown_ = false;
    header_.show("Setup from phone");
    d_->fillRect(0, HEAD_H, W, H - HEAD_H, BG);
    d_->setTextDatum(lgfx::textdatum_t::top_left);
  }

  // Dark modules on white with a 4-module quiet zone, the way phone cameras read them best.
  void code(const String& text) { d_->qrcode(text.c_str(), kQrX, kQrY, kQrW, 1, true); }

  void heading(const char* step, const char* title) {
    auto& d = *d_;
    d.setFont(&fonts::DejaVu12);
    d.setTextColor(ORANGE, BG);
    const int w = d.drawString(step, kTextX, kQrY);
    d.setTextColor(TEXT, BG);
    d.drawString(title, kTextX + w + 5, kQrY);
  }

  void line(const String& s, int x, int y, uint16_t color) {
    d_->setFont(&fonts::Font0);
    d_->setTextColor(color, BG);
    d_->drawString(s.c_str(), x, y);
  }

  void hint() { line("Del cancels  L log", kTextX, H - 11, DIM); }

  Header& header_;
  Gfx* d_ = nullptr;
  bool logShown_ = false;
};
}  // namespace ui
