#include "shim.h"
#include "gen_tts.h"
#include "sessions.h"
#include "setup_form.h"
#include "config.h"
#include "captive.h"
#include "agent_proto.h"
#include "prompt_history.h"
#include "language.h"
#include "chat_request.h"
#include "notes.h"
#include "power.h"
#include "approvals.h"
#include "usage.h"

static int fails = 0, checks = 0;
#define CHECK(cond, ...)                                     \
  do {                                                       \
    ++checks;                                                \
    if (!(cond)) {                                           \
      ++fails;                                               \
      printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond); \
      printf(__VA_ARGS__);                                   \
      printf("\n");                                          \
    }                                                        \
  } while (0)

static void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x); v.push_back(x >> 8); }
static void put32(std::vector<uint8_t>& v, uint32_t x) { put16(v, x); put16(v, x >> 16); }
static void tag(std::vector<uint8_t>& v, const char* t) { v.insert(v.end(), t, t + 4); }

constexpr uint32_t kExact = 1;  // data size field = real size
struct WavSpec {
  uint16_t format = 1, channels = 1, bits = 16;
  uint32_t rate = 16000;
  uint32_t riffSize = 0;          // value written (placeholder ok)
  uint32_t dataField = kExact;    // kExact, or a literal placeholder
  bool list = false;              // an odd-sized LIST chunk before fmt
  uint32_t fmtSize = 16;
};
static std::vector<uint8_t> wav(const WavSpec& s, const std::vector<int16_t>& samples) {
  std::vector<uint8_t> v;
  tag(v, "RIFF"); put32(v, s.riffSize); tag(v, "WAVE");
  if (s.list) { tag(v, "LIST"); put32(v, 5); v.insert(v.end(), {'I', 'N', 'F', 'O', 'x'}); v.push_back(0); }
  tag(v, "fmt "); put32(v, s.fmtSize);
  put16(v, s.format); put16(v, s.channels); put32(v, s.rate);
  put32(v, s.rate * s.channels * s.bits / 8); put16(v, s.channels * s.bits / 8); put16(v, s.bits);
  for (uint32_t i = 16; i < s.fmtSize; ++i) v.push_back(0);
  tag(v, "data"); put32(v, s.dataField == kExact ? samples.size() * 2 : s.dataField);
  for (int16_t x : samples) put16(v, x);
  return v;
}
static bool parse(const std::vector<uint8_t>& bytes, tts::WavFormat& w, String& err) {
  SD.files["/t.wav"] = bytes;
  File f = SD.open("/t.wav", FILE_READ);
  return tts::parseWav(f, w, err);
}

static void testChunks() {
  auto c = tts::chunks("Hello there");
  CHECK(c.size() == 1 && c[0] == "Hello there.", "got '%s'", c.empty() ? "" : c[0].c_str());

  c = tts::chunks("**Bold** answer\n- item one\n- item two\n# Heading\n`code` here\n> quoted");
  CHECK(c.size() == 1 && c[0] == "Bold answer. item one. item two. Heading. code here. quoted.", "got '%s'",
        c.empty() ? "" : c[0].c_str());

  c = tts::chunks("It\xE2\x80\x99s \xE2\x80\x94 fine\xE2\x80\xA6   really\t\tfine!");
  CHECK(c.size() == 1 && c[0] == "It's - fine... really fine!", "got '%s'", c.empty() ? "" : c[0].c_str());

  CHECK(tts::chunks("").empty(), "empty answer");
  CHECK(tts::chunks("*** ## ``\n\n- \n---\n-").empty(), "only markdown noise");

  // Sentences pack greedily and every piece ends on a sentence boundary when one fits.
  String para;
  for (int i = 0; i < 12; ++i) para += "This is sentence number " + String(i) + " of the answer. ";
  c = tts::chunks(para);
  CHECK(c.size() == 3, "%zu chunks", c.size());
  for (auto& p : c) {
    CHECK(p.length() <= 190 && p.length() > 100, "len %u: '%s'", p.length(), p.c_str());
    CHECK(p[p.length() - 1] == '.', "ends mid-sentence: '%s'", p.c_str());
  }
  CHECK(c[0].startsWith("This is sentence number 0") && c[1].startsWith("This is sentence number"),
        "order: '%s'", c[1].c_str());

  // One long sentence: split after a comma past the middle, else at a space.
  String longOne;
  for (int i = 0; i < 50; ++i) longOne += (i == 30 ? "word, " : "word ");
  longOne += "end.";
  c = tts::chunks(longOne);
  CHECK(c.size() == 2 && c[0].length() <= 190, "%zu chunks", c.size());
  CHECK(c[0][c[0].length() - 1] == ',', "comma split: '%s'", c[0].c_str());

  String early = "Well, ";
  for (int i = 0; i < 60; ++i) early += "word ";
  c = tts::chunks(early);
  CHECK(c[0].length() > 150 && c[0].length() <= 190, "early comma ignored, len %u", c[0].length());

  // No spaces at all: hard cuts at 190, capped at three pieces.
  String blob;
  for (int i = 0; i < 1000; ++i) blob += 'x';
  c = tts::chunks(blob);
  CHECK(c.size() == 3 && c[0].length() == 190 && c[2].length() == 190, "%zu chunks", c.size());

  // Exactly 190 characters stays whole; 191 splits.
  String n190;
  for (int i = 0; i < 189; ++i) n190 += 'a';
  n190 += '.';
  CHECK(tts::chunks(n190).size() == 1, "190 chars is one chunk");
  CHECK(tts::chunks(n190 + " b.").size() == 2, "191+ splits");
}

static void testParse() {
  std::vector<int16_t> s(100, 7);
  tts::WavFormat w;
  String err;
  CHECK(parse(wav({}, s), w, err) && w.dataOffset == 44 && w.dataBytes == 200 && w.rate == 16000 &&
            w.channels == 1, "plain: %s off=%u bytes=%u", err.c_str(), w.dataOffset, w.dataBytes);

  for (uint32_t placeholder : {0u, 0xFFFFFFFFu, 100000u}) {
    WavSpec sp;
    sp.dataField = placeholder;
    sp.riffSize = 0xFFFFFFFF;
    w = {};
    CHECK(parse(wav(sp, s), w, err) && w.dataBytes == 200, "placeholder %x: %s bytes=%u", placeholder, err.c_str(),
          w.dataBytes);
  }

  WavSpec odd;
  odd.list = true;
  odd.fmtSize = 18;
  odd.channels = 2;
  odd.rate = 48000;
  w = {};
  CHECK(parse(wav(odd, s), w, err) && w.dataOffset == 12 + 14 + 26 + 8 && w.channels == 2 && w.rate == 48000,
        "LIST + fmt18: %s off=%u", err.c_str(), w.dataOffset);

  WavSpec flt;
  flt.format = 3;
  flt.bits = 32;
  err = "";
  CHECK(!parse(wav(flt, s), w, err) && err == "unsupported WAV: format 3, 32-bit, 1 ch", "float: '%s'", err.c_str());

  WavSpec mp;
  mp.channels = 6;
  err = "";
  CHECK(!parse(wav(mp, s), w, err) && err.indexOf("6 ch") >= 0, "6ch: '%s'", err.c_str());

  WavSpec slow;
  slow.rate = 4000;
  err = "";
  CHECK(!parse(wav(slow, s), w, err) && err == "unsupported WAV rate 4000", "rate: '%s'", err.c_str());

  const std::string json = "{\"error\":{\"message\":\"nope\"}}";
  err = "";
  CHECK(!parse(std::vector<uint8_t>(json.begin(), json.end()), w, err) && err == "reply is not a WAV file", "json: '%s'",
        err.c_str());

  auto noData = wav({}, {});
  noData.resize(36);  // header cut before the data chunk
  err = "";
  CHECK(!parse(noData, w, err) && err == "WAV has no fmt or data chunk", "truncated: '%s'", err.c_str());
}

static std::vector<int16_t> tone(double hz, double amp, size_t n, uint32_t rate = 16000) {
  std::vector<int16_t> s(n);
  for (size_t i = 0; i < n; ++i) s[i] = static_cast<int16_t>(amp * std::sin(2 * M_PI * hz * i / rate));
  return s;
}

static size_t resampleCount(uint32_t rate, size_t n) {
  tts::Resampler r{rate};
  size_t out = 0;
  for (size_t i = 0; i < n; ++i) r.push(1000, [&](float) { ++out; });
  return out;
}

// Steady-state level of a tone after resampling from `rate`, in dB relative to the input.
static double passDb(uint32_t rate, double hz) {
  tts::Resampler r{rate};
  std::vector<float> got;
  for (int16_t x : tone(hz, 10000, rate, rate)) r.push(x, [&](float v) { got.push_back(v); });
  float p = 0;
  for (size_t i = got.size() / 2; i < got.size(); ++i) p = std::max(p, std::fabs(got[i]));
  return 20 * std::log10(p / 10000);
}

static void testResampler() {
  CHECK(resampleCount(48000, 48000) == 16000, "48k");
  CHECK(resampleCount(44100, 44100) == 16000, "44.1k");
  CHECK(resampleCount(24000, 24000) == 16000, "24k");
  CHECK(resampleCount(22050, 22050) == 16000, "22.05k");
  CHECK(resampleCount(16000, 16000) == 16000, "16k");
  CHECK(resampleCount(8000, 8000) == 16000, "8k");

  for (uint32_t rate : {24000u, 48000u}) {
    const double d1k = passDb(rate, 1000), d3k = passDb(rate, 3500), d5k = passDb(rate, 5000);
    CHECK(std::fabs(d1k) < 1 && d3k > -2 && d5k > -2, "%u: speech band 1k %.1f, 3.5k %.1f, 5k %.1f dB", rate, d1k,
          d3k, d5k);
  }
  // Above 8 kHz a 24 kHz clip would fold back into the speech band (10 kHz lands on 6 kHz).
  const double d9k = passDb(24000, 9000), d10k = passDb(24000, 10000), d48 = passDb(48000, 12000);
  CHECK(d9k < -15 && d10k < -25, "24k aliases: 9 kHz %.1f dB, 10 kHz %.1f dB", d9k, d10k);
  CHECK(d48 < -25, "48k alias at 12 kHz: %.1f dB", d48);

  tts::Resampler up{8000};
  std::vector<float> got;
  for (int16_t x : {5, -5}) up.push(x, [&](float v) { got.push_back(v); });
  CHECK(got == std::vector<float>({5, 5, -5, -5}), "8k hold");
}

static std::vector<int16_t> monoOut() {
  std::vector<int16_t> m;
  for (size_t i = 0; i + 1 < adv_audio::spoken.size(); i += 2) {
    if (adv_audio::spoken[i] != adv_audio::spoken[i + 1]) return {};
    m.push_back(adv_audio::spoken[i]);
  }
  return m;
}

// Plays one clip and returns what the speaker got (empty if L and R differ).
static std::vector<int16_t> render(const WavSpec& sp, const std::vector<int16_t>& s) {
  SD.files[tts::clipPath(0).str()] = wav(sp, s);
  adv_audio::reset();
  auto p = tts::play(1, [] { return false; });
  CHECK(p.end == tts::PlayEnd::Finished, "render: %s", p.error.c_str());
  return monoOut();
}

static int peakOf(const std::vector<int16_t>& m, size_t from) {
  int p = 0;
  for (size_t i = from; i < m.size(); ++i) p = std::max(p, std::abs(int(m[i])));
  return p;
}

