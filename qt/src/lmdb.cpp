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
    std::shared_ptr<qtng_core::Database> core;
};

class TransactionPrivate
{
public:
    shared_ptr<qtng_core::Transaction> core;
    mutable QHash<QString, std::shared_ptr<Database>> dbCache;

    std::shared_ptr<Database> database(const QString &name);
    std::shared_ptr<const Database> database(const QString &name) const;
};

class LmdbIteratorPrivate
{
public:
    explicit LmdbIteratorPrivate(qtng_core::LmdbIterator &&itor)
        : core(new qtng_core::LmdbIterator(std::move(itor)))
    {
    }
    explicit LmdbIteratorPrivate(qtng_core::ConstLmdbIterator &&itor)
        : constCore(new qtng_core::ConstLmdbIterator(std::move(itor)))
    {
    }
public:
    qtng_core::LmdbIterator &iter() { return *core; }
    qtng_core::ConstLmdbIterator &citer() { return *constCore; }
private:
    unique_ptr<qtng_core::LmdbIterator> core;
    unique_ptr<qtng_core::ConstLmdbIterator> constCore;
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

std::shared_ptr<Database> TransactionPrivate::database(const QString &name)
{
    if (std::shared_ptr<Database> cached = dbCache.value(name)) {
        return cached;
    }
    std::shared_ptr<qtng_core::Database> coreDb = core->db(toStdString(name));
    if (!coreDb) {
        return std::shared_ptr<Database>();
    }
    DatabasePrivate *priv = new DatabasePrivate;
    priv->core = coreDb;
    std::shared_ptr<Database> wrapper(new Database(priv));
    dbCache.insert(name, wrapper);
    return wrapper;
}

std::shared_ptr<const Database> TransactionPrivate::database(const QString &name) const
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

std::shared_ptr<const Database> Transaction::db(const QString &name) const
{
    Q_D(const Transaction);
    return d->database(name);
}

std::shared_ptr<Database> Transaction::db(const QString &name)
{
    Q_D(Transaction);
    return d->database(name);
}

QSharedPointer<Transaction> Transaction::sub()
{
    Q_D(Transaction);
    shared_ptr<qtng_core::Transaction> coreTx = d->core->sub();
    if (!coreTx) {
        return QSharedPointer<Transaction>();
    }
    TransactionPrivate *priv = new TransactionPrivate;
    priv->core = coreTx;
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

ConstLmdbIterator::~ConstLmdbIterator()
{
    delete d_ptr;
}

QByteArray ConstLmdbIterator::key() const
{
    if (!d_ptr) {
        return QByteArray();
    }
    return toQByteArray(d_ptr->citer().key());
}

QByteArray ConstLmdbIterator::value() const
{
    if (!d_ptr) {
        return QByteArray();
    }
    return toQByteArray(d_ptr->citer().value());
}

bool ConstLmdbIterator::isEnd() const
{
    return !d_ptr || d_ptr->citer().isEnd();
}

const char *ConstLmdbIterator::data() const
{
    if (!d_ptr) {
        return nullptr;
    }
    return d_ptr->citer().data();
}

size_t ConstLmdbIterator::size() const
{
    if (!d_ptr) {
        return 0;
    }
    return d_ptr->citer().size();
}

bool ConstLmdbIterator::operator==(const ConstLmdbIterator &other) const
{
    if (isEnd() || other.isEnd()) {
        return isEnd() == other.isEnd();
    }
    return d_ptr->citer() == other.d_ptr->citer();
}

ConstLmdbIterator &ConstLmdbIterator::operator++()
{
    if (d_ptr) {
        d_ptr->citer().operator++();
    }
    return *this;
}

ConstLmdbIterator &ConstLmdbIterator::operator--()
{
    if (d_ptr) {
        d_ptr->citer().operator--();
    }
    return *this;
}

LmdbIterator::~LmdbIterator()
{
    delete d_ptr;
}

QByteArray LmdbIterator::key() const
{
    if (!d_ptr) {
        return QByteArray();
    }
    return toQByteArray(d_ptr->iter().key());
}

QByteArray LmdbIterator::value() const
{
    if (!d_ptr) {
        return QByteArray();
    }
    return toQByteArray(d_ptr->iter().value());
}

bool LmdbIterator::isEnd() const
{
    return !d_ptr || d_ptr->iter().isEnd();
}

char *LmdbIterator::data() const
{
    if (!d_ptr) {
        return nullptr;
    }
    return d_ptr->iter().data();
}

size_t LmdbIterator::size() const
{
    if (!d_ptr) {
        return 0;
    }
    return d_ptr->iter().size();
}

bool LmdbIterator::operator==(const LmdbIterator &other) const
{
    if (isEnd() || other.isEnd()) {
        return isEnd() == other.isEnd();
    }
    return d_ptr->iter() == other.d_ptr->iter();
}

LmdbIterator &LmdbIterator::operator++()
{
    if (d_ptr) {
        d_ptr->iter().operator++();
    }
    return *this;
}

LmdbIterator &LmdbIterator::operator--()
{
    if (d_ptr) {
        d_ptr->iter().operator--();
    }
    return *this;
}

Database::iterator Database::insert(const QByteArray &key, const QByteArray &value)
{
    Q_D(Database);
    return LmdbIterator(new LmdbIteratorPrivate(d->core->insert(toStdString(key), toStdString(value))));
}

Database::iterator Database::reserve(const QByteArray &key, size_t size)
{
    Q_D(Database);
    return LmdbIterator(new LmdbIteratorPrivate(d->core->reserve(toStdString(key), size)));
}

qint64 Database::insert(const Database &other)
{
    Q_D(Database);
    return d->core->insert(*other.d_ptr->core);
}

void Database::clear()
{
    Q_D(Database);
    d->core->clear();
}

QList<QByteArray> Database::keys() const
{
    Q_D(const Database);
    QList<QByteArray> ks;
    for (const string &k : d->core->keys()) {
        ks.append(toQByteArray(k));
    }
    return ks;
}

QStringList Database::strKeys() const
{
    Q_D(const Database);
    QStringList ks;
    for (const string &k : d->core->strKeys()) {
        ks.append(toQString(k));
    }
    return ks;
}

int Database::remove(const QByteArray &key)
{
    Q_D(Database);
    return d->core->remove(toStdString(key));
}

QByteArray Database::take(const QByteArray &key)
{
    Q_D(Database);
    return toQByteArray(d->core->take(toStdString(key)));
}

bool Database::contains(const QByteArray &key) const
{
    Q_D(const Database);
    return d->core->contains(toStdString(key));
}

bool Database::isNull() const
{
    Q_D(const Database);
    return d->core->isNull();
}

bool Database::isEmpty() const
{
    Q_D(const Database);
    return d->core->isEmpty();
}

qint64 Database::size() const
{
    Q_D(const Database);
    return d->core->size();
}

Database::iterator Database::begin()
{
    Q_D(Database);
    return LmdbIterator(new LmdbIteratorPrivate(d->core->begin()));
}

Database::const_iterator Database::constBegin() const
{
    Q_D(const Database);
    return ConstLmdbIterator(new LmdbIteratorPrivate(d->core->constBegin()));
}

Database::iterator Database::end()
{
    Q_D(Database);
    return LmdbIterator(new LmdbIteratorPrivate(d->core->end()));
}

Database::const_iterator Database::constEnd() const
{
    Q_D(const Database);
    return ConstLmdbIterator(new LmdbIteratorPrivate(d->core->constEnd()));
}

Database::iterator Database::erase(iterator &itor)
{
    Q_D(Database);
    if (!itor.d_ptr) {
        return LmdbIterator(nullptr);
    }
    return LmdbIterator(new LmdbIteratorPrivate(d->core->erase(itor.d_ptr->iter())));
}

Database::iterator Database::find(const QByteArray &key)
{
    Q_D(Database);
    return LmdbIterator(new LmdbIteratorPrivate(d->core->find(toStdString(key))));
}

Database::const_iterator Database::constFind(const QByteArray &key) const
{
    Q_D(const Database);
    return ConstLmdbIterator(new LmdbIteratorPrivate(d->core->constFind(toStdString(key))));
}

Database::const_iterator Database::lowerBound(const QByteArray &key) const
{
    Q_D(const Database);
    const qtng_core::Database *cdb = d->core.get();
    return ConstLmdbIterator(new LmdbIteratorPrivate(cdb->lowerBound(toStdString(key))));
}

Database::iterator Database::lowerBound(const QByteArray &key)
{
    Q_D(Database);
    return LmdbIterator(new LmdbIteratorPrivate(d->core->lowerBound(toStdString(key))));
}

Database::const_iterator Database::upperBound(const QByteArray &key) const
{
    Q_D(const Database);
    const qtng_core::Database *cdb = d->core.get();
    return ConstLmdbIterator(new LmdbIteratorPrivate(cdb->upperBound(toStdString(key))));
}

Database::iterator Database::upperBound(const QByteArray &key)
{
    Q_D(Database);
    return LmdbIterator(new LmdbIteratorPrivate(d->core->upperBound(toStdString(key))));
}

}  // namespace QTNETWORKNG_NAMESPACE
