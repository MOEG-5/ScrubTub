// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the scrubtub authors.
#include "Database.h"

#include <QFile>

#include <utility>

namespace scrubtub {

namespace {

// Historical v1 schema (without stream_index/settings), kept for the
// migration tests. Constraints reject negative sizes/durations/views and
// zero or negative known dimensions (TECH_SPEC.md section 4).
const char* kSchemaV1 = R"SQL(
CREATE TABLE IF NOT EXISTS meta(
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE roots(
    id INTEGER PRIMARY KEY,
    path TEXT NOT NULL,
    cmp_key TEXT NOT NULL UNIQUE,
    status TEXT NOT NULL DEFAULT 'ok' CHECK(status IN ('ok','missing','error')),
    status_detail TEXT,
    scan_generation INTEGER NOT NULL DEFAULT 0,
    last_complete_scan_ms INTEGER,
    include_hidden INTEGER NOT NULL DEFAULT 0 CHECK(include_hidden IN (0,1))
);

CREATE TABLE videos(
    id INTEGER PRIMARY KEY,
    root_id INTEGER NOT NULL REFERENCES roots(id) ON DELETE CASCADE,
    rel_path TEXT NOT NULL,
    cmp_key TEXT NOT NULL,
    file_name TEXT NOT NULL,
    size_bytes INTEGER CHECK(size_bytes IS NULL OR size_bytes >= 0),
    mtime_ms INTEGER,
    revision INTEGER NOT NULL DEFAULT 1 CHECK(revision >= 1),
    duration_ms INTEGER CHECK(duration_ms IS NULL OR duration_ms > 0),
    coded_width INTEGER CHECK(coded_width IS NULL OR coded_width > 0),
    coded_height INTEGER CHECK(coded_height IS NULL OR coded_height > 0),
    display_width INTEGER CHECK(display_width IS NULL OR display_width > 0),
    display_height INTEGER CHECK(display_height IS NULL OR display_height > 0),
    rotation_deg INTEGER CHECK(rotation_deg IS NULL OR rotation_deg IN (0,90,180,270)),
    codec TEXT,
    rating INTEGER CHECK(rating IS NULL OR (rating BETWEEN 1 AND 5)),
    views INTEGER NOT NULL DEFAULT 0 CHECK(views >= 0),
    added_ms INTEGER NOT NULL,
    last_opened_ms INTEGER,
    availability TEXT NOT NULL DEFAULT 'unprobed'
        CHECK(availability IN ('unprobed','available','missing','unavailable')),
    probe_status TEXT NOT NULL DEFAULT 'pending'
        CHECK(probe_status IN ('pending','ok','error','timeout')),
    probe_error TEXT,
    seen_generation INTEGER,
    UNIQUE(root_id, cmp_key)
);
CREATE INDEX IF NOT EXISTS idx_videos_root_seen ON videos(root_id, seen_generation);
CREATE INDEX IF NOT EXISTS idx_videos_availability ON videos(availability);
CREATE INDEX IF NOT EXISTS idx_videos_sort ON videos(size_bytes, duration_ms, rating, views);
CREATE INDEX IF NOT EXISTS idx_videos_name ON videos(root_id, file_name);

CREATE TABLE tags(
    id INTEGER PRIMARY KEY,
    norm TEXT NOT NULL UNIQUE,
    label TEXT NOT NULL
);

CREATE TABLE video_tags(
    video_id INTEGER NOT NULL REFERENCES videos(id) ON DELETE CASCADE,
    tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
    origin TEXT NOT NULL CHECK(origin IN ('manual','filename','folder','technical')),
    UNIQUE(video_id, tag_id, origin)
);
CREATE INDEX IF NOT EXISTS idx_video_tags_tag ON video_tags(tag_id);
CREATE INDEX IF NOT EXISTS idx_video_tags_video ON video_tags(video_id);

CREATE TABLE tag_suppressions(
    video_id INTEGER NOT NULL REFERENCES videos(id) ON DELETE CASCADE,
    tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
    UNIQUE(video_id, tag_id)
);

CREATE TABLE jobs(
    id INTEGER PRIMARY KEY,
    video_id INTEGER NOT NULL REFERENCES videos(id) ON DELETE CASCADE,
    revision INTEGER NOT NULL,
    kind TEXT NOT NULL CHECK(kind IN ('probe','poster','storyboard')),
    state TEXT NOT NULL DEFAULT 'queued' CHECK(state IN ('queued','running','error')),
    retries INTEGER NOT NULL DEFAULT 0 CHECK(retries >= 0),
    error TEXT,
    UNIQUE(video_id, revision, kind)
);
CREATE INDEX IF NOT EXISTS idx_jobs_state ON jobs(state, kind);

CREATE TABLE cache_entries(
    video_id INTEGER NOT NULL REFERENCES videos(id) ON DELETE CASCADE,
    revision INTEGER NOT NULL,
    profile TEXT NOT NULL,
    kind TEXT NOT NULL,
    rel_path TEXT NOT NULL,
    bytes INTEGER NOT NULL CHECK(bytes >= 0),
    sample_times TEXT,
    last_access_ms INTEGER NOT NULL,
    UNIQUE(video_id, revision, profile)
);

CREATE INDEX IF NOT EXISTS idx_cache_access ON cache_entries(last_access_ms);
)SQL";

// Schema v2 = v1 plus the selected-stream index and the settings table.
// Both were added after v1 shipped; the migration below applies them
// idempotently to databases that were created before they existed.
const char* kSchemaV2 = R"SQL(
ALTER TABLE videos ADD COLUMN stream_index INTEGER;
CREATE TABLE IF NOT EXISTS settings(
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL,
    version INTEGER NOT NULL DEFAULT 1
);
)SQL";

} // namespace

