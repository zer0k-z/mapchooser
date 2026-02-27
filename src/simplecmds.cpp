#include <stdio.h>
#include <string.h>
#include "simplecmds.h"

#define MAX_COMMANDS 128
#define MAX_COMMAND_NAME 64

struct Command
{
	char name[MAX_COMMAND_NAME];
	CommandCallback_t callback;
};

static Command g_commands[MAX_COMMANDS];
static int g_command_count = 0;
static CommandCallback_t g_wildcard_callback = nullptr;

// Find command by name
static Command* FindCommand(const char *name)
{
	for (int i = 0; i < g_command_count; i++)
	{
		if (_stricmp(g_commands[i].name, name) == 0)
		{
			return &g_commands[i];
		}
	}
	return nullptr;
}

void scmd::SetWildcardCallback(CommandCallback_t callback)
{
	g_wildcard_callback = callback;
}

// Register a command
bool scmd::RegisterCmd(const char *name, CommandCallback_t callback)
{
	if (!name || !callback || g_command_count >= MAX_COMMANDS)
	{
		return false;
	}

	int name_len = strlen(name);
	if (name_len == 0 || name_len >= MAX_COMMAND_NAME)
	{
		return false;
	}

	// Check if already exists
	if (FindCommand(name))
	{
		return false;
	}

	Command *cmd = &g_commands[g_command_count++];
	V_strncpy(cmd->name, name, MAX_COMMAND_NAME);
	cmd->callback = callback;

	return true;
}

// Unregister a command
bool scmd::UnregisterCmd(const char *name)
{
	for (int i = 0; i < g_command_count; i++)
	{
		if (_stricmp(g_commands[i].name, name) == 0)
		{
			// Shift remaining commands
			for (int j = i; j < g_command_count - 1; j++)
			{
				g_commands[j] = g_commands[j + 1];
			}
			g_command_count--;
			return true;
		}
	}
	return false;
}

// Handle client commands (direct console commands like "cmd_test")
META_RES scmd::OnClientCommand(CPlayerSlot slot, const CCommand &args)
{
	if (args.ArgC() < 1)
	{
		return MRES_IGNORED;
	}

	Command *cmd = FindCommand(args[0]);
	if (cmd && cmd->callback)
	{
		cmd->callback(slot.Get(), &args);
		return MRES_SUPERCEDE;
	}
	return MRES_IGNORED;
}

// Handle dispatched console commands (covers say/say_team chat triggers and console overrides)
META_RES scmd::OnDispatchConCommand(ConCommandRef cmd, const CCommandContext &ctx, const CCommand &args)
{
	if (!cmd.IsValidRef())
	{
		return MRES_IGNORED;
	}

	CPlayerSlot slot = ctx.GetPlayerSlot();
	if (slot.Get() < 0)
	{
		return MRES_IGNORED;
	}

	const char *commandName = cmd.GetName();

	if (!V_stricmp(commandName, "say") || !V_stricmp(commandName, "say_team"))
	{
		if (args.ArgC() < 2)
		{
			return MRES_IGNORED;
		}

		const char *msg = args[1];
		if (msg[0] != CHAT_TRIGGER && msg[0] != CHAT_SILENT_TRIGGER)
		{
			return MRES_IGNORED;
		}

		bool silent = (msg[0] == CHAT_SILENT_TRIGGER);

		CCommand cmdArgs;
		cmdArgs.Tokenize(msg);

		if (cmdArgs.ArgC() < 1)
		{
			return MRES_IGNORED;
		}

		// Skip the trigger character
		const char *triggeredName = cmdArgs[0] + 1;

		bool matched = false;
		for (int i = 0; i < g_command_count; i++)
		{
			if (!g_commands[i].callback)
			{
				continue;
			}

			// If the command has the console prefix (e.g. "cmd_test"), also match the short form ("test")
			const char *cmdName = g_commands[i].name;
			int prefixLen = strlen(CONSOLE_PREFIX);
			if (V_strnicmp(cmdName, CONSOLE_PREFIX, prefixLen) == 0)
			{
				cmdName = cmdName + prefixLen;
			}

			if (!V_stricmp(triggeredName, cmdName))
			{
				matched = true;
				META_RES result = g_commands[i].callback(slot.Get(), &cmdArgs);
				if (silent || result == MRES_SUPERCEDE)
				{
					return MRES_SUPERCEDE; // suppress the chat message
				}
			}
		}

		if (!matched && g_wildcard_callback)
		{
			META_RES result = g_wildcard_callback(slot.Get(), &cmdArgs);
			if (silent || result == MRES_SUPERCEDE)
			{
				return MRES_SUPERCEDE;
			}
		}
	}
	else
	{
		// Console command override: check if any registered command matches
		for (int i = 0; i < g_command_count; i++)
		{
			if (!g_commands[i].callback)
			{
				continue;
			}

			const char *cmdName = g_commands[i].name;
			int prefixLen = strlen(CONSOLE_PREFIX);
			if (V_strnicmp(cmdName, CONSOLE_PREFIX, prefixLen) == 0)
			{
				cmdName = cmdName + prefixLen;
			}

			if (!V_stricmp(commandName, cmdName))
			{
				META_RES result = g_commands[i].callback(slot.Get(), &args);
				if (result == MRES_SUPERCEDE)
				{
					return result;
				}
			}
		}
	}

	return MRES_IGNORED;
}
