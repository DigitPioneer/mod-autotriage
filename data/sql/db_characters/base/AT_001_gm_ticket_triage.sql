-- mod-autotriage: initial schema migration
-- Target database : characters
-- Run once during module installation.
-- AzerothCore's module SQL runner will apply this automatically
-- when the module directory is present at build/startup time.

CREATE TABLE IF NOT EXISTS `gm_ticket_triage` (
    `id`               INT UNSIGNED     NOT NULL AUTO_INCREMENT,
    `ticket_id`        INT UNSIGNED     NOT NULL            COMMENT 'References gm_ticket.id',
    `player_guid`      INT UNSIGNED     NOT NULL            COMMENT 'Character low GUID',
    `category`         ENUM(
                           'BUG',
                           'ABUSE',
                           'QUESTION',
                           'OTHER'
                       )                NOT NULL DEFAULT 'OTHER',
    `priority`         TINYINT UNSIGNED NOT NULL DEFAULT 3  COMMENT '1=Critical 2=High 3=Medium 4=Low',
    `message_snippet`  VARCHAR(255)     NOT NULL DEFAULT '',
    `auto_reply_sent`  TINYINT(1)       NOT NULL DEFAULT 0  COMMENT '1 if player was online and received the auto-reply',
    `created_at`       DATETIME         NOT NULL DEFAULT CURRENT_TIMESTAMP,

    PRIMARY KEY (`id`),
    KEY `idx_ticket_id`   (`ticket_id`),
    KEY `idx_player_guid` (`player_guid`),
    KEY `idx_category`    (`category`),
    KEY `idx_priority`    (`priority`)
) ENGINE = InnoDB
  DEFAULT CHARSET = utf8mb4
  COLLATE = utf8mb4_unicode_ci
  COMMENT = 'Auto-triage classification log — populated by mod-autotriage';
