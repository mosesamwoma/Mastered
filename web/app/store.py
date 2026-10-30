import json
import sqlite3
import threading
import time

JSON_FIELDS = ("cli_args", "sources", "report")
UPDATABLE = {
    "status", "progress", "message", "error", "started", "finished", "report",
    "sources", "worker", "heartbeat", "cancel", "preview",
}
PENDING_STATUSES = ("uploading", "queued", "running")

SCHEMA = """
CREATE TABLE IF NOT EXISTS jobs (
    id TEXT PRIMARY KEY,
    status TEXT NOT NULL,
    created REAL NOT NULL,
    updated REAL NOT NULL,
    started REAL,
    finished REAL,
    progress REAL NOT NULL DEFAULT 0,
    message TEXT NOT NULL DEFAULT '',
    error TEXT NOT NULL DEFAULT '',
    track_name TEXT NOT NULL,
    cli_args TEXT NOT NULL,
    sources TEXT NOT NULL DEFAULT '{}',
    client TEXT NOT NULL,
    report TEXT,
    worker TEXT,
    heartbeat REAL,
    cancel INTEGER NOT NULL DEFAULT 0,
    preview INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS jobs_status ON jobs (status, created);
CREATE INDEX IF NOT EXISTS jobs_client ON jobs (client, created);
"""


def decode(row):
    if row is None:
        return None
    record = dict(row)
    for name in JSON_FIELDS:
        record[name] = json.loads(record[name]) if record[name] is not None else None
    record["cancel"] = bool(record["cancel"])
    record["preview"] = bool(record["preview"])
    return record


class JobStore:
    def __init__(self, path):
        self.lock = threading.Lock()
        self.connection = sqlite3.connect(str(path), timeout=15, check_same_thread=False, isolation_level=None)
        self.connection.row_factory = sqlite3.Row
        self.connection.execute("PRAGMA journal_mode=WAL")
        self.connection.execute("PRAGMA synchronous=NORMAL")
        self.connection.execute("PRAGMA busy_timeout=15000")
        self.connection.executescript(SCHEMA)

    def close(self):
        with self.lock:
            self.connection.close()

    def transaction(self, work):
        with self.lock:
            cursor = self.connection.cursor()
            cursor.execute("BEGIN IMMEDIATE")
            try:
                result = work(cursor)
            except BaseException:
                cursor.execute("ROLLBACK")
                raise
            cursor.execute("COMMIT")
            return result

    def reserve(self, job_id, track_name, cli_args, client, now, max_pending, per_client_pending, per_client_hour):
        def work(cursor):
            marks = ",".join("?" for _ in PENDING_STATUSES)
            pending = cursor.execute(
                f"SELECT COUNT(*) FROM jobs WHERE status IN ({marks}) AND cancel = 0", PENDING_STATUSES
            ).fetchone()[0]
            if pending >= max_pending:
                return "busy", 0.0
            client_pending = cursor.execute(
                f"SELECT COUNT(*) FROM jobs WHERE client = ? AND status IN ({marks}) AND cancel = 0",
                (client, *PENDING_STATUSES),
            ).fetchone()[0]
            if client_pending >= per_client_pending:
                return "client_busy", 0.0
            recent = [
                row[0]
                for row in cursor.execute(
                    "SELECT created FROM jobs WHERE client = ? AND created > ? ORDER BY created ASC",
                    (client, now - 3600.0),
                )
            ]
            if len(recent) >= per_client_hour:
                return "rate", max(1.0, recent[0] + 3600.0 - now)
            cursor.execute(
                "INSERT INTO jobs (id, status, created, updated, message, track_name, cli_args, client) "
                "VALUES (?, 'uploading', ?, ?, 'Waiting for upload', ?, ?, ?)",
                (job_id, now, now, track_name, json.dumps(cli_args), client),
            )
            return "ok", 0.0

        return self.transaction(work)

    def get(self, job_id):
        with self.lock:
            row = self.connection.execute("SELECT * FROM jobs WHERE id = ?", (job_id,)).fetchone()
        return decode(row)

    def update(self, job_id, **fields):
        unknown = set(fields) - UPDATABLE
        if unknown:
            raise ValueError(f"cannot update {sorted(unknown)}")
        values = {}
        for name, value in fields.items():
            if name in JSON_FIELDS:
                value = json.dumps(value)
            elif name in ("cancel", "preview"):
                value = 1 if value else 0
            values[name] = value
        values["updated"] = time.time()
        assignments = ", ".join(f"{name} = ?" for name in values)
        with self.lock:
            cursor = self.connection.execute(
                f"UPDATE jobs SET {assignments} WHERE id = ?", (*values.values(), job_id)
            )
        return cursor.rowcount > 0

    def mark_failed(self, job_id, message, only_status=None):
        now = time.time()
        sql = "UPDATE jobs SET status = 'failed', error = ?, message = 'Failed', finished = ?, updated = ? WHERE id = ?"
        params = [message, now, now, job_id]
        if only_status:
            sql += " AND status = ?"
            params.append(only_status)
        with self.lock:
            cursor = self.connection.execute(sql, params)
        return cursor.rowcount > 0

    def delete(self, job_id):
        with self.lock:
            cursor = self.connection.execute("DELETE FROM jobs WHERE id = ?", (job_id,))
        return cursor.rowcount > 0

    def claim(self, worker, max_running, now):
        def work(cursor):
            running = cursor.execute("SELECT COUNT(*) FROM jobs WHERE status = 'running' AND cancel = 0").fetchone()[0]
            if running >= max_running:
                return None
            row = cursor.execute(
                "SELECT id FROM jobs WHERE status = 'queued' AND cancel = 0 ORDER BY created ASC, rowid ASC LIMIT 1"
            ).fetchone()
            if row is None:
                return None
            cursor.execute(
                "UPDATE jobs SET status = 'running', worker = ?, started = ?, heartbeat = ?, updated = ?, "
                "message = 'Starting', progress = 0 WHERE id = ?",
                (worker, now, now, now, row[0]),
            )
            return row[0]

        job_id = self.transaction(work)
        return self.get(job_id) if job_id else None

    def queue_position(self, job_id):
        with self.lock:
            row = self.connection.execute(
                "SELECT COUNT(*) FROM jobs WHERE status = 'queued' AND cancel = 0 AND "
                "(created, rowid) < (SELECT created, rowid FROM jobs WHERE id = ?)",
                (job_id,),
            ).fetchone()
        return row[0] + 1

    def expired_ids(self, finished_before, uploading_before):
        with self.lock:
            rows = self.connection.execute(
                "SELECT id FROM jobs WHERE (status IN ('done', 'failed') AND finished < ?) "
                "OR (status = 'uploading' AND created < ?)",
                (finished_before, uploading_before),
            ).fetchall()
        return [row[0] for row in rows]

    def stale_running(self, heartbeat_before):
        with self.lock:
            rows = self.connection.execute(
                "SELECT * FROM jobs WHERE status = 'running' AND heartbeat < ?", (heartbeat_before,)
            ).fetchall()
        return [decode(row) for row in rows]

    def all_ids(self):
        with self.lock:
            rows = self.connection.execute("SELECT id FROM jobs").fetchall()
        return {row[0] for row in rows}

    def running_for_worker(self, worker):
        with self.lock:
            rows = self.connection.execute(
                "SELECT id FROM jobs WHERE status = 'running' AND worker = ?", (worker,)
            ).fetchall()
        return [row[0] for row in rows]
