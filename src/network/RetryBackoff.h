#pragma once

#include <Arduino.h>

// A retry pause that still answers Cancel. `cancelled` is polled every slice
// (an input update, a touch route, a flag); a true result ends the pause
// early and is returned so the caller can abort the retry. Keeps std::function
// out of the transfer path: callers pass a lambda and it is inlined.
template <typename Cancelled>
bool retryBackoff(const unsigned long waitMs, Cancelled&& cancelled) {
  constexpr unsigned long SLICE_MS = 50;
  const unsigned long startedAt = millis();
  while (millis() - startedAt < waitMs) {
    if (cancelled()) return true;
    delay(SLICE_MS);
  }
  return cancelled();
}
