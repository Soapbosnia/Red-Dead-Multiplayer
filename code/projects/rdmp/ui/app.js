// Red Dead Multiplayer main menu. Talks to the client (src/client/frontend.cpp):
//   page -> client: callEvent("rdmp:<name>", json)    client -> page: rdmp.receive({type, data})
"use strict";

const $ = (sel) => document.querySelector(sel);
const $$ = (sel) => Array.from(document.querySelectorAll(sel));

function send(name, data) {
  if (typeof callEvent === "function") callEvent("rdmp:" + name, JSON.stringify(data || {}));
  else console.log("[rdmp] would send", name, data); // opened in a normal browser
}

// ---- state ----
const lists = { internet: new Map(), local: new Map(), favorites: new Map(), recent: new Map() };
const listErrors = {};
let favorites = [];
let currentList = "internet";
let selected = null; // "host:port"
let pendingPassword = null;

const keyOf = (host, port) => host + ":" + port;
const isFavorite = (key) => favorites.some((f) => keyOf(f.host, f.port) === key);

// ---- status line ----
let statusTimer = 0;
function status(message, kind) {
  const el = $("#status");
  el.textContent = message;
  el.className = "status show" + (kind ? " " + kind : "");
  clearTimeout(statusTimer);
  if (kind !== "busy") statusTimer = setTimeout(() => el.classList.remove("show"), 4000);
}

// ---- pages / tabs ----
$$(".nav-item[data-page]").forEach((b) => b.addEventListener("click", () => {
  $$(".nav-item[data-page]").forEach((x) => x.classList.toggle("active", x === b));
  $$(".page").forEach((p) => p.classList.toggle("active", p.id === "page-" + b.dataset.page));
}));
$("#quit").addEventListener("click", () => send("quit"));

$$(".tab").forEach((t) => t.addEventListener("click", () => selectList(t.dataset.list)));

function selectList(list) {
  currentList = list;
  $$(".tab").forEach((t) => t.classList.toggle("active", t.dataset.list === list));
  refresh();
}

function refresh() {
  lists[currentList].clear();
  delete listErrors[currentList];
  // favourites / recent show their entries at once, filled in as the servers answer
  if (currentList === "favorites" || currentList === "recent") {
    for (const e of (currentList === "favorites" ? favorites : recentEntries)) {
      lists[currentList].set(keyOf(e.host, e.port), { host: e.host, port: e.port, name: e.name || "", online: null });
    }
  }
  render();
  send("refresh", { list: currentList });
}
$("#refresh").addEventListener("click", refresh);
$("#search").addEventListener("input", render);

// ---- server table ----
function pingClass(ping) { return ping < 0 ? "" : ping < 80 ? "ping-good" : ping < 180 ? "ping-mid" : "ping-bad"; }

function render() {
  const filter = $("#search").value.trim().toLowerCase();
  const rows = [...lists[currentList].values()]
    .filter((s) => s.online !== false || currentList === "favorites" || currentList === "recent") // probes that found nothing
    .filter((s) => !filter || (s.name || "").toLowerCase().includes(filter) || keyOf(s.host, s.port).includes(filter))
    .sort((a, b) => (b.online === true) - (a.online === true) || (b.players || 0) - (a.players || 0) || (a.ping || 9999) - (b.ping || 9999));

  const body = $("#rows");
  body.textContent = "";
  for (const s of rows) {
    const key = keyOf(s.host, s.port);
    const tr = document.createElement("tr");
    tr.className = (key === selected ? "selected " : "") + (s.online === false ? "offline" : "");
    const cells = [
      { cls: "c-fav", text: isFavorite(key) ? "★" : "" },
      { cls: "c-lock", text: s.password ? "\u{1F512}" : "" },
      { cls: "name", text: s.name || (s.online === false ? "(offline)" : s.online === null ? "…" : key) },
      { cls: "c-num", text: s.online ? (s.players + " / " + s.maxPlayers) : "" },
      { cls: "c-num " + pingClass(s.ping), text: s.online && s.ping >= 0 ? s.ping + " ms" : "" },
      { cls: "c-addr", text: key },
    ];
    for (const c of cells) {
      const td = document.createElement("td");
      td.className = c.cls;
      td.textContent = c.text;
      tr.appendChild(td);
    }
    tr.addEventListener("click", () => select(key, s));
    tr.addEventListener("dblclick", () => { select(key, s); connectSelected(); });
    body.appendChild(tr);
  }

  const empty = $("#empty");
  empty.style.display = rows.length ? "none" : "block";
  empty.textContent = listErrors[currentList]
    ? listErrors[currentList]
    : currentList === "favorites" ? "No favourites yet: select a server and add it."
    : currentList === "recent" ? "Servers you join show up here."
    : "Looking for servers…";
  updateButtons();
}

