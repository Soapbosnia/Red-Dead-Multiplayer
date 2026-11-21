// devtools (client): keys for testing in game.
//   F7  print your position
//   F8  die (the game's own KILL_ACTOR native, so the death animation plays)
//   F9  list the players you can see
// Remove this resource from servers people play on.

Events.on("clientKey", function (key, down) {
    if (!down) return;
    const me = getLocalPlayer();
    if (key === "f7" && me) {
        const p = getElementPosition(me);
        outputChatBox("pos " + p.x.toFixed(1) + " " + p.y.toFixed(1) + " " + p.z.toFixed(1) +
                      "  hp " + Math.round(getElementHealth(me)));
    }
    if (key === "f8") {
        const actor = getLocalActor();
        if (actor) invokeNative("KILL_ACTOR", "i>n", actor);
    }
    if (key === "f9") {
        const names = getElementsByType("player").map((p) => getPlayerName(p));
        outputChatBox(names.length + " player(s): " + names.join(", "));
    }
});

console.log("[devtools] client script loaded: F7 position, F8 die, F9 players");
