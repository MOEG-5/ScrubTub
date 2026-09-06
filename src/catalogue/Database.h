// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 the itub authors.
// Minimal RAII wrapper over SQLite's C API (TECH_SPEC.md section 4).
// One connection per worker thread; WAL; foreign keys; bounded busy timeout;
// transactional writes with synchronous=FULL for annotation durability.
#pragma once

#include <QByteArray>
#include <QString>

#include <sqlite3.h>

#include <functional>
#include <optional>

namespace itub {

class Statement;

class Database {
public:
    Database() = default;
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Opens (creating if needed) at path with WAL, foreign keys, busy timeout,
    // and synchronous=FULL. Returns false with reason on failure.
    bool open(const QString& path, QString* error);
    void close();

    bool isValid() const { return m_db != nullptr; }
    QString lastError() const;
    qint64 lastInsertRowId() const;
    int lastChangeCount() const;

    // Executes SQL without results; false on error.
    bool exec(const char* sql);

    // Prepares a statement; check isValid() on the result.
    Statement prepare(const char* sql);

    // Convenience for scalar queries.
    std::optional<qint64> scalarInt(const char* sql);
    std::optional<QString> scalarText(const char* sql);

    // Runs fn inside BEGIN IMMEDIATE/COMMIT; rolls back on false or throw.
    bool transaction(const std::function<bool()>& fn);

    // Consistent online backup of this database into destPath using SQLite's
    // backup API (includes WAL content — never copy only the live file).
    bool backupTo(const QString& destPath, QString* error);

    // Copies the database at srcPath into this (open) database, replacing
    // its contents; used after the source has been validated.
    bool restoreFrom(const QString& srcPath, QString* error);

    // PRAGMA integrity_check; empty result string means ok.
    QString integrityCheckError();

    sqlite3* handle() const { return m_db; }

    // Schema management: applies migrations transactionally, refuses newer
    // schemas. Returns false with reason (including "newer schema").
    bool migrate(QString* error);

    static constexpr int kSchemaVersion = 1;

private:
    sqlite3* m_db = nullptr;
};

class Statement {
public:
    Statement() = default;
    Statement(sqlite3* db, sqlite3_stmt* stmt) : m_db(db), m_stmt(stmt) {}
    ~Statement();
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    bool isValid() const { return m_stmt != nullptr; }

    // Binds (1-based indexes). TEXT binds are transient copies (safe for
    // temporaries); blob the same.
    void bind(int index, qint64 value);
    void bind(int index, int value);
    void bind(int index, double value);
    void bind(int index, const QString& value);
    void bind(int index, const QByteArray& value);
    void bindNull(int index);

    // SQLITE_ROW → true and advances; false when done or on error.
    bool step();

    // For INSERT/UPDATE/DELETE: true when the statement finished (SQLITE_DONE).
    bool run();
    QString errorString() const;

    // Column readers (0-based).
    qint64 int64(int col) const;
    QString text(int col) const;
    bool isNull(int col) const;

    void reset();

private:
    sqlite3* m_db = nullptr;
    sqlite3_stmt* m_stmt = nullptr;
};

} // namespace itub
