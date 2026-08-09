#include "bridge/core_access.h"
#include "lmdb.h"

#include <QtCore/qhash.h>

using namespace std;
using namespace QTNETWORKNG_NAMESPACE;
using namespace qtng_bridge;

namespace QTNETWORKNG_NAMESPACE {

class LmdbPrivate
{
public:
    shared_ptr<qtng_core::Lmdb> core;
};

class DatabasePrivate
{
public:
    qtng_core::Database *core = nullptr;
};

class TransactionPrivate
{
public:
    shared_ptr<qtng_core::Transaction> core;
    mutable QHash<QString, Database *> dbCache;

    Database &database(const QString &name);
    const Database &database(const QString &name) const;
};

class LmdbIteratorPrivate
{
public:
    qtng_core::LmdbIterator core;
};

LmdbBuilder::LmdbBuilder(const QString &dirPath)
    : m_dirPath(dirPath)
{
}

LmdbBuilder &LmdbBuilder::maxMapSize(size_t size)
{
    m_maxMapSize = size;
    return *this;
}

LmdbBuilder &LmdbBuilder::maxReaders(int readers)
{
    m_maxReaders = readers;
    return *this;
}

LmdbBuilder &LmdbBuilder::maxDbs(int maxDbs)
{
    m_maxDbs = maxDbs;
    return *this;
}

LmdbBuilder &LmdbBuilder::noSync(bool noSync)
{
    m_noSync = noSync;
    return *this;
}

LmdbBuilder &LmdbBuilder::noSubDir(bool noSubDir)
{
    m_noSubDir = noSubDir;
    return *this;
}

LmdbBuilder &LmdbBuilder::writeMap(bool writable)
{
    m_writeMap = writable;
    return *this;
}

QSharedPointer<Lmdb> LmdbBuilder::create()
{
    qtng_core::Lmdb::Builder builder(toStdString(m_dirPath));
    builder.maxMapSize(m_maxMapSize).maxReaders(m_maxReaders).maxDbs(m_maxDbs).noSync(m_noSync).noSubDir(m_noSubDir)
            .writeMap(m_writeMap);
    shared_ptr<qtng_core::Lmdb> core = builder.create();
    if (!core) {
        return QSharedPointer<Lmdb>();
    }
    LmdbPrivate *priv = new LmdbPrivate;
    priv->core = core;
    return QSharedPointer<Lmdb>(new Lmdb(priv));
}

Lmdb::~Lmdb()
{
    delete d_ptr;
}

Database &TransactionPrivate::database(const QString &name)
{
    if (Database *cached = dbCache.value(name)) {
        return *cached;
    }
    qtng_core::Database &coreDb = core->db(toStdString(name));
    DatabasePrivate *priv = new DatabasePrivate;
    priv->core = &coreDb;
    Database *wrapper = new Database(priv);
    dbCache.insert(name, wrapper);
    return *wrapper;
}

const Database &TransactionPrivate::database(const QString &name) const
{
    return const_cast<TransactionPrivate *>(this)->database(name);
}

QSharedPointer<const Transaction> Lmdb::toRead()
{
    Q_D(Lmdb);
    shared_ptr<const qtng_core::Transaction> tx = d->core->toRead();
    TransactionPrivate *priv = new TransactionPrivate;
    priv->core = std::const_pointer_cast<qtng_core::Transaction>(tx);
    return QSharedPointer<const Transaction>(new Transaction(priv));
}

QSharedPointer<Transaction> Lmdb::toWrite()
{
    Q_D(Lmdb);
    shared_ptr<qtng_core::Transaction> tx = d->core->toWrite();
    TransactionPrivate *priv = new TransactionPrivate;
    priv->core = tx;
    return QSharedPointer<Transaction>(new Transaction(priv));
}

QString Lmdb::version() const
{
    Q_D(const Lmdb);
    return toQString(d->core->version());
}

void Lmdb::sync(bool force)
{
    Q_D(Lmdb);
    d->core->sync(force);
}

bool Lmdb::backupTo(const QString &dirPath)
{
    Q_D(Lmdb);
    return d->core->backupTo(toStdString(dirPath));
}

Transaction::~Transaction()
{
    delete d_ptr;
}

const Database &Transaction::db(const QString &name) const
{
    Q_D(const Transaction);
    return d->database(name);
}

Database &Transaction::db(const QString &name)
{
    Q_D(Transaction);
    return d->database(name);
}

QSharedPointer<Transaction> Transaction::sub()
{
    Q_D(Transaction);
    TransactionPrivate *priv = new TransactionPrivate;
    priv->core = d->core->sub();
    return QSharedPointer<Transaction>(new Transaction(priv));
}

QSharedPointer<const Transaction> Transaction::sub() const
{
    return const_cast<Transaction *>(this)->sub();
}

QSharedPointer<Transaction> Transaction::fork()
{
    Q_D(Transaction);
    TransactionPrivate *priv = new TransactionPrivate;
    priv->core = d->core->fork();
    return QSharedPointer<Transaction>(new Transaction(priv));
}

QSharedPointer<const Transaction> Transaction::fork() const
{
    return const_cast<Transaction *>(this)->fork();
}

bool Transaction::commit()
{
    Q_D(Transaction);
    return d->core->commit();
}

void Transaction::abort()
{
    Q_D(Transaction);
    d->core->abort();
}

Database::~Database()
{
    delete d_ptr;
}

}  // namespace QTNETWORKNG_NAMESPACE
