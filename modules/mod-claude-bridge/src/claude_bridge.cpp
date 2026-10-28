/*
 * mod-claude-bridge: lets an external agent (Claude Code) talk through and command a playerbot.
 *
 * Incoming: whispers to the bridge bot and party chat in its group are appended to per-bot chat logs
 * as "unix_time|CHANNEL|sender|text" lines by the asynchronous writer. The world observer publishes
 * current JSON snapshots and event records to shared files. Messages with non-ASCII text
 * (conversation, e.g. Ukrainian) are kept away from the playerbot command parser; ASCII ones still reach it as commands.
 * Outgoing: console commands make the bot speak or forward a command to its playerbot AI:
 *   .claude say <bot> <text>              /say
 *   .claude yell <bot> <text>             /yell
 *   .claude party <bot> <text>            party chat
 *   .claude whisper <bot> <player> <text> whisper
 *   .claude cmd <bot> <command>           playerbot command, as if whispered by the bot's master
 */

#include "Chat.h"
#include "ChatCommand.h"
#include "CellImpl.h"
#include "CommandScript.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "UpdateTime.h"
#include "WorldPacket.h"
#include "World.h"
#include "WorldSessionMgr.h"
#include "action_dispatcher.h"
#include "world_state_observer.h"
#include "world_state_writer.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    // Bridge bots: "ClaudeBridge.BotNames" (comma separated), falling back to the older single "ClaudeBridge.BotName".
    bool IsBridgeBot(std::string const& name)
    {
        std::string list = sConfigMgr->GetOption<std::string>("ClaudeBridge.BotNames", "");
        if (list.empty())
            list = sConfigMgr->GetOption<std::string>("ClaudeBridge.BotName", "Claude");
        std::size_t start = 0;
        std::size_t count = 0;
        while (start <= list.size() && count++ < 16)
        {
            std::size_t end = list.find(',', start);
            std::string item = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
            item.erase(0, item.find_first_not_of(" \t"));
            item.erase(item.find_last_not_of(" \t") + 1);
            if (!item.empty() && item == name)
                return true;
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        return false;
    }

    bool IsChatContextBot(std::string const& name)
    {
        std::string list = sConfigMgr->GetOption<std::string>("ClaudeBridge.ChatContextBotNames", "Claude");
        std::size_t start = 0;
        std::size_t count = 0;
        while (start <= list.size() && count++ < 16)
        {
            std::size_t end = list.find(',', start);
            std::string item = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
            std::size_t first = item.find_first_not_of(" \t");
            if (first == std::string::npos)
                item.clear();
            else
                item.erase(0, first);
            std::size_t last = item.find_last_not_of(" \t");
            if (last != std::string::npos)
                item.erase(last + 1);
            if (!item.empty() && item == name)
                return true;
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        return false;
    }

    // Each bridge bot gets its own log: <ClaudeBridge.LogDir>/<lowercase bot name>-chat.log
    void AppendLog(std::string const& botName, std::string const& channel, std::string const& sender,
        std::string const& text, Player const* contextPlayer = nullptr)
    {
        std::string loggedText = text;
        if (contextPlayer && IsChatContextBot(botName))
        {
            std::string context = GetWorldChatContext(contextPlayer);
            if (!context.empty())
                loggedText += "\n\n" + context;
        }
        GetWorldStateWriter().PublishChatLine(botName, channel, sender, loggedText);
    }

    std::vector<Player*> GetGroupPlayers(Group* group, Player* first = nullptr, Player* second = nullptr)
    {
        std::vector<Player*> players;
        auto addPlayer = [&players](Player* player)
        {
            if (player && player->IsInWorld() && std::find(players.begin(), players.end(), player) == players.end())
                players.push_back(player);
        };

        if (group)
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                addPlayer(itr->GetSource());

        addPlayer(first);
        addPlayer(second);
        return players;
    }

    Player* FindBot(ChatHandler* handler, std::string const& name)
    {
        Player* bot = ObjectAccessor::FindPlayerByName(name);
        if (!bot)
            handler->PSendSysMessage("claude: player '{}' is not online", name);
        return bot;
    }

    // Speak in the faction language like a real player does; the client draws no chat bubble for LANG_UNIVERSAL.
    Language BotLanguage(Player* bot) { return bot->GetTeamId() == TEAM_ALLIANCE ? LANG_COMMON : LANG_ORCISH; }

    // Playerbot commands are plain ASCII words; anything else (e.g. Cyrillic) is conversation meant only for Claude.
    bool IsConversation(std::string const& msg)
    {
        for (unsigned char c : msg)
            if (c >= 0x80)
                return true;
        return false;
    }

    // Set while .claude cmd replays a command through the chat hooks, so it is neither logged nor filtered.
    thread_local bool replayingCommand = false;
}

class ClaudeBridgePlayerScript : public PlayerScript
{
public:
    ClaudeBridgePlayerScript() : PlayerScript("ClaudeBridgePlayerScript", {
        PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT
    }) { }

    using PlayerScript::OnPlayerCanUseChat;

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Player* receiver) override
    {
        if (replayingCommand || type != CHAT_MSG_WHISPER || lang == LANG_ADDON || !receiver || !IsBridgeBot(receiver->GetName()))
            return true;

        AppendLog(receiver->GetName(), "WHISPER", player->GetName(), msg, receiver);
        if (!IsConversation(msg))
            return true;

        // keep conversation away from the playerbot command parser, but still show the sender their whisper
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER_INFORM, LANG_UNIVERSAL, receiver, receiver, msg);
        player->SendDirectMessage(&data);
        return false;
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Group* group) override
    {
        if (replayingCommand || lang == LANG_ADDON || IsBridgeBot(player->GetName()))
            return true;

        std::vector<Player*> members = GetGroupPlayers(group, player);
        bool botInGroup = false;
        for (Player* member : members)
            if (IsBridgeBot(member->GetName()))
                botInGroup = true;
        if (!botInGroup)
            return true;

        for (Player* member : members)
            if (IsBridgeBot(member->GetName()))
                AppendLog(member->GetName(), "PARTY", player->GetName(), msg, member);

        if (!IsConversation(msg))
            return true;

        // deliver the message to the group ourselves and stop it before the playerbot command parser
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, ChatMsg(type), Language(lang), player, nullptr, msg);
        group->BroadcastPacket(&data, false, type == CHAT_MSG_PARTY || type == CHAT_MSG_PARTY_LEADER ? group->GetMemberGroup(player->GetGUID()) : -1);
        return false;
    }
};

