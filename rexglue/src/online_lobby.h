#pragma once

#include <string>

namespace xerenge::online {

// With --online and no --lobby_server: starts this copy's lobby server and its
// LAN beacon. Called when the game starts; a no-op otherwise, and when called
// again.
void StartLobbyNetwork();

// --lobby_server as an IPv4 address (its name resolved here), without a port; "" with no
// server. The title cannot look a name up itself: its DNS lookup answers nothing.
std::string LobbyServerAddress();

}  // namespace xerenge::online