static void testPlayback() {
  auto never = [] { return false; };
  for (uint32_t rate : {8000u, 11025u, 16000u, 22050u, 24000u, 44100u, 48000u, 96000u}) {
    WavSpec mono, stereo;
    mono.rate = stereo.rate = rate;
    stereo.channels = 2;
    const auto ms = tone(300, 2000, rate / 2, rate);  // half a second
    std::vector<int16_t> ss;
    for (int16_t v : ms) ss.insert(ss.end(), {static_cast<int16_t>(v - 1000), static_cast<int16_t>(v + 1000)});
    const auto m = render(mono, ms);
    CHECK(m.size() == ms.size() * 16000 / rate, "%u Hz: %zu frames out", rate, m.size());
    CHECK(*std::max_element(adv_audio::writes.begin(), adv_audio::writes.end()) <= tts::kOutFrames,
          "%u write overflows g_out", rate);
    CHECK(!m.empty() && render(stereo, ss) == m, "%u Hz stereo plays as its L/R average", rate);
  }

  // Two clips back to back, each played from its own start, then one drain and the DAC muted again.
  WavSpec a;
  const auto second = render(a, tone(1000, 3000, 1600));
  SD.files[tts::clipPath(0).str()] = wav(a, tone(500, 1000, 1600));
  SD.files[tts::clipPath(1).str()] = wav(a, tone(1000, 3000, 1600));
  adv_audio::reset();
  auto p = tts::play(2, never);
  auto m = monoOut();
  CHECK(p.end == tts::PlayEnd::Finished && m.size() == 3200 && std::vector<int16_t>(m.begin() + 1600, m.end()) == second,
        "two clips: %zu", m.size());
  CHECK(std::string(adv_audio::events.begin(), adv_audio::events.end()) == "udm", "finish order");

  // Streamed reply with a placeholder size and an odd trailing byte plays to EOF.
  WavSpec streamed;
  streamed.dataField = 0xFFFFFFFF;
  auto bytes = wav(streamed, std::vector<int16_t>(1000, 9));
  bytes.push_back(0x55);
  SD.files[tts::clipPath(0).str()] = bytes;
  adv_audio::reset();
  p = tts::play(1, never);
  CHECK(p.end == tts::PlayEnd::Finished && monoOut().size() == 1000, "placeholder playback %zu", monoOut().size());

  // A stop mid-clip: mute before the drain, so the queued audio is never heard.
  SD.files[tts::clipPath(0).str()] = wav(a, std::vector<int16_t>(16000, 4));
  int polls = 0;
  adv_audio::reset();
  p = tts::play(2, [&] { return ++polls > 3; });
  CHECK(p.end == tts::PlayEnd::Stopped && monoOut().size() < 16000, "stop: %zu frames", monoOut().size());
  CHECK(std::string(adv_audio::events.begin(), adv_audio::events.end()) == "umdm", "stop order");

  // A bad second clip still leaves the speaker drained and muted, with a readable reason.
  SD.files[tts::clipPath(1).str()] = {'n', 'o', 'p', 'e'};
  SD.files[tts::clipPath(0).str()] = wav(a, std::vector<int16_t>(160, 1));
  adv_audio::reset();
  p = tts::play(2, never);
  CHECK(p.end == tts::PlayEnd::Failed && p.error == "reply is not a WAV file" && monoOut().size() == 160,
        "bad clip: %s", p.error.c_str());
  CHECK(std::string(adv_audio::events.begin(), adv_audio::events.end()) == "udm", "fail order");

  SD.files.erase(tts::clipPath(0).str());
  adv_audio::reset();
  p = tts::play(1, never);
  CHECK(p.end == tts::PlayEnd::Failed && p.error == "no audio on SD", "missing: %s", p.error.c_str());
}

// The speaker voicing: bass below 400 Hz cut, the speech band boosted by kBoost.
static void testVoicing() {
  tts::Limiter l;
  CHECK(l(1000) == 1000 * tts::kBoost, "quiet sample boosted");
  CHECK(l(-1000) == -1000 * tts::kBoost, "quiet negative sample boosted");

  WavSpec sp;
  auto gainDb = [&](double hz) { return 20 * std::log10(peakOf(render(sp, tone(hz, 1000, 8000)), 4000) / (1000.0 * tts::kBoost)); };
  const double g100 = gainDb(100), g1k = gainDb(1000), g2k = gainDb(2000), g6k = gainDb(6000);
  CHECK(g100 < -15, "100 Hz cut: %.1f dB", g100);
  CHECK(std::fabs(g1k) < 1 && std::fabs(g2k) < 1 && std::fabs(g6k) < 1, "speech band flat: 1k %.1f, 2k %.1f, 6k %.1f dB",
        g1k, g2k, g6k);
}

// Full scale is limited, never wrapped or clipped past the ceiling; quiet speech after it gets the
// whole boost back.
static void testLimiter() {
  WavSpec sp;
  const auto quiet = render(sp, tone(2000, 1000, 16000));
  const auto loud = render(sp, tone(2000, 32767, 16000));
  int flips = 0;
  for (size_t i = 0; i < quiet.size() && i < loud.size(); ++i) {
    if (std::abs(int(quiet[i])) > 500 && (quiet[i] > 0) != (loud[i] > 0)) ++flips;
  }
  const int peak = peakOf(loud, 0);
  CHECK(loud.size() == 16000 && peak <= tts::kCeiling && peak >= 0.9 * tts::kCeiling, "loud peak %d", peak);
  CHECK(flips == 0, "%d samples changed sign (wrapped)", flips);

  auto in = tone(2000, 32767, 16000);
  const auto tail = tone(1000, 1000, 8000);
  in.insert(in.end(), tail.begin(), tail.end());
  const auto both = render(sp, in), alone = render(sp, tail);
  int worst = 0;
  for (size_t i = 1; i <= 1600 && i <= both.size() && i <= alone.size(); ++i) {
    worst = std::max(worst, std::abs(both[both.size() - i] - alone[alone.size() - i]));
  }
  CHECK(both.size() == in.size() && worst <= 2, "gain recovered after the loud part: off by %d", worst);
}

// ---------- sessions ----------
struct Msg {
  bool user;
  String text;
};
using Turns = std::vector<std::pair<std::string, std::string>>;

static std::string fileText(const std::string& path) {
  const auto& v = SD.files[path];
  return std::string(v.begin(), v.end());
}

static sessions::Meta meta(const char* id, const char* title = "t", uint32_t created = 1793975525) {
  sessions::Meta m;
  m.id = id;
  m.title = title;
  m.model = "claude-haiku-5-5";
  m.created = created;
  return m;
}

static Turns loadTurns(const char* id, sessions::Meta* out = nullptr) {
  Turns t;
  sessions::Meta m;
  const bool ok = sessions::load(id, m, [&t](const String& u, const String& a) { t.push_back({u.str(), a.str()}); });
  if (!ok) t.push_back({"<load failed>", ""});
  if (out) *out = m;
  return t;
}

static std::string show(const Turns& t) {
  std::string s;
  for (const auto& p : t) s += "(" + p.first + "|" + p.second + ")";
  return s;
}

static void testSessionFormat() {
  SD.reset();
  const auto m = meta("20261106-143205", "what's a good name");
  CHECK(sessions::append(m, "hi \"there\"\nline2", "Miso.\tOr Pepper \xE2\x9C\x93"), "first turn saved");
  CHECK(sessions::append(m, "q2", "a2"), "second turn saved");
  const std::string want =
      "{\"v\":1,\"title\":\"what's a good name\",\"model\":\"claude-haiku-5-5\",\"created\":1793975525}\n"
      "{\"u\":\"hi \\\"there\\\"\\nline2\"}\n"
      "{\"a\":\"Miso.\\tOr Pepper \xE2\x9C\x93\"}\n"
      "{\"u\":\"q2\"}\n{\"a\":\"a2\"}\n";
  const std::string got = fileText("/kloudputer/sessions/20261106-143205.jsonl");
  CHECK(got == want, "file is\n%s", got.c_str());
  CHECK(sessions::count() == 1, "count %zu", sessions::count());
  const auto l = sessions::list(50);
  CHECK(l.total == 1 && l.items.size() == 1 && l.items[0].id == "20261106-143205" &&
            l.items[0].title == "what's a good name" && l.items[0].model == "claude-haiku-5-5" &&
            l.items[0].created == 1793975525,
        "listed %zu/%zu '%s'", l.items.size(), l.total, l.items.empty() ? "" : l.items[0].title.c_str());
  const Turns t = loadTurns("20261106-143205");
  CHECK(show(t) == "(hi \"there\"\nline2|Miso.\tOr Pepper \xE2\x9C\x93)(q2|a2)", "resumed %s", show(t).c_str());
}

static void testSessionResume() {
  SD.reset();
  const auto m = meta("20261106-143205");
  for (int i = 1; i <= 16; ++i) sessions::append(m, ("q" + std::to_string(i)).c_str(), ("a" + std::to_string(i)).c_str());
  std::vector<Msg> h;
  sessions::Meta got;
  const bool ok = sessions::load("20261106-143205", got, [&h](const String& u, const String& a) {
    h.push_back({true, u});
    h.push_back({false, a});
    sessions::trimHistory(h, 10, 8000);
  });
  CHECK(ok && got.title == "t" && got.id == "20261106-143205", "meta '%s'", got.title.c_str());
  CHECK(h.size() == 10 && h.front().user && h.front().text == "q12" && !h.back().user && h.back().text == "a16",
        "history %zu msgs, first '%s'", h.size(), h.empty() ? "" : h.front().text.c_str());

  SD.reset();
  const std::string big(3000, 'x');
  for (int i = 1; i <= 4; ++i) sessions::append(m, ("q" + std::to_string(i)).c_str(), big.c_str());
  h.clear();
  sessions::load("20261106-143205", got, [&h](const String& u, const String& a) {
    h.push_back({true, u});
    h.push_back({false, a});
    sessions::trimHistory(h, 10, 8000);
  });
  size_t chars = 0;
  for (const auto& x : h) chars += x.text.length();
  CHECK(h.size() == 4 && h.front().user && h.front().text == "q3" && chars <= 8000, "char-bounded history %zu msgs %zu chars",
        h.size(), chars);

  h = {{false, "a0"}, {true, "q1"}, {false, "a1"}, {true, "q2"}, {false, "a2"}};
  sessions::trimHistory(h, 3, 8000);
  CHECK(h.size() == 2 && h.front().user && h.front().text == "q2", "window starts on a prompt: %zu '%s'", h.size(),
        h.empty() ? "" : h.front().text.c_str());

  SD.reset();
  for (int i = 1; i <= 30; ++i) sessions::append(m, ("q" + std::to_string(i)).c_str(), ("a" + std::to_string(i)).c_str());
  const Turns t = loadTurns("20261106-143205");
  CHECK(t.size() == 20 && t.front().first == "q11" && t.back().second == "a30", "long session rebuilt %zu turns from %s",
        t.size(), t.empty() ? "" : t.front().first.c_str());
}

static void testSessionDamage() {
  SD.reset();
  const auto m = meta("20261106-143205");
  sessions::append(m, "q1", "a1");
  sessions::append(m, "q2", "a2 is long enough to cut");
  auto& bytes = SD.files["/kloudputer/sessions/20261106-143205.jsonl"];
  bytes.resize(bytes.size() - 8);  // power lost mid-reply
  CHECK(show(loadTurns("20261106-143205")) == "(q1|a1)", "truncated: %s", show(loadTurns("20261106-143205")).c_str());
  sessions::append(m, "q3", "a3");
  CHECK(show(loadTurns("20261106-143205")) == "(q1|a1)(q3|a3)", "appended after truncation: %s",
        show(loadTurns("20261106-143205")).c_str());

  SD.reset();
  SD.dirs.insert("/kloudputer");
  SD.dirs.insert("/kloudputer/sessions");
  const std::string damaged =
      "{\"v\":1,\"title\":\"imported\",\"model\":\"claude-opus-5-5\",\"created\":0,\"source\":\"web\"}\n"
      "{\"u\":\"q1\"}\n"
      "not json at all\n"
      "{\"a\":\"a1\",\"model\":\"x\",\"tokens\":{\"in\":3,\"out\":[1,2]}}\n"
      "{\"note\":\"from an import\"}\n"
      "[1,2,3]\n"
      "\n"
      "{\"u\":5}\n"
      "{\"u\":\"q2\",\"t\":1}\n"
      "{\"a\":\"a2\"}\n"
      "{\"a\":\"orphan reply\"}\n"
      "{\"u\":\"" + std::string(sessions::kMaxLine, 'z') + "\"}\n"
      "{\"a\":\"reply to the oversized prompt\"}\n"
      "{\"u\":\"q3\"}\n"
      "{\"a\":\"a3\"}\n"
      "{\"u\":\"q4 never answ";
  SD.files["/kloudputer/sessions/x.jsonl"].assign(damaged.begin(), damaged.end());
  sessions::Meta got;
  const Turns t = loadTurns("x", &got);
  CHECK(show(t) == "(q1|a1)(q2|a2)(q3|a3)", "damaged lines skipped: %s", show(t).c_str());
  CHECK(got.title == "imported" && got.model == "claude-opus-5-5" && got.created == 0, "header '%s'", got.title.c_str());

  SD.reset();
  sessions::append(m, "q1", "a1");
  auto& f = SD.files["/kloudputer/sessions/20261106-143205.jsonl"];
  const std::string headerOnly = "{\"v\":1,\"title\":\"just a header\",\"model\":\"claude-haiku-5-5\",\"created\":5}\n";
  f.assign(headerOnly.begin(), headerOnly.end());
  const auto l = sessions::list(50);
  CHECK(l.items.size() == 1 && l.items[0].title == "just a header" && l.items[0].created == 5, "header-only listed '%s'",
        l.items.empty() ? "" : l.items[0].title.c_str());
  CHECK(loadTurns("20261106-143205").empty(), "header-only resumes empty: %s", show(loadTurns("20261106-143205")).c_str());

  const std::string noHeader = "garbage first line\n{\"u\":\"q\"}\n{\"a\":\"a\"}\n";
  f.assign(noHeader.begin(), noHeader.end());
  sessions::Meta nh;
  CHECK(show(loadTurns("20261106-143205", &nh)) == "(q|a)" && nh.title.isEmpty(), "garbled header: %s",
        show(loadTurns("20261106-143205")).c_str());
  CHECK(!sessions::load("missing", nh, [](const String&, const String&) {}), "missing file fails to load");
}

