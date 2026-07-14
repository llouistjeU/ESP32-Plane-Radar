#include "data/aircraft_types.h"

#include <cstddef>
#include <cstring>

#include "config.h"

namespace data::aircraft_types {

namespace {

struct TypeEntry {
  const char* icao;
  const char* name;
};

// ICAO type designator -> short human-readable model name.
// Not exhaustive by design — covers the airliners/turboprops/bizjets you're
// most likely to actually see on the radar. Unknown codes just fall back to
// showing the raw ICAO designator, so it's safe to leave gaps. Add rows as
// you spot codes you don't recognize; `plane.type` in the tag is the raw
// ICAO code you'd search for.
constexpr TypeEntry kTypes[] = {
    // --- Boeing 737 family ---
    {"B731", "737-100"},
    {"B732", "737-200"},
    {"B733", "737-300"},
    {"B734", "737-400"},
    {"B735", "737-500"},
    {"B736", "737-600"},
    {"B737", "737-700"},
    {"B738", "737-800"},
    {"B739", "737-900"},
    {"B37M", "737 MAX 7"},
    {"B38M", "737 MAX 8"},
    {"B39M", "737 MAX 9"},
    {"B3XM", "737 MAX 10"},
    // --- Boeing 747 family ---
    {"B741", "747-100"},
    {"B742", "747-200"},
    {"B743", "747-300"},
    {"B744", "747-400"},
    {"B748", "747-8"},
    {"BLCF", "747 Dreamlifter"},
    // --- Boeing 757 / 767 ---
    {"B752", "757-200"},
    {"B753", "757-300"},
    {"B762", "767-200"},
    {"B763", "767-300"},
    {"B764", "767-400"},
    // --- Boeing 777 ---
    {"B772", "777-200"},
    {"B77L", "777-200LR"},
    {"B773", "777-300"},
    {"B77W", "777-300ER"},
    {"B778", "777-8"},
    {"B779", "777-9"},
    // --- Boeing 787 ---
    {"B788", "787-8"},
    {"B789", "787-9"},
    {"B78X", "787-10"},
    // --- Airbus A220 / A320 family ---
    {"BCS1", "A220-100"},
    {"BCS3", "A220-300"},
    {"A318", "A318"},
    {"A319", "A319"},
    {"A320", "A320"},
    {"A321", "A321"},
    {"A19N", "A319neo"},
    {"A20N", "A320neo"},
    {"A21N", "A321neo"},
    // --- Airbus A300 / A310 ---
    {"A306", "A300-600"},
    {"A310", "A310"},
    // --- Airbus A330 / A340 / A350 / A380 ---
    {"A332", "A330-200"},
    {"A333", "A330-300"},
    {"A338", "A330-800"},
    {"A339", "A330-900"},
    {"A342", "A340-200"},
    {"A343", "A340-300"},
    {"A345", "A340-500"},
    {"A346", "A340-600"},
    {"A359", "A350-900"},
    {"A35K", "A350-1000"},
    {"A388", "A380-800"},
    // --- Embraer E-Jets ---
    {"E170", "E170"},
    {"E75L", "E175"},
    {"E190", "E190"},
    {"E195", "E195"},
    {"E290", "E190-E2"},
    {"E295", "E195-E2"},
    // --- Regional jets / turboprops ---
    {"CRJ2", "CRJ200"},
    {"CRJ7", "CRJ700"},
    {"CRJ9", "CRJ900"},
    {"CRJX", "CRJ1000"},
    {"AT45", "ATR 42"},
    {"AT72", "ATR 72"},
    {"AT76", "ATR 72-600"},
    {"DH8A", "Dash 8-100"},
    {"DH8B", "Dash 8-200"},
    {"DH8C", "Dash 8-300"},
    {"DH8D", "Dash 8-400"},
    {"F70", "Fokker 70"},
    {"F100", "Fokker 100"},
    // --- Business jets / GA you'll spot near smaller fields ---
    {"C172", "Cessna 172"},
    {"C182", "Cessna 182"},
    {"C208", "Cessna Caravan"},
    {"PC12", "Pilatus PC-12"},
    {"E50P", "Phenom 100"},
    {"E55P", "Phenom 300"},
    {"GLF6", "Gulfstream G650"},
    {"GLEX", "Global Express"},
    {"C25A", "Citation CJ2"},
    {"C25B", "Citation CJ3"},
    {"C56X", "Citation Excel"},
    {"C68A", "Citation Latitude"},
    // --- Cargo widebodies ---
    {"MD11", "MD-11"},
    {"IL76", "Il-76"},
    {"A124", "An-124"},
};

constexpr size_t kTypeCount = sizeof(kTypes) / sizeof(kTypes[0]);

}  // namespace

const char* modelName(const char* icao_type) {
  if (icao_type == nullptr || icao_type[0] == '\0') {
    return icao_type;
  }
  for (size_t i = 0; i < kTypeCount; ++i) {
    if (strcmp(icao_type, kTypes[i].icao) == 0) {
      return kTypes[i].name;
    }
  }
  return icao_type;  // Unknown designator: show the raw ICAO code as-is.
}

bool isFavoriteType(const char* icao_type) {
  if (icao_type == nullptr || icao_type[0] == '\0') {
    return false;
  }
  for (size_t i = 0; i < config::kFavoriteAircraftPrefixCount; ++i) {
    const char* prefix = config::kFavoriteAircraftPrefixes[i];
    const size_t len = strlen(prefix);
    if (strncmp(icao_type, prefix, len) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace data::aircraft_types
