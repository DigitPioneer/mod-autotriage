#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ScriptMgr.h"
#include "World.h"
#include "WorldSession.h"

// ---------------------------------------------------------------------------
// AutoTriagePeriodicScript
//
// Runs on every world update. When the configured interval elapses it queries
// for open, unassigned, high-priority tickets that have been waiting longer
// than the per-priority escalation threshold and broadcasts a reminder to
// every online GM.
//
// Config keys (all optional — defaults listed):
//   AutoTriage.GmPingInterval         — seconds between checks         (300)
//   AutoTriage.EscalateAfter.Critical — minutes before pinging for P1  (10)
//   AutoTriage.EscalateAfter.High     — minutes before pinging for P2  (30)
// ---------------------------------------------------------------------------

class AutoTriagePeriodicScript : public WorldScript
{
    uint32 _timerMs;   // countdown in milliseconds

public:
    AutoTriagePeriodicScript() : WorldScript("AutoTriagePeriodicScript"), _timerMs(0) {}

    void OnUpdate(uint32 diff) override
    {
        if (!sConfigMgr->GetOption<bool>("AutoTriage.Enable", true))
            return;

        if (_timerMs > diff)
        {
            _timerMs -= diff;
            return;
        }

        uint32 intervalSec = sConfigMgr->GetOption<uint32>("AutoTriage.GmPingInterval", 300);
        _timerMs = intervalSec * 1000;

        CheckAndNotify();
    }

private:
    void CheckAndNotify() const
    {
        uint32 critThreshold = sConfigMgr->GetOption<uint32>("AutoTriage.EscalateAfter.Critical", 10);
        uint32 highThreshold = sConfigMgr->GetOption<uint32>("AutoTriage.EscalateAfter.High",     30);

        // Tickets that are:
        //   - not yet resolved in our triage table
        //   - still open in the game's ticket table (closedBy = 0)
        //   - unassigned (assignedTo = 0)
        //   - Priority 1 or 2 (Critical / High)
        //   - Waiting longer than the per-priority threshold
        QueryResult result = CharacterDatabase.Query(
            "SELECT t.ticket_id, t.player_guid, t.category, t.priority, "
            "TIMESTAMPDIFF(MINUTE, t.created_at, NOW()) AS age_min "
            "FROM gm_ticket_triage t "
            "INNER JOIN gm_ticket g ON g.id = t.ticket_id "
            "WHERE t.resolved_at IS NULL "
            "  AND g.closedBy = 0 "
            "  AND (g.assignedTo = 0 OR g.assignedTo IS NULL) "
            "  AND ( "
            "        (t.priority = 1 AND TIMESTAMPDIFF(MINUTE, t.created_at, NOW()) >= {}) "
            "     OR (t.priority = 2 AND TIMESTAMPDIFF(MINUTE, t.created_at, NOW()) >= {}) "
            "  ) "
            "ORDER BY t.priority ASC, t.created_at ASC",
            critThreshold, highThreshold
        );

        if (!result)
            return;

        uint32 count = 0;
        do
        {
            Field* f     = result->Fetch();
            uint32 id    = f[0].Get<uint32>();
            uint32 pguid = f[1].Get<uint32>();
            std::string cat = f[2].Get<std::string>();
            uint8  prio  = f[3].Get<uint8>();
            int32  age   = f[4].Get<int32>();

            std::string msg = "[AutoTriage] Unassigned ticket #" + std::to_string(id) +
                " | " + cat +
                " | Priority: " + PriorityName(prio) +
                " | Open for " + std::to_string(age) + " min"
                " — please assign or respond.";

            BroadcastToGMs(msg);

            LOG_INFO("module",
                "mod-autotriage: Escalation ping — Ticket #{} | Category: {} | Priority: {} | Age: {} min | PlayerGUID: {}",
                id, cat, prio, age, pguid);

            ++count;
        }
        while (result->NextRow());

        if (count > 0)
            LOG_INFO("module", "mod-autotriage: Sent escalation ping to GMs for {} unassigned ticket(s).", count);
    }

    static const char* PriorityName(uint8 p)
    {
        switch (p)
        {
            case 1: return "Critical";
            case 2: return "High";
            default: return "?";
        }
    }

    // Sends a plain system message to every online GM-or-above session.
    //
    // AzerothCore exposes the session map via sWorld->GetAllSessions().
    // If your fork uses a different name (GetSessionMap, GetSessions, etc.)
    // adjust the call below — the pattern itself is standard.
    static void BroadcastToGMs(const std::string& msg)
    {
        SessionMap const& sessions = sWorld->GetAllSessions();
        for (auto const& [accountId, session] : sessions)
        {
            if (!session)
                continue;
            Player* player = session->GetPlayer();
            if (!player || !player->IsInWorld())
                continue;
            if (session->GetSecurity() < SEC_GAMEMASTER)
                continue;

            ChatHandler(session).SendSysMessage(msg);
        }
    }
};

void AddSC_AutoTriagePeriodic()
{
    new AutoTriagePeriodicScript();
}
