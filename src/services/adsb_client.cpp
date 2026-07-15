#include "services/adsb_client.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <cstring>

#include "config.h"

namespace services::adsb {

namespace {

constexpr char kApiBase[] = "https://opendata.adsb.fi/api/v3/lat/";
constexpr float kKmPerNm = 1.852f;
constexpr int kConnectAttemptMs = 200;
// Measured: at 25km, ~10 KB of a ~16 KB response arrived within the old
// 10s budget (~1 KB/s effective) — the per-iteration overhead in the read
// loop below, not network speed, was the bottleneck. Larger buffer (see
// below) addresses the root cause; this wider budget is a safety margin.
constexpr unsigned long kRequestTimeoutMs = 20000;

Aircraft s_aircraft[kMaxAircraft];
size_t s_aircraft_count = 0;
PollFn s_poll_fn = nullptr;

// wifiLoop() (WiFiManager housekeeping) plus the button check both cost a
// little time — negligible once, but the read loops below call this on
// *every* pass, and at typical read speeds that's hundreds of passes for an
// ~11-12 KB response. Throttling to at most once per 20ms keeps the button
// and WiFi housekeeping responsive to a human, while cutting the accumulated
// overhead that was still causing truncated reads even with the larger
// buffer and longer timeout.
constexpr unsigned long kPollThrottleMs = 20;
unsigned long s_last_poll_ms = 0;

void pollNetwork() {
  const unsigned long now = millis();
  if (now - s_last_poll_ms < kPollThrottleMs) {
    return;
  }
  s_last_poll_ms = now;
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

int performGetWithPoll(HTTPClient& http) {
  http.setConnectTimeout(kConnectAttemptMs);
  const unsigned long deadline = millis() + kRequestTimeoutMs;
  while (millis() < deadline) {
    pollNetwork();
    const int code = http.GET();
    if (code > 0) {
      return code;
    }
    if (code != HTTPC_ERROR_CONNECTION_REFUSED &&
        code != HTTPC_ERROR_NOT_CONNECTED) {
      return code;
    }
    delay(5);
  }
  return HTTPC_ERROR_READ_TIMEOUT;
}

bool readResponseBodyWithPoll(HTTPClient& http, String& payload) {
  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    return false;
  }

  const int content_length = http.getSize();
  if (content_length > 0) {
    payload.reserve(static_cast<unsigned>(content_length + 1));
  }

  // static, not a stack-local array: mbedTLS (which WiFiClientSecure uses
  // under the hood for the HTTPS connection) is documented to use a
  // significant amount of stack itself during TLS record decryption. The
  // ESP32's default loop-task stack is only 8 KB total; stacking a 4096-byte
  // buffer on top of whatever mbedTLS needs, right at the point where it's
  // actively decrypting incoming data, risks a stack overflow — which
  // corrupts nearby memory in unpredictable ways and could explain the
  // erratic symptoms (garbled JSON, spurious WiFi reconnects) seen at larger
  // payload sizes. `static` keeps the same 4096-byte chunk size (so read
  // throughput is unaffected) but places it in fixed storage instead of on
  // the stack.
  static uint8_t buffer[4096];
  const unsigned long deadline = millis() + kRequestTimeoutMs;
  while (millis() < deadline) {
    pollNetwork();
    const int available = stream->available();
    if (available > 0) {
      const int to_read =
          available > static_cast<int>(sizeof(buffer)) ? static_cast<int>(sizeof(buffer))
                                                       : available;
      const int read_bytes = stream->readBytes(buffer, to_read);
      if (read_bytes > 0) {
        payload.concat(reinterpret_cast<const char*>(buffer),
                       static_cast<unsigned>(read_bytes));
      }
    }
    if (content_length > 0 &&
        static_cast<int>(payload.length()) >= content_length) {
      break;
    }
    if (!http.connected() && stream->available() <= 0) {
      break;
    }
    delay(1);
  }

  return payload.length() > 0;
}

float kmToNauticalMiles(float km) { return km / kKmPerNm; }

bool readJsonFloat(const JsonObject& obj, const char* key, float* out) {
  if (obj[key].is<float>() || obj[key].is<double>() || obj[key].is<int>()) {
    *out = obj[key].as<float>();
    return true;
  }
  return false;
}

float pickNoseHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickTrackHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickGroundSpeed(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "gs", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "tas", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "ias", &v)) {
    return v;
  }
  return 0.0f;
}

