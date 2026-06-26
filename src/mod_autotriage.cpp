#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptDefines/TicketScript.h" // → TicketMgr.h → GmTicket

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>

// ---------------------------------------------------------------------------
// Triage enumerations
// ---------------------------------------------------------------------------

enum TriageCategory : uint8
{
    TRIAGE_BUG      = 0,
    TRIAGE_ABUSE    = 1,
    TRIAGE_QUESTION = 2,
    TRIAGE_OTHER    = 3,
};

static constexpr const char* TRIAGE_CATEGORY_NAMES[] = { "BUG", "ABUSE", "QUESTION", "OTHER" };

// 1 = Critical  2 = High  3 = Medium  4 = Low
enum TriagePriority : uint8
{
    TRIAGE_PRIORITY_CRITICAL = 1,
    TRIAGE_PRIORITY_HIGH     = 2,
    TRIAGE_PRIORITY_MEDIUM   = 3,
    TRIAGE_PRIORITY_LOW      = 4,
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return s;
}

static bool ContainsAny(const std::string& haystack,
                         std::initializer_list<const char*> needles)
{
    for (const char* needle : needles)
        if (haystack.find(needle) != std::string::npos)
            return true;
    return false;
}

// ---------------------------------------------------------------------------
// Classification
// ABUSE is checked first so "cheat exploit" isn't mis-filed as BUG.
// ---------------------------------------------------------------------------

static TriageCategory ClassifyMessage(const std::string& rawMessage)
{
    std::string msg = ToLower(rawMessage);

    if (ContainsAny(msg, {
        "hack", "hacking", "cheat", "cheating", "bot", "botting",
        "gold sell", "gold farm", "spam", "troll", "harass",
        "abuse", "inappropriate", "griefing", "racism", "racist",
        "offensive name", "offensive language"
    }))
        return TRIAGE_ABUSE;

    if (ContainsAny(msg, {
        "bug", "broken", "crash", "error", "not work", "doesn't work",
        "doesnt work", "stuck", "glitch", "freeze", "disconnect", "dc",
        "lag spike", "issue", "can't move", "cannot move", "invisible wall",
        "missing item", "lost item", "quest bug", "clipping", "fell through",
        "exploit"
    }))
        return TRIAGE_BUG;

    if (ContainsAny(msg, {
        "how", "what", "where", "when", "why", "?", "help",
        "can i", "could you", "please assist", "need help",
        "question", "is it possible", "how do i", "how do you"
    }))
        return TRIAGE_QUESTION;

    return TRIAGE_OTHER;
}

static uint8 AssignPriority(TriageCategory category, const std::string& lowerMsg)
{
    switch (category)
    {
        case TRIAGE_BUG:
            // Economy / data-integrity bugs escalate to Critical
            if (ContainsAny(lowerMsg, {
                "dupe", "duplication", "item dupe", "crash",
                "data loss", "exploit", "gold dupe"
            }))
                return TRIAGE_PRIORITY_CRITICAL;
            return TRIAGE_PRIORITY_HIGH;
        case TRIAGE_ABUSE:
            return TRIAGE_PRIORITY_HIGH;
        case TRIAGE_QUESTION:
            return TRIAGE_PRIORITY_LOW;
        default:
            return TRIAGE_PRIORITY_MEDIUM;
    }
}

// ---------------------------------------------------------------------------
// TicketScript — hooks into GM ticket lifecycle events.
//
// Hook naming confirmed from:
//   src/server/game/Scripting/ScriptDefines/TicketScript.h
// GmTicket API confirmed from:
//   src/server/game/Tickets/TicketMgr.h
//   GmTicket::GetPlayer() => ObjectAccessor::FindConnectedPlayer(_playerGuid)
//   GmTicket has no public GetPlayerGuid(); use GetPlayer()->GetGUID() instead.
// ---------------------------------------------------------------------------

class AutoTriageScript : public TicketScript
{
public:
    AutoTriageScript() : TicketScript("AutoTriageScript") {}

    void OnTicketClose(GmTicket* ticket) override
    {
        if (!ticket || !sConfigMgr->GetOption<bool>("AutoTriage.Enable", true))
            return;

        // Stamp resolution time so stats and the periodic escalation query both work.
        // The WHERE guard prevents double-stamping if the hook fires more than once.
        CharacterDatabase.Execute(
            "UPDATE `gm_ticket_triage` SET `resolved_at` = NOW() "
            "WHERE `ticket_id` = {} AND `resolved_at` IS NULL",
            ticket->GetId()
        );
    }

