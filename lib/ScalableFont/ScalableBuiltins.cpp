#include "ScalableBuiltins.h"
#if CROSSINK_SCALABLE_FONTS
#include <GfxRenderer.h>
#include <Logging.h>

#include <optional>

#include "../../src/ReaderFontSizeStep.h"
#include "../../src/fontIds.h"
#include "HalScalableFont.h"
#include "ScalableAssets.generated.h"
namespace {
// Families in CrossPointSettings::FONT_FAMILY order: Lexend Deca, Bitter, EB Garamond.
constexpr unsigned FamilyCount = 3;
// Lexend Deca and Bitter keep their bitmap font IDs for the four original
// sizes, so existing layout caches stay valid. EB Garamond was never a bitmap
// font on these readers and uses the generated per-size IDs throughout.
constexpr unsigned LegacyFamilyCount = 2;
std::optional<HalScalableFont> faces[FamilyCount * 4];
bool ready[FamilyCount] = {};
const uint8_t* const assets[] = {lexenddeca_regularOutline,    lexenddeca_boldOutline,   lexenddeca_italicOutline,
                                 lexenddeca_bolditalicOutline, bitter_regularOutline,    bitter_boldOutline,
                                 bitter_italicOutline,         bitter_bolditalicOutline, ebgaramond_regularOutline,
                                 ebgaramond_boldOutline,       ebgaramond_italicOutline, ebgaramond_bolditalicOutline};
constexpr size_t lengths[] = {
    sizeof(lexenddeca_regularOutline),    sizeof(lexenddeca_boldOutline),   sizeof(lexenddeca_italicOutline),
    sizeof(lexenddeca_bolditalicOutline), sizeof(bitter_regularOutline),    sizeof(bitter_boldOutline),
    sizeof(bitter_italicOutline),         sizeof(bitter_bolditalicOutline), sizeof(ebgaramond_regularOutline),
    sizeof(ebgaramond_boldOutline),       sizeof(ebgaramond_italicOutline), sizeof(ebgaramond_bolditalicOutline)};
static_assert(sizeof(assets) / sizeof(assets[0]) == FamilyCount * 4, "one outline per family style");
constexpr int ids[LegacyFamilyCount][4] = {
    {LEXENDDECA_10_FONT_ID, LEXENDDECA_12_FONT_ID, LEXENDDECA_14_FONT_ID, LEXENDDECA_16_FONT_ID},
    {BITTER_10_FONT_ID, BITTER_12_FONT_ID, BITTER_14_FONT_ID, BITTER_16_FONT_ID}};
}  // namespace
int scalableBuiltinFontId(int id) {
  for (unsigned f = 0; f < LegacyFamilyCount; ++f)
    for (unsigned s = 0; s < 4; ++s)
      if (ids[f][s] == id && !ready[f]) return UI_12_FONT_ID;
  const int result = int(uint32_t(id) ^ OutlineFingerprint ^ 0x53544600u ^ HalScalableFont::renderingRevision());
  return result ? result : 1;
}
int scalableBuiltinReaderFontId(unsigned family, unsigned points) {
  if (family >= FamilyCount || !ready[family]) return UI_12_FONT_ID;
  // Preserve existing cache IDs for the original four sizes.
  for (unsigned s = 0; family < LegacyFamilyCount && s < 4; ++s)
    if (points == 10 + s * 2) return scalableBuiltinFontId(ids[family][s]);
  return scalableBuiltinFontId(0x54540000 | (family << 8) | points);
}
void ensureScalableBuiltinFamily(GfxRenderer& renderer, unsigned family) {
  ScalableFontAccess access;
  if (family >= FamilyCount || ready[family]) return;
  bool ok = true;
  for (unsigned style = 0; style < 4; ++style) {
    unsigned i = family * 4 + style;
    faces[i].emplace();
    if (!faces[i]->openMemory(assets[i], lengths[i])) {
      ok = false;
      break;
    }
  }
  if (ok) {
    for (uint8_t points = ScalableContentMinPointSize; points <= ScalableContentMaxPointSize && ok; ++points) {
      for (unsigned style = 0; style < 4; ++style) {
        if (!faces[family * 4 + style]->atSize(points)) {
          LOG_ERR("TTF", "Built-in size %u unavailable", unsigned(points));
          ok = false;
          break;
        }
      }
    }
  }
  if (!ok) {
    for (unsigned style = 0; style < 4; ++style) faces[family * 4 + style].reset();
    LOG_ERR("TTF", "Built-in family unavailable; using UI recovery font");
  }
  ready[family] = ok;
  for (uint8_t points = ScalableContentMinPointSize; points <= ScalableContentMaxPointSize; ++points) {
    if (ok) {
      EpdFontFamily font(faces[family * 4]->atSize(points), faces[family * 4 + 1]->atSize(points),
                         faces[family * 4 + 2]->atSize(points), faces[family * 4 + 3]->atSize(points));
      renderer.insertFont(scalableBuiltinReaderFontId(family, points), font);
      for (unsigned size = 0; family < LegacyFamilyCount && size < 4; ++size)
        if (points == 10 + size * 2) renderer.insertFont(ids[family][size], font);
    } else {
      const auto it = renderer.getFontMap().find(UI_12_FONT_ID);
      if (it != renderer.getFontMap().end()) {
        for (unsigned size = 0; family < LegacyFamilyCount && size < 4; ++size)
          // Recovery reader fonts must not borrow the selected UI font: the
          // latter can be released independently for selection/storage changes.
          if (points == 10 + size * 2) renderer.insertFont(ids[family][size], it->second.withFallbackFonts(nullptr));
      }
    }
  }
}
#endif