static void testSessionIds() {
  CHECK(sessions::stampOf(1793975525) == "20261106-143205", "stamp %s", sessions::stampOf(1793975525).c_str());
  struct { const char* stamp; const char* newest; const char* want; } cases[] = {
      {"20261106-143205", "", "20261106-143205"},
      {"20261106-143205", "20261105-235959-004", "20261106-143205"},
      {"20261106-143205", "20261106-143205", "20261106-143205-001"},
      {"20261106-143205", "20261106-143205-002", "20261106-143205-003"},
      {"20261106-143204", "20261106-143205", "20261106-143205-001"},
      {"", "20261106-143205-001", "20261106-143205-002"},
      {"", "", "00000000-000000"},
      {"", "00000000-000000", "00000000-000000-001"},
      {"", "x-999", "x-999-001"},
  };
  for (const auto& c : cases) {
    const String id = sessions::nextId(c.stamp, c.newest);
    CHECK(id == c.want, "nextId('%s','%s') = %s", c.stamp, c.newest, id.c_str());
  }
  String newest;
  bool rising = true;
  for (int i = 0; i < 1200 && rising; ++i) {
    const String id = sessions::nextId(i % 400 == 399 ? "20261106-143205" : "", newest);
    rising = newest < id;
    newest = id;
  }
  CHECK(rising, "1200 ids in a row keep rising, ended at %s", newest.c_str());

  SD.reset();
  SD.dirs.insert("/kloudputer");
  SD.mkdir("/kloudputer/sessions");
  const char* order[] = {"20261106-143205", "00000000-000000-001", "20251231-235959", "20261106-143205-001",
                         "00000000-000000"};
  for (const char* id : order) sessions::append(meta(id), "q", "a");
  SD.files["/kloudputer/sessions/._20261107-000000.jsonl"] = {'x'};
  SD.files["/kloudputer/sessions/notes.txt"] = {'x'};
  SD.mkdir("/kloudputer/sessions/old.jsonl");
  const auto l = sessions::list(3);
  std::string ids;
  for (const auto& it : l.items) ids += it.id.str() + " ";
  CHECK(ids == "20261106-143205-001 20261106-143205 20251231-235959 " && l.total == 5, "newest first, capped: %s(of %zu)",
        ids.c_str(), l.total);
  CHECK(sessions::count() == 5 && sessions::newestId() == "20261106-143205-001", "count %zu newest %s", sessions::count(),
        sessions::newestId().c_str());
}

static void testSessionTitles() {
  struct { const char* prompt; const char* want; } cases[] = {
      {"what\xE2\x80\x99s a good name for a cat?", "what's a good name for a cat?"},
      {"Can you help me plan a weekend trip to the old town with trains both ways",
       "Can you help me plan a weekend..."},
      {"  two\n\nlines \t and   spaces  ", "two lines and spaces"},
      {"Supercalifragilisticexpialidocious-and-then-some", "Supercalifragilisticexpialidocio..."},
      {"exactly thirty-two characters ok", "exactly thirty-two characters ok"},
  };
  for (const auto& c : cases) {
    const String t = sessions::titleFrom(c.prompt);
    CHECK(t == c.want, "title('%s') = '%s'", c.prompt, t.c_str());
  }
}

static void testSessionDelete() {
  SD.reset();
  sessions::append(meta("20261106-143205"), "q", "a");
  sessions::append(meta("20261106-150000"), "q", "a");
  CHECK(sessions::remove("20261106-143205"), "delete reports success");
  const auto l = sessions::list(50);
  CHECK(l.total == 1 && l.items.size() == 1 && l.items[0].id == "20261106-150000" &&
            !SD.files.count("/kloudputer/sessions/20261106-143205.jsonl"),
        "deleted one gone, other kept (%zu left)", l.total);
  CHECK(!sessions::remove("20261106-143205"), "deleting twice fails");

  SD.reset();
  SD.readOnly = true;
  CHECK(!sessions::append(meta("20261106-143205"), "q", "a") && SD.files.empty(), "a refused write reports failure");
}

static void testSessionWhen() {
  setenv("TZ", "UTC0", 1);
  tzset();
  const uint32_t t = 1793975525;  // Nov 6 14:32:05 UTC
  CHECK(sessions::when(0, t) == "-", "no clock: %s", sessions::when(0, t).c_str());
  CHECK(sessions::when(t, t + 3600) == "14:32", "same day: %s", sessions::when(t, t + 3600).c_str());
  CHECK(sessions::when(t, t + 2 * 86400) == "Nov 6", "older: %s", sessions::when(t, t + 2 * 86400).c_str());
  CHECK(sessions::when(t, 0) == "Nov 6", "clock unset now: %s", sessions::when(t, 0).c_str());
  setenv("TZ", "<+04>-4", 1);
  tzset();
  CHECK(sessions::when(t, t) == "18:32", "local zone: %s", sessions::when(t, t).c_str());
  setenv("TZ", "UTC0", 1);
  tzset();
}

// ---------- phone setup ----------
static const std::vector<phone::Choice> kModelChoices = {
    {"claude-haiku-5-5", "Haiku 5.5"}, {"claude-sonnet-5-5", "Sonnet 5.5"}, {"claude-opus-5-5", "Opus 5.5"}};

static void testUrlDecode() {
  struct { const char* in; const char* want; } cases[] = {
      {"a+b", "a b"}, {"%41%62c", "Abc"}, {"100%25", "100%"}, {"%2B%2b", "++"}, {"caf%C3%A9", "caf\xC3\xA9"},
      {"%G1", "%G1"}, {"%4", "%4"}, {"end%", "end%"}, {"%%41", "%A"}, {"%zz%41", "%zzA"}, {"", ""},
  };
  for (const auto& c : cases) {
    const String got = phone::urlDecode(c.in);
    CHECK(got == c.want, "urlDecode('%s') = '%s'", c.in, got.c_str());
  }
}

static void testParseForm() {
  phone::Form f = phone::parseForm(
      "ssid=Home+Net&pass=p%40ss+word&akey=+sk-ant-abc%0A&gkey=gsk_x&model=claude-opus-5-5&tz=%3C%2B04%3E-4&junk=1");
  CHECK(f.ssid == "Home Net" && f.wifiPass == "p@ss word" && f.anthropicKey == "sk-ant-abc" && f.groqKey == "gsk_x" &&
            f.model == "claude-opus-5-5" && f.tz == "<+04>-4",
        "parsed ssid '%s' pass '%s' akey '%s' tz '%s'", f.ssid.c_str(), f.wifiPass.c_str(), f.anthropicKey.c_str(),
        f.tz.c_str());
  f = phone::parseForm("other=Typed%20Net&ssid=__other__&pass=");
  CHECK(f.ssid == "Typed Net" && f.wifiPass.isEmpty(), "other network: '%s'", f.ssid.c_str());
  f = phone::parseForm("ssid=a&ssid=b&model&=x&&akey");
  CHECK(f.ssid == "b" && f.model.isEmpty() && f.anthropicKey.isEmpty(), "odd pairs: ssid '%s'", f.ssid.c_str());
  f = phone::parseForm("");
  CHECK(f.ssid.isEmpty() && f.tz.isEmpty(), "empty body");
}

static void testBody() {
  phone::Body b;
  b.clear();
  std::vector<uint8_t> chunk(3000, 'a');
  b.add(chunk.data(), chunk.size());
  CHECK(!b.oversized && b.text.length() == 3000, "3000 bytes fit");
  b.add(chunk.data(), chunk.size());
  CHECK(b.oversized && b.text.length() == phone::kMaxBody, "6000 bytes capped at %u (oversized %d)", b.text.length(),
        b.oversized);
  b.clear();
  std::vector<uint8_t> exact(phone::kMaxBody, 'b');
  b.add(exact.data(), exact.size());
  CHECK(!b.oversized && b.text.length() == phone::kMaxBody, "exactly the cap is fine");
}

static void testValidate() {
  auto form = [] {
    phone::Form f;
    f.ssid = "HomeNet";
    f.wifiPass = "correct horse";
    f.anthropicKey = "sk-ant-api03-abcdefgh";
    f.groqKey = "gsk_abcdefgh";
    f.model = "claude-sonnet-5-5";
    f.tz = "<+04>-4";
    return f;
  };
  CHECK(phone::validate(form(), false, kModelChoices) == "", "good form: %s", phone::validate(form(), false, kModelChoices).c_str());
  CHECK(phone::validate(phone::Form(), true, kModelChoices) == "", "all empty keeps everything");
  struct { const char* what; void (*edit)(phone::Form&); bool haveKey; const char* want; } cases[] = {
      {"bad anthropic prefix", [](phone::Form& f) { f.anthropicKey = "sk-proj-123"; }, true, "should start with sk-ant-"},
      {"bad groq prefix", [](phone::Form& f) { f.groqKey = "gsx_1"; }, true, "should start with gsk_"},
      {"space inside a key", [](phone::Form& f) { f.anthropicKey = "sk-ant-ab cd"; }, true, "spaces"},
      {"key over 256", [](phone::Form& f) { f.anthropicKey = ("sk-ant-" + std::string(250, 'x')).c_str(); }, true, "too long"},
      {"no key anywhere", [](phone::Form& f) { f.anthropicKey = ""; }, false, "Paste your Anthropic"},
      {"password without network", [](phone::Form& f) { f.ssid = ""; }, true, "Choose the network"},
      {"short wifi password", [](phone::Form& f) { f.wifiPass = "short"; }, true, "8 to 64"},
      {"ssid over 32", [](phone::Form& f) { f.ssid = std::string(33, 'n').c_str(); }, true, "at most 32"},
      {"control char in ssid", [](phone::Form& f) { f.ssid = "Home\x01"; }, true, "odd characters"},
      {"unknown model", [](phone::Form& f) { f.model = "gpt-4"; }, true, "Pick a model"},
      {"bad time zone", [](phone::Form& f) { f.tz = "Asia/Tbilisi; rm"; }, true, "time zone"},
  };
  for (const auto& c : cases) {
    phone::Form f = form();
    c.edit(f);
    const String got = phone::validate(f, c.haveKey, kModelChoices);
    CHECK(got.indexOf(c.want) >= 0, "%s: '%s'", c.what, got.c_str());
  }
  phone::Form open = form();
  open.wifiPass = "";
  CHECK(phone::validate(open, true, kModelChoices) == "", "open network without a password is fine");
}

// No 5-character run of a secret may reach the page: a saved key shows only its last 4 characters.
static int leaks(const String& html, const String& secret) {
  int n = 0;
  for (size_t i = 0; i + 5 <= secret.length(); ++i) n += html.indexOf(secret.substring(i, i + 5)) >= 0;
  return n;
}

static void writeText(const char* path, const String& text) {
  FILE* f = fopen(path, "wb");
  fwrite(text.c_str(), 1, text.length(), f);
  fclose(f);
}

static void testNeverEcho() {
  const String akey = "sk-ant-api03-Zq9XwV7pL2mN8rT4yU6eWbK3dF5gH1jQ-abcd";
  const String gkey = "gsk_Qw8Er7Ty6Ui5Op4As3Df2Gh1Jk9LzXc7Vb5Nmwxyz";
  const phone::Current now = phone::currentFrom("HomeNet", akey, gkey, "claude-haiku-5-5", "<+04>-4");
  phone::Form typed;
  typed.ssid = "Cafe<script>alert(1)</script>";
  typed.wifiPass = "Wq7Rt5Yp3Lk9";
  typed.anthropicKey = "sk-ant-api03-Hq4Zp8Wv2Nx6Kd";
  typed.groqKey = "gsk_Rj5Tm9Xc3Bv7Qy";
  typed.model = "claude-opus-5-5";
  typed.tz = "<+05>-5";
  const String fresh = phone::page(now, kModelChoices, phone::Form(), "");
  const String again = phone::page(now, kModelChoices, typed, "Pick a model from the list.");
  int total = 0;
  for (const String* html : {&fresh, &again}) {
    total += leaks(*html, akey.substring(7)) + leaks(*html, gkey.substring(4)) + leaks(*html, typed.wifiPass) +
             leaks(*html, typed.anthropicKey.substring(13)) + leaks(*html, typed.groqKey.substring(4));
  }
  CHECK(total == 0, "%d secret fragments reached the page", total);
  CHECK(fresh.indexOf("ends in ...abcd") >= 0 && fresh.indexOf("ends in ...wxyz") >= 0, "saved keys show their tails");
  CHECK(again.indexOf("Pick a model from the list.") >= 0 && again.indexOf("value=\"claude-opus-5-5\" selected") >= 0 &&
            again.indexOf("value=\"&lt;+05&gt;-5\"") >= 0 && again.indexOf("Cafe&lt;script&gt;") >= 0 &&
            again.indexOf("<script>alert") < 0,
        "after an error the non-secret fields come back, escaped");
  CHECK(fresh.indexOf("{{") < 0 && again.indexOf("{{") < 0, "no placeholder left");
  int urls = 0, inPage = 0;  // url(...) may only point inside the page, like the logo's clip path
  for (int at = fresh.indexOf("url("); at >= 0; at = fresh.indexOf("url(", at + 1)) {
    ++urls;
    inPage += fresh[at + 4] == '#';
  }
  CHECK(fresh.indexOf(" src=") < 0 && fresh.indexOf("<link") < 0 && fresh.indexOf("href=") < 0 && urls == inPage &&
            fresh.indexOf("@import") < 0,
        "the page loads nothing from outside (%d of %d url() in the page)", inPage, urls);
  const phone::Current blank = phone::currentFrom("", "", "", "claude-haiku-5-5", "UTC0");
  const String first = phone::page(blank, kModelChoices, phone::Form(), "");
  CHECK(first.indexOf("Saved") < 0 && first.indexOf("Choose a network") >= 0, "first setup shows no saved keys");
  writeText("page_for_node.html", fresh);
}

