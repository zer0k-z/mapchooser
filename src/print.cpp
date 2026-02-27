#include "print.h"
#include "plugin.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ISmmPlugin.h>
#include <networksystem/inetworkmessages.h>
#include <engine/igameeventsystem.h>
#include "usermessages.pb.h"

// HUD destination values for TextMsg
#define MC_HUD_PRINTTALK 3

static void SendTextMsg(int slot, const char *text)
{
    if (!g_pNetworkMessages || !g_pGameEventSystem)
        return;

    INetworkMessageInternal *netmsg = g_pNetworkMessages->FindNetworkMessagePartial("TextMsg");
    if (!netmsg)
        return;

    auto msg = netmsg->AllocateMessage()->ToPB<CUserMessageTextMsg>();
    if (!msg)
        return;

    // CS2 requires a leading space for color codes on the first character to apply.
    char spaced[512];
    spaced[0] = ' ';
    strncpy(spaced + 1, text, sizeof(spaced) - 2);
    spaced[sizeof(spaced) - 1] = '\0';

    msg->set_dest(MC_HUD_PRINTTALK);
    msg->add_param(spaced);

    if (slot < 0)
    {
        // broadcast
        g_pGameEventSystem->PostEventAbstract(0, false, -1, nullptr, netmsg, msg, 0, BUF_RELIABLE);
    }
    else
    {
        // single recipient bit-mask: bit N = slot N
        uint64 mask = (1ULL << slot);
        g_pGameEventSystem->PostEventAbstract(0, false, 1, &mask, netmsg, msg, 0, BUF_RELIABLE);
    }

    delete msg;
}

void PrintChatToSlot(int slot, const char *fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    SendTextMsg(slot, buf);
}

void PrintChatAll(const char *fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    SendTextMsg(-1, buf);
}
