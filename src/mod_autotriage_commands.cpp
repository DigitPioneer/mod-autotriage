#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "TicketMgr.h"   // GmTicket, sTicketMgr

#include <algorithm>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const char* PriorityLabel(uint8 p)
{
    switch (p)
    {
        case 1: return "|cffFF4444Critical|r";
        case 2: return "|cffFF8800High|r";
        case 3: return "|cffFFFF00Medium|r";
        case 4: return "|cff44FF44Low|r";
        default: return "?";
    }
}

static const char* PriorityName(uint8 p)
{
    switch (p)
    {
        case 1: return "Critical";
        case 2: return "High";
        case 3: return "Medium";
        case 4: return "Low";
        default: return "?";
    }
}

// ---------------------------------------------------------------------------
// CommandScript
//
// Commands (all require SEC_GAMEMASTER):
//   .triage list [BUG|ABUSE|QUESTION|OTHER] [1-4]
//   .triage info  <ticket_id>
//   .triage assign <ticket_id>
//   .triage setpriority <ticket_id> <1-4>
//   .triage stats
// ---------------------------------------------------------------------------

class AutoTriageCommandScript : public CommandScript
{
public:
    AutoTriageCommandScript() : CommandScript("AutoTriageCommandScript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable sub =
        {
            { "list",        SEC_GAMEMASTER, false, &HandleList,        "" },
            { "info",        SEC_GAMEMASTER, false, &HandleInfo,        "" },
            { "assign",      SEC_GAMEMASTER, false, &HandleAssign,      "" },
            { "setpriority", SEC_GAMEMASTER, false, &HandleSetPriority, "" },
            { "stats",       SEC_GAMEMASTER, false, &HandleStats,       "" },
        };

        static ChatCommandTable root =
        {
            { "triage", SEC_GAMEMASTER, false, nullptr, "", sub },
        };

        return root;
    }

    // -----------------------------------------------------------------------
    // .triage list [category] [priority]
    //   Optional first arg: BUG | ABUSE | QUESTION | OTHER | 1 | 2 | 3 | 4
    // -----------------------------------------------------------------------
    static bool HandleList(ChatHandler* handler, char const* args)
    {
        // Build a validated WHERE clause suffix from the optional filter arg.
        std::string filter;
        if (args && *args)
        {
            std::string arg(args);
            // trim and uppercase
            arg.erase(0, arg.find_first_not_of(" \t"));
            arg.erase(arg.find_last_not_of(" \t") + 1);
            std::transform(arg.begin(), arg.end(), arg.begin(), ::toupper);

            if (arg == "BUG" || arg == "ABUSE" || arg == "QUESTION" || arg == "OTHER")
                filter = " AND t.category = '" + arg + "'";
            else if (arg == "1" || arg == "2" || arg == "3" || arg == "4")
                filter = " AND t.priority = " + arg;
            else
            {
                handler->SendSysMessage("[Triage] Filter must be BUG | ABUSE | QUESTION | OTHER | 1 | 2 | 3 | 4.");
                return true;
            }
        }

        // gm_ticket.closedBy = 0 means the ticket is still open.
        std::string sql =
            "SELECT t.ticket_id, t.player_guid, t.category, t.priority, "
            "t.created_at, TIMESTAMPDIFF(MINUTE, t.created_at, NOW()) AS age_min "
            "FROM gm_ticket_triage t "
            "LEFT JOIN gm_ticket g ON g.id = t.ticket_id "
            "WHERE t.resolved_at IS NULL AND (g.closedBy = 0 OR g.id IS NULL)" +
            filter +
            " ORDER BY t.priority ASC, t.created_at ASC LIMIT 25";

        QueryResult result = CharacterDatabase.Query(sql.c_str());

        if (!result)
        {
            handler->SendSysMessage("[Triage] No open tickets found.");
            return true;
        }

        handler->PSendSysMessage("|cffFFFFFF%-6s %-9s %-10s %-20s %s|r",
            "TktID", "Category", "Priority", "Created", "Age");

        do
        {
            Field* f     = result->Fetch();
            uint32 id    = f[0].Get<uint32>();
            // f[1] = player_guid (unused in list for brevity)
            std::string  cat = f[2].Get<std::string>();
            uint8  prio  = f[3].Get<uint8>();
            std::string  ts  = f[4].Get<std::string>();
            int32  age   = f[5].Get<int32>();

            handler->PSendSysMessage("#%-5u %-9s %-10s %-20s %dm",
                id, cat.c_str(), PriorityName(prio), ts.c_str(), age);
        }
        while (result->NextRow());

        return true;
    }