Database::~Database()
{
    close();
}

bool Database::open(const QString& path, QString* error)
{
    close();
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_EXRESCODE;
    if (sqlite3_open_v2(QFile::encodeName(path).constData(), &m_db, flags, nullptr) != SQLITE_OK) {
        if (error)
            *error = m_db ? QString::fromUtf8(sqlite3_errmsg(m_db))
                          : QStringLiteral("could not allocate sqlite handle");
        close();
        return false;
    }
    // Tunables per spec: WAL for concurrent readers, FULL for annotation
    // durability (benchmarked batched scan writes later), bounded busy wait.
    bool ok = exec("PRAGMA journal_mode=WAL") && exec("PRAGMA synchronous=FULL")
        && exec("PRAGMA foreign_keys=ON") && exec("PRAGMA busy_timeout=5000")
        && exec("PRAGMA cache_size=-8192");
    if (!ok) {
        if (error)
            *error = lastError();
        close();
        return false;
    }
    if (error)
        *error = QString();
    return true;
}

void Database::close()
{
    if (m_db) {
        sqlite3_close_v2(m_db);
        m_db = nullptr;
    }
}

QString Database::lastError() const
{
    return m_db ? QString::fromUtf8(sqlite3_errmsg(m_db)) : QStringLiteral("no database");
}

qint64 Database::lastInsertRowId() const
{
    return m_db ? sqlite3_last_insert_rowid(m_db) : 0;
}

int Database::lastChangeCount() const
{
    return m_db ? sqlite3_changes(m_db) : 0;
}

bool Database::exec(const char* sql)
{
    if (!m_db)
        return false;
    char* msg = nullptr;
    if (sqlite3_exec(m_db, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
        qWarning("SQL failed: %s (%s)", sql, msg ? msg : "?");
        sqlite3_free(msg);
        return false;
    }
    sqlite3_free(msg);
    return true;
}

Statement Database::prepare(const char* sql)
{
    if (!m_db)
        return {};
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        qWarning("prepare failed: %s (%s)", sql, lastError().toUtf8().constData());
        return {};
    }
    return Statement(m_db, stmt);
}

std::optional<qint64> Database::scalarInt(const char* sql)
{
    Statement st = prepare(sql);
    if (!st.isValid() || !st.step())
        return {};
    return st.int64(0);
}

std::optional<QString> Database::scalarText(const char* sql)
{
    Statement st = prepare(sql);
    if (!st.isValid() || !st.step())
        return {};
    return st.text(0);
}

bool Database::transaction(const std::function<bool()>& fn)
{
    if (!exec("BEGIN IMMEDIATE"))
        return false;
    if (!fn()) {
        exec("ROLLBACK");
        return false;
    }
    return exec("COMMIT");
}