class ClaudeBridgeCommandScript : public CommandScript
{
public:
    ClaudeBridgeCommandScript() : CommandScript("ClaudeBridgeCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable claudeTable = {
            {"say",     HandleSay,     SEC_ADMINISTRATOR, Console::Yes},
            {"yell",    HandleYell,    SEC_ADMINISTRATOR, Console::Yes},
            {"party",   HandleParty,   SEC_ADMINISTRATOR, Console::Yes},
            {"whisper", HandleWhisper, SEC_ADMINISTRATOR, Console::Yes},
            {"cmd",     HandleCmd,     SEC_ADMINISTRATOR, Console::Yes},
            {"action",  HandleAction,  SEC_ADMINISTRATOR, Console::Yes},
        };
        static ChatCommandTable commandTable = {
            {"claude", claudeTable},
        };
        return commandTable;
    }

    static bool HandleSay(ChatHandler* handler, std::string botName, Tail text)
    {
        Player* bot = FindBot(handler, botName);
        if (!bot || text.empty())
            return false;
        bot->Say(std::string(text), BotLanguage(bot));
        return true;
    }

    static bool HandleYell(ChatHandler* handler, std::string botName, Tail text)
    {
        Player* bot = FindBot(handler, botName);
        if (!bot || text.empty())
            return false;
        bot->Yell(std::string(text), BotLanguage(bot));
        return true;
    }

    static bool HandleParty(ChatHandler* handler, std::string botName, Tail text)
    {
        Player* bot = FindBot(handler, botName);
        if (!bot || text.empty())
            return false;
        Group* group = bot->GetGroup();
        if (!group)
        {
            handler->PSendSysMessage("claude: {} is not in a group", botName);
            return false;
        }
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_PARTY, BotLanguage(bot), bot, nullptr, std::string(text));
        group->BroadcastPacket(&data, false);
        return true;
    }

    static bool HandleWhisper(ChatHandler* handler, std::string botName, std::string targetName, Tail text)
    {
        Player* bot = FindBot(handler, botName);
        Player* target = FindBot(handler, targetName);
        if (!bot || !target || text.empty())
            return false;
        bot->Whisper(std::string(text), LANG_UNIVERSAL, target);
        return true;
    }

    // Forward a playerbot command: replay it through the chat hooks as a whisper from the bot's master,
    // so mod-playerbots handles it exactly like a real whisper.
    static bool HandleCmd(ChatHandler* handler, std::string botName, Tail text)
    {
        Player* bot = FindBot(handler, botName);
        if (!bot || text.empty())
            return false;
        Player* master = nullptr;
        if (Group* group = bot->GetGroup())
            master = ObjectAccessor::FindPlayer(group->GetLeaderGUID());
        if (!master || master == bot)
        {
            handler->PSendSysMessage("claude: {} has no group leader to act as master", botName);
            return false;
        }
        std::string msg(text);
        replayingCommand = true;
        bool const passed = sScriptMgr->OnPlayerCanUseChat(master, CHAT_MSG_WHISPER, LANG_UNIVERSAL, msg, bot);
        replayingCommand = false;
        if (!passed)
            handler->PSendSysMessage("claude: command was consumed by a chat hook");
        return true;
    }

    // Typed bridge action: all values are bounded scalar tokens and the dispatcher maps
    // the final enum to a fixed PlayerbotAI action. This command is SOAP/console-only.
    static bool HandleAction(ChatHandler* handler, std::string actorName, std::string requesterName,
        std::string groupId, std::string actionId, std::string actionType,
        std::string spellId, std::string targetGuid, std::string optionIndex)
    {
        if (!handler->IsConsole())
        {
            handler->SendSysMessage("claude: typed actions are console-only");
            return false;
        }

        ActionResult const result = DispatchTypedAction(actorName, requesterName, groupId, actionId, actionType,
            spellId, targetGuid, optionIndex);
        handler->PSendSysMessage("ACTION_RESULT: {}", result.ToJson());
        return true;
    }
};

void AddClaudeBridgeScripts()
{
    new ClaudeBridgePlayerScript();
    new ClaudeBridgeCommandScript();
}
