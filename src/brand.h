// Kloudputer — the product's public name and its folder on the SD card, kept in one place.
#pragma once

// A macro, so paths stay plain string literals: DATA_DIR "/sessions".
#define DATA_DIR "/kloudputer"

namespace brand {
constexpr const char* kProduct = "Kloudputer";
constexpr const char* kTagline = "pocket client for Claude";
constexpr const char* kDisclaimer = "Unofficial. Not affiliated with Anthropic.";
constexpr const char* kOldDataDir = "/claudeputer";  // the folder before the rename; moved once at boot
}  // namespace brand