bool Database::backupTo(const QString& destPath, QString* error)
{
    if (!m_db) {
        if (error)
            *error = QStringLiteral("database not open");
        return false;
    }
    sqlite3* dest = nullptr;
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_EXRESCODE;
    if (sqlite3_open_v2(QFile::encodeName(destPath).constData(), &dest, flags,
                        nullptr) != SQLITE_OK) {
        if (error)
            *error = dest ? QString::fromUtf8(sqlite3_errmsg(dest))
                          : QStringLiteral("could not create backup file");
        if (dest)
            sqlite3_close_v2(dest);
        return false;
    }
    sqlite3_backup* backup = sqlite3_backup_init(dest, "main", m_db, "main");
    if (!backup) {
        if (error)
            *error = QString::fromUtf8(sqlite3_errmsg(dest));
        sqlite3_close_v2(dest);
        return false;
    }
    // -1: copy all pages in one step; the catalogue DB is small.
    sqlite3_backup_step(backup, -1);
    const int rc = sqlite3_backup_finish(backup);
    sqlite3_close_v2(dest);
    if (rc != SQLITE_OK) {
        if (error)
            *error = QString::fromUtf8(sqlite3_errmsg(m_db));
        QFile::remove(destPath);
        return false;
    }
    if (error)
        *error = QString();
    return true;
}

