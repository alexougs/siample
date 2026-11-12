#include "Theme.h"
#include "BinaryData.h"

namespace siample::theme
{

juce::Typeface::Ptr regularTypeface()
{
    static const auto face = juce::Typeface::createSystemTypefaceFor (BinaryData::InterRegular_ttf,
                                                                      (size_t) BinaryData::InterRegular_ttfSize);
    return face;
}

juce::Typeface::Ptr boldTypeface()
{
    static const auto face = juce::Typeface::createSystemTypefaceFor (BinaryData::InterSemiBold_ttf,
                                                                      (size_t) BinaryData::InterSemiBold_ttfSize);
    return face;
}

juce::Typeface::Ptr headingTypeface()
{
    static const auto face = juce::Typeface::createSystemTypefaceFor (BinaryData::SpaceGroteskBold_ttf,
                                                                      (size_t) BinaryData::SpaceGroteskBold_ttfSize);
    return face;
}

} // namespace siample::theme
