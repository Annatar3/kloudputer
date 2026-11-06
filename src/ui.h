// Kloudputer — the header bar and the chat view for the 240x135 screen.
// Flicker-free: every redraw writes each pixel straight to its final color (opaque text over its own
// background, fills only where the result is background), so nothing blanks before it is redrawn.
#pragma once
#include <cstring>
#include <deque>
#include "text.h"
#include "theme.h"

namespace ui {
constexpr size_t kMaxLines = 300;
constexpr int HEAD_H = 14;

struct Icons {
  bool speaker = false;  // spoken answers on
  int wifi = 0;          // signal bars 0-3
  int battery = -1;      // percent; negative when the board gives no reading
  bool operator==(const Icons& o) const { return speaker == o.speaker && wifi == o.wifi && battery == o.battery; }
};

// The top bar, shared by the chat and the menus. Whichever screen is up names it with show(); status
// changes from anywhere (speech progress, recording) then redraw it in place.
class Header {
 public:
  void begin(Gfx& d) { d_ = &d; }

  void show(const String& title) {
    const bool same = shown_ && title == title_;
    title_ = title;
    shown_ = true;
    if (!same) {
      d_->fillRect(0, 0, W, HEAD_H, BAR);
      cloud(*d_, 2, 0, 15, HEAD_H, 10.5f, Cloud::Filled, ORANGE, BAR, false);
      drawnIcons_ = {false, -1, -2};
    }
    draw();
  }

  // Right-aligned status: the model name, or a passing state like "REC 1.2s".
  void setNote(const String& text, uint16_t color = DIM) {
    note_ = text;
    noteColor_ = color;
    draw();
  }

  void setIcons(const Icons& icons) {
    icons_ = icons;
    draw();
  }

 private:
  static constexpr int kTextX = 19, kTextY = 4;

  void draw() {
    if (!shown_) return;
    auto& d = *d_;
    // Icons sit right to left; the text gets what's left of them.
    const int batX = icons_.battery >= 0 ? W - 15 : -1;
    const int wifiX = (batX >= 0 ? batX : W - 2) - 3 - 8;
    const int spkX = wifiX - 3 - 8;
    const int textEnd = spkX - 3;
    if (!(icons_ == drawnIcons_)) {
      d.fillRect(textEnd, kTextY - 1, W - textEnd, 9, BAR);
      if (batX >= 0) batteryIcon(d, batX, kTextY, icons_.battery);
      wifiIcon(d, wifiX, kTextY, icons_.wifi);
      if (icons_.speaker) speakerIcon(d, spkX, kTextY);
      drawnIcons_ = icons_;
    }

    const int cols = (textEnd - kTextX) / CHAR_W;
    String title = title_;
    if (static_cast<int>(title.length() + 1 + note_.length()) > cols) title = "";  // a long status wins
    d.setTextDatum(lgfx::textdatum_t::top_left);
    d.setFont(&fonts::Font0);
    d.setTextColor(TEXT, BAR);
    d.drawString(title.c_str(), kTextX, kTextY);
    const int restX = kTextX + title.length() * CHAR_W;
    const int restCols = cols - title.length();
    String note = note_.substring(0, restCols);
    while (static_cast<int>(note.length()) < restCols) note = String(" ") + note;
    d.setTextColor(noteColor_, BAR);
    d.drawString(note.c_str(), restX, kTextY);
    const int usedEnd = kTextX + cols * CHAR_W;
    d.fillRect(usedEnd, kTextY, textEnd - usedEnd, 8, BAR);
  }

  Gfx* d_ = nullptr;
  bool shown_ = false;
  String title_;
  String note_;
  uint16_t noteColor_ = DIM;
  Icons icons_;
  Icons drawnIcons_{false, -1, -2};
};

// The conversation: your lines carry an orange ">", the first line of each Claude reply a small mark,
// notes (status, errors, help) just their color. Older lines scroll back with Fn+; / Fn+.
class ChatView {
 public:
  explicit ChatView(Header& header) : header_(header) {}
  void begin(Gfx& d) { d_ = &d; }

  void show() {
    shown_ = true;
    header_.show(title_);
    d_->fillRect(0, HEAD_H, W, kTop - HEAD_H, BG);
    d_->drawFastHLine(0, kInputY, W, LINE);
    d_->fillRect(0, kInputY + 1, W, H - kInputY - 1, BAR);
    drawBody();
    drawInput();
  }

  // Stops drawing (a menu has the screen); messages still collect for the next show().
  void hide() { shown_ = false; }
  bool shown() const { return shown_; }

  void add(const String& text, uint16_t color) { append(text, color, Kind::Note); }
  void addUser(const String& text) { append(text, SOFT, Kind::User); }
  void addClaude(const String& text) { append(text, TEXT, Kind::Claude); }

