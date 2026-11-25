#pragma once
// One shot a player fired, sent the moment their game fires it (MTA's bullet sync): the shooter's client sends it to
// the server, the server stamps who fired it and passes it to everyone else, and their games fire that same line from
// the shooter's puppet. Reliable: a lost shot would be a lost hit.
#include <networking/rpc/rpc.h>

#include <cstdint>

namespace RDMP::Shared {

struct ShotFired {
    static constexpr auto kIdentifier = FW_RPC_IDENTIFIER("RDMP::ShotFired");

    uint64_t shooter = 0;  // server -> clients: the shooter's network id (the server sets it; a client's is ignored)
    int8_t weapon    = -1;  // shared/weapons.h
    // The shot's line: from the shooter's camera through their crosshair point (a player's bullet starts at the camera).
    float startX = 0.0f, startY = 0.0f, startZ = 0.0f;
    float endX = 0.0f, endY = 0.0f, endZ = 0.0f;

    void Serialize(MafiaNet::BitStream* bs, bool write) {
        bs->Serialize(write, shooter);
        bs->Serialize(write, weapon);
        bs->Serialize(write, startX);
        bs->Serialize(write, startY);
        bs->Serialize(write, startZ);
        bs->Serialize(write, endX);
        bs->Serialize(write, endY);
        bs->Serialize(write, endZ);
    }
};

}  // namespace RDMP::Shared
