#pragma once

namespace rdmp {

// Black "LOADING" overlay window over the game window (hides the boot splash and loading art). Starts at DLL load.
void StartLoadingOverlay();

// Closes the overlay after delayMs (call once the player is in the world).
void HideLoadingOverlay(unsigned delayMs);

}  // namespace rdmp
