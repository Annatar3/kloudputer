// Kloudputer — a scrolling list menu under the shared header: label left, value right, a footer hint.
#pragma once
#include <vector>
#include "ui.h"

namespace ui {
struct MenuItem {
  String label;
  String value;
  String hint;  // footer while this row is selected; the menu's own hint when empty
};

class Menu {
 public:
  explicit Menu(Header& header) : header_(header) {}
  void begin(Gfx& d) { d_ = &d; }

  void open(const char* title, std::vector<MenuItem> items, int selected, const char* hint) {
    items_ = std::move(items);
    hint_ = hint;
    notice_ = "";
    sel_ = std::min(std::max(selected, 0), static_cast<int>(items_.size()) - 1);
    top_ = std::max(0, std::min(sel_ - kVisible / 2, static_cast<int>(items_.size()) - kVisible));
    header_.show(title);
    auto& d = *d_;
    d.fillRect(0, HEAD_H, W, kTop - HEAD_H, BG);
    d.fillRect(0, kTop + kVisible * kRowH, W, kFooterY - kTop - kVisible * kRowH, BG);
    d.fillRect(0, kFooterY, W, H - kFooterY, BAR);
    drawRows();
    drawFooter();
  }

  // Moves the selection, wrapping at either end.
  void move(int delta) {
    const int n = items_.size();
    if (n == 0) return;
    const int old = sel_, oldTop = top_;
    sel_ = ((sel_ + delta) % n + n) % n;
    if (sel_ < top_) top_ = sel_;
    if (sel_ >= top_ + kVisible) top_ = sel_ - kVisible + 1;
    notice_ = "";
    if (top_ != oldTop) {
      drawRows();
    } else {
      drawRow(old);
      drawRow(sel_);
    }
    drawFooter();
  }

  void setValue(int row, const String& value) {
    items_[row].value = value;
    drawRow(row);
  }

  // A message in the footer (what an action just did) until the selection moves.
  void notice(const String& text, uint16_t color) {
    notice_ = text;
    noticeColor_ = color;
    drawFooter();
  }

  int selected() const { return sel_; }

 private:
  static constexpr int kTop = HEAD_H + 3, kRowH = 15, kFooterY = H - 11;
  static constexpr int kVisible = (kFooterY - kTop) / kRowH;
  static constexpr int kRowEnd = W - 3;  // the scrollbar column starts here

  void drawRows() {
    for (int r = 0; r < kVisible; ++r) drawRow(top_ + r);
    drawScrollbar();
  }

  void drawRow(int i) {
    const int y = kTop + (i - top_) * kRowH;
    if (i < top_ || i >= top_ + kVisible) return;
    auto& d = *d_;
    if (i >= static_cast<int>(items_.size())) {
      d.fillRect(0, y, kRowEnd, kRowH, BG);
      return;
    }
    const MenuItem& it = items_[i];
    const bool on = i == sel_;
    const uint16_t bg = on ? SEL : BG;
    if (on) {
      // Background outside the rounded highlight: margin, the gap under it, and its corner squares.
      constexpr int kX = 3, kR = 3, kW = kRowEnd - kX, kH = kRowH - 1;
      d.fillRect(0, y, kX, kRowH, BG);
      d.fillRect(kX, y + kH, kW, 1, BG);
      for (int cx : {kX, kRowEnd - kR})
        for (int cy : {y, y + kH - kR}) d.fillRect(cx, cy, kR, kR, BG);
      d.fillRoundRect(kX, y, kW, kH, kR, SEL);
      d.fillRect(3, y + 2, 2, kRowH - 5, ORANGE);
    } else {
      d.fillRect(0, y, kRowEnd, kRowH, BG);
    }
    constexpr int kLabelX = 11, kGap = 8, kMinLabel = 90;
    const int midY = y + kRowH / 2, valueEnd = kRowEnd - 5;
    d.setFont(&fonts::DejaVu12);
    // A long value (an SSID) gives way first, down to leaving the label kMinLabel px; then a long label
    // (a session title) is cut, so short values like dates always show whole.
    String label = it.label;
    const int labelW = d.textWidth(label.c_str());
    const int valueRoom = (valueEnd - kLabelX - std::min(labelW, kMinLabel) - kGap) / CHAR_W;
    String value = it.value;
    if (static_cast<int>(value.length()) > valueRoom) value = value.substring(0, std::max(valueRoom - 2, 0)) + "..";
    const int labelRoom = valueEnd - static_cast<int>(value.length()) * CHAR_W - kGap - kLabelX;
    if (labelW > labelRoom) {
      while (label.length() && d.textWidth((label + "..").c_str()) > labelRoom) label.remove(label.length() - 1, 1);
      label.trim();
      label += "..";
    }
    d.setTextDatum(lgfx::textdatum_t::middle_left);
    d.setTextColor(on ? ORANGE : TEXT, bg);
    d.drawString(label.c_str(), kLabelX, midY);
    d.setFont(&fonts::Font0);
    d.setTextDatum(lgfx::textdatum_t::middle_right);
    d.setTextColor(on ? TEXT : DIM, bg);
    d.drawString(value.c_str(), valueEnd, midY);
    d.setTextDatum(lgfx::textdatum_t::top_left);
  }

  void drawScrollbar() {
    auto& d = *d_;
    const int n = items_.size(), h = kVisible * kRowH, x = kRowEnd;
    d.fillRect(x, kTop, W - x, h, BG);
    if (n <= kVisible) return;
    const int thumbH = h * kVisible / n;
    d.fillRect(x + 1, kTop + h * top_ / n, 2, thumbH, DIM);
  }

  void drawFooter() {
    auto& d = *d_;
    const bool hasNotice = notice_.length() > 0;
    const bool own = sel_ < static_cast<int>(items_.size()) && items_[sel_].hint.length();
    String text = hasNotice ? notice_ : own ? items_[sel_].hint : String(hint_);
    constexpr int kX = 4, kCols = (W - 2 * kX) / CHAR_W;
    if (static_cast<int>(text.length()) > kCols) text = text.substring(0, kCols - 2) + "..";
    d.setTextDatum(lgfx::textdatum_t::top_left);
    d.setFont(&fonts::Font0);
    d.setTextColor(hasNotice ? noticeColor_ : DIM, BAR);
    d.drawString(fit(text, kCols).c_str(), kX, kFooterY + 2);
  }

  Header& header_;
  Gfx* d_ = nullptr;
  std::vector<MenuItem> items_;
  const char* hint_ = "";
  String notice_;
  uint16_t noticeColor_ = DIM;
  int sel_ = 0;
  int top_ = 0;
};
}  // namespace ui