  // A stand-in the next message replaces: "Thinking...", or an agent's latest step.
  void placeholder(const String& text, uint16_t color, bool mark) {
    endPlaceholder();
    placeholder_ = wrapInto(text, color, mark ? Kind::Claude : Kind::Note);
    scroll_ = 0;
    drawBody();
  }

  void thinking() { placeholder("Thinking...", ORANGE, true); }

  void setTitle(const String& title) { title_ = title; }

  void clear() {
    lines_.clear();
    placeholder_ = 0;
    scroll_ = 0;
    drawBody();
  }

  // delta > 0 scrolls back into history.
  void scroll(int delta) {
    scroll_ = std::min(std::max(scroll_ + delta, 0), maxScroll());
    drawBody();
  }

  void setInput(const String& s, const char* prompt = kPrompt, bool mask = false) {
    input_ = s;
    prompt_ = prompt;
    mask_ = mask;
    drawInput();
  }

  // Every message counts, so a menu can echo the newest one an action reported.
  uint32_t added() const { return added_; }
  const String& lastText() const { return last_; }
  uint16_t lastColor() const { return lastColor_; }

 private:
  static constexpr const char* kPrompt = "> ";
  static constexpr int kTop = HEAD_H + 4;  // first text row
  static constexpr int kInputY = H - 17;   // separator line above the input bar
  static constexpr int kLineH = 10, kGap = 3;
  static constexpr int kMarkW = 14;  // the ">" / reply mark column
  static constexpr int kCols = (W - kMarkW - 4) / CHAR_W;
  static constexpr int kTextEnd = kMarkW + kCols * CHAR_W;  // the scrollbar column starts here

  enum class Kind : uint8_t { Note, User, Claude };
  struct Line {
    String text;
    uint16_t color;
    Kind kind;
    bool first;  // first line of its message
  };

  void pushLine(const String& s, uint16_t color, Kind kind, bool first) {
    lines_.push_back({s, color, kind, first});
    while (lines_.size() > kMaxLines) lines_.pop_front();
  }

  void endPlaceholder() {
    for (; placeholder_ > 0 && !lines_.empty(); --placeholder_) lines_.pop_back();
    placeholder_ = 0;
  }

  void append(const String& raw, uint16_t color, Kind kind) {
    endPlaceholder();
    wrapInto(raw, color, kind);
    last_ = asciify(raw);
    lastColor_ = color;
    ++added_;
    scroll_ = 0;
    drawBody();
  }

  // Word-wraps one message into lines; returns how many.
  int wrapInto(const String& raw, uint16_t color, Kind kind) {
    const String text = asciify(raw);
    int count = 0;
    bool first = true;
    int start = 0;
    const int len = text.length();
    while (start <= len) {
      int nl = text.indexOf('\n', start);
      if (nl < 0) nl = len;
      String para = text.substring(start, nl);
      if (para.length() == 0) {
        pushLine("", color, kind, first);
        first = false;
        ++count;
      }
      while (para.length() > 0) {
        int cut = para.length();
        if (cut > kCols) {
          cut = kCols;
          const int sp = para.lastIndexOf(' ', kCols);
          if (sp > 0) cut = sp;
        }
        pushLine(para.substring(0, cut), color, kind, first);
        first = false;
        ++count;
        para = para.substring(cut);
        while (para.length() && para[0] == ' ') para.remove(0, 1);
      }
      if (nl >= len) break;
      start = nl + 1;
    }
    return count;
  }

  // A small gap separates turns; runs of notes (help, info) stay packed.
  bool gapBefore(int i) const {
    if (i == 0 || !lines_[i].first) return false;
    return !(lines_[i].kind == Kind::Note && lines_[i - 1].kind == Kind::Note);
  }

  // Scrolling stops once the oldest line reaches the top of a full body.
  int maxScroll() const {
    const int total = lines_.size();
    int used = 0, fit = 0;
    while (fit < total) {
      const int need = used + kLineH + (fit > 0 && gapBefore(fit) ? kGap : 0);
      if (need > kInputY - kTop) break;
      used = need;
      ++fit;
    }
    return total - fit;
  }

  void drawBody() {
    if (!shown_) return;
    auto& d = *d_;
    // Walk up from the newest visible line until the body is full; the top line drops its gap.
    const int end = static_cast<int>(lines_.size()) - scroll_;
    int start = end, used = 0;
    while (start > 0) {
      const int need = used + kLineH + (start < end && gapBefore(start) ? kGap : 0);
      if (need > kInputY - kTop) break;
      used = need;
      --start;
    }
    d.setTextDatum(lgfx::textdatum_t::top_left);
    d.setFont(&fonts::Font0);
    int y = kTop;
    for (int i = start; i < end; ++i) {
      if (i > start && gapBefore(i)) {
        d.fillRect(0, y, kTextEnd, kGap, BG);
        y += kGap;
      }
      drawLine(lines_[i], y);
      y += kLineH;
    }
    d.fillRect(0, y, kTextEnd, kInputY - y, BG);
    drawScrollbar(start, end);
  }

