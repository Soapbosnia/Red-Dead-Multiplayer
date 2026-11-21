#pragma once
/*
 * Server browser data, fetched off the game thread: each server answers GET /rdmp on its game port (name, players,
 * max players, password; RDMPServer main.cpp), the Internet list comes from the MafiaHub masterlist.
 * Requests are started from the main thread and their results picked up there with Poll().
 */

#include <cstdint>
#include <string>
#include <vector>

namespace RDMP::ServerQuery {
    struct Address {
        std::string host;
        uint16_t port = 27015;
    };

    struct Info {
        std::string list; // the browser tab this answers: "internet", "local", "favorites", "recent"
        std::string host;
        uint16_t port = 0;
        bool online   = false;
        std::string name;
        std::string version;
        int players    = 0;
        int maxPlayers = 0;
        bool password  = false;
        int pingMs     = -1; // HTTP round trip
    };

    // Asks every address (in parallel); one Info per address arrives through Poll (online = false when it did not answer).
    void QueryAddresses(const std::string &list, const std::vector<Address> &addresses);
    // The masterlist's servers for `gameMode`, then each one asked like above.
    void QueryMasterlist(const std::string &masterlistUrl, const std::string &gameMode);
    // Results finished since the last call (main thread).
    std::vector<Info> Poll();
    // An error to show for a list (masterlist unreachable), once.
    bool PollError(std::string &list, std::string &message);

    void Shutdown();
} // namespace RDMP::ServerQuery