bool Database::restoreFrom(const QString& srcPath, QString* error)
{
    if (!m_db) {
        if (error)
            *error = QStringLiteral("database not open");
        return false;
    }
    sqlite3* src = nullptr;
    const int flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_EXRESCODE;
    if (sqlite3_open_v2(QFile::encodeName(srcPath).constData(), &src, flags,
                        nullptr) != SQLITE_OK) {
        if (error)
            *error = src ? QString::fromUtf8(sqlite3_errmsg(src))
                         : QStringLiteral("could not open backup for restore");
        if (src)
            sqlite3_close_v2(src);
        return false;
    }
    sqlite3_backup* backup = sqlite3_backup_init(m_db, "main", src, "main");
    if (!backup) {
        if (error)
            *error = QString::fromUtf8(sqlite3_errmsg(m_db));
        sqlite3_close_v2(src);
        return false;
    }
    sqlite3_backup_step(backup, -1);
    const int rc = sqlite3_backup_finish(backup);
    sqlite3_close_v2(src);
    if (rc != SQLITE_OK) {
        if (error)
            *error = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    if (error)
        *error = QString();
    return true;
}

QString Database::integrityCheckError()
{
    if (!m_db)
        return QStringLiteral("database not open");
    Statement st = prepare("PRAGMA integrity_check");
    if (!st.isValid() || !st.step())
        return lastError();
    const QString result = st.text(0);
    return result == QLatin1String("ok") ? QString() : result;
}

bool Database::migrate(QString* error)
{
    if (!m_db) {
        if (error)
            *error = QStringLiteral("database not open");
        return false;
    }

    // Indexes are idempotent and not part of the schema contract, so they are
    // ensured on every open (an existing library picks them up without a
    // version bump). cache_entries(kind, ...) turns the poster/progress joins
    // and preview-eligibility checks into covering index scans.
    static const char* const kIndexes[] = {
        "CREATE INDEX IF NOT EXISTS idx_cache_kind ON cache_entries(kind, video_id, revision)",
    };
    const auto ensureIndexes = [this, &kIndexes] {
        for (const char* sql : kIndexes)
            if (!exec(sql))
                return false;
        return true;
    };

    const auto metaTable =
        scalarInt("SELECT 1 FROM sqlite_master WHERE type='table' AND name='meta'");
    std::optional<qint64> version;
    if (metaTable.has_value())
        version = scalarInt("SELECT value FROM meta WHERE key='schema_version'");

    // Refuse newer schemas before any DDL (TECH_SPEC.md section 4).
    if (version.has_value() && version.value() > kSchemaVersion) {
        if (error)
            *error = QStringLiteral("database schema v%1 is newer than supported v%2")
                         .arg(version.value())
                         .arg(kSchemaVersion);
        return false;
    }

    // Fresh database: create the current schema and stamp the version.
    if (!version.has_value()) {
        if (!exec(kSchemaV1) || !exec(kSchemaV2)) {
            if (error)
                *error = lastError();
            return false;
        }
        if (!ensureIndexes()) {
            if (error)
                *error = lastError();
            return false;
        }
        const bool stamped = transaction([this]() {
            Statement st = prepare(
                "INSERT OR REPLACE INTO meta(key,value) VALUES('schema_version',?)");
            if (!st.isValid())
                return false;
            st.bind(1, static_cast<qint64>(kSchemaVersion));
            return st.run();
        });
        if (error)
            *error = stamped ? QString() : lastError();
        return stamped;
    }

    if (version.value() == 1) {
        bool hasStreamIndex = false;
        {
            Statement cols = prepare(
                "SELECT name FROM pragma_table_info('videos') WHERE name='stream_index'");
            hasStreamIndex = cols.step();
        }
        const bool ok = transaction([this, hasStreamIndex]() -> bool {
            if (!hasStreamIndex && !exec(
                "ALTER TABLE videos ADD COLUMN stream_index INTEGER"))
                return false;
            if (!exec("CREATE TABLE IF NOT EXISTS settings("
                      "key TEXT PRIMARY KEY, value TEXT NOT NULL, "
                      "version INTEGER NOT NULL DEFAULT 1)"))
                return false;
            Statement stamp = prepare(
                "UPDATE meta SET value='2' WHERE key='schema_version'");
            return stamp.isValid() && stamp.run();
        });
        if (error)
            *error = ok ? QString() : lastError();
        if (!ok)
            return false;
        if (!ensureIndexes()) {
            if (error)
                *error = lastError();
            return false;
        }
        return true;
    }

    if (!ensureIndexes()) {
        if (error)
            *error = lastError();
        return false;
    }

    if (error)
        *error = QString();
    return true;
}

Statement::~Statement()
{
    if (m_stmt)
        sqlite3_finalize(m_stmt);
}

Statement::Statement(Statement&& other) noexcept
    : m_db(other.m_db), m_stmt(other.m_stmt)
{
    other.m_db = nullptr;
    other.m_stmt = nullptr;
}

Statement& Statement::operator=(Statement&& other) noexcept
{
    if (this != &other) {
        if (m_stmt)
            sqlite3_finalize(m_stmt);
        m_db = std::exchange(other.m_db, nullptr);
        m_stmt = std::exchange(other.m_stmt, nullptr);
    }
    return *this;
}

void Statement::bind(int index, qint64 value)
{
    sqlite3_bind_int64(m_stmt, index, value);
}

void Statement::bind(int index, int value)
{
    sqlite3_bind_int(m_stmt, index, value);
}

void Statement::bind(int index, double value)
{
    sqlite3_bind_double(m_stmt, index, value);
}

void Statement::bind(int index, const QString& value)
{
    const QByteArray utf8 = value.toUtf8();
    sqlite3_bind_text(m_stmt, index, utf8.constData(), static_cast<int>(utf8.size()),
                      SQLITE_TRANSIENT);
}

void Statement::bind(int index, const QByteArray& value)
{
    sqlite3_bind_blob(m_stmt, index, value.constData(), static_cast<int>(value.size()),
                      SQLITE_TRANSIENT);
}

void Statement::bindNull(int index)
{
    sqlite3_bind_null(m_stmt, index);
}

bool Statement::step()
{
    return m_stmt && sqlite3_step(m_stmt) == SQLITE_ROW;
}

bool Statement::run()
{
    return m_stmt && sqlite3_step(m_stmt) == SQLITE_DONE;
}

QString Statement::errorString() const
{
    return m_db ? QString::fromUtf8(sqlite3_errmsg(m_db)) : QString();
}

qint64 Statement::int64(int col) const
{
    return sqlite3_column_int64(m_stmt, col);
}

QString Statement::text(int col) const
{
    const auto* data = sqlite3_column_text(m_stmt, col);
    if (!data)
        return {};
    return QString::fromUtf8(reinterpret_cast<const char*>(data),
                             sqlite3_column_bytes(m_stmt, col));
}

bool Statement::isNull(int col) const
{
    return sqlite3_column_type(m_stmt, col) == SQLITE_NULL;
}

void Statement::reset()
{
    if (m_stmt)
        sqlite3_reset(m_stmt);
}

} // namespace scrubtub
