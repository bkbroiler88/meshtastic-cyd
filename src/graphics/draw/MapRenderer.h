#pragma once

#include "configuration.h"

#if defined(BASEUI_HAS_MAP)

#include "graphics/Screen.h"
#include <OLEDDisplay.h>
#include <OLEDDisplayUi.h>

struct _InputEvent;
typedef struct _InputEvent InputEvent;

namespace graphics
{

/// Plan-view map of heard nodes, drawn north-up around our own position.
///
/// Monochrome by construction: BaseUI draws into OLEDDisplay's 1-bit framebuffer, so a
/// node's state shows as fill and shape rather than colour. A single TFTColorRegion over
/// the map band supplies the two colours on a TFT.
///
/// This is the geometry-free half of the map. Road/water polylines from SD slot in behind
/// the node overlay later without changing this frame's contract.
namespace MapRenderer
{
/// FrameCallback. Registered in Screen::setFrames() at framesetInfo.positions.map.
void drawMapFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y);

/// Hit-test a touch against the on-screen controls. Returns true when the map consumed
/// the event, false to let the frame carousel have it - swipes must keep changing frames.
bool handleInput(const InputEvent *event);

/// Drop back to auto-fit and re-centre on our own position. Called from the D-pad centre
/// and whenever the frame is re-entered.
void resetView();
} // namespace MapRenderer

} // namespace graphics

#endif
