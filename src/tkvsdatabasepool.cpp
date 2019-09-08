/* Copyright (c) 2010-2019, AOYAMA Kazuharu
 * All rights reserved.
 *
 * This software may be used and distributed according to the terms of
 * the New BSD License, which is incorporated herein by reference.
 */

#include "tkvsdatabasepool.h"
#include "tsqldatabasepool.h"
#include "tsystemglobal.h"
#include <TWebApplication>
#include <QStringList>
#include <QDateTime>
#include <QMap>
#include <QThread>
#include <ctime>

/*!
  \class TKvsDatabasePool
  \brief The TKvsDatabasePool class manages a collection of TKvsDatabase instances.
*/

constexpr auto CONN_NAME_FORMAT = "kvs%02d_%d";


class KvsTypeHash : public QMap<QString, int>
{
public:
    KvsTypeHash() : QMap<QString, int>()
    {
        insert("MONGODB", TKvsDatabase::MongoDB);
        insert("REDIS", TKvsDatabase::Redis);
    }
};
Q_GLOBAL_STATIC(KvsTypeHash, kvsTypeHash)


TKvsDatabasePool *TKvsDatabasePool::instance()
{
    static TKvsDatabasePool *databasePool = []() {
        auto *pool = new TKvsDatabasePool(Tf::app()->databaseEnvironment());
        pool->maxConnects = Tf::app()->maxNumberOfThreadsPerAppServer();
        pool->init();
        return pool;
    }();
    return databasePool;
}


TKvsDatabasePool::TKvsDatabasePool(const QString &environment) :
    QObject(),
    dbEnvironment(environment)
{ }


TKvsDatabasePool::~TKvsDatabasePool()
{
    timer.stop();

    for (int type = 0; type < TKvsDatabase::TypeNum; type++) {
        if (!isKvsAvailable((TKvsDatabase::Type)type)) {
            continue;
        }

        auto &cache = cachedDatabase[type];
        QString name;
        while (cache.pop(name)) {
            TKvsDatabase::database(name).close();
            TKvsDatabase::removeDatabase(name);
        }

        auto &stack = availableNames[type];
        while (stack.pop(name)) {
            TKvsDatabase::removeDatabase(name);
        }
    }

    delete[] cachedDatabase;
    delete[] lastCachedTime;
    delete[] availableNames;
}


void TKvsDatabasePool::init()
{
    if (cachedDatabase) {
        return;
    }

    cachedDatabase = new TStack<QString>[kvsTypeHash()->count()];
    lastCachedTime = new TAtomic<uint>[kvsTypeHash()->count()];
    availableNames = new TStack<QString>[kvsTypeHash()->count()];
    bool aval = false;

    // Adds databases previously
    for (QMapIterator<QString, int> it(*kvsTypeHash()); it.hasNext(); ) {
        const QString &drv = it.next().key();
        int type = it.value();

        if (!isKvsAvailable((TKvsDatabase::Type)type)) {
            tSystemDebug("KVS database not available. type:%d", (int)type);
            continue;
        } else {
            aval = true;
            tSystemDebug("KVS database available. type:%d", (int)type);
        }

        auto &stack = availableNames[type];
        for (int i = 0; i < maxConnects; ++i) {
            TKvsDatabase db = TKvsDatabase::addDatabase(drv, QString().sprintf(CONN_NAME_FORMAT, type, i));
            if (!db.isValid()) {
                tWarn("KVS init parameter is invalid");
                break;
            }

            setDatabaseSettings(db, (TKvsDatabase::Type)type, dbEnvironment);
            stack.push(db.connectionName());  // push onto stack
            tSystemDebug("Add KVS successfully. name:%s", qPrintable(db.connectionName()));
        }
    }

    if (aval) {
        // Starts the timer to close extra-connection
        timer.start(10000, this);
    }
}


bool TKvsDatabasePool::isKvsAvailable(TKvsDatabase::Type type) const
{
    switch (type) {
    case TKvsDatabase::MongoDB:
        return Tf::app()->isMongoDbAvailable();
        break;

    case TKvsDatabase::Redis:
        return Tf::app()->isRedisAvailable();
        break;

    default:
        throw RuntimeException("No such KVS type", __FILE__, __LINE__);
        break;
    }
}


QSettings &TKvsDatabasePool::kvsSettings(TKvsDatabase::Type type) const
{
    switch (type) {
    case TKvsDatabase::MongoDB:
        if (Tf::app()->isMongoDbAvailable()) {
            return Tf::app()->mongoDbSettings();
        }
        break;

    case TKvsDatabase::Redis:
        if (Tf::app()->isRedisAvailable()) {
            return Tf::app()->redisSettings();
        }
        break;

    default:
        throw RuntimeException("No such KVS type", __FILE__, __LINE__);
        break;
    }

    throw RuntimeException("Logic error", __FILE__, __LINE__);
}


