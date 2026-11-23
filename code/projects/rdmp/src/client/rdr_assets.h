#pragma once
/*
 * The game's own title art for the menu, read at runtime from the player's game files (never shipped with the mod):
 * flash.rpf's title movie (startscreen): the brush-letter RED and DEAD masks and brush strokes for the logo.
 * The archives are RPF6 (AES table of contents) with zstd payloads, decoded with the game's own decompressor
 * (fw::Host::inflate). Main thread, inside the ImGui frame.
 */

#include <imgui.h>

namespace rdmp::fw {
    struct Host;
}

namespace RDMP::RdrAssets {
    // Reads the art once and registers it as textures; false when the game files could not be read (the logo falls back
    // to type).
    bool Load(const rdmp::fw::Host *host);

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
