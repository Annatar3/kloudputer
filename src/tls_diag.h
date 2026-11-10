// Kloudputer — finding out why a reply stopped early. The framework's TLS client closes the connection on any
// read error and only logs the reason, so TlsClient notes the mbedTLS code first, and CountedReader notes how
// much arrived and how long the read took. Together they tell a server that closed early from a stalled
// network from a reset.
#pragma once
#include <WiFiClientSecure.h>
#include <mbedtls/ssl.h>

class TlsClient : public WiFiClientSecure {
 public:
  int lastReadError = 0;  // the mbedTLS code that ended reading, or 0 when TLS reported nothing

  int available() override {
    if (!_stillinPlainStart && _connected && sslclient && sslclient->socket >= 0) {
      const int ret = mbedtls_ssl_read(&sslclient->ssl_ctx, nullptr, 0);
      if (ret < 0 && ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) lastReadError = ret;
    }
    return WiFiClientSecure::available();
  }
};

// Passes a stream through to ArduinoJson while counting what came out of it.
struct CountedReader {
  Stream& stream;
  size_t bytes = 0;
  uint32_t startedMs = millis();

  int read() {
    char c;
    return readBytes(&c, 1) ? static_cast<uint8_t>(c) : -1;
  }
  size_t readBytes(char* buffer, size_t length) {
    const size_t got = stream.readBytes(buffer, length);
    bytes += got;
    return got;
  }
};

// "after 18234 bytes in 3.1 s; the server closed the connection; heap 41k, block 18k"
inline String readFailure(const TlsClient& client, size_t bytes, uint32_t startedMs) {
  const int e = client.lastReadError;
  String why;
  if (e == 0) why = "no data came for the whole timeout";
  else if (e == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) why = "the server closed the connection";
  else if (e == MBEDTLS_ERR_SSL_CONN_EOF) why = "the connection dropped";
  else why = "tls -0x" + String(-e, HEX);
  return "after " + String(static_cast<unsigned>(bytes)) + " bytes in " + String((millis() - startedMs) / 1000.0, 1) +
         " s; " + why + "; heap " + String(ESP.getFreeHeap() / 1024) + "k, block " + String(ESP.getMaxAllocHeap() / 1024) + "k";
}

inline String readFailure(const TlsClient& client, const CountedReader& reader) {
  return readFailure(client, reader.bytes, reader.startedMs);
}
