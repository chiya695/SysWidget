#pragma once
#include <windows.h>
#include "config.h"
#include "metrics.h"

// Create/destroy cached GDI fonts.
void Render_Init();
void Render_Free();

// Work out how big the widget wants to be for the current config + data, at the
// given DPI. Returned size is in physical pixels.
SIZE Render_Measure(const Config& cfg, const Metrics& m, int dpi);

// Paint the widget into hdc, filling the whole client rect.
void Render_Paint(HDC hdc, const RECT& client, const Config& cfg, const Metrics& m, int dpi);