static void testTzMeaning() {
  struct { const char* tz; int eastMinutes; } cases[] = {
      {"UTC0", 0}, {"<+04>-4", 240}, {"<-0530>5:30", -330}, {"<+0545>-5:45", 345}};
  for (const auto& c : cases) {
    setenv("TZ", c.tz, 1);
    tzset();
    const time_t t = 1793975525;
    tm local;
    localtime_r(&t, &local);
    CHECK(local.tm_gmtoff == c.eastMinutes * 60, "%s is %ld min east, want %d", c.tz, local.tm_gmtoff / 60, c.eastMinutes);
  }
  setenv("TZ", "UTC0", 1);
  tzset();
}

static void testApCredentials() {
  const uint8_t mac[6] = {0x24, 0x6F, 0x28, 0x00, 0x1A, 0x2B};
  CHECK(phone::apName(mac) == "Kloudputer-1A2B", "name %s", phone::apName(mac).c_str());
  static uint32_t counter;
  counter = 0;
  const String a = phone::apPassword([] { return counter++; });
  const String b = phone::apPassword([] { return counter++ * 2654435761u; });
  bool clean = a.length() == 10 && b.length() == 10;
  for (const String* p : {&a, &b}) {
    for (size_t i = 0; i < p->length(); ++i) clean = clean && strchr(phone::kPassChars, (*p)[i]) && !strchr("il1o0", (*p)[i]);
  }
  CHECK(clean && a == "abcdefghjk" && a != b, "passwords '%s' '%s'", a.c_str(), b.c_str());
  CHECK(phone::wifiQr("Kloudputer-1A2B", "k7mnp2xq4r") == "WIFI:T:WPA;S:Kloudputer-1A2B;P:k7mnp2xq4r;;", "qr %s",
        phone::wifiQr("Kloudputer-1A2B", "k7mnp2xq4r").c_str());
  CHECK(phone::wifiQr("a;b,c", "p:q\\\"") == "WIFI:T:WPA;S:a\\;b\\,c;P:p\\:q\\\\\\\";;", "escaped qr %s",
        phone::wifiQr("a;b,c", "p:q\\\"").c_str());
}

static void putFile(const char* path, const char* text) {
  SD.files[path].assign(text, text + strlen(text));
}

static void testPrecedence() {
  SD.reset();
  g_nvs.clear();
  SD.dirs.insert("/kloudputer");
  putFile("/keys.txt", "ANTHROPIC_API_KEY=sk-ant-from-sd-keys\nWIFI_SSID=SdNet\n");
  putFile("/kloudputer/config.txt", "# mine\nGROQ_API_KEY = \"gsk_from_sd_config\"\nMODEL=claude-sonnet-5-5\nTZ=EST5\n");
  Config sd;
  loadConfig(sd);
  loadPhone(sd);
  CHECK(sd.anthropicKey == "sk-ant-from-sd-keys" && sd.anthropicFrom == "/keys.txt" &&
            sd.groqKey == "gsk_from_sd_config" && sd.groqFrom == "/kloudputer/config.txt" &&
            sd.model == "claude-sonnet-5-5" && sd.tz == "EST5" && sd.source == "/keys.txt + /kloudputer/config.txt",
        "SD only: akey from '%s' groq from '%s' source '%s'", sd.anthropicFrom.c_str(), sd.groqFrom.c_str(),
        sd.source.c_str());

  CHECK(savePhone("sk-ant-from-phone", "", "", "<+04>-4"), "phone save stored");
  Config both;
  loadConfig(both);
  loadPhone(both);
  CHECK(both.anthropicKey == "sk-ant-from-phone" && both.anthropicFrom == "phone" &&
            both.groqKey == "gsk_from_sd_config" && both.groqFrom == "/kloudputer/config.txt" && both.tz == "<+04>-4" &&
            both.model == "claude-sonnet-5-5" && both.source == "phone + /keys.txt + /kloudputer/config.txt",
        "phone outranks SD: akey '%s' from '%s', tz '%s', source '%s'", both.anthropicKey.c_str(),
        both.anthropicFrom.c_str(), both.tz.c_str(), both.source.c_str());

  savePhone("", "gsk_from_phone", "claude-opus-5-5", "");
  Config later;
  loadConfig(later);
  loadPhone(later);
  CHECK(later.anthropicKey == "sk-ant-from-phone" && later.groqKey == "gsk_from_phone" && later.groqFrom == "phone" &&
            later.model == "claude-opus-5-5" && later.tz == "<+04>-4",
        "empty fields keep what's saved: akey '%s' groq '%s'", later.anthropicKey.c_str(), later.groqKey.c_str());

  SD.reset();
  Config noCard;
  loadPhone(noCard);
  CHECK(noCard.anthropicKey == "sk-ant-from-phone" && noCard.source == "phone", "no SD card: source '%s'",
        noCard.source.c_str());
}

// ---------- captive network: DNS, clients, log ----------
static std::vector<uint8_t> dnsQuery(const std::string& name, uint16_t type, bool edns = false, int questions = 1,
                                     uint8_t opcode = 0) {
  std::vector<uint8_t> q = {0xBE, 0xEF, static_cast<uint8_t>(opcode << 3 | 0x01), 0x00, 0, static_cast<uint8_t>(questions),
                            0, 0, 0, 0, 0, static_cast<uint8_t>(edns)};
  for (int n = 0; n < questions; ++n) {
    size_t start = 0;
    while (start <= name.size()) {
      size_t dot = name.find('.', start);
      if (dot == std::string::npos) dot = name.size();
      q.push_back(dot - start);
      q.insert(q.end(), name.begin() + start, name.begin() + dot);
      start = dot + 1;
    }
    q.push_back(0);
    q.insert(q.end(), {static_cast<uint8_t>(type >> 8), static_cast<uint8_t>(type), 0, 1});
  }
  if (edns) q.insert(q.end(), {0, 0, 41, 0x10, 0, 0, 0, 0, 0, 0, 0});  // OPT: 4096-byte UDP, no options
  return q;
}

static uint16_t be16(const std::vector<uint8_t>& v, size_t at) { return v[at] << 8 | v[at + 1]; }

static void testDnsAnswers() {
  const uint8_t ip[4] = {192, 0, 2, 1};
  std::vector<uint8_t> reply;
  captive::DnsQuestion asked;

  auto q = dnsQuery("ConnectivityCheck.GStatic.com", 1);
  bool ok = captive::dnsReply(q.data(), q.size(), ip, reply, asked);
  const size_t qEnd = q.size();
  CHECK(ok && asked.name == "connectivitycheck.gstatic.com" && asked.type == 1, "A: asked '%s' type %d", asked.name.c_str(),
        asked.type);
  CHECK(ok && reply.size() == qEnd + 16 && be16(reply, 0) == 0xBEEF && reply[2] == 0x81 && reply[3] == 0x80 &&
            be16(reply, 4) == 1 && be16(reply, 6) == 1 && be16(reply, 8) == 0 && be16(reply, 10) == 0,
        "A: header %02x %02x counts %d/%d/%d/%d", reply.size() > 3 ? reply[2] : 0, reply.size() > 3 ? reply[3] : 0,
        reply.size() > 11 ? be16(reply, 4) : -1, reply.size() > 11 ? be16(reply, 6) : -1,
        reply.size() > 11 ? be16(reply, 8) : -1, reply.size() > 11 ? be16(reply, 10) : -1);
  CHECK(ok && std::equal(q.begin() + 12, q.end(), reply.begin() + 12), "A: question echoed byte for byte (0x20 case kept)");
  const std::vector<uint8_t> answer = {0xC0, 12, 0, 1, 0, 1, 0, 0, 0, 10, 0, 4, 192, 0, 2, 1};
  CHECK(ok && std::equal(answer.begin(), answer.end(), reply.begin() + qEnd), "A: answer is 192.0.2.1 for 10 s");

  q = dnsQuery("connectivitycheck.gstatic.com", 28);
  ok = captive::dnsReply(q.data(), q.size(), ip, reply, asked);
  CHECK(ok && asked.type == 28 && reply[3] == 0x80 && be16(reply, 6) == 0 && be16(reply, 8) == 1 &&
            be16(reply, q.size() + 2) == 6 && reply.size() == q.size() + 12 + 22,
        "AAAA: NODATA with an SOA (an %d ns %d)", be16(reply, 6), be16(reply, 8));

  q = dnsQuery("anything.example", 255);
  ok = captive::dnsReply(q.data(), q.size(), ip, reply, asked);
  CHECK(ok && be16(reply, 6) == 1 && be16(reply, q.size() + 2) == 1, "ANY: answered with the A record");

  q = dnsQuery("www.google.com", 1, true);
  ok = captive::dnsReply(q.data(), q.size(), ip, reply, asked);
  CHECK(ok && asked.name == "www.google.com" && be16(reply, 10) == 0 && reply.size() == q.size() - 11 + 16 &&
            reply[reply.size() - 4] == 192 && reply[reply.size() - 1] == 1,
        "EDNS: the OPT record is dropped, the answer follows the question (size %zu)", reply.size());

  q = dnsQuery("a.example", 1, false, 2);
  ok = captive::dnsReply(q.data(), q.size(), ip, reply, asked);
  CHECK(ok && reply.size() == 12 && (reply[3] & 0x0F) == 3 && be16(reply, 4) == 0, "two questions: bare NXDOMAIN");
  q = dnsQuery("a.example", 1, false, 1, 4);
  ok = captive::dnsReply(q.data(), q.size(), ip, reply, asked);
  CHECK(ok && reply.size() == 12 && (reply[3] & 0x0F) == 3, "NOTIFY opcode: bare NXDOMAIN");

  const std::string l63(63, 'a'), l61(61, 'b');
  q = dnsQuery(l63 + "." + l63 + "." + l63 + "." + l61, 1);  // 255 bytes on the wire, the most DNS allows
  ok = captive::dnsReply(q.data(), q.size(), ip, reply, asked);
  CHECK(ok && asked.name.length() == 253, "longest legal name answered (%u chars)", asked.name.length());
  q = dnsQuery(l63 + "." + l63 + "." + l63 + "." + l63, 1);
  CHECK(!captive::dnsReply(q.data(), q.size(), ip, reply, asked), "a name over 255 bytes is ignored");

  struct { const char* what; std::vector<uint8_t> packet; } bad[] = {
      {"16 bytes", std::vector<uint8_t>(16, 0)},
      {"a response", [] { auto v = dnsQuery("x.example", 1); v[2] |= 0x80; return v; }()},
      {"label runs past the end", [] { auto v = dnsQuery("example.com", 1); v[12] = 40; return v; }()},
      {"type and class cut off", [] { auto v = dnsQuery("example.com", 1); v.resize(v.size() - 3); return v; }()},
      {"compressed name", [] { auto v = dnsQuery("example.com", 1); v[12] = 0xC0; v[13] = 12; return v; }()},
      {"64-byte label", dnsQuery(std::string(64, 'x') + ".com", 1)},
  };
  for (const auto& b : bad) {
    CHECK(!captive::dnsReply(b.packet.data(), b.packet.size(), ip, reply, asked) && reply.empty(), "%s: no reply",
          b.what);
  }
  CHECK(captive::dnsTypeName(65) == "HTTPS" && captive::dnsTypeName(99) == "TYPE99", "type names");
}

static void testCapport() {
  CHECK(captive::wantsCapport("application/captive+json"), "exact Accept");
  CHECK(captive::wantsCapport("text/html, Application/Captive+JSON;q=0.9"), "listed, any case");
  CHECK(!captive::wantsCapport("application/json") && !captive::wantsCapport("") &&
            !captive::wantsCapport("text/html,application/xhtml+xml"),
        "ordinary Accept headers get the page");
  const String json = captive::capportJson(phone::kPageUrl);
  JsonDocument doc;
  CHECK(!deserializeJson(doc, json.c_str()) && doc["captive"] == true && doc["user-portal-url"] == phone::kPageUrl,
        "capport JSON: %s", json.c_str());
}