function select(key, server) {
  selected = key;
  $("#address").value = key;
  render();
}

function updateButtons() {
  const key = $("#address").value.trim();
  const fav = $("#favorite-toggle");
  fav.disabled = !parseAddress(key);
  fav.textContent = isFavorite(key) ? "Remove favorite" : "Add to favorites";
}
$("#address").addEventListener("input", () => { selected = null; render(); });
$("#address").addEventListener("keydown", (e) => { if (e.key === "Enter") connectSelected(); });

function parseAddress(text) {
  const m = /^\s*([^\s:]+)(?::(\d{1,5}))?\s*$/.exec(text || "");
  if (!m) return null;
  const port = m[2] ? Number(m[2]) : 27015;
  return port > 0 && port < 65536 ? { host: m[1], port } : null;
}

function findServer(key) {
  for (const list of Object.values(lists)) if (list.has(key)) return list.get(key);
  return null;
}

function connectSelected() {
  const address = parseAddress($("#address").value);
  if (!address) return status("Enter a server address (host:port).", "error");
  const server = findServer(keyOf(address.host, address.port));
  if (server && server.password) {
    pendingPassword = address;
    $("#password-for").textContent = (server.name || keyOf(address.host, address.port)) + " needs a password.";
    $("#password").value = "";
    $("#password-modal").hidden = false;
    $("#password").focus();
    return;
  }
  send("connect", { host: address.host, port: address.port });
}
$("#connect").addEventListener("click", connectSelected);

$("#favorite-toggle").addEventListener("click", () => {
  const address = parseAddress($("#address").value);
  if (!address) return;
  const key = keyOf(address.host, address.port);
  const server = findServer(key);
  send("favorite", { host: address.host, port: address.port, name: server ? server.name : "", add: !isFavorite(key) });
});

$("#password-cancel").addEventListener("click", () => { $("#password-modal").hidden = true; pendingPassword = null; });
$("#password-ok").addEventListener("click", submitPassword);
$("#password").addEventListener("keydown", (e) => { if (e.key === "Enter") submitPassword(); if (e.key === "Escape") $("#password-cancel").click(); });
function submitPassword() {
  if (!pendingPassword) return;
  send("connect", { host: pendingPassword.host, port: pendingPassword.port, password: $("#password").value });
  $("#password-modal").hidden = true;
  pendingPassword = null;
}

// ---- settings ----
function showDistance() { $("#distance-value").textContent = $("#nametagDistance").value + " m"; }
$("#nametagDistance").addEventListener("input", showDistance);
$("#save").addEventListener("click", () => send("settings", {
  nickname: $("#nickname").value.trim(),
  nametags: $("#nametags").checked,
  nametagDistance: Number($("#nametagDistance").value),
  chat: $("#chat").checked,
}));

// ---- messages from the client ----
let recentEntries = [];
window.rdmp = {
  receive(message) {
    const { type, data } = message;
    if (type === "settings") {
      $("#nickname").value = data.nickname || "";
      $("#nametags").checked = !!data.nametags;
      $("#nametagDistance").value = data.nametagDistance || 60;
      $("#chat").checked = !!data.chat;
      showDistance();
    } else if (type === "version") {
      $("#version").textContent = data.version ? "v" + data.version : "";
    } else if (type === "lists") {
      favorites = data.favorites || [];
      recentEntries = data.recent || [];
      render();
    } else if (type === "server") {
      const list = lists[data.list];
      if (!list) return;
      list.set(keyOf(data.host, data.port), { ...data, name: data.name || (list.get(keyOf(data.host, data.port)) || {}).name || "" });
      if (data.list === currentList) render();
    } else if (type === "listError") {
      listErrors[data.list] = data.message;
      if (data.list === currentList) render();
    } else if (type === "status") {
      const kind = data.state === "error" ? "error" : data.state === "connecting" ? "busy" : data.state === "connected" ? "ok" : "";
      status(data.message, kind);
      if (data.state === "error" || data.state === "connected") send("refresh", { list: "recent" });
    }
  },
};

send("ready");
selectList("internet");
