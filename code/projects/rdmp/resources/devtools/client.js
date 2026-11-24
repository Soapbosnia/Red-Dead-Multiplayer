// devtools (client): keys for testing in game. Remove this resource from servers people play on.
//   F4  god mode (cancels the damage you take)   F7  your position      F8  die (killPed)      F9  players you can see

let god = false;

// Returning false cancels the damage: your game puts the health back and the server never hears of it.
Events.on("clientPlayerDamage", (attacker, weapon, bodyPart, loss) => !god);

Events.on("clientKey", (key, down) => {
    if (!down) return;
    const me = getLocalPlayer();
    if (key === "f4") {
        god = !god;
        outputChatBox("god mode " + (god ? "on" : "off"));
    }
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
