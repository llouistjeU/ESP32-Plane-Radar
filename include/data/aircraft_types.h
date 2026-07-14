#pragma once

namespace data::aircraft_types {

/**
 * Look up a short, human-readable model name for an ICAO aircraft type
 * designator (e.g. "B738" -> "737-800", "A388" -> "A380-800").
 *
 * Falls back to returning `icao_type` unchanged if the designator isn't in
 * the table, so unrecognized/rare types still show something on the tag
 * instead of going blank. Add more rows to `kTypes` in aircraft_types.cpp
 * whenever you spot a code you don't recognize.
 */
const char* modelName(const char* icao_type);

/**
 * True if `icao_type` matches one of the "favorite" prefixes configured in
 * config::kFavoriteAircraftPrefixes (e.g. "B74" matches all 747 variants).
 */
bool isFavoriteType(const char* icao_type);

}  // namespace data::aircraft_types
