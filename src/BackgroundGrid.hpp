// EncxdedCanvas: single background grid (lines or dots) drawn behind the canvas contents.
#pragma once
#include <include/core/SkCanvas.h>

class World;
struct DrawData;

void draw_background_grid(World& world, SkCanvas* canvas, const DrawData& drawData);
