#include "services/adsb_client.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <cstring>

#include "config.h"
#include "debug.h"

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
    LOGLN("adsb: http.begin failed");
    return false;
  }

  http.setTimeout(kRequestTimeoutMs);
  const int code = performGetWithPoll(http);
  if (code != HTTP_CODE_OK) {
    LOGF("adsb: HTTP %d\n", code);
    http.end();
    return false;
  }

  // adsb.fi/readsb returns ~25-35 fields per aircraft (squawk, category,
  // nav_*, rssi, seen, messages, ...) of which this radar only reads 9. The
  // filter tells the parser to skip everything else, cutting both parse time
  // and peak memory roughly in proportion to how many fields we drop.
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

  // Measured: a healthy ~10 KB response finished in 84 ms, while every
  // failure spent the full 20 s and still only ever accumulated ~8-12 KB.
  // That is not a slow network — it's the old approach collecting the whole
  // body into one String first, which needs a single *contiguous* heap block
  // the size of the response. Total free heap looked fine (~27 KB), but heap
  // fragmentation means the largest contiguous block can be far smaller, so
  // growing the String past ~10 KB fails and the read loop then spins
  // pointlessly until the deadline. Parsing straight from the network stream
  // removes the big String entirely: ArduinoJson consumes the body
  // incrementally and only ever holds the small filtered result.
  //
  // largest block = biggest single contiguous allocation still possible. If
  // this sits near ~10 KB while free heap reads ~27 KB, fragmentation is
  // confirmed as the cause of the old failures.
  LOGF("adsb: content-length=%d, free heap=%u, largest block=%u, RSSI=%d dBm\n",
                http.getSize(), ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
                WiFi.RSSI());

  const unsigned long parse_start = millis();
  JsonDocument doc;
  const DeserializationError err = deserializeJson(
      doc, http.getStream(), DeserializationOption::Filter(filter));
  LOGF("adsb: stream parse %lu ms, result=%s, free heap=%u\n",
                millis() - parse_start, err.c_str(), ESP.getFreeHeap());
  http.end();
  if (err) {
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
  LOGF("adsb: %u aircraft\n", static_cast<unsigned>(n));
  return true;
}

// A retry costs one extra request but avoids leaving stale aircraft
// positions on screen for a whole cycle when a fetch does fail (bad HTTP
// code, or a parse error on a genuinely malformed/interrupted response).
// Note: earlier this was meant to paper over truncated reads; the timing
// measurement showed those were caused by the String-based body collection
// (see above), which streaming now removes at the source.
constexpr int kMaxFetchAttempts = 3;

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  for (int attempt = 1; attempt <= kMaxFetchAttempts; ++attempt) {
    if (fetchUpdateOnce(center_lat, center_lon, fetch_radius_km)) {
      return true;
    }
    if (attempt < kMaxFetchAttempts) {
      LOGF("adsb: retrying (attempt %d/%d)\n", attempt + 1,
                    kMaxFetchAttempts);
    }
  }
  return false;
}

}  // namespace services::adsb
