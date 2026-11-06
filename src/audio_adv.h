// Kloudputer — direct mic capture and speaker playback on the Cardputer ADV (ES8311 codec over I2S).
//
// On IDF 5.5 the ADV mic reads a constant -1 once its pins were used by M5Unified's speaker first
// (esp-idf #18621), so M5Unified's speaker and mic stay disabled (main.cpp) and we own the hardware:
// one full-duplex ESP-IDF I2S standard controller on freshly reset pins, and the ES8311 configured
// over I2C following Espressif's driver sequence, written *after* the I2S clock is running, because
// the ES8311 silently ignores some writes (notably SYSTEM 0x0D) while it has no clock.
//
// The clock is started once and never stopped: the first recording after boot heard fine, but after
// stopping the clock and re-initialising, the codec's analog input stayed dead (digital path fine,
// ADC output 0/-1). Always-on also means push-to-talk starts instantly with no clipped first word,
// and playback shares that clock through the duplex TX channel instead of restarting anything.
#pragma once
#include <M5Cardputer.h>
#include <driver/i2s_std.h>

namespace adv_audio {
constexpr uint8_t kCodec = 0x18;  // ES8311 on the internal I2C bus
constexpr uint32_t kI2cHz = 400000;
constexpr uint32_t kRate = 16000;
constexpr gpio_num_t kBck = GPIO_NUM_41, kWs = GPIO_NUM_43, kDin = GPIO_NUM_46, kDout = GPIO_NUM_42;
constexpr size_t kDmaDescs = 8, kDmaFrames = 480;  // ~240 ms of DMA buffering covers SD write hiccups
constexpr uint8_t kDacMuted = 0x00, kDacTop = 0xBF;  // DAC volume register: -95.5 dB, 0 dB

inline i2s_chan_handle_t g_rx = nullptr;
inline i2s_chan_handle_t g_tx = nullptr;
inline uint8_t g_dacVol = kDacTop;  // applied only while playing

inline bool wr(uint8_t reg, uint8_t val) { return M5.In_I2C.writeRegister8(kCodec, reg, val, kI2cHz); }
inline uint8_t rd(uint8_t reg) { return M5.In_I2C.readRegister8(kCodec, reg, kI2cHz); }

// ES8311 as I2S slave, MCLK derived from BCLK: 16 kHz x 32 bits = 512 kHz BCLK, x8 = 256 fs.
inline void codecInit() {
  wr(0x00, 0x1F);  // reset everything M5Unified's speaker/mic setup left behind
  delay(20);
  wr(0x00, 0x00);
  static constexpr uint8_t seq[][2] = {
      {0x44, 0x08},              // GPIO: better I2C noise immunity; ADC data on both slots
      {0x45, 0x00},              // GP: no special modes
      {0x0B, 0x00}, {0x0C, 0x00},
      {0x10, 0x1F}, {0x11, 0x7F},  // system bias, as in Espressif's driver
      {0x00, 0x80},              // CSM on, slave mode
      {0x01, 0xBF},              // MCLK from the BCLK pin, ADC and DAC clocks on
      {0x02, 0x18},              // pre-divide /1, pre-multiply x8
      {0x03, 0x10},              // ADC single speed, OSR
      {0x04, 0x10},              // DAC OSR
      {0x05, 0x00},              // ADC/DAC clock dividers /1
      {0x09, 0x0C},              // serial in: I2S, 16-bit
      {0x0A, 0x0C},              // serial out: I2S, 16-bit, unmuted
      {0x13, 0x10},              // DAC output to the speaker driver
      {0x1B, 0x0A}, {0x1C, 0x6A},  // ADC high-pass / EQ bypass, cancel DC offset
      {0x17, 0xBF},              // ADC volume 0 dB
      {0x0E, 0x02},              // power up PGA + ADC modulator
      {0x12, 0x00},              // power up the DAC
      {0x32, kDacMuted},         // muted until something plays, so an idle speaker can't hiss
      {0x14, 0x17},              // MIC1P/N input, analog PGA +21 dB
      {0x0D, 0x01},              // power up analog circuitry
      {0x15, 0x40},
      {0x37, 0x08},              // DAC equalizer bypassed
  };
  for (const auto& s : seq) wr(s[0], s[1]);
  for (int i = 0; i < 3; ++i) {  // re-arm the capture path with the clock running
    delay(30);
    wr(0x0D, 0x01);
  }
}

// Chip ID (FD FE = 83 11 for an ES8311) + the registers that decide whether capture runs.
inline String codecDump() {
  static constexpr uint8_t regs[] = {0xFD, 0xFE, 0x00, 0x01, 0x02, 0x09, 0x0A, 0x0D, 0x0E, 0x14, 0x17};
  String s;
  char b[4];
  for (uint8_t r : regs) {
    snprintf(b, sizeof(b), "%02X ", rd(r));
    s += b;
  }
  return s;
}

inline bool codecPresent() { return rd(0xFD) == 0x83 && rd(0xFE) == 0x11; }

// Idempotent: brings up the I2S clock and the codec on the first call, then just reports success.
inline bool start(String& err) {
  if (g_rx) return true;
  for (gpio_num_t p : {kBck, kWs, kDin, kDout}) gpio_reset_pin(p);
  // Weak pull-down on the data line: a constant -1 then proves the codec actively drives it high,
  // while an undriven line reads 0.
  gpio_set_pull_mode(kDin, GPIO_PULLDOWN_ONLY);
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan.dma_desc_num = kDmaDescs;
  chan.dma_frame_num = kDmaFrames;
  chan.auto_clear_after_cb = true;  // TX sends zeros once we stop writing, instead of looping its last buffer
  if (i2s_new_channel(&chan, &g_tx, &g_rx) != ESP_OK) {
    g_tx = g_rx = nullptr;
    err = "i2s channel";
    return false;
  }
  i2s_std_config_t cfg = {};
  cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kRate);
  cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  cfg.gpio_cfg.bclk = kBck;
  cfg.gpio_cfg.ws = kWs;
  cfg.gpio_cfg.dout = kDout;
  cfg.gpio_cfg.din = kDin;
  if (i2s_channel_init_std_mode(g_tx, &cfg) != ESP_OK || i2s_channel_init_std_mode(g_rx, &cfg) != ESP_OK ||
      i2s_channel_enable(g_tx) != ESP_OK || i2s_channel_enable(g_rx) != ESP_OK) {
    for (i2s_chan_handle_t* h : {&g_tx, &g_rx}) {
      i2s_channel_disable(*h);
      i2s_del_channel(*h);
      *h = nullptr;
    }
    err = "i2s init";
    return false;
  }
  codecInit();  // clock is running now
  return true;
}