    // -----------------------------------------------------------------------
    // .triage info <ticket_id>
    // -----------------------------------------------------------------------
    static bool HandleInfo(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
        {
            handler->SendSysMessage("[Triage] Usage: .triage info <ticket_id>");
            return true;
        }

        uint32 ticketId = static_cast<uint32>(std::atoi(args));
        if (!ticketId)
        {
            handler->SendSysMessage("[Triage] Invalid ticket ID.");
            return true;
        }

        QueryResult result = CharacterDatabase.Query(
            "SELECT t.ticket_id, t.player_guid, t.category, t.priority, "
            "t.priority_override, t.message_snippet, t.auto_reply_sent, "
            "t.created_at, t.resolved_at, "
            "TIMESTAMPDIFF(SECOND, t.created_at, IFNULL(t.resolved_at, NOW())) AS age_sec "
            "FROM gm_ticket_triage t WHERE t.ticket_id = {}",
            ticketId);

        if (!result)
        {
            handler->PSendSysMessage("[Triage] Ticket #%u not found.", ticketId);
            return true;
        }

        Field* f             = result->Fetch();
        uint32 id            = f[0].Get<uint32>();
        uint32 pguid         = f[1].Get<uint32>();
        std::string cat      = f[2].Get<std::string>();
        uint8  prio          = f[3].Get<uint8>();
        bool   overridden    = f[4].Get<uint8>() != 0;
        std::string snippet  = f[5].Get<std::string>();
        bool   replied       = f[6].Get<uint8>() != 0;
        std::string created  = f[7].Get<std::string>();
        std::string resolved = f[8].IsNull() ? "Open" : f[8].Get<std::string>();
        int64  ageSec        = f[9].Get<int64>();

        handler->PSendSysMessage("=== Triage: Ticket #%u ===", id);
        handler->PSendSysMessage("Player GUID : %u", pguid);
        handler->PSendSysMessage("Category    : %s", cat.c_str());
        handler->PSendSysMessage("Priority    : %s %s", PriorityLabel(prio),
            overridden ? "(manually set)" : "(auto-classified)");
        handler->PSendSysMessage("Created     : %s (%lld min ago)", created.c_str(), ageSec / 60);
        handler->PSendSysMessage("Resolved    : %s", resolved.c_str());
        handler->PSendSysMessage("Auto-reply  : %s", replied ? "Yes" : "No");
        handler->PSendSysMessage("Message     : %.200s%s",
            snippet.c_str(), snippet.size() >= 200 ? "..." : "");

        return true;
    }

    // -----------------------------------------------------------------------
    // .triage assign <ticket_id>
    //   Assigns the ticket to the calling GM in the game's ticket system.
    // -----------------------------------------------------------------------
    static bool HandleAssign(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
        {
            handler->SendSysMessage("[Triage] Usage: .triage assign <ticket_id>");
            return true;
        }

        Player* gm = handler->GetPlayer();
        if (!gm)
        {
            handler->SendSysMessage("[Triage] This command requires an in-game session.");
            return true;
        }

        uint32 ticketId = static_cast<uint32>(std::atoi(args));
        if (!ticketId)
        {
            handler->SendSysMessage("[Triage] Invalid ticket ID.");
            return true;
        }

        // sTicketMgr is defined in TicketMgr.h as TicketMgr::instance().
        // If GetGmTicketById is named differently in your fork, check TicketMgr.h.
        GmTicket* ticket = sTicketMgr->GetGmTicketById(ticketId);
        if (!ticket || ticket->IsClosed())
        {
            handler->PSendSysMessage("[Triage] Ticket #%u not found or already closed.", ticketId);
            return true;
        }

        bool isAdmin = gm->GetSession()->GetSecurity() >= SEC_ADMINISTRATOR;
        ticket->SetAssignedTo(gm->GetGUID(), isAdmin);

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        ticket->SaveToDB(trans);
        CharacterDatabase.CommitTransaction(trans);

        handler->PSendSysMessage("[Triage] Ticket #%u assigned to %s.",
            ticketId, gm->GetName().c_str());

        return true;
    }