bool isOnGround(const JsonObject& plane) {
  if (!plane["alt_baro"].is<const char*>()) {
    return false;
  }
  return strcmp(plane["alt_baro"].as<const char*>(), "ground") == 0;
}

void copyJsonStringTrimmed(const JsonObject& obj, const char* key, char* out,
                           size_t out_len) {
  out[0] = '\0';
  if (out_len == 0 || !obj[key].is<const char*>()) {
    return;
  }
  const char* s = obj[key].as<const char*>();
  size_t n = strnlen(s, out_len - 1);
  while (n > 0 && s[n - 1] == ' ') {
    --n;
  }
  memcpy(out, s, n);
  out[n] = '\0';
}

void formatAltitudeTag(const JsonObject& plane, char* out, size_t out_len) {
  out[0] = '\0';
  if (out_len == 0) {
    return;
  }

  if (plane["alt_baro"].is<const char*>()) {
    const char* s = plane["alt_baro"].as<const char*>();
    if (strcmp(s, "ground") == 0) {
      strncpy(out, "GND", out_len - 1);
      out[out_len - 1] = '\0';
      return;
    }
  }

  float alt = 0.0f;
  if (readJsonFloat(plane, "alt_baro", &alt) ||
      readJsonFloat(plane, "alt_geom", &alt)) {
    snprintf(out, out_len, "%d ft", static_cast<int>(lroundf(alt)));
  }
}

void fillTagFields(Aircraft* ac, const JsonObject& plane) {
  copyJsonStringTrimmed(plane, "flight", ac->callsign, sizeof(ac->callsign));
  if (ac->callsign[0] == '\0') {
    copyJsonStringTrimmed(plane, "hex", ac->callsign, sizeof(ac->callsign));
  }

  copyJsonStringTrimmed(plane, "t", ac->type, sizeof(ac->type));
  formatAltitudeTag(plane, ac->alt, sizeof(ac->alt));
}

}  // namespace

void setPollFn(PollFn fn) { s_poll_fn = fn; }

size_t aircraftCount() { return s_aircraft_count; }

const Aircraft* aircraftList() { return s_aircraft; }