  void drawLine(const Line& l, int y) {
    auto& d = *d_;
    if (l.first && l.kind == Kind::User) {
      d.fillRect(0, y, 2, 8, BG);
      d.setTextColor(ORANGE, BG);
      d.drawString(">", 2, y);
      d.fillRect(2 + CHAR_W, y, kMarkW - 2 - CHAR_W, 8, BG);
    } else if (l.first && l.kind == Kind::Claude) {
      replyMark(d, 0, y, ORANGE, BG);
      d.fillRect(11, y, kMarkW - 11, 7, BG);
      d.fillRect(0, y + 7, kMarkW, 1, BG);
    } else {
      d.fillRect(0, y, kMarkW, 8, BG);
    }
    d.setTextColor(l.color, BG);
    d.drawString(fit(l.text, kCols).c_str(), kMarkW, y);
    d.fillRect(0, y + 8, kTextEnd, kLineH - 8, BG);
  }

  // Shown only while scrolled back, so it doubles as the "you're in history" sign.
  void drawScrollbar(int start, int end) {
    auto& d = *d_;
    const int x = kTextEnd, h = kInputY - kTop;
    const int total = lines_.size();
    if (scroll_ == 0 || total == 0) {
      d.fillRect(x, kTop, W - x, h, BG);
      return;
    }
    const int thumbH = std::max(4, h * (end - start) / total);
    const int thumbY = kTop + std::min(h - thumbH, h * start / total);
    d.fillRect(x, kTop, W - x, thumbY - kTop, BG);
    d.fillRect(x, thumbY, 1, thumbH, BG);
    d.fillRect(x + 1, thumbY, 2, thumbH, DIM);
    d.fillRect(x + 3, thumbY, W - x - 3, thumbH, BG);
    d.fillRect(x, thumbY + thumbH, W - x, kTop + h - thumbY - thumbH, BG);
  }

  void drawInput() {
    if (!shown_) return;
    auto& d = *d_;
    constexpr int kX = 4, kTextY = kInputY + 5, kHintEnd = W - 4;
    static const char* const kHint = "Tab menu";
    const bool hint = input_.isEmpty() && strcmp(prompt_, kPrompt) == 0;
    const int hintX = kHintEnd - static_cast<int>(strlen(kHint)) * CHAR_W;
    String body = input_;
    if (mask_) {
      body = "";
      for (size_t i = 0; i < input_.length(); ++i) body += '*';
    }
    const int bodyX = kX + static_cast<int>(strlen(prompt_)) * CHAR_W;
    const int room = ((hint ? hintX - CHAR_W : kHintEnd) - bodyX) / CHAR_W - 1;  // minus the cursor
    if (static_cast<int>(body.length()) > room) body = "<" + body.substring(body.length() - (room - 1));

    d.setTextDatum(lgfx::textdatum_t::top_left);
    d.setFont(&fonts::Font0);
    d.setTextColor(ORANGE, BAR);
    d.drawString(prompt_, kX, kTextY);
    d.setTextColor(TEXT, BAR);
    d.drawString(body.c_str(), bodyX, kTextY);
    // The block cursor stands one pixel taller than the text, so clear that row beside it too.
    const int cursorX = bodyX + body.length() * CHAR_W;
    d.fillRect(kX, kTextY - 1, cursorX - kX, 1, BAR);
    d.fillRect(cursorX, kTextY - 1, 5, 9, ORANGE);
    const int restEnd = hint ? hintX : W;
    d.fillRect(cursorX + 5, kTextY - 1, restEnd - cursorX - 5, 9, BAR);
    if (hint) {
      d.setTextColor(DIM, BAR);
      d.drawString(kHint, hintX, kTextY);
      d.fillRect(hintX, kTextY - 1, W - hintX, 1, BAR);
      d.fillRect(kHintEnd, kTextY, W - kHintEnd, 8, BAR);
    }
  }

  Header& header_;
  Gfx* d_ = nullptr;
  String title_ = brand::kProduct;
  bool shown_ = false;
  std::deque<Line> lines_;
  int placeholder_ = 0;  // lines at the end that the next message replaces
  int scroll_ = 0;
  String input_;
  const char* prompt_ = kPrompt;
  bool mask_ = false;
  uint32_t added_ = 0;
  String last_;
  uint16_t lastColor_ = DIM;
};
}  // namespace ui
