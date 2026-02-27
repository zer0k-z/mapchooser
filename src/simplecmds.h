#ifndef _INCLUDE_SIMPLECMDS_H_
#define _INCLUDE_SIMPLECMDS_H_

#include <ISmmPlugin.h>
#include <iplayerinfo.h>
#include <igameevents.h>

class CCSPlayerController;

// Command callback type
typedef META_RES (*CommandCallback_t)(int controller_id, const CCommand *args);

namespace scmd
{
	// Register a client command
	bool RegisterCmd(const char *name, CommandCallback_t callback);
	
	// Unregister a command
	bool UnregisterCmd(const char *name);

	// Set a wildcard callback invoked when no specific command matches.
	// args[0] is the trigger + command name (e.g. "!3"), so the name is args[0]+1.
	void SetWildcardCallback(CommandCallback_t callback);

	// Hook functions to be called from metamod
	META_RES OnClientCommand(CPlayerSlot slot, const CCommand &args);
	META_RES OnDispatchConCommand(ConCommandRef cmd, const CCommandContext &ctx, const CCommand &args);
};

// Helper class for easy command registration
class CmdRegister
{
public:
	CmdRegister(const char *name, CommandCallback_t callback)
	{
		scmd::RegisterCmd(name, callback);
	}
};

// Macro for easy command definition
#define CMD(name) \
	static META_RES name##_callback(int controller_id, const CCommand *args); \
	static CmdRegister name##_reg(#name, name##_callback); \
	static META_RES name##_callback(int controller_id, const CCommand *args)

#define CMD_NAMED(ident, cmdname) \
	static META_RES ident##_callback(int controller_id, const CCommand *args); \
	static CmdRegister ident##_reg(cmdname, ident##_callback); \
	static META_RES ident##_callback(int controller_id, const CCommand *args)

// Define a wildcard/catch-all handler for unmatched commands.
// Inside the body, use (args[0] + 1) to get the command name after the trigger.
#define CMD_WILDCARD(ident) \
	static META_RES ident##_callback(int controller_id, const CCommand *args); \
	struct ident##_WildcardReg { ident##_WildcardReg() { scmd::SetWildcardCallback(ident##_callback); } }; \
	static ident##_WildcardReg ident##_reg; \
	static META_RES ident##_callback(int controller_id, const CCommand *args)

#define CHAT_TRIGGER        '!'
#define CHAT_SILENT_TRIGGER '/'
#define CONSOLE_PREFIX      "cmd_"

#endif // _INCLUDE_SIMPLECMDS_H_