bool fetchUpdateOnce(double center_lat, double center_lon, float fetch_radius_km) {
  const float dist_nm = kmToNauticalMiles(fetch_radius_km);

  String url = kApiBase;
  url += String(center_lat, 6);
  url += "/lon/";
  url += String(center_lon, 6);
  url += "/dist/";
  url += String(dist_nm, 1);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("adsb: http.begin failed");
    return false;
  }

  http.setTimeout(kRequestTimeoutMs);
  const int code = performGetWithPoll(http);
  if (code != HTTP_CODE_OK) {
    Serial.printf("adsb: HTTP %d\n", code);
    http.end();
    return false;
  }

  String payload;
  const bool got_body = readResponseBodyWithPoll(http, payload);
  // Diagnostic: if content_length is -1, the server didn't declare a fixed
  // size up front (e.g. chunked transfer) and we're relying on the
  // connection closing to know we're done. If content_length is a real
  // number but payload_bytes is smaller, the read loop above hit its
  // timeout (or the connection dropped) before the full body arrived —
  // that's a truncated payload, not a memory problem, and explains
  // InvalidInput/IncompleteInput parse errors even with plenty of free heap.
  // RSSI (WiFi signal strength, dBm): roughly -30 to -60 = strong,
  // -60 to -75 = ok, below -80 = weak enough to genuinely slow transfers.
  // Logged here to separate "weak signal" from "read-loop overhead" as the
  // cause of a slow/truncated fetch — the content-length vs received
  // comparison alone can't tell those two apart.
  Serial.printf("adsb: content-length=%d, payload received=%u bytes, RSSI=%d dBm\n",
                http.getSize(), static_cast<unsigned>(payload.length()), WiFi.RSSI());
  if (!got_body) {
    Serial.println("adsb: empty response");
    http.end();
    return false;
  }
  http.end();

  // adsb.fi/readsb returns ~25-35 fields per aircraft (squawk, category,
  // nav_*, rssi, seen, messages, ...) of which this radar only reads 9. On a
  // busy 20-25 km radius near dense airspace, parsing (and holding in RAM)
  // every field for every aircraft can exceed what the ESP32-C3's limited
  // heap can handle — on top of the memory the HTTPS/TLS connection itself
  // already needs. A filter tells the parser to skip everything else,
  // cutting peak memory use (and parse time) roughly in proportion to how
  // many fields we drop.
  JsonDocument filter;
  JsonObject filter_ac = filter["ac"].add<JsonObject>();
  filter_ac["lat"] = true;
  filter_ac["lon"] = true;
  filter_ac["true_heading"] = true;
  filter_ac["mag_heading"] = true;
  filter_ac["track"] = true;
  filter_ac["dir"] = true;
  filter_ac["gs"] = true;
  filter_ac["tas"] = true;
  filter_ac["ias"] = true;
  filter_ac["flight"] = true;
  filter_ac["hex"] = true;
  filter_ac["t"] = true;
  filter_ac["alt_baro"] = true;
  filter_ac["alt_geom"] = true;

  const uint32_t heap_before = ESP.getFreeHeap();
  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  Serial.printf("adsb: free heap before/after parse: %u / %u bytes\n",
                heap_before, ESP.getFreeHeap());
  if (err) {
    Serial.printf("adsb: JSON parse error: %s\n", err.c_str());
    return false;
  }

  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac.isNull()) {
    s_aircraft_count = 0;
    return true;
  }

  size_t n = 0;
  for (JsonObject plane : ac) {
    if (n >= kMaxAircraft) {
      break;
    }
    if (!plane["lat"].is<float>() || !plane["lon"].is<float>()) {
      continue;
    }
    if (isOnGround(plane) && !config::kAdsbShowGroundAircraft) {
      continue;
    }

    s_aircraft[n].lat = plane["lat"].as<float>();
    s_aircraft[n].lon = plane["lon"].as<float>();
    s_aircraft[n].nose_deg = pickNoseHeading(plane);
    s_aircraft[n].track_deg = pickTrackHeading(plane);
    s_aircraft[n].gs_knots = pickGroundSpeed(plane);
    fillTagFields(&s_aircraft[n], plane);
    ++n;
  }

  s_aircraft_count = n;
  Serial.printf("adsb: %u aircraft\n", static_cast<unsigned>(n));
  return true;
}

// Failures at larger payload sizes (~11-14 KB) turned out to be
// intermittent — near-identical sizes sometimes arrive complete and
// sometimes get cut short, consistent with occasional WiFi packet loss
// rather than a fixed software bottleneck (that part's already addressed
// above via the buffer size, poll throttling, and wider timeout). Retrying
// immediately on failure, instead of waiting out the rest of the normal
// fetch interval, means a single bad read no longer leaves stale aircraft
// positions on screen for a full cycle (or several, if the run of bad luck
// continues) — it just tries again right away.
constexpr int kMaxFetchAttempts = 3;

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  for (int attempt = 1; attempt <= kMaxFetchAttempts; ++attempt) {
    if (fetchUpdateOnce(center_lat, center_lon, fetch_radius_km)) {
      return true;
    }
    if (attempt < kMaxFetchAttempts) {
      Serial.printf("adsb: retrying (attempt %d/%d)\n", attempt + 1,
                    kMaxFetchAttempts);
    }
  }
  return false;
}

}  // namespace services::adsb