TKvsDatabase TKvsDatabasePool::database(TKvsDatabase::Type type)
{
    TKvsDatabase db;

    if (!isKvsAvailable(type)) {
        switch (type) {
        case TKvsDatabase::MongoDB:
            tSystemError("MongoDB not available. Check the settings file.");
            break;

        case TKvsDatabase::Redis:
            tSystemError("Redis not available. Check the settings file.");
            break;

        default:
            throw RuntimeException("No such KVS type", __FILE__, __LINE__);
            break;
        }
        return db;
    }

    auto &cache = cachedDatabase[(int)type];
    auto &stack = availableNames[(int)type];

    for (;;) {
        QString name;
        if (cache.pop(name)) {
            db = TKvsDatabase::database(name);
            if (Q_LIKELY(db.isOpen())) {
                tSystemDebug("Gets cached KVS database: %s", qPrintable(db.connectionName()));
                return db;
            } else {
                tSystemError("Pooled database is not open: %s  [%s:%d]", qPrintable(db.connectionName()), __FILE__, __LINE__);
                stack.push(name);
                continue;
            }
        }

        if (Q_LIKELY(stack.pop(name))) {
            db = TKvsDatabase::database(name);
            if (Q_UNLIKELY(db.isOpen())) {
                tSystemWarn("Gets a opend KVS database: %s", qPrintable(db.connectionName()));
                return db;
            } else {
                db.moveToThread(QThread::currentThread());  // move to thread

                if (Q_UNLIKELY(!db.open())) {
                    tError("KVS Database open error. Invalid database settings, or maximum number of KVS connection exceeded.");
                    tSystemError("KVS database open error: %s", qPrintable(db.connectionName()));
                    return TKvsDatabase();
                }

                tSystemDebug("KVS opened successfully  env:%s connectname:%s dbname:%s", qPrintable(dbEnvironment), qPrintable(db.connectionName()), qPrintable(db.databaseName()));
                tSystemDebug("Gets KVS database: %s", qPrintable(db.connectionName()));
                return db;
            }
        }
    }

    throw RuntimeException("No pooled connection", __FILE__, __LINE__);
}


bool TKvsDatabasePool::setDatabaseSettings(TKvsDatabase &database, TKvsDatabase::Type type, const QString &env) const
{
    // Initiates database
    const QSettings &settings = kvsSettings(type);
    QString databaseName = settings.value(env + "/DatabaseName").toString().trimmed();

    if (databaseName.isEmpty()) {
        if (type != TKvsDatabase::Redis) {
            tWarn("KVS Database name empty string");
            return false;
        }
    } else {
        tSystemDebug("KVS db name:%s  driver name:%s", qPrintable(databaseName), qPrintable(database.driverName()));
        database.setDatabaseName(databaseName);
    }

    QString hostName = settings.value(env + "/HostName").toString().trimmed();
    tSystemDebug("KVS HostName: %s", qPrintable(hostName));
    if (!hostName.isEmpty()) {
        database.setHostName(hostName);
    }

    int port = settings.value(env + "/Port").toInt();
    tSystemDebug("KVS Port: %d", port);
    if (port > 0) {
        database.setPort(port);
    }

    QString userName = settings.value(env + "/UserName").toString().trimmed();
    tSystemDebug("KVS UserName: %s", qPrintable(userName));
    if (!userName.isEmpty()) {
        database.setUserName(userName);
    }

    QString password = settings.value(env + "/Password").toString().trimmed();
    tSystemDebug("KVS Password: %s", qPrintable(password));
    if (!password.isEmpty()) {
        database.setPassword(password);
    }

    QString connectOptions = settings.value(env + "/ConnectOptions").toString().trimmed();
    tSystemDebug("KVS ConnectOptions: %s", qPrintable(connectOptions));
    if (!connectOptions.isEmpty()) {
        database.setConnectOptions(connectOptions);
    }

    return true;
}


void TKvsDatabasePool::pool(TKvsDatabase &database)
{
    if (Q_LIKELY(database.isValid())) {
        int type = kvsTypeHash()->value(database.driverName(), -1);
        if (Q_UNLIKELY(type < 0)) {
            throw RuntimeException("No such KVS type", __FILE__, __LINE__);
        }

        cachedDatabase[type].push(database.connectionName());
        lastCachedTime[type].store((uint)std::time(nullptr));
        tSystemDebug("Pooled KVS database: %s", qPrintable(database.connectionName()));
    }
    database = TKvsDatabase();  // Sets an invalid object
}


void TKvsDatabasePool::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == timer.timerId()) {
        QString name;

        // Closes extra-connection
        for (int t = 0; t < kvsTypeHash()->count(); t++) {
            if (!isKvsAvailable((TKvsDatabase::Type)t)) {
                continue;
            }

            auto &cache = cachedDatabase[t];
            while (lastCachedTime[t].load() < (uint)std::time(nullptr) - 30
                   && cache.pop(name)) {
                TKvsDatabase::database(name).close();
                tSystemDebug("Closed KVS database connection, name: %s", qPrintable(name));
                availableNames[t].push(name);
            }
        }
    } else {
        QObject::timerEvent(event);
    }
}


QString TKvsDatabasePool::driverName(TKvsDatabase::Type type)
{
    return kvsTypeHash()->key((int)type);
}
