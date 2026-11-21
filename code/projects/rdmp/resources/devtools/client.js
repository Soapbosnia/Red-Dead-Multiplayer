// devtools (client): keys for testing in game. Remove this resource from servers people play on.
//   F7  your position      F8  die (killPed)      F9  players you can see

Events.on("clientKey", (key, down) => {
    if (!down) return;
    const me = getLocalPlayer();
    if (key === "f7" && me) {
        const p = getElementPosition(me);
        outputChatBox("pos " + p.x.toFixed(1) + " " + p.y.toFixed(1) + " " + p.z.toFixed(1) + "  hp " + Math.round(getElementHealth(me)));
    }
    if (key === "f8" && me) killPed(me);
    if (key === "f9") {
        const names = getElementsByType("player").map(getPlayerName);
        outputChatBox(names.length + " player(s): " + names.join(", "));
    }
});
