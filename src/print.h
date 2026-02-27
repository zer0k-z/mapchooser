#pragma once

// Print a chat message to a single player slot (0-based).
void PrintChatToSlot(int slot, const char *fmt, ...);

// Print a chat message to all connected players.
void PrintChatAll(const char *fmt, ...);
