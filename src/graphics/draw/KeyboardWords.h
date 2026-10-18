#pragma once

namespace graphics
{
/// Suggestion vocabulary for the touch keyboard: lowercase words separated by '\n', most
/// likely first. One flash-resident string, scanned linearly on each keystroke - no index,
/// so it costs no RAM.
extern const char kKeyboardWords[];
} // namespace graphics
