// devtools (server): chat commands for testing. Remove this resource from servers people play on.
//   /pos   /tp x y z   /kill   /heal   /big text

const commands = {
    pos(player) {
        const p = getElementPosition(player);
        outputChatBox("pos " + p.x.toFixed(2) + " " + p.y.toFixed(2) + " " + p.z.toFixed(2), player);
    },
    tp(player, x, y, z) {
        if (z === undefined) return outputChatBox("usage: /tp x y z", player);
        setElementPosition(player, Number(x), Number(y), Number(z));
    },
    kill(player) { killPed(player); },
    heal(player) { setElementHealth(player, 100); },
    big(player, ...words) { showGameText(null, words.join(" ") || "Red Dead Multiplayer", 3000, "big"); },
};

Events.on("playerCommand", (player, command, ...args) => {
    const fn = commands[command];
    if (fn) fn(player, ...args);
    else outputChatBox("commands: /" + Object.keys(commands).join(" /"), player);
});