    // -----------------------------------------------------------------------
    // .triage setpriority <ticket_id> <1-4>
    //   Overrides the auto-assigned priority and sets priority_override = 1.
    // -----------------------------------------------------------------------
    static bool HandleSetPriority(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
        {
            handler->SendSysMessage("[Triage] Usage: .triage setpriority <ticket_id> <1=Critical 2=High 3=Medium 4=Low>");
            return true;
        }

        char buf[64];
        std::strncpy(buf, args, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        char* tok1 = std::strtok(buf, " ");
        char* tok2 = std::strtok(nullptr, " ");

        if (!tok1 || !tok2)
        {
            handler->SendSysMessage("[Triage] Usage: .triage setpriority <ticket_id> <1-4>");
            return true;
        }

        uint32 ticketId = static_cast<uint32>(std::atoi(tok1));
        uint8  newPrio  = static_cast<uint8>(std::atoi(tok2));

        if (!ticketId || newPrio < 1 || newPrio > 4)
        {
            handler->SendSysMessage("[Triage] Priority must be 1 (Critical), 2 (High), 3 (Medium), or 4 (Low).");
            return true;
        }

        CharacterDatabase.Execute(
            "UPDATE `gm_ticket_triage` SET `priority` = {}, `priority_override` = 1 "
            "WHERE `ticket_id` = {}",
            uint32(newPrio), ticketId
        );

        handler->PSendSysMessage("[Triage] Ticket #%u priority set to %s.",
            ticketId, PriorityLabel(newPrio));

        return true;
    }

    // -----------------------------------------------------------------------
    // .triage stats
    //   Category counts, priority counts, and resolution time summary.
    // -----------------------------------------------------------------------
    static bool HandleStats(ChatHandler* handler, char const* /*args*/)
    {
        // --- Category breakdown ---
        handler->SendSysMessage("=== Triage Stats by Category ===");
        {
            QueryResult r = CharacterDatabase.Query(
                "SELECT category, COUNT(*) FROM gm_ticket_triage GROUP BY category ORDER BY category");
            if (r)
                do {
                    Field* f = r->Fetch();
                    handler->PSendSysMessage("  %-9s : %u",
                        f[0].Get<std::string>().c_str(), f[1].Get<uint32>());
                } while (r->NextRow());
            else
                handler->SendSysMessage("  (no data)");
        }

        // --- Priority breakdown ---
        handler->SendSysMessage("=== Triage Stats by Priority ===");
        {
            QueryResult r = CharacterDatabase.Query(
                "SELECT priority, COUNT(*) FROM gm_ticket_triage GROUP BY priority ORDER BY priority");
            if (r)
                do {
                    Field* f = r->Fetch();
                    uint8  p = f[0].Get<uint8>();
                    handler->PSendSysMessage("  P%u %-9s : %u", p, PriorityName(p), f[1].Get<uint32>());
                } while (r->NextRow());
            else
                handler->SendSysMessage("  (no data)");
        }

        // --- Resolution time summary (resolved tickets only) ---
        handler->SendSysMessage("=== Resolution Time Summary ===");
        {
            QueryResult r = CharacterDatabase.Query(
                "SELECT COUNT(*), "
                "AVG(TIMESTAMPDIFF(SECOND, created_at, resolved_at)), "
                "MIN(TIMESTAMPDIFF(SECOND, created_at, resolved_at)), "
                "MAX(TIMESTAMPDIFF(SECOND, created_at, resolved_at)) "
                "FROM gm_ticket_triage WHERE resolved_at IS NOT NULL");

            if (r)
            {
                Field* f     = r->Fetch();
                uint32 total = f[0].Get<uint32>();
                if (total > 0)
                {
                    auto fmtTime = [](uint32 s) -> std::string {
                        char buf[16];
                        std::snprintf(buf, sizeof(buf), "%u:%02u:%02u", s/3600, (s%3600)/60, s%60);
                        return buf;
                    };
                    uint32 avg = f[1].IsNull() ? 0 : static_cast<uint32>(f[1].Get<double>());
                    uint32 mn  = f[2].IsNull() ? 0 : f[2].Get<uint32>();
                    uint32 mx  = f[3].IsNull() ? 0 : f[3].Get<uint32>();
                    handler->PSendSysMessage("  Resolved  : %u tickets", total);
                    handler->PSendSysMessage("  Average   : %s", fmtTime(avg).c_str());
                    handler->PSendSysMessage("  Fastest   : %s", fmtTime(mn).c_str());
                    handler->PSendSysMessage("  Slowest   : %s", fmtTime(mx).c_str());
                }
                else
                    handler->SendSysMessage("  No resolved tickets yet.");
            }
        }

        // --- Open ticket count ---
        {
            QueryResult r = CharacterDatabase.Query(
                "SELECT COUNT(*) FROM gm_ticket_triage WHERE resolved_at IS NULL");
            if (r)
                handler->PSendSysMessage("Open (unresolved) : %u", r->Fetch()[0].Get<uint32>());
        }

        return true;
    }
};

void AddSC_AutoTriageCommands()
{
    new AutoTriageCommandScript();
}
