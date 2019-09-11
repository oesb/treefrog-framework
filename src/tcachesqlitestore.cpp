/* Copyright (c) 2019, AOYAMA Kazuharu
 * All rights reserved.
 *
 * This software may be used and distributed according to the terms of
 * the New BSD License, which is incorporated herein by reference.
 */

#include "tcachesqlitestore.h"
#include "tsystemglobal.h"
#include "tsqlquery.h"
#include <QByteArray>
#include <QDateTime>
#include <QReadWriteLock>
#include <TAtomicPtr>
#include <atomic>

constexpr auto TABLE_NAME = "kb";
constexpr auto KEY_COLUMN  = "k";
constexpr auto BLOB_COLUMN = "b";
constexpr auto TIMESTAMP_COLUMN = "t";
constexpr int  PAGESIZE = 4096;


inline QString lastError()
{
    return Tf::currentSqlDatabase(Tf::app()->databaseIdForInternalUse()).lastError().text();
}


static bool exec(const QString &sql)
{
    TSqlQuery query(Tf::app()->databaseIdForInternalUse());
    query.prepare(sql);
    bool ret = query.exec();
    if (! ret) {
        tSystemError("SQLite error : %s, query:'%s' [%s:%d]", qPrintable(lastError()), qPrintable(sql), __FILE__, __LINE__);
    }
    return ret;
}


bool TCacheSQLiteStore::createTable(const QString &table)
{
    exec(QStringLiteral("pragma page_size=%1").arg(PAGESIZE));
    return exec(QStringLiteral("create table if not exists %1 (%2 text primary key, %3 integer, %4 blob)").arg(table, KEY_COLUMN, TIMESTAMP_COLUMN, BLOB_COLUMN));
}


TCacheSQLiteStore::TCacheSQLiteStore(qint64 thresholdFileSize, const QByteArray &table) :
    _thresholdFileSize(thresholdFileSize),
    _table(table.isEmpty() ? QString(TABLE_NAME) : QString(table))
{ }


TCacheSQLiteStore::~TCacheSQLiteStore()
{
    close();
}


bool TCacheSQLiteStore::open()
{
    T_ONCE(createTable(TABLE_NAME));
    return true;
}


void TCacheSQLiteStore::close()
{ }


bool TCacheSQLiteStore::isOpen() const
{
    return true;
}


int TCacheSQLiteStore::count()
{
    int cnt = -1;

    TSqlQuery query(Tf::app()->databaseIdForInternalUse());
    QString sql = QStringLiteral("select count(1) from %1").arg(_table);

    if (query.exec(sql) && query.next()) {
        cnt = query.value(0).toInt();
    }
    return cnt;
}


bool TCacheSQLiteStore::exists(const QByteArray &key)
{
    int exist = 0;
    TSqlQuery query(Tf::app()->databaseIdForInternalUse());
    QString sql = QStringLiteral("select exists(select 1 from %1 where %2=:name limit 1)").arg(_table).arg(KEY_COLUMN);

    query.prepare(sql);
    query.bind(":name", key);
    if (query.exec() && query.next()) {
        exist = query.value(0).toInt();
    }
    return (exist > 0);
}


QByteArray TCacheSQLiteStore::get(const QByteArray &key)
{
    QByteArray value;
    qint64 expire = 0;
    qint64 current = QDateTime::currentMSecsSinceEpoch();

    if (read(key, value, expire)) {
        if (expire < current) {
            value.clear();
            remove(key);
        }
    }
    return value;
}


bool TCacheSQLiteStore::set(const QByteArray &key, const QByteArray &value, qint64 msecs)
{
    if (key.isEmpty() || msecs <= 0) {
        return false;
    }

    remove(key);
    qint64 expire = QDateTime::currentMSecsSinceEpoch() + msecs;
    return write(key, value, expire);
}


bool TCacheSQLiteStore::read(const QByteArray &key, QByteArray &blob, qint64 &timestamp)
{
    bool ret = false;

    if (key.isEmpty()) {
        return ret;
    }

    TSqlQuery query(Tf::app()->databaseIdForInternalUse());
    query.prepare(QStringLiteral("select %1,%2 from %3 where %4=:key").arg(TIMESTAMP_COLUMN, BLOB_COLUMN, _table, KEY_COLUMN));
    query.bind(":key", key);
    ret = query.exec();
    if (ret) {
        if (query.next()) {
            timestamp = query.value(0).toLongLong();
            blob = query.value(1).toByteArray();
        }
    } else {
        tSystemError("SQLite error : %s [%s:%d]", qPrintable(lastError()), __FILE__, __LINE__);
    }
    return ret;
}