// Level 1-10: 10 is 0 dB, each step down is -4 dB (8 register steps of 0.5 dB).
inline void setVolume(int level) { g_dacVol = kDacTop - (10 - constrain(level, 1, 10)) * 8; }

// Queues interleaved L/R frames for the speaker; blocks while the DMA ring is full.
inline bool write(const int16_t* frames, size_t bytes) {
  size_t done = 0;
  return g_tx && i2s_channel_write(g_tx, frames, bytes, &done, pdMS_TO_TICKS(1000)) == ESP_OK && done == bytes;
}

// Pushes a full DMA ring of silence: returns once everything queued before it has been played.
inline void drain() {
  static const int16_t kZeros[2 * kDmaFrames] = {};
  for (size_t i = 0; i <= kDmaDescs; ++i) write(kZeros, sizeof(kZeros));
}

// The DAC stays muted while idle, so a stop is instant (mute, then drain) and silence is truly silent.
inline void unmute() { wr(0x32, g_dacVol); }
inline void mute() { wr(0x32, kDacMuted); }

// Reads interleaved L/R 16-bit frames; returns bytes read.
inline size_t read(int16_t* buf, size_t bytes, uint32_t timeoutMs) {
  size_t got = 0;
  if (g_rx) i2s_channel_read(g_rx, buf, bytes, &got, pdMS_TO_TICKS(timeoutMs));
  return got;
}

// Drops whatever the DMA ring captured before now, so a recording starts at the key press.
inline void flush(int16_t* buf, size_t bytes) {
  while (read(buf, bytes, 0) == bytes) {
  }
}

// While capturing: is the bit clock / word select actually on the pins, and does the data line move?
// Samples each pad's input level in a tight loop (far faster than the 512 kHz BCLK).
struct PinActivity {
  int highPct = 0;
  int toggles = 0;
};
inline PinActivity probePin(gpio_num_t pin) {
  gpio_input_enable(pin);  // output pins need their input buffer on to read back their own level
  constexpr int kN = 20000;
  int highs = 0, toggles = 0, last = gpio_get_level(pin);
  for (int i = 0; i < kN; ++i) {
    const int v = gpio_get_level(pin);
    highs += v;
    toggles += v != last;
    last = v;
  }
  return {highs * 100 / kN, toggles};
}

// Line test: make the codec send digital zeros on its serial output (ADCDAT_SEL = 0+0). If the data
// still reads non-zero, the codec isn't clocking anything out at all. Returns % of non-zero frames.
inline int lineTestNonZeroPct(int16_t* buf, size_t bytes) {
  wr(0x44, 0x38);
  delay(20);
  read(buf, bytes, 200);  // flush what was captured before the switch
  const size_t frames = read(buf, bytes, 200) / 4;
  wr(0x44, 0x08);
  size_t nz = 0;
  for (size_t i = 0; i < frames; ++i) nz += buf[2 * i] != 0 || buf[2 * i + 1] != 0;
  return frames ? static_cast<int>(nz * 100 / frames) : -1;
}
}  // namespace adv_audio
