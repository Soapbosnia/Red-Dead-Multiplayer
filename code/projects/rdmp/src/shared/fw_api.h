#pragma once
// C API between rdmp.dll (the game mod, its own static CRT) and rdmp_client.dll (the MafiaHub Framework client,
// headless Integrations::Client::Instance + client scripting). Only POD crosses the boundary.
//
// Threading: rdmp.dll makes every call from the GAME'S MAIN THREAD, once per rendered frame. The framework client,
// its networking pump and client scripts therefore run on the main thread too, which is what lets client script
// natives call game natives directly through the Host table below.
#include <cstdint>

#include "shared/types.h"

namespace rdmp::fw {

// v3: server actions via poll_action. v4: blips, get_local_id.
// v5: driven from the game's main thread; Host table (client script natives call into the game); cache folder.
// v6: invoke_native returns kResultSlots slots (Vector3 results); actions carry text (kill, HUD text); Host:
//     show_text (styles) instead of print_text, kill_local, get_camera.
// v7: PlayerState carries MoveInput (stick, desired heading, gait) instead of the sparse desire.
// v8: PlayerState carries the controller state (controls[30]).
// v9: vehicles (get_vehicles, set_vehicle, get_assigned_vehicle); PlayerState::vehicle.
constexpr uint32_t kApiVersion = 9;
constexpr int kResultSlots = 4;

enum class Conn : int32_t { Disconnected = 0, Connecting = 1, Connected = 2 };

struct Remote {
    uint64_t netId = 0;
    char name[net::kNameLen] = {};
    net::PlayerState s;  // seq/pos/heading/speed/flags/move
    float health = 100.0f;
    int32_t pingMs = 0;  // that player's round trip to the server
};

// Our link to the server: round trip and the server tick (the server forwards updates once per tick).
struct Link {
    int32_t pingMs = 0;
    float tickMs = 0;
};

// Server override of our own avatar's position (a teleport).
struct Forced {
    float x = 0, y = 0, z = 0;
};

// A server-requested action for the game (server script natives). Applied on the main thread.
enum class ActionKind : int32_t {
    Spawn        = 1,  // a,b,c = position, d = heading degrees
    Fade         = 2,  // a = 1 fade-in / 0 fade-out, b = milliseconds
    CameraTarget = 3,  // attach the gameplay camera to the local player
    Control      = 4,  // a = 1 enable / 0 disable player control
    Health       = 5,  // a = health value to write
    Kill         = 6,  // kill the local player (the game's death, with its animation)
    HudText      = 7,  // a = TextStyle, b = milliseconds, text
};

// Engine HUD text styles (Host::show_text).
enum class TextStyle : int32_t {
    Subtitle = 0,  // bottom-centre line (PRINT_OBJECTIVE_B); one at a time, a new one replaces it
    Big      = 1,  // big centred title (PRINT_BIG)
};

struct Action {
    ActionKind kind = ActionKind::Spawn;
    float a = 0.0f;
    float b = 0.0f;
    float c = 0.0f;
    float d = 0.0f;
    char text[256] = {};
};

// The active game camera (Host::get_camera): orientation rows and position in world space (Y up), and the
// vertical field of view in degrees.
struct Camera {
    float right[3];
    float up[3];
    float back[3];  // the camera looks along -back
    float pos[3];
    float fovY;
};

// A script blip (server entity RDMP::Blip, shared/blip.h), as currently replicated to us.
enum BlipFlags : uint8_t {
    kBlipHidden    = 1u << 0,  // exists but not drawn (setBlipVisible false)
    kBlipRadarOnly = 1u << 1,  // minimap only (SET_BLIP_HUDMAP_ONLY)
    kBlipMapOnly   = 1u << 2,  // pause map only (SET_BLIP_PAUSEMAP_ONLY)
    kBlipHasColor  = 1u << 3,  // rgba was set by a script (otherwise the icon's own colours)
};

struct Blip {
    uint64_t netId = 0;
    float x = 0, y = 0, z = 0;  // world position, Y up (for an attached blip: the target's position on the server)
    uint64_t attachedTo = 0;    // network id of the element it follows, 0 = none
    uint32_t rgba = 0xFFFFFFFF; // 0xRRGGBBAA
    float scale = 1.0f;
    float maxDistance = -1.0f;  // < 0 = unlimited
    int16_t priority = 0;
    uint16_t icon = 0;          // BlipIcon, 299..572
    uint8_t flags = 0;          // BlipFlags
};

// A vehicle (server entity RDMP::Vehicle, shared/vehicle.h: a horse), as currently replicated to us.
struct Vehicle {
    uint64_t netId = 0;
    uint32_t model = 0;        // actor definition index (976..1005: rideable animals)
    float x = 0, y = 0, z = 0;  // Y up
    float heading = 0;          // degrees
    float speed = 0;            // m/s
    net::MoveInput input;       // the input its rider gives it (zero = stands)
    uint64_t occupant = 0;      // network id of its rider, 0 = none
    uint32_t timeMs = 0;        // send time of this pose on our clock (numbers / times the sample), 0 = server's
    int32_t mine = 0;           // 1 = we own it (we ride it): we write its state with set_vehicle
};

using LogFn = void (*)(const char* line);

// What rdmp.dll offers the client DLL. Every function runs synchronously on the caller's thread, which is the game's
// main thread (see the header comment).
struct Host {
    uint32_t version;  // = kApiVersion
    LogFn log;
    // Any game native by hash (joaat of the lowercase name; game/scriptnatives.h). `args` are 8-byte slots (ints,
    // floats and bools in the low 4 bytes, pointers whole). `result` receives kResultSlots slots: the low 4 bytes of the
    // first are the native's return value; a Vector3 return fills more.
    // False when the hash is not registered or the registry check failed.
    bool (*invoke_native)(uint32_t hash, const uint64_t* args, int argc, uint64_t* result);
    // The local player: false while there is none. pos = Y-up world position, heading in degrees.
    bool (*get_local_player)(float pos[3], float* heading, float* health, uint32_t* actorHandle);
    void (*teleport_local)(const float pos[3], float heading);
    void (*fade)(int fadeIn, float milliseconds);
    void (*set_control)(int enabled);
    void (*camera_to_player)();
    void (*set_health)(float health);
    int (*game_has_focus)();
    // Engine HUD text (TextStyle), `milliseconds` long.
    void (*show_text)(int style, const char* text, int milliseconds);
    // Kills the local player through the game (KILL_ACTOR: death animation, then the usual death flow).
    void (*kill_local)();
    // The active game camera; false while there is none.
    bool (*get_camera)(Camera* out);
    // Our UI holds the keyboard (chat input open): player control off until released (scripts' control kept apart).
    void (*set_input_lock)(int locked);
    // Where the game draws a remote player (their puppet), Y up; false while it has none.
    bool (*get_remote_position)(uint64_t netId, float out[3]);
};

// Function table filled by rdmp_client_get_api (one export keeps the dll boundary small).
struct Api {
    uint32_t version;
    // logFolder: framework log files; cacheFolder: resources downloaded from servers; host: see Host (kept).
    int (*init)(const char* logFolder, const char* cacheFolder, const Host* host);
    int (*connect)(const char* address, int port, const char* nickname);
    Conn (*update)();  // pump networking, replication and client scripts; once per frame
    void (*set_local)(const net::PlayerState* s, float health);
    int (*get_remotes)(Remote* out, int max);  // remote players currently streamed to us
    int (*poll_forced)(Forced* out);           // 1 once per server teleport of our avatar
    int (*poll_action)(Action* out);           // 1 per queued server action
    void (*shutdown)();
    void (*get_link)(Link* out);
    int (*get_blips)(Blip* out, int max);  // every blip currently replicated to us
    uint64_t (*get_local_id)();            // our own avatar's network id, 0 until assigned
    int (*get_vehicles)(Vehicle* out, int max);  // every vehicle currently streamed to us
    // The state of the vehicle we ride and own (ignored for any other).
    void (*set_vehicle)(uint64_t netId, const float pos[3], float heading, float speed, const net::MoveInput* input);
    uint64_t (*get_assigned_vehicle)();          // the vehicle the server has our avatar on, 0 = none
};

}  // namespace rdmp::fw

// extern "C" __declspec(dllexport) int rdmp_client_get_api(rdmp::fw::Api* out);  (1 = ok)
using rdmp_client_get_api_fn = int (*)(rdmp::fw::Api* out);