bool TCacheSQLiteStore::write(const QByteArray &key, const QByteArray &blob, qint64 timestamp)
{
    bool ret = false;

    if (key.isEmpty()) {
        return ret;
    }

    TSqlQuery query(Tf::app()->databaseIdForInternalUse());
    QString sql = QStringLiteral("insert into %1 (%2,%3,%4) values (:key,:ts,:blob)").arg(_table, KEY_COLUMN, TIMESTAMP_COLUMN, BLOB_COLUMN);

    query.prepare(sql);
    query.bind(":key", key).bind(":ts", timestamp).bind(":blob", blob);
    ret = query.exec();
    if (! ret) {
        tSystemError("SQLite error : %s [%s:%d]", qPrintable(lastError()), __FILE__, __LINE__);
    }
    return ret;
}


bool TCacheSQLiteStore::remove(const QByteArray &key)
{
    bool ret = false;

    if (key.isEmpty()) {
        return ret;
    }

    TSqlQuery query(Tf::app()->databaseIdForInternalUse());
    QString sql = QStringLiteral("delete from %1 where %2=:key").arg(_table, KEY_COLUMN);

    query.prepare(sql);
    query.bind(":key", key);
    ret = query.exec();
    if (! ret) {
        tSystemError("SQLite error : %s [%s:%d]", qPrintable(lastError()), __FILE__, __LINE__);
    }
    return ret;
}


void TCacheSQLiteStore::clear()
{
    removeAll();
    vacuum();
}


int TCacheSQLiteStore::removeOlder(int num)
{
    int cnt = -1;

    if (num < 1) {
        return cnt;
    }

    TSqlQuery query(Tf::app()->databaseIdForInternalUse());
    QString sql = QStringLiteral("delete from %1 where ROWID in (select ROWID from %1 order by t asc limit :num)").arg(_table);

    query.prepare(sql);
    query.bind(":num", num);
    if (query.exec()) {
        cnt = query.numRowsAffected();
    } else {
        tSystemError("SQLite error : %s [%s:%d]", qPrintable(lastError()), __FILE__, __LINE__);
    }
    return cnt;
}


int TCacheSQLiteStore::removeOlderThan(qint64 timestamp)
{
    int cnt = -1;

    TSqlQuery query(Tf::app()->databaseIdForInternalUse());
    QString sql = QStringLiteral("delete from %1 where %2<:ts").arg(_table, TIMESTAMP_COLUMN);

    query.prepare(sql);
    query.bind(":ts", timestamp);
    if (query.exec()) {
        cnt = query.numRowsAffected();
    } else {
        tSystemError("SQLite error : %s [%s:%d]", qPrintable(lastError()), __FILE__, __LINE__);
    }
    return cnt;
}


int TCacheSQLiteStore::removeAll()
{
    int cnt = -1;

    TSqlQuery query(Tf::app()->databaseIdForInternalUse());
    QString sql = QStringLiteral("delete from %1").arg(_table);

    if (query.exec(sql)) {
        cnt = query.numRowsAffected();
    } else {
        tSystemError("SQLite error : %s [%s:%d]", qPrintable(lastError()), __FILE__, __LINE__);
    }
    return cnt;
}


bool TCacheSQLiteStore::vacuum()
{
    bool ret = exec(QStringLiteral("vacuum"));
    return ret;
}


qint64 TCacheSQLiteStore::dbSize()
{
    qint64 sz = -1;

    TSqlQuery query(Tf::app()->databaseIdForInternalUse());
    bool ok = query.exec(QStringLiteral("pragma page_size"));
    if (ok && query.next()) {
        qint64 size = query.value(0).toLongLong();

        ok = query.exec(QStringLiteral("pragma page_count"));
        if (ok && query.next()) {
            qint64 count = query.value(0).toLongLong();
            sz = size * count;
        }
    }
    return sz;
}


void TCacheSQLiteStore::gc()
{
    int removed = removeOlderThan(QDateTime::currentMSecsSinceEpoch());
    tSystemDebug("removeOlderThan: %d\n", removed);
    vacuum();

    if (_thresholdFileSize > 0 && dbSize() > _thresholdFileSize) {
        for (int i = 0; i < 3; i++) {
            removed += removeOlder(count() * 0.3);
            vacuum();
            if (dbSize() < _thresholdFileSize * 0.8) {
                break;
            }
        }
        tSystemDebug("removeOlder: %d\n", removed);
    }
}


QMap<QString, QVariant> TCacheSQLiteStore::defaultSettings() const
{
    QMap<QString, QVariant> settings {
        {"DriverType", "QSQLITE"},
        {"DatabaseName", "cachedb"},
        {"PostOpenStatements", "PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON; PRAGMA busy_timeout=5000; PRAGMA synchronous=NORMAL;"},
    };
    return settings;
}
