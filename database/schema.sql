PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;

BEGIN TRANSACTION;

CREATE TABLE IF NOT EXISTS vehicles (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    plate_number TEXT NOT NULL COLLATE NOCASE UNIQUE
        CHECK (
            length(plate_number) BETWEEN 1 AND 20
            AND plate_number = upper(plate_number)
            AND plate_number NOT GLOB '*[^A-Z0-9]*'
        ),
    owner_name TEXT NOT NULL,
    vehicle_type TEXT,
    make TEXT,
    model TEXT,
    color TEXT,
    contact_number TEXT,
    email TEXT,
    registration_expires_on TEXT,
    photo_path TEXT,
    notes TEXT,
    is_active INTEGER NOT NULL DEFAULT 1 CHECK (is_active IN (0, 1)),
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS access_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    vehicle_id INTEGER,
    plate_number TEXT NOT NULL COLLATE NOCASE,
    direction TEXT NOT NULL DEFAULT 'entry'
        CHECK (direction IN ('entry', 'exit')),
    decision TEXT NOT NULL
        CHECK (decision IN ('authorized', 'denied', 'unreadable', 'manual')),
    gate_action TEXT NOT NULL DEFAULT 'kept_closed'
        CHECK (gate_action IN ('opened', 'kept_closed', 'not_requested', 'error')),
    detector_confidence REAL
        CHECK (detector_confidence IS NULL OR detector_confidence BETWEEN 0.0 AND 1.0),
    ocr_confidence REAL
        CHECK (ocr_confidence IS NULL OR ocr_confidence BETWEEN 0.0 AND 1.0),
    image_path TEXT,
    notes TEXT,
    detected_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (vehicle_id) REFERENCES vehicles(id) ON DELETE SET NULL
);

CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT NOT NULL COLLATE NOCASE UNIQUE,
    password_hash TEXT NOT NULL,
    role TEXT NOT NULL DEFAULT 'operator'
        CHECK (role IN ('administrator', 'operator', 'viewer')),
    is_active INTEGER NOT NULL DEFAULT 1 CHECK (is_active IN (0, 1)),
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_login_at TEXT
);

CREATE TABLE IF NOT EXISTS audit_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER,
    action TEXT NOT NULL,
    entity_type TEXT,
    entity_id INTEGER,
    details TEXT,
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE SET NULL
);

CREATE TABLE IF NOT EXISTS system_status (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    camera_state TEXT NOT NULL DEFAULT 'unknown',
    detector_state TEXT NOT NULL DEFAULT 'unknown',
    gate_state TEXT NOT NULL DEFAULT 'closed',
    last_plate TEXT,
    last_heartbeat TEXT,
    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL,
    description TEXT,
    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_vehicles_active_plate
    ON vehicles(is_active, plate_number);
CREATE INDEX IF NOT EXISTS idx_access_events_detected_at
    ON access_events(detected_at DESC);
CREATE INDEX IF NOT EXISTS idx_access_events_plate
    ON access_events(plate_number, detected_at DESC);
CREATE INDEX IF NOT EXISTS idx_access_events_decision
    ON access_events(decision, detected_at DESC);

CREATE TRIGGER IF NOT EXISTS vehicles_set_updated_at
AFTER UPDATE ON vehicles
FOR EACH ROW
WHEN NEW.updated_at = OLD.updated_at
BEGIN
    UPDATE vehicles SET updated_at = CURRENT_TIMESTAMP WHERE id = NEW.id;
END;

CREATE VIEW IF NOT EXISTS daily_access_summary AS
SELECT
    date(detected_at, 'localtime') AS event_date,
    count(*) AS total_events,
    sum(CASE WHEN decision = 'authorized' THEN 1 ELSE 0 END) AS authorized_count,
    sum(CASE WHEN decision = 'denied' THEN 1 ELSE 0 END) AS denied_count,
    sum(CASE WHEN gate_action = 'opened' THEN 1 ELSE 0 END) AS gates_opened
FROM access_events
GROUP BY date(detected_at, 'localtime');

INSERT OR IGNORE INTO system_status (id) VALUES (1);
INSERT OR IGNORE INTO settings (key, value, description) VALUES
    ('gate_open_seconds', '5', 'How long the gate-open signal remains active.'),
    ('duplicate_event_seconds', '30', 'Suppress repeated events for the same plate.'),
    ('event_image_retention_days', '90', 'Days to keep event snapshot files.'),
    ('unknown_plate_action', 'deny', 'Action for an unregistered or inactive plate.');

COMMIT;
PRAGMA wal_checkpoint(TRUNCATE);