    // Correct virtual method name — NOT OnGmTicketCreate.
    void OnTicketCreate(GmTicket* ticket) override
    {
        if (!ticket || !sConfigMgr->GetOption<bool>("AutoTriage.Enable", true))
            return;

        // At ticket creation the submitting player is always connected.
        // GetPlayer() calls FindConnectedPlayer internally; guard anyway for
        // the rare disconnect-during-submit race.
        Player* player = ticket->GetPlayer();

        std::string const& rawMessage = ticket->GetMessage();
        std::string lowerMsg          = ToLower(rawMessage);
        TriageCategory category       = ClassifyMessage(rawMessage);
        uint8 priority                = AssignPriority(category, lowerMsg);
        const char* categoryName      = TRIAGE_CATEGORY_NAMES[category];
        uint32 ticketId               = ticket->GetId();
        uint32 playerGuid             = player ? player->GetGUID().GetCounter() : 0;

        // Auto-reply — only possible while player is online.
        bool replySent = false;
        if (player && sConfigMgr->GetOption<bool>("AutoTriage.SendAutoReply", true))
        {
            ChatHandler(player->GetSession()).SendSysMessage(BuildReply(category));
            replySent = true;
        }

        // Persist triage result.
        std::string snippet = rawMessage.substr(0, 255);
        CharacterDatabase.EscapeString(snippet);

        CharacterDatabase.Execute(
            "INSERT INTO `gm_ticket_triage` "
            "(`ticket_id`, `player_guid`, `category`, `priority`, `message_snippet`, `auto_reply_sent`) "
            "VALUES ({}, {}, '{}', {}, '{}', {})",
            ticketId, playerGuid, categoryName,
            uint32(priority), snippet, replySent ? 1 : 0
        );

        LOG_INFO("module",
            "mod-autotriage: Ticket #{} | PlayerGUID {} | Category: {} | Priority: {} | ReplySent: {}",
            ticketId, playerGuid, categoryName, priority, replySent ? "yes" : "no");
    }

private:
    static std::string BuildReply(TriageCategory category)
    {
        switch (category)
        {
            case TRIAGE_BUG:
                return sConfigMgr->GetOption<std::string>(
                    "AutoTriage.Reply.Bug",
                    "[Support] Your bug report has been received (Priority: HIGH). A GM will investigate shortly.");
            case TRIAGE_ABUSE:
                return sConfigMgr->GetOption<std::string>(
                    "AutoTriage.Reply.Abuse",
                    "[Support] Your misconduct report has been received. Our GM team will review it promptly.");
            case TRIAGE_QUESTION:
                return sConfigMgr->GetOption<std::string>(
                    "AutoTriage.Reply.Question",
                    "[Support] Thank you for your question. A GM will assist you as soon as possible.");
            default:
                return sConfigMgr->GetOption<std::string>(
                    "AutoTriage.Reply.Other",
                    "[Support] Your ticket has been received and is queued for GM review.");
        }
    }
};

// ---------------------------------------------------------------------------
// WorldScript — logs effective config on startup and reload so admins can
// confirm the .conf file was actually picked up.
// ---------------------------------------------------------------------------

class AutoTriageWorldScript : public WorldScript
{
public:
    AutoTriageWorldScript() : WorldScript("AutoTriageWorldScript") {}

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        bool enabled    = sConfigMgr->GetOption<bool>("AutoTriage.Enable", true);
        bool autoReply  = sConfigMgr->GetOption<bool>("AutoTriage.SendAutoReply", true);

        if (enabled)
            LOG_INFO("module",
                "mod-autotriage loaded — AutoReply: {} | Bug: '{}' | Abuse: '{}' | Question: '{}' | Other: '{}'",
                autoReply ? "on" : "off",
                sConfigMgr->GetOption<std::string>("AutoTriage.Reply.Bug",      "(default)"),
                sConfigMgr->GetOption<std::string>("AutoTriage.Reply.Abuse",    "(default)"),
                sConfigMgr->GetOption<std::string>("AutoTriage.Reply.Question", "(default)"),
                sConfigMgr->GetOption<std::string>("AutoTriage.Reply.Other",    "(default)")
            );
        else
            LOG_INFO("module", "mod-autotriage: disabled via AutoTriage.Enable = 0");
    }
};

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void AddSC_AutoTriage()
{
    new AutoTriageScript();
    new AutoTriageWorldScript();
}
