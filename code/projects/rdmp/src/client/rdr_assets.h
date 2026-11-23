#pragma once
/*
 * The game's own type and art for the menu, read at runtime from the player's game files (never shipped with the mod):
 *   fonts.rpf  - rage bitmap fonts (glyph table + 128 x 128 DXT5 pages): a slab serif (the game's menu face), a condensed
 *                face and a small serif; served to Dear ImGui through a custom font loader, so every widget can use them.
 *   flash.rpf  - the title movie (startscreen): the brush-letter RED and DEAD masks and brush strokes for the logo.
 * The archives are RPF6 (AES table of contents) with zstd payloads, decoded with the game's own decompressor
 * (fw::Host::inflate). Main thread, inside the ImGui frame.
 */

#include <imgui.h>

namespace rdmp::fw {
    struct Host;
}

namespace RDMP::RdrAssets {
    // Reads and registers everything once (fonts into the ImGui atlas, art as textures); false when the game files could
    // not be read (the callers fall back to their own fonts).
    bool Load(const rdmp::fw::Host *host);

    enum class Face { Serif, Narrow, Body };
    // Null until Load() succeeded.
    ImFont *Font(Face face);

    enum class Art { Red, Dead, Stroke, StrokeThin, Splatter };
    struct Texture {
        ImTextureRef ref;
        float width  = 0; // pixels
        float height = 0;
        bool valid   = false;
    };
    // White masks (alpha = coverage): tint them when drawing.
    Texture Get(Art art);
} // namespace RDMP::RdrAssets
