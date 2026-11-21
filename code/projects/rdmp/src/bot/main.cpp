/*
 * rdmp_bot - headless Framework v44 client used as a test bot.
 *
 *   rdmp_bot bot <name> <host> <port> [cx cy cz] [radius] [seconds]
 *
 * It connects like the game does, takes the avatar the server assigns, and walks it in a
 * circle, which is enough to prove replication, ownership and the forced-state path without a
 * second copy of the game. The bridge mode of the v10 client is not ported yet (v10/client/).
 */

#include <utils/safe_win32.h>

#include "client/fw_client.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

int main(int argc, char **argv) {
    if (argc < 5 || std::strcmp(argv[1], "bot") != 0) {
        std::printf("usage: %s bot <name> <host> <port> [cx cy cz] [radius] [seconds]\n", argv[0]);
        return 1;
    }

    const std::string name = argv[2];
    const std::string host = argv[3];
    const int port         = std::atoi(argv[4]);
    const float cx         = argc > 7 ? std::strtof(argv[5], nullptr) : -2208.91f;
    const float cy         = argc > 7 ? std::strtof(argv[6], nullptr) : 16.51f;
    const float cz         = argc > 7 ? std::strtof(argv[7], nullptr) : 2605.41f;
    const float radius     = argc > 8 ? std::strtof(argv[8], nullptr) : 2.0f;
    const int seconds      = argc > 9 ? std::atoi(argv[9]) : 0;

    RDMP::FwClient client;
    // Downloaded resources go to the bot folder of cache next to the exe (like the game client's cache), not the user profile.
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string cache = exe;
    cache = cache.substr(0, cache.find_last_of("\\/") + 1) + "cache\\bot";
    if (!client.InitHeadless(cache)) {
        std::printf("[bot] client init failed\n");
        return 1;
    }
    if (!client.Connect(host, port, name)) {
        std::printf("[bot] connect failed\n");
        return 1;
    }

    const auto start = std::chrono::steady_clock::now();
    float t          = 0.0f;
    while (true) {
        const auto now     = std::chrono::steady_clock::now();
        const float elapsed = std::chrono::duration<float>(now - start).count();
        if (seconds > 0 && elapsed > static_cast<float>(seconds)) {
            break;
        }

        t += 1.0f / 120.0f;
        if (auto *me = client.Owned()) {
            // Walk a circle at 1.4 m/s; the server replicates it to everyone in range.
            me->position = {cx + std::cos(t) * radius, cy, cz + std::sin(t) * radius};
            // Movement input like a player walking along the circle (shared/types.h MoveInput): stick forward, desired
            // heading = the direction of travel (engine convention: heading = atan2(-dx, -dz), the desire word holds
            // heading + pi), walk gait. Puppets replay this input.
            const float dx = -std::sin(t), dz = std::cos(t);
            const float hd = std::atan2(-dx, -dz);
            me->heading     = hd * 57.29578f;
            me->speed       = 1.4f;
            me->moveRight   = 0.0f;
            me->moveBack    = -1.0f;
            me->moveHeading = hd + 3.14159265f;
            me->gait        = 1; // walk
            me->actAck  = me->actSeq; // a bot applies nothing, but must ack so the server's action queue moves on
        }

        client.Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    client.Shutdown();
    std::printf("[bot] done\n");
    return 0;
}