static void testClients() {
  using captive::Client;
  struct { const char* ua; Client want; } cases[] = {
      {"Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/60.0.3112.32 Safari/537.36",
       Client::AndroidProbe},
      {"Dalvik/2.1.0 (Linux; U; Android 14; SM-S918B Build/UP1A.231005.007)", Client::AndroidSystem},
      {"Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Mobile Safari/537.36",
       Client::Chrome},
      {"Mozilla/5.0 (Linux; Android 14; SM-S918B Build/UP1A.231005.007; wv) AppleWebKit/537.36 (KHTML, like Gecko) "
       "Version/4.0 Chrome/130.0.6723.58 Mobile Safari/537.36",
       Client::Webview},
      {"Mozilla/5.0 (Linux; Android 14; SAMSUNG SM-S918B) AppleWebKit/537.36 (KHTML, like Gecko) SamsungBrowser/26.0 "
       "Chrome/122.0.0.0 Mobile Safari/537.36",
       Client::Samsung},
      {"CaptiveNetworkSupport-481.0.1 wispr", Client::Ios},
      {"Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 "
       "Mobile/15E148 Safari/604.1",
       Client::Ios},
      {"curl/8.5.0", Client::Other},
      {"", Client::Other},
  };
  for (const auto& c : cases) {
    const Client got = captive::classify(c.ua);
    CHECK(got == c.want, "'%.40s...' is %s, want %s", c.ua, captive::clientName(got), captive::clientName(c.want));
  }
}

static void testEventLog() {
  captive::EventLog log;
  for (int i = 0; i < 45; ++i) log.add(i * 1000, "event " + String(i));
  CHECK(log.lines().size() == captive::EventLog::kSize && log.lines().front() == "  5s event 5" &&
            log.lines().back() == " 44s event 44" && log.added() == 45,
        "ring keeps the last 40: front '%s' back '%s'", log.lines().front().c_str(), log.lines().back().c_str());
  CHECK(captive::httpEvent("GET", "connectivitycheck.gstatic.com", "/generate_204", 302, captive::Client::AndroidProbe,
                           false) == "GET connectivitycheck.gstatic.com/generate_204 302 android-probe",
        "http event");
  CHECK(captive::httpEvent("GET", "192.0.2.1", "/", 200, captive::Client::AndroidSystem, true) ==
            "GET 192.0.2.1/ 200 android-system capport",
        "capport marker");
  const uint8_t mac[6] = {1, 2, 3, 4, 0x4E, 0xA1};
  CHECK(captive::stationEvent(true, mac, -38) == "join 4e:a1 -38dBm" && captive::stationEvent(false, mac, 0) == "leave 4e:a1",
        "station events");
  CHECK(captive::dnsEvent({"mtalk.google.com", 28}) == "dns mtalk.google.com AAAA", "dns event");
}

// Android's NetworkMonitor skips its portal probe when the probe host resolves to a private address
// (Java's isSiteLocal/isLinkLocal), and every name resolves to the setup address.
static void testSetupAddress() {
  const uint8_t* a = phone::kApIp;
  const bool privateIp = a[0] == 10 || (a[0] == 172 && (a[1] & 0xF0) == 16) || (a[0] == 192 && a[1] == 168) ||
                         (a[0] == 169 && a[1] == 254);
  const String host = String(a[0]) + "." + String(a[1]) + "." + String(a[2]) + "." + String(a[3]);
  CHECK(!privateIp, "setup address %s is private by Android's rule", host.c_str());
  CHECK(host == phone::kApHost && String(phone::kPageUrl) == "http://" + host + "/", "address constants agree: %s %s",
        phone::kApHost, phone::kPageUrl);
}

// ---------- agent protocol (connector/PROTOCOL.md v1) ----------
static std::string hexOf(const uint8_t* b, size_t n) {
  std::string s;
  char h[3];
  for (size_t i = 0; i < n; ++i) {
    snprintf(h, sizeof h, "%02x", b[i]);
    s += h;
  }
  return s;
}

static const char* kCode = "cp1-AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8";
static const uint8_t kNonce[12] = {0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab};

static agent::Keys vectorKeys() {
  uint8_t secret[32];
  agent::Keys k;
  agent::parsePairingCode(kCode, secret);
  agent::deriveKeys(secret, k);
  return k;
}

static void testAgentVector() {
  uint8_t secret[32];
  CHECK(agent::parsePairingCode(kCode, secret) && hexOf(secret, 32) == "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
        "pairing code gives S");
  agent::Keys k;
  CHECK(agent::deriveKeys(secret, k), "keys derived");
  CHECK(k.topicUp == "cp-lQakgFwxcIsQkFbCO6dVXw" && k.topicDown == "cp-2QNwEsFuNcN6LqaSzNRoCg", "topics %s %s",
        k.topicUp.c_str(), k.topicDown.c_str());
  CHECK(hexOf(k.keyUp, 32) == "093a5da7f6e59518747167e3489862684f77e1947894d4e518671e6c69752e10" &&
            hexOf(k.keyDown, 32) == "75f24d2b1b5bbff8c48897267724db79912b4b465cd07144d017978a48bc86d3",
        "keys %s / %s", hexOf(k.keyUp, 32).c_str(), hexOf(k.keyDown, 32).c_str());
  const String plain = agent::buildUp(agent::Kind::Ask, "0badc0de", 1, 1762444800, "is cms-api up?");
  CHECK(plain == agent::kVectorPlain, "built plaintext %s", plain.c_str());
  const String body = agent::seal(k.keyUp, k.topicUp, plain, kNonce);
  CHECK(body == agent::kVectorBody, "sealed body %s", body.c_str());
  String opened;
  CHECK(agent::open(k.keyUp, k.topicUp, agent::kVectorBody, opened) && opened == agent::kVectorPlain, "opened %s",
        opened.c_str());
  CHECK(agent::selfTest(), "self test on this crypto");
}

static void testAgentTamper() {
  const agent::Keys k = vectorKeys();
  String out;
  const String body = agent::kVectorBody;
  for (size_t at : {size_t(2), size_t(20), size_t(body.length() - 3)}) {  // nonce, ciphertext, tag
    String bad = body;
    bad.setCharAt(at, bad[at] == 'A' ? 'B' : 'A');
    CHECK(!agent::open(k.keyUp, k.topicUp, bad, out), "byte %u changed: rejected", static_cast<unsigned>(at));
  }
  CHECK(!agent::open(k.keyDown, k.topicUp, body, out), "wrong key rejected");
  CHECK(!agent::open(k.keyUp, k.topicDown, body, out), "wrong AAD (the other topic) rejected");
  CHECK(!agent::open(k.keyUp, k.topicUp, body.substring(0, body.length() - 4), out), "truncated body rejected");
  CHECK(!agent::open(k.keyUp, k.topicUp, body.substring(0, 30), out), "body shorter than nonce + tag rejected");
  CHECK(!agent::open(k.keyUp, k.topicUp, body + "=", out) && !agent::open(k.keyUp, k.topicUp, String("+") + body, out),
        "padding or standard-base64 characters rejected");
  const String fresh = agent::seal(k.keyDown, k.topicDown, "{\"v\":1}");
  CHECK(agent::open(k.keyDown, k.topicDown, fresh, out) && out == "{\"v\":1}" && fresh != agent::seal(k.keyDown, k.topicDown, "{\"v\":1}"),
        "random nonces: same plaintext, different bodies");
  CHECK(agent::seal(k.keyUp, k.topicUp, String(std::string(agent::kMaxPlain + 1, 'x').c_str())).isEmpty(), "over 2,800 bytes refused");
}

static void testAgentReplay() {
  const uint32_t now = 1762444800;
  agent::ReplayGuard g;
  CHECK(g.accept("0badc0de", 1, now - 900, now) && !g.accept("0badc0de", 2, now - 901, now), "15 minutes old is the edge");
  CHECK(g.accept("0badc0de", 3, now + 120, now) && !g.accept("0badc0de", 4, now + 121, now), "2 minutes ahead is the edge");
  CHECK(!g.accept("0badc0de", 1, now - 900, now), "same (j, n) again is a replay");
  CHECK(g.accept("deadbeef", 1, now, now) && g.accept("", 1, now, now) && !g.accept("", 1, now, now),
        "same n in another job is fine; an empty job is its own job");
  CHECK(!g.accept("0badc0de", 5, now, 0), "no clock, nothing accepted");
  CHECK(!g.accept("0badc0de", 1, now - 900, now + 901), "a forgotten pair still fails the window");
}

static void testPairingCodes() {
  uint8_t s[32];
  const String good = kCode;
  struct { const char* what; String code; } bad[] = {
      {"other version", "cp2-" + good.substring(4)},
      {"one short", good.substring(0, 46)},
      {"one long", good + "A"},
      {"standard base64 char", good.substring(0, 10) + "+" + good.substring(11)},
      {"padding", good.substring(0, 46) + "="},
      {"stray bits in the last char", good.substring(0, 46) + "9"},
      {"empty", ""},
  };
  for (const auto& b : bad) CHECK(!agent::parsePairingCode(b.code, s), "%s: rejected", b.what);
  CHECK(agent::parsePairingCode(good, s), "the vector's code parses");
  std::vector<uint8_t> raw;
  CHECK(agent::b64urlDecode("_-8", raw) && raw.size() == 2 && raw[0] == 0xff && raw[1] == 0xef &&
            agent::b64urlEncode(raw.data(), raw.size()) == "_-8",
        "base64url round trip with - and _");
}

static void testAgentReport() {
  agent::Report r;
  CHECK(r.add(2, 3, "c") && r.add(0, 3, "a") && !r.complete() && r.add(2, 3, "c") && r.add(1, 3, "b") && r.complete() &&
            r.text() == "abc",
        "0-based, out of order, duplicate: '%s'", r.text().c_str());
  r.reset();
  CHECK(r.add(3, 3, "z") && r.add(1, 3, "x") && r.add(2, 3, "y") && r.complete() && r.text() == "xyz", "1-based: '%s'",
        r.text().c_str());
  r.reset();
  CHECK(r.add(0, 2, "a") && !r.add(1, 3, "b") && !r.add(5, 2, "c") && !r.add(-1, 2, "d") && !r.complete(),
        "parts that don't fit are refused");
  r.reset();
  CHECK(!r.add(0, agent::Report::kMaxParts + 1, "a") && !r.add(0, 0, "a"), "part counts are bounded");
  r.reset();
  CHECK(r.add(0, 2, "a") && r.add(2, 2, "c") && !r.complete(), "indices 0 and 2 of 2 never complete");
  r.reset();
  for (int i : {20, 3, 15, 1, 2, 8, 5, 4, 7, 12}) r.add(i, 20, String("p") + String(i) + " ");
  CHECK(!r.complete(), "a 20-part report waits for its first 8 parts");
  r.add(6, 20, "p6 ");
  CHECK(r.complete() && r.text().startsWith("p1 p2 p3 p4 p5 p6 p7 p8 ") && r.text().indexOf("20 parts") > 0 &&
            r.text().indexOf("p12") < 0 && r.have() <= agent::Report::kKeep + 1,
        "long report: first 8 shown, the cut noted, the rest not kept ('%s', %d kept)", r.text().c_str(), r.have());
}

static void testAgentMessages() {
  agent::Message m;
  CHECK(agent::parse("{\"v\":1,\"k\":\"hello\",\"j\":\"\",\"n\":7,\"ts\":1762444800,\"name\":\"Hermes\",\"busy\":true,\"version\":\"0.1.0\"}", m) &&
            m.kind == agent::Kind::Hello && m.name == "Hermes" && m.busy && m.n == 7 && m.job.isEmpty(),
        "hello");
  CHECK(agent::parse("{\"v\":1,\"k\":\"part\",\"j\":\"0badc0de\",\"n\":4,\"ts\":1762444800,\"i\":1,\"of\":2,\"text\":\"half\"}", m) &&
            m.kind == agent::Kind::Part && m.i == 1 && m.of == 2 && m.text == "half",
        "part");
  CHECK(agent::parse("{\"v\":1,\"k\":\"done\",\"j\":\"0badc0de\",\"n\":5,\"ts\":1762444800,\"ok\":true,\"secs\":42.5,\"cost\":0.031}", m) &&
            m.ok && m.secs == 42.5 && std::fabs(m.cost - 0.031) < 1e-6,
        "done");
  CHECK(agent::parse("{\"v\":1,\"k\":\"approval\",\"j\":\"0badc0de\",\"n\":6,\"ts\":1762444800,\"req\":\"r1\",\"tool\":\"Bash\",\"detail\":\"rm -rf /tmp/x\"}", m) &&
            m.kind == agent::Kind::Approval && m.tool == "Bash",
        "approval (phase B) parses");
  const char* bad[] = {"{\"v\":2,\"k\":\"ack\",\"j\":\"\",\"n\":1,\"ts\":1}", "{\"v\":1,\"k\":\"nope\",\"j\":\"\",\"n\":1,\"ts\":1}",
                       "{\"v\":1,\"k\":\"ack\",\"j\":\"\",\"n\":1}", "{\"v\":1,\"k\":\"ack\",\"j\":\"\",\"n\":-1,\"ts\":1}",
                       "[1,2]", "not json"};
  for (const char* b : bad) CHECK(!agent::parse(b, m), "rejected: %s", b);
  CHECK(agent::buildUp(agent::Kind::Ping, "", 1762444801, 1762444800) == "{\"v\":1,\"k\":\"ping\",\"j\":\"\",\"n\":1762444801,\"ts\":1762444800}" &&
            agent::buildUp(agent::Kind::Cancel, "0badc0de", 2, 5) == "{\"v\":1,\"k\":\"cancel\",\"j\":\"0badc0de\",\"n\":2,\"ts\":5}",
        "ping and cancel carry no text");
  const String job = agent::newJobId();
  CHECK(job.length() == 8 && strspn(job.c_str(), "0123456789abcdef") == 8, "job id %s", job.c_str());
}

