#pragma once

#include <I18nKeys.h>

#include <array>

#include "CrossPointSettings.h"

// Labels of the built-in reader font families, indexed like
// CrossPointSettings::FONT_FAMILY. Every font picker (Settings, the reader
// drawer, font selection and the web settings) lists these before SD fonts.
inline constexpr std::array<StrId, CrossPointSettings::BUILTIN_FONT_COUNT> BUILTIN_FONT_FAMILY_LABELS = {
    StrId::STR_LEXEND_DECA,
    StrId::STR_BITTER,
#if CROSSINK_SCALABLE_FONTS
    StrId::STR_EB_GARAMOND,
#endif
};
