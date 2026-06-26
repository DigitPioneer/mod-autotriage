-- mod-autotriage: AT_002 — schema extensions
-- Target database : characters
-- Run after AT_001 has been applied.
--
-- Adds:
--   resolved_at      — stamped by OnTicketClose; used for resolution-time stats
--   priority_override — 1 when a GM used .triage setpriority (vs auto-classification)

ALTER TABLE `gm_ticket_triage`
    ADD COLUMN `resolved_at`       DATETIME     NULL     DEFAULT NULL
        COMMENT 'Stamped by OnTicketClose hook; NULL = ticket still open'
        AFTER `auto_reply_sent`,

    ADD COLUMN `priority_override` TINYINT(1)   NOT NULL DEFAULT 0
        COMMENT '1 when priority was manually overridden via .triage setpriority'
        AFTER `priority`,

    ADD KEY `idx_resolved_at` (`resolved_at`);