static void testRelay() {
  agent::RelayEvent e;
  CHECK(agent::parseRelayLine("{\"id\":\"hwQ2YpKdmg\",\"time\":1762444800,\"event\":\"open\",\"topic\":\"cp-2QNwEsFuNcN6LqaSzNRoCg\"}", e) &&
            e.event == "open" && e.id == "hwQ2YpKdmg",
        "open line");
  CHECK(agent::parseRelayLine("{\"id\":\"p7sbbmrVYd\",\"time\":1762444845,\"event\":\"keepalive\",\"topic\":\"cp-2QNwEsFuNcN6LqaSzNRoCg\"}", e) &&
            e.event == "keepalive",
        "keepalive line");
  CHECK(agent::parseRelayLine("{\"id\":\"sPs71M8A2T\",\"time\":1762444850,\"expires\":1762488050,\"event\":\"message\","
                              "\"topic\":\"cp-2QNwEsFuNcN6LqaSzNRoCg\",\"message\":\"oKGio6Slpqe\"}", e) &&
            e.event == "message" && e.id == "sPs71M8A2T" && e.message == "oKGio6Slpqe",
        "message line");
  CHECK(!agent::parseRelayLine("", e) && !agent::parseRelayLine("{\"id\":\"x\"}", e) && !agent::parseRelayLine("<html>", e),
        "non-events rejected");

  agent::RelayUrl r;
  CHECK(agent::parseRelay("https://ntfy.sh", r) && r.tls && r.host == "ntfy.sh" && r.port == 443 && r.path.isEmpty(), "default relay");
  CHECK(agent::parseRelay("http://192.168.1.20:8080/ntfy/", r) && !r.tls && r.host == "192.168.1.20" && r.port == 8080 &&
            r.path == "/ntfy",
        "LAN relay: %s %d %s", r.host.c_str(), r.port, r.path.c_str());
  for (const char* bad : {"ftp://ntfy.sh", "https://", "ntfy.sh", "https://ntfy .sh", "https://a:0", "https://a:99999",
                          "https://a:8x", "https://a/x?y=1", "https://u@a"}) {
    CHECK(!agent::parseRelay(bad, r), "relay URL rejected: %s", bad);
  }
  agent::parseRelay("https://ntfy.sh", r);
  CHECK(agent::streamRequest(r, "cp-2QNwEsFuNcN6LqaSzNRoCg", "sPs71M8A2T") ==
            String("GET /cp-2QNwEsFuNcN6LqaSzNRoCg/json?since=sPs71M8A2T HTTP/1.0\r\nHost: ntfy.sh\r\nUser-Agent: ") +
                agent::kUserAgent + "\r\n\r\n",
        "stream request");
  const String post = agent::publishRequest(r, "cp-lQakgFwxcIsQkFbCO6dVXw", "abc");
  CHECK(post.startsWith("POST /cp-lQakgFwxcIsQkFbCO6dVXw HTTP/1.0\r\n") && post.indexOf("\r\nX-Firebase: no\r\n") > 0 &&
            post.indexOf("Content-Length: 3\r\n\r\nabc") > 0,
        "publish request");
  CHECK(agent::statusCode("HTTP/1.1 429 Too Many Requests") == 429 && agent::statusCode("garbage") == 0, "status line");
  CHECK(agent::retryAfter("Retry-After: 17", 30) == 17 && agent::retryAfter("retry-after: Wed, 21 Oct", 30) == 30 &&
            agent::retryAfter("X-Other: 5", 30) == 30,
        "Retry-After");
  CHECK(agent::backoffMs(0) == 1000 && agent::backoffMs(3) == 8000 && agent::backoffMs(20) == 60000, "backoff");
}

// ---------- agents on the setup page and in NVS ----------
static const char* kCode2 = "cp1-_____________________________________________w";

static void testAgentForm() {
  phone::Form f = phone::parseForm("aname=+Hermes+&acode=+" + String(kCode) + "%0A&arelay=https%3A%2F%2Fntfy.sh&aremove=0&aremove=2&aremove=x&aremove=12");
  CHECK(f.agentName == "Hermes" && f.agentCode == kCode && f.agentRelay == "https://ntfy.sh" &&
            f.removeAgents == std::vector<int>({0, 2}),
        "agent fields: '%s' '%s' removes %zu", f.agentName.c_str(), f.agentRelay.c_str(), f.removeAgents.size());
  const std::vector<phone::AgentRow> four = {{"A", ""}, {"B", ""}, {"C", ""}, {"Hermes", "https://ntfy.sh"}};
  struct { const char* what; const char* body; std::vector<phone::AgentRow> existing; const char* want; } cases[] = {
      {"a new agent", "aname=Hermes&acode=cp1-AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8", {}, ""},
      {"LAN relay", "aname=Lab&acode=cp1-AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8&arelay=http%3A%2F%2F10.0.0.5%3A8080", {}, ""},
      {"no name", "acode=cp1-AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8", {}, "name and paste"},
      {"relay alone", "arelay=https%3A%2F%2Fntfy.sh", {}, "name and paste"},
      {"bad code", "aname=Hermes&acode=cp1-short", {}, "pairing code"},
      {"bad relay", "aname=Hermes&acode=cp1-AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8&arelay=ntfy.sh", {}, "relay"},
      {"long name", "aname=AAAAAAAAAAAAAAAAAAAAAAAAA&acode=cp1-AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8", {}, "24"},
      {"a fifth agent", "aname=Fifth&acode=cp1-AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8", four, "room for 4"},
      {"re-pairing one of four", "aname=Hermes&acode=cp1-AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8", four, ""},
      {"a fifth after a removal", "aname=Fifth&acode=cp1-AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8&aremove=1", four, ""},
  };
  for (const auto& c : cases) {
    const String got = phone::validate(phone::parseForm(c.body), true, kModelChoices, c.existing);
    CHECK(String(c.want).isEmpty() ? got.isEmpty() : got.indexOf(c.want) >= 0, "%s: '%s'", c.what, got.c_str());
  }

  phone::Current now = phone::currentFrom("HomeNet", "", "", "claude-haiku-5-5", "UTC0");
  now.agents = {{"Hermes", ""}, {"Lab", "http://10.0.0.5:8080"}};
  phone::Form typed = phone::parseForm(String("aname=Ops&acode=") + kCode2 + "&arelay=https%3A%2F%2Fexample.org");
  const String html = phone::page(now, kModelChoices, typed, "There's room for 4 agents.");
  CHECK(leaks(html, String(kCode2).substring(4)) == 0 && leaks(html, String(kCode).substring(4)) == 0,
        "no pairing code reaches the page");
  CHECK(html.indexOf("<span>Hermes</span><small>ntfy.sh") >= 0 && html.indexOf("<span>Lab</span><small>10.0.0.5") >= 0 &&
            html.indexOf("value=\"Ops\"") >= 0 && html.indexOf("value=\"https://example.org\"") >= 0,
        "agents listed by name and relay host; name and relay kept after an error");
}

static void testAgentStore() {
  g_nvs.clear();
  std::vector<AgentEntry> list;
  editAgents(list, {}, {"Hermes", kCode, ""});
  editAgents(list, {}, {"Lab", kCode2, "http://10.0.0.5:8080"});
  CHECK(saveAgents(list), "saved");
  std::vector<AgentEntry> back = loadAgents();
  CHECK(back.size() == 2 && back[0].name == "Hermes" && back[0].code == kCode && back[0].relay.isEmpty() &&
            back[1].relay == "http://10.0.0.5:8080",
        "two agents round trip (%zu)", back.size());
  editAgents(back, {0}, {"Lab", kCode, ""});
  CHECK(back.size() == 1 && back[0].name == "Lab" && back[0].code == kCode && back[0].relay.isEmpty(),
        "remove one and re-pair the other by name");
  saveAgents(back);
  CHECK(loadAgents().size() == 1 && !g_nvs["cputer"].count("ag1n") && !g_nvs["cputer"].count("ag1c") &&
            !g_nvs["cputer"].count("ag0r"),
        "no stale slot or secret left behind");
}

// ---------- the rename: SD folder ----------
static void testDataDirMigration() {
  SD.reset();
  SD.dirs.insert("/claudeputer");
  SD.dirs.insert("/claudeputer/sessions");
  putFile("/claudeputer/config.txt", "GROQ_API_KEY=gsk_x\n");
  putFile("/claudeputer/sessions/20261106-143205.jsonl", "{\"v\":1,\"title\":\"kept\",\"model\":\"m\",\"created\":5}\n");
  CHECK(migrateDataDir() == Migration::Moved, "old folder moved");
  CHECK(SD.exists("/kloudputer/config.txt") && !SD.exists("/claudeputer") && sessions::count() == 1 &&
            sessions::list(5).items[0].title == "kept",
        "config and sessions found under /kloudputer");
  CHECK(migrateDataDir() == Migration::None, "a second boot leaves it alone");
  SD.reset();
  SD.dirs.insert("/claudeputer");
  SD.dirs.insert("/kloudputer");
  putFile("/kloudputer/config.txt", "new");
  CHECK(migrateDataDir() == Migration::None && SD.exists("/claudeputer") && fileText("/kloudputer/config.txt") == "new",
        "an existing /kloudputer is never overwritten");
  SD.reset();
  CHECK(migrateDataDir() == Migration::None, "nothing to move on a fresh card");
}


static void testPromptHistory() {
  prompts::History h;
  String out;
  CHECK(!h.older("", out) && !h.newer(out), "an empty history recalls nothing");
  h.add("first");
  h.add("second");
  h.add("second");
  h.add("");
  h.add("/pair cp1-secretsecretsecret");
  h.add("third");
  CHECK(h.older("half typed", out) && out == "third", "Up gives the newest, got '%s'", out.c_str());
  CHECK(h.older("ignored", out) && out == "second", "then older");
  CHECK(h.older("ignored", out) && out == "first", "then the oldest");
  CHECK(!h.older("ignored", out) && out == "first", "no older than the oldest, and out is untouched");
  CHECK(h.newer(out) && out == "second", "Down goes newer");
  CHECK(h.newer(out) && out == "third", "to the newest");
  CHECK(h.newer(out) && out == "half typed", "past the newest returns what you were typing, got '%s'", out.c_str());
  CHECK(!h.newer(out), "and then there is nothing newer");
  CHECK(h.older("", out) && out == "third", "browsing starts over after reaching the draft");
  h.stop();
  CHECK(!h.newer(out), "typing over a recalled prompt ends browsing");
  CHECK(h.older("new draft", out) && out == "third", "a new browse starts at the newest");
  h.add("fourth");
  CHECK(h.older("", out) && out == "fourth", "sending a prompt ends browsing and joins the history");
  for (int i = 0; i < 40; ++i) h.add(String("p") + String(i));
  int n = 0;
  String last;
  h.stop();
  while (h.older("", out)) { last = out; ++n; }
  CHECK(n == (int)prompts::History::kMax && last == "p10", "keeps the newest %d, oldest '%s' after %d", (int)prompts::History::kMax, last.c_str(), n);
}

static void testLanguage() {
  CHECK(String(lang::kLanguages[0].code) == "" && String(lang::kLanguages[0].name) == "Auto", "Auto is first and sends nothing");
  CHECK(lang::indexOf("") == 0 && lang::indexOf("de") != 0, "known codes map to their row");
  CHECK(String(lang::kLanguages[lang::indexOf("fr")].name) == "French", "round trip");
  CHECK(lang::indexOf("xx") == 0 && lang::indexOf("ru") == 0, "unknown and non-Latin codes fall back to Auto");
  CHECK(lang::step(0, -1) == lang::kCount - 1 && lang::step(lang::kCount - 1, 1) == 0, "stepping wraps both ways");
  CHECK(lang::step(3, 1) == 4 && lang::step(3, -1) == 2, "and moves one at a time");
  CHECK(lang::formField("B", "") == "", "Auto adds no field");
  CHECK(lang::formField("B", "de") == "--B\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\nde\r\n",
        "one well-formed multipart part, got '%s'", lang::formField("B", "de").c_str());
  std::set<std::string> seen;
  for (const auto& l : lang::kLanguages) CHECK(seen.insert(l.code).second, "duplicate code %s", l.code);
}

static JsonDocument parseJson(const String& s) {
  JsonDocument d;
  CHECK(!deserializeJson(d, s.c_str()), "valid json: %s", s.c_str());
  return d;
}

