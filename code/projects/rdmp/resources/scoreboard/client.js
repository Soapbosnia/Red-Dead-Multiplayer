// scoreboard (client): hold Tab to see who is on the server. An example of the dx* drawing API.

const ROW = 26;
const WIDTH = 420;

Events.on("clientRender", () => {
    if (!isKeyDown("tab") || isChatInputActive()) return;
    const screen = guiGetScreenSize();
    const players = getElementsByType("player");
    const height = 56 + players.length * ROW;
    const x = (screen.x - WIDTH) / 2;
    const y = screen.y * 0.25;

    dxDrawRectangle(x, y, WIDTH, height, tocolor(0, 0, 0, 170), 6);
    dxDrawRectangle(x, y, WIDTH, 34, tocolor(120, 30, 20, 220), 6);
    dxDrawText("Red Dead Multiplayer", x + 14, y + 8, tocolor(255, 255, 255), 1.1, "left", true);
    dxDrawText(players.length + " player" + (players.length === 1 ? "" : "s"), x + WIDTH - 14, y + 9, tocolor(220, 220, 220), 1, "right");

    const me = getLocalPlayer();
    players.forEach((player, i) => {
        const rowY = y + 44 + i * ROW;
        const self = me && String(player) === String(me);
        dxDrawText(getPlayerName(player), x + 14, rowY, self ? tocolor(255, 210, 120) : tocolor(255, 255, 255), 1, "left", true);
        const hp = Math.round(getElementHealth(player));
        dxDrawRectangle(x + WIDTH - 114, rowY + 6, 100, 8, tocolor(40, 40, 40, 220), 2);
        dxDrawRectangle(x + WIDTH - 114, rowY + 6, Math.max(0, Math.min(100, hp)), 8, tocolor(200, 60, 50), 2);
    });
});
