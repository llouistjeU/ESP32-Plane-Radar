#pragma once

#include <Arduino.h>

/**
 * Central on/off switch for all Serial logging.
 *
 * false (default): every LOGF/LOGLN below compiles away to nothing. No text
 *   is formatted, nothing is written to Serial, and the strings aren't even
 *   included in the firmware. This matters on the ESP32-C3 Super Mini, which
 *   uses the chip's built-in USB (ARDUINO_USB_CDC_ON_BOOT=1): unlike a
 *   separate USB-serial chip — which just discards output when nobody's
 *   listening — the built-in USB can wait for a host to read the data. With
 *   logging compiled out there is nothing to wait on, so running the board
 *   from a plain USB charger (no PC attached) carries no risk.
 *
 * true: full diagnostics over Serial at 115200 baud. Flip this back when you
 *   need to investigate something, e.g. with the board plugged into a laptop
 *   and a serial terminal open.
 */
constexpr bool kDebugLog = false;

// `if constexpr` on a compile-time-false condition means the compiler drops
// the whole call, including the format string. Wrapped in do/while so these
// behave like a normal statement (safe after a bare `if` without braces).
#define LOGF(...)                    \
  do {                               \
    if constexpr (kDebugLog) {       \
      Serial.printf(__VA_ARGS__);    \
    }                                \
  } while (0)

#define LOGLN(...)                   \
  do {                               \
    if constexpr (kDebugLog) {       \
      Serial.println(__VA_ARGS__);   \
    }                                \
  } while (0)