static void testChatRequest() {
  std::vector<ChatMsg> h{{true, "hi"}, {false, "hello"}, {true, "what is new?"}};
  RequestOptions plain;
  JsonDocument a = parseJson(requestBody("claude-haiku-5-5", h, plain));
  CHECK(a["tools"].isNull(), "no tools unless web search is on");
  CHECK(a["fallbacks"].isNull(), "haiku has no server-side fallback");
  CHECK(a["messages"].size() == 3 && String((const char*)a["messages"][1]["role"]) == "assistant", "roles alternate");
  CHECK(String((const char*)a["system"]) == kSystemPrompt, "plain system prompt is unchanged");

  RequestOptions web;
  web.webSearch = true;
  JsonDocument b = parseJson(requestBody("claude-sonnet-5-5", h, web));
  CHECK(b["tools"].size() == 1 && String((const char*)b["tools"][0]["name"]) == "web_search", "one search tool");
  CHECK(String((const char*)b["tools"][0]["type"]) == "web_search_20250305", "basic variant that every model takes");
  CHECK((int)b["tools"][0]["max_uses"] == kMaxSearches, "search count is capped");
  CHECK(String((const char*)b["fallbacks"]) == "default", "sonnet keeps its fallback");
  CHECK(String((const char*)b["system"]).indexOf("You can search the web") >= 0, "the prompt says search is available");

  RequestOptions spoken;
  spoken.spoken = true;
  CHECK(String((const char*)parseJson(requestBody("claude-haiku-5-5", h, spoken))["system"]).indexOf("spoken aloud") >= 0, "spoken prompt added");

  RequestOptions notes;
  notes.notes = "my vm is atlas\nI prefer metric";
  const String sys = (const char*)parseJson(requestBody("claude-haiku-5-5", h, notes))["system"];
  CHECK(sys.indexOf("my vm is atlas\nI prefer metric") >= 0 && sys.startsWith(kSystemPrompt), "notes follow the base prompt");
  RequestOptions huge;
  for (int i = 0; i < 400; ++i) huge.notes += "0123456789";
  CHECK(systemPrompt(huge).length() < String(kSystemPrompt).length() + kMaxNotesChars + 200, "notes are capped, got %u", (unsigned)systemPrompt(huge).length());
}

static void testChatReply() {
  // What a real search answer looks like: text, the search call, its results, then more text with citations.
  const char* wire = R"({"id":"msg_1","type":"message","role":"assistant","model":"claude-sonnet-5-5",
    "content":[{"type":"text","text":"Let me check. "},
      {"type":"server_tool_use","id":"srvtoolu_1","name":"web_search","input":{"query":"cms-api status"}},
      {"type":"web_search_tool_result","tool_use_id":"srvtoolu_1","content":[{"type":"web_search_result","url":"https://example.com","title":"Status","encrypted_content":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","page_age":"1 day"}]},
      {"type":"text","text":"It is up.","citations":[{"type":"web_search_result_location","url":"https://example.com","cited_text":"all good"}]}],
    "stop_reason":"end_turn","usage":{"input_tokens":812,"output_tokens":64,"server_tool_use":{"web_search_requests":1}}})";
  JsonDocument filter;
  replyFilter(filter);
  JsonDocument resp;
  CHECK(!deserializeJson(resp, wire, DeserializationOption::Filter(filter)), "filtered parse");
  CHECK(resp["content"][2]["encrypted_content"].isNull() && resp["content"][2]["content"].isNull(), "search results never reach RAM");
  ChatResult r;
  readReply(resp, r);
  CHECK(r.ok && r.text == "Let me check. It is up.", "text blocks join and tool blocks are skipped, got '%s'", r.text.c_str());
  CHECK(r.inTok == 812 && r.outTok == 64 && r.searches == 1, "usage and search count");

  JsonDocument plainResp = parseJson(R"({"content":[{"type":"text","text":"Hi"}],"stop_reason":"max_tokens","usage":{"input_tokens":1,"output_tokens":2}})");
  ChatResult p;
  readReply(plainResp, p);
  CHECK(p.text == "Hi [cut off]" && p.searches == 0, "no search usage reads as zero, got '%s'", p.text.c_str());
  JsonDocument refusal = parseJson(R"({"content":[],"stop_reason":"refusal","usage":{}})");
  ChatResult q;
  readReply(refusal, q);
  CHECK(q.ok && q.text == "(Claude declined this one.)", "refusal text");
  JsonDocument paused = parseJson(R"({"content":[{"type":"text","text":"Partway"}],"stop_reason":"pause_turn","usage":{}})");
  ChatResult z;
  readReply(paused, z);
  CHECK(z.text == "Partway [search paused]", "a paused search turn is labeled");
}

static void testSearchRejected() {
  CHECK(searchRejected(400, "The web_search tool is not enabled for your organization"), "400 naming the tool");
  CHECK(searchRejected(403, "Web search is not permitted"), "403 naming web search");
  CHECK(!searchRejected(400, "max_tokens must be positive"), "unrelated 400 is not retried");
  CHECK(!searchRejected(429, "web_search rate limit"), "a rate limit is not a refusal of the tool");
  CHECK(!searchRejected(200, "web_search"), "success never retries");
}

static void testNotes() {
  notes::Notes n;
  CHECK(n.add("  my vm   is\tatlas\n") == notes::Add::Added && n.items()[0] == "my vm is atlas", "whitespace is tidied, got '%s'", n.items()[0].c_str());
  CHECK(n.add("my vm is atlas") == notes::Add::Duplicate && n.items().size() == 1, "an exact repeat is not stored twice");
  CHECK(n.add(" \n ") == notes::Add::Empty, "blank is refused");
  String longNote;
  for (int i = 0; i < 40; ++i) longNote += "abcdefgh ";
  n.add(longNote);
  CHECK(n.items()[1].length() <= notes::kMaxChars, "long notes are cut to %d, got %u", (int)notes::kMaxChars, (unsigned)n.items()[1].length());
  CHECK(n.joined() == String("- my vm is atlas\n- ") + n.items()[1], "joined as a list without a trailing newline");
  for (int i = 0; i < 30; ++i) n.add(String("note ") + String(i));
  CHECK(n.items().size() == notes::kMaxNotes && n.add("one more") == notes::Add::Full, "stops at %d notes", (int)notes::kMaxNotes);
  CHECK(!n.remove(0) && !n.remove(notes::kMaxNotes + 1) && n.remove(1) && n.items()[0] != "my vm is atlas", "remove is 1-based and checks the range");
  n.clear();
  CHECK(n.items().empty() && n.joined() == "", "clear");

  notes::Notes p;
  p.parse("# my notes\n\nfirst one\r\n  second   one  \nfirst one\nlast without newline");
  CHECK(p.items().size() == 4 && p.items()[0] == "first one" && p.items()[1] == "second one" && p.items()[3] == "last without newline",
        "comments and blanks skipped, got %u", (unsigned)p.items().size());
  notes::Notes round;
  round.parse(p.serialize());
  CHECK(round.items() == p.items(), "serialize then parse is the same list");

  SD.reset();
  SD.dirs.insert(DATA_DIR);
  CHECK(notes::load().items().empty(), "no file means no notes");
  CHECK(notes::save(p), "saved");
  CHECK(notes::load().items() == p.items(), "reloaded after saving");
  CHECK(SD.files.count(String(notes::kPath).str() + ".tmp") == 0, "the temp file is gone");
  notes::Notes smaller = p;
  smaller.remove(1);
  CHECK(notes::save(smaller) && notes::load().items().size() == 3, "saving again replaces the old list");
  SD.readOnly = true;
  CHECK(!notes::save(p), "a refusing card reports failure");
  SD.readOnly = false;
  CHECK(notes::load().items().size() == 3, "and leaves the old list alone");

  String note;
  CHECK(notes::rememberText("Remember that my vm is atlas.", note) && note == "my vm is atlas.", "spoken form, got '%s'", note.c_str());
  CHECK(notes::rememberText("/remember buy milk", note) && note == "buy milk", "command form");
  CHECK(!notes::rememberText("remember when we talked about it?", note), "a bare remember is a question for Claude");
  CHECK(!notes::rememberText("do you remember that song", note), "only at the start");
}

static void testScreenSleep() {
  using power::Level;
  power::ScreenSleep z;
  const uint32_t T = 60000;
  auto at = [&](uint32_t now, bool key = false) { return z.tick(now, T, key); };
  CHECK(at(0).level == Level::Awake && !at(1000).changed, "awake at first, nothing to change");
  CHECK(at(T - 1).level == Level::Awake, "still awake just before the timeout");
  auto dim = at(T);
  CHECK(dim.level == Level::Dim && dim.changed && !dim.swallow, "dims at the timeout");
  CHECK(!at(T + 5000).changed, "and only tells the backlight once");
  CHECK(at(2 * T - 1).level == Level::Dim, "dim until twice the timeout");
  auto off = at(2 * T);
  CHECK(off.level == Level::Off && off.changed, "off at twice the timeout");
  auto wake = at(2 * T + 10, true);
  CHECK(wake.level == Level::Awake && wake.changed && wake.swallow, "a key wakes it and is swallowed after off");
  CHECK(at(2 * T + 20).level == Level::Awake && at(2 * T + T - 1).level == Level::Awake, "the clock restarted at the key");
  at(3 * T + 100);
  auto fromDim = at(3 * T + 200, true);
  CHECK(fromDim.level == Level::Awake && fromDim.changed && !fromDim.swallow, "a key from dim wakes and is typed");
  at(10 * T);
  auto poked = z.poke(10 * T + 1, T);
  CHECK(poked.level == Level::Awake && poked.changed && !poked.swallow, "a poke wakes it without swallowing anything");
  CHECK(z.tick(10 * T + 2, T, false).level == Level::Awake, "and restarts the clock");

  power::ScreenSleep n;
  CHECK(n.tick(100000000u, 0, false).level == Level::Awake && !n.tick(200000000u, 0, false).changed, "Never keeps it awake");
  power::ScreenSleep w;
  w.tick(4294967000u, T, true);
  CHECK(w.tick(4294967000u + 100u, T, false).level == Level::Awake, "just after the counter wraps is not a huge idle time");
  CHECK(w.tick(4294967000u + T + 100u, T, false).level == Level::Dim, "and the timeout still counts across the wrap");

  CHECK(power::optionFor(0) == 0 && power::optionFor(120000) == 3, "saved values map to their option");
  CHECK(power::optionFor(12345) == power::kDefaultOption, "an unknown saved value gets the default");
  CHECK(power::kOptions[power::kDefaultOption].ms == 60000, "default is one minute");
  CHECK(power::stepOption(0, -1) == power::kOptionCount - 1 && power::stepOption(power::kOptionCount - 1, 1) == 0, "options wrap");
}

static void testTitles() {
  CHECK(sessions::cleanTitle("\"Fixing the cms-api deploy.\"") == "Fixing the cms-api deploy", "quotes and the final period go, got '%s'", sessions::cleanTitle("\"Fixing the cms-api deploy.\"").c_str());
  CHECK(sessions::cleanTitle("**Tea brewing times**\nextra line") == "Tea brewing times", "first line only, no markdown stars");
  CHECK(sessions::cleanTitle("") == "" && sessions::cleanTitle("\"\"") == "", "nothing in, nothing out");
  CHECK(sessions::cleanTitle("Caf\xC3\xA9 plans!") == "Caf? plans", "non-ASCII becomes ? like everywhere else");
  CHECK(sessions::cleanTitle("A title that is clearly much longer than the screen can show").length() <= sessions::kTitleChars + 3, "long titles are cut");

  CHECK(sessions::matches("Fixing the cms-api deploy", "") && sessions::matches("Fixing the cms-api deploy", "   "), "empty query matches all");
  CHECK(sessions::matches("Fixing the cms-api deploy", "CMS") && sessions::matches("Fixing the cms-api deploy", "deploy fix"), "case-insensitive, any order");
  CHECK(!sessions::matches("Fixing the cms-api deploy", "deploy tea"), "every word must appear");

  SD.reset();
  SD.dirs.insert(DATA_DIR);
  SD.dirs.insert(sessions::kDir);
  sessions::Meta m;
  m.id = "20261106-143205";
  m.title = "first prompt text";
  m.model = "claude-haiku-5-5";
  m.created = 1762444800;
  CHECK(sessions::append(m, "first prompt text", "an answer, with \"quotes\"\nand a newline"), "saved");
  CHECK(sessions::retitle(m.id, "Tea brewing times"), "retitled");
  const auto& bytes = SD.files[String(sessions::pathOf(m.id)).str()];
  const std::string text(bytes.begin(), bytes.end());
  CHECK(text.find("Tea brewing times") != std::string::npos && text.find("first prompt text\"}") != std::string::npos, "new title in the header, turns untouched");
  CHECK(SD.files.count(String(sessions::pathOf(m.id)).str() + ".tmp") == 0, "no temp file left");
  sessions::Meta back;
  std::vector<std::pair<String, String>> turns;
  CHECK(sessions::load(m.id, back, [&](const String& u, const String& a) { turns.push_back({u, a}); }), "still loads");
  CHECK(back.title == "Tea brewing times" && back.model == "claude-haiku-5-5" && back.created == 1762444800, "header fields survive, got '%s'", back.title.c_str());
  CHECK(turns.size() == 1 && turns[0].second == "an answer, with \"quotes\"\nand a newline", "the turn is intact");
  CHECK(sessions::append(back, "second", "reply two"), "and the chat can go on");
  CHECK(!sessions::retitle("20990101-000000", "x"), "a missing session is refused");
  SD.readOnly = true;
  CHECK(!sessions::retitle(m.id, "Other"), "a refusing card reports failure");
  SD.readOnly = false;
  sessions::Meta again;
  sessions::load(m.id, again, [](const String&, const String&) {});
  CHECK(again.title == "Tea brewing times", "and leaves the old title");

  const auto msgs = titleMessages("p", "r");
  CHECK(msgs.size() == 1 && msgs[0].user && msgs[0].text.startsWith("Prompt: p\nAnswer: r"), "title request carries both sides");
  String big;
  for (int i = 0; i < 100; ++i) big += "0123456789";
  CHECK(titleMessages(big, big)[0].text.length() < 700, "and keeps them short");
  const RequestOptions to = titleOptions();
  JsonDocument d = parseJson(requestBody(kTitleModel, msgs, to));
  CHECK((int)d["max_tokens"] == 24 && String((const char*)d["system"]) == to.system && d["tools"].isNull(), "tiny, own prompt, no tools");
}

static void testApprovals() {
  using approvals::Answer;
  CHECK(agent::buildApprove("0badc0de", 3, 1762444800, "a1b2c3d4", true) == "{\"v\":1,\"k\":\"approve\",\"j\":\"0badc0de\",\"n\":3,\"ts\":1762444800,\"req\":\"a1b2c3d4\",\"allow\":true}",
        "allow wire form, got %s", agent::buildApprove("0badc0de", 3, 1762444800, "a1b2c3d4", true).c_str());
  CHECK(agent::buildApprove("0badc0de", 4, 1, "a1b2c3d4", false).indexOf("\"allow\":false") > 0, "deny is an explicit false");
  agent::Message m;
  CHECK(agent::parse(agent::buildApprove("0badc0de", 3, 1762444800, "a1b2c3d4", true), m) && m.kind == agent::Kind::Approve && m.req == "a1b2c3d4", "our own message parses");

  CHECK(approvals::answerFrom("y") == Answer::Allow && approvals::answerFrom("Y") == Answer::Allow, "y allows");
  CHECK(approvals::answerFrom("n") == Answer::Deny && approvals::answerFrom("N") == Answer::Deny, "n denies");
  CHECK(approvals::answerFrom("") == Answer::None && approvals::answerFrom("a") == Answer::None && approvals::answerFrom(" ") == Answer::None, "other keys decide nothing");
  CHECK(approvals::answerFrom("ny") == Answer::Deny && approvals::answerFrom("yn") == Answer::Allow, "the first of two fast presses wins");
  CHECK(approvals::answerFrom("xy") == Answer::Allow, "a stray key before it is skipped");

  approvals::Queue q;
  CHECK(q.empty() && q.expire(100) == 0, "empty to start");
  CHECK(q.add("r1", "Bash", "ls", 1000) == approvals::Queue::Add::Kept && q.add("r2", "Edit", "/x", 2000) == approvals::Queue::Add::Kept, "kept in order");
  CHECK(q.add("r1", "Bash", "other", 3000) == approvals::Queue::Add::Repeat && q.size() == 2, "a repeated request id is ignored");
  CHECK(q.add("", "Bash", "ls", 3000) == approvals::Queue::Add::Invalid, "a request without an id is refused");
  CHECK(q.front().req == "r1" && q.front().detail == "ls", "first in, first shown");
  CHECK(q.pop().req == "r1" && q.front().req == "r2", "answering moves to the next");

  approvals::Queue t;
  t.add("a", "Bash", "x", 1000);
  t.add("b", "Bash", "y", 1000 + approvals::kTtlMs - 1);
  CHECK(t.expire(1000 + approvals::kTtlMs - 1) == 0, "nothing expires before five minutes");
  CHECK(t.expire(1000 + approvals::kTtlMs) == 1 && t.front().req == "b", "the old one goes at five minutes, the newer stays");
  CHECK(t.expire(1000 + 2 * approvals::kTtlMs) == 1 && t.empty(), "and then the rest");
  approvals::Queue w;
  w.add("a", "Bash", "x", 4294967000u);
  CHECK(w.expire(4294967000u + 1000) == 0 && w.expire(4294967000u + approvals::kTtlMs) == 1, "expiry survives the millis() wrap");

  approvals::Queue full;
  for (size_t i = 0; i < approvals::kMaxWaiting; ++i) CHECK(full.add(String("r") + String((int)i), "Bash", "x", 0) == approvals::Queue::Add::Kept, "fits %d", (int)i);
  CHECK(full.add("overflow", "Bash", "x", 0) == approvals::Queue::Add::Full, "the ninth is refused so the caller can deny it");
  const auto all = full.takeAll();
  CHECK(all.size() == approvals::kMaxWaiting && full.empty(), "takeAll empties it for a fail-closed denial");
}

static void testUsage() {
  bool known = false;
  CHECK(usage::costMicros("claude-haiku-5-5", 1000000, 0, 0, &known) == 100000 && known, "a million Haiku input tokens is $0.10");
  CHECK(usage::costMicros("claude-haiku-5-5", 0, 1000000, 0) == 500000, "and $0.50 out");
  CHECK(usage::costMicros("claude-sonnet-5-5", 812, 64, 0) == 2264, "812 in and 64 out on Sonnet is 2264 micro-dollars, got %u", usage::costMicros("claude-sonnet-5-5", 812, 64, 0));
  CHECK(usage::costMicros("claude-fable-5-1", 100, 100, 2) == 1000 + 5000 + 20000, "searches add a cent each");
  CHECK(usage::costMicros("my-own-model", 5000, 5000, 1, &known) == 10000 && !known, "an unpriced model counts only its searches and says so");
  CHECK(usage::costMicros("claude-haiku-5-5", -5, -5, -1) == 0, "nonsense counts cost nothing");
  CHECK(usage::costMicros("claude-haiku-5-5", 7, 0, 0) == 1, "rounds to the nearest millionth (0.7 -> 1)");
  CHECK(usage::costMicros("claude-haiku-5-5", 4, 0, 0) == 0, "and down (0.4 -> 0)");

  CHECK(usage::label("claude-haiku-5-5") == "$0.1/$0.5" && usage::label("claude-sonnet-5-5") == "$2/$10" && usage::label("claude-fable-5-1") == "$10/$50",
        "menu labels, got %s", usage::label("claude-haiku-5-5").c_str());
  CHECK(usage::label("other") == "?", "unknown model has no label");

  CHECK(usage::money(0) == "$0" && usage::money(400) == "$0.0004" && usage::money(9999) == "$0.0100", "under a cent shows four decimals, got %s", usage::money(9999).c_str());
  CHECK(usage::money(10000) == "$0.01" && usage::money(123456) == "$0.12" && usage::money(12300000) == "$12.30", "a cent and up shows two");

  usage::Tally t;
  usage::add(t, 500, 0);
  CHECK(t.chat == 500 && t.day == 500 && t.total == 500 && t.date == 0, "before the clock is set it still adds up");
  usage::add(t, 300, 20261109);
  CHECK(t.date == 20261109 && t.day == 300 && t.total == 800 && t.chat == 800, "the first dated spend starts that day's count");
  usage::add(t, 200, 20261109);
  CHECK(t.day == 500, "same day accumulates");
  usage::add(t, 50, 0);
  CHECK(t.day == 550 && t.date == 20261109, "a spend with no clock stays in the current day");
  CHECK(usage::spentToday(t, 20261109) == 550 && usage::spentToday(t, 20261110) == 0 && usage::spentToday(t, 0) == 550, "today reads zero once the date has moved on");
  usage::add(t, 70, 20261110);
  CHECK(t.day == 70 && t.total == 1120 && t.chat == 1120, "a new day restarts the day and keeps the total");
  usage::Tally big;
  big.total = 4294967000u;
  usage::add(big, 1000, 1);
  CHECK(big.total == 4294967295u, "totals stop at the top instead of wrapping to zero");
}

static void testHistoryFile() {
  SD.reset();
  SD.dirs.insert(DATA_DIR);
  prompts::History h;
  size_t bytes = prompts::load(h);
  CHECK(bytes == 0 && h.size() == 0, "no file, no history");
  for (const char* line : {"first", "second one", "/pair X cp1-secret", "second one", "third"}) {
    if (h.add(line)) CHECK(prompts::append(h, line, bytes), "appended %s", line);
  }
  const auto& file = SD.files[String(prompts::kPath).str()];
  CHECK(std::string(file.begin(), file.end()) == "first\nsecond one\nthird\n", "one prompt per line, no pairing code, no repeat");
  CHECK(bytes == file.size(), "the running size matches the file");

  prompts::History back;
  CHECK(prompts::load(back) == file.size() && back.size() == 3, "reloaded after a restart");
  String out;
  CHECK(back.older("", out) && out == "third" && back.older("", out) && out == "second one", "newest first on Up");

  prompts::History many;
  size_t n = 0;
  for (int i = 0; i < 100; ++i) {
    const String line = String("prompt ") + String(i);
    if (many.add(line)) prompts::append(many, line, n);
  }
  prompts::History kept;
  prompts::load(kept);
  CHECK(kept.size() == prompts::History::kMax, "only the newest %d come back", (int)prompts::History::kMax);
  CHECK(kept.older("", out) && out == "prompt 99", "and they are the newest");

  size_t nearFull = prompts::kMaxFileBytes - 3;
  CHECK(many.add("one more") && prompts::append(many, "one more", nearFull), "a full file is rewritten");
  CHECK(nearFull == many.serialize().length() && SD.files[String(prompts::kPath).str()].size() == nearFull, "down to what is remembered");
  SD.readOnly = true;
  size_t z = 0;
  CHECK(!prompts::append(many, "x", z) && z == 0, "a refusing card reports failure");
  SD.readOnly = false;
}

static void testSessionSearch() {
  SD.reset();
  SD.dirs.insert(DATA_DIR);
  SD.dirs.insert(sessions::kDir);
  sessions::Meta m;
  m.id = "20261106-143205";
  m.title = "Kubernetes deploy";
  m.model = "claude-haiku-5-5";
  String filler;
  for (int i = 0; i < 60; ++i) filler += "lorem ipsum ";
  CHECK(sessions::append(m, "how do I restart a pod?", filler + "use kubectl rollout RESTART on the Deployment"), "saved");
  CHECK(sessions::append(m, "and the borscht recipe", "beets, cabbage, dill"), "saved again");
  CHECK(sessions::contains(m.id, "restart"), "a word from a prompt");
  CHECK(sessions::contains(m.id, "ROLLOUT deployment"), "words from a reply, any case");
  CHECK(sessions::contains(m.id, "borscht kubectl"), "words from different turns");
  CHECK(!sessions::contains(m.id, "restart pizza"), "every word has to be there");
  CHECK(!sessions::contains(m.id, "kubernetes") && !sessions::contains(m.id, "haiku"), "the header line (title, model) is not searched");
  CHECK(sessions::contains(m.id, "") && sessions::contains(m.id, "  "), "an empty query matches");
  CHECK(!sessions::contains("20990101-000000", "restart"), "a missing chat matches nothing");
  for (size_t pad = 0; pad < 300; ++pad) {  // the word lands on every position of the 256-byte read boundary
    sessions::Meta p;
    p.id = "20261107-000000";
    SD.files.erase(String(sessions::pathOf(p.id)).str());
    String lead;
    for (size_t i = 0; i < pad; ++i) lead += 'x';
    sessions::append(p, lead + " needle", "ok");
    if (!sessions::contains(p.id, "needle")) {
      CHECK(false, "missed the word when it straddles a read boundary (pad %u)", (unsigned)pad);
      break;
    }
  }
  CHECK(true, "boundary sweep done");
}

int main() {
  testChunks();
  testPromptHistory();
  testHistoryFile();
  testSessionSearch();
  testLanguage();
  testChatRequest();
  testChatReply();
  testSearchRejected();
  testNotes();
  testScreenSleep();
  testTitles();
  testParse();
  testResampler();
  testPlayback();
  testVoicing();
  testLimiter();
  testSessionFormat();
  testSessionResume();
  testSessionDamage();
  testSessionIds();
  testSessionTitles();
  testSessionDelete();
  testSessionWhen();
  testUrlDecode();
  testParseForm();
  testBody();
  testValidate();
  testNeverEcho();
  testTzMeaning();
  testApCredentials();
  testPrecedence();
  testDnsAnswers();
  testCapport();
  testClients();
  testEventLog();
  testSetupAddress();
  testAgentVector();
  testAgentTamper();
  testAgentReplay();
  testPairingCodes();
  testAgentReport();
  testAgentMessages();
  testApprovals();
  testUsage();
  testRelay();
  testAgentForm();
  testAgentStore();
  testDataDirMigration();
  printf("%d checks, %d failed\n", checks, fails);
  return fails != 0;
}
