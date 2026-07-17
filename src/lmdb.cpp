#include <algorithm>
#include <cassert>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "qtng/lmdb.h"
#include "qtng/utils/logging.h"
#include "./liblmdb/lmdb.h"

using namespace std;

namespace qtng {

#define QTLMDB_DEBUG 1

NG_LOGGER("qtng.lmdb");

class LmdbIteratorPrivate
{
public:
    LmdbIteratorPrivate(string key, MDB_cursor *cursor, MDB_val mdbValue)
        : key(key)
        , cursor(cursor)
        , mdbValue(mdbValue)
    {
    }
public:
    void load(MDB_cursor_op op);
public:
    string key;
    MDB_cursor *cursor;
    MDB_val mdbValue;
};

class DatabasePrivate
{
public:
    DatabasePrivate(MDB_txn *txn, MDB_dbi dbi, bool readOnly)
        : txn(txn)
        , dbi(dbi)
        , readOnly(readOnly)
    {
    }
public:
    MDB_cursor *makeCursor();
    MDB_cursor *setCursor(string &key, MDB_val &mdbKey, MDB_val &mdbData, MDB_cursor_op op = MDB_SET);
    LmdbIteratorPrivate *end(MDB_cursor *cursor = nullptr);
public:
    MDB_txn * const txn;
    MDB_dbi dbi;
    bool readOnly;
};

class TransactionPrivate
{
public:
    TransactionPrivate(MDB_env *env, MDB_txn *txn, bool readOnly)
        : env(env)
        , txn(txn)
        , finished(false)
        , readOnly(readOnly)
    {
    }
public:
    Database &open(const string &name);
public:
    map<string, shared_ptr<Database>> dbs;
    MDB_env * const env;
    MDB_txn * const txn;
    bool finished;
    bool readOnly;
};

class LmdbPrivate
{
public:
    MDB_env *env;
};

void LmdbIteratorPrivate::load(MDB_cursor_op op)
{
    assert(cursor);
    MDB_val mdbKey;
    memset(&mdbKey, 0, sizeof(MDB_val));
    int rt = mdb_cursor_get(cursor, &mdbKey, &mdbValue, op);
    if (rt) {
#if QTLMDB_DEBUG
        if (rt != MDB_NOTFOUND || (op != MDB_FIRST && op != MDB_NEXT &&
                                   op != MDB_PREV && op != MDB_SET && op != MDB_SET_KEY &&
                                   op != MDB_LAST)) {
            ngInfo() << "can not open lmdb cursor:" << mdb_strerror(rt) << key << op;
        }
#endif
        key.clear();
    } else {
        key = string(static_cast<const char *>(mdbKey.mv_data), mdbKey.mv_size);
    }
}

MDB_cursor *DatabasePrivate::makeCursor()
{
    MDB_cursor *cursor;
    int rt = mdb_cursor_open(txn, dbi, &cursor);
    if (rt) {
#if QTLMDB_DEBUG
        ngWarning() << "can not open lmdb cursor:" << mdb_strerror(rt);
#endif
        return nullptr;
    }
    return cursor;
}

MDB_cursor *DatabasePrivate::setCursor(string &key, MDB_val &mdbKey, MDB_val &mdbData, MDB_cursor_op op)
{
    MDB_cursor *cursor = makeCursor();
    if (!cursor) {
        return nullptr;
    }

    mdbKey.mv_size = key.size();
    mdbKey.mv_data = const_cast<char *>(key.data());

    memset(&mdbData, 0, sizeof(MDB_val));

    int rt = mdb_cursor_get(cursor, &mdbKey, &mdbData, op);
    if (rt) {
        mdb_cursor_close(cursor);
        if (rt != MDB_NOTFOUND) {
#if QTLMDB_DEBUG
            ngWarning() << "can not iterate lmdb cursor:" << mdb_strerror(rt);
#endif
        }
        return nullptr;
    }
    return cursor;
}

ConstLmdbIterator::~ConstLmdbIterator()
{
    if (!d_ptr) {
        return;
    }
    mdb_cursor_close(d_ptr->cursor);
    delete d_ptr;
}

string ConstLmdbIterator::key() const
{
    if (isEnd()) {
        return string();
    }
    return d_ptr->key;
}

string ConstLmdbIterator::value() const
{
    if (isEnd()) {
        return string();
    }
    return string(static_cast<const char *>(d_ptr->mdbValue.mv_data), d_ptr->mdbValue.mv_size);
}

const char *ConstLmdbIterator::data() const
{
    if (isEnd()) {
        return nullptr;
    }
    return static_cast<const char *>(d_ptr->mdbValue.mv_data);
}

size_t ConstLmdbIterator::size() const
{
    if (isEnd()) {
        return 0;
    }
    return d_ptr->mdbValue.mv_size;
}

bool ConstLmdbIterator::isEnd() const
{
    return !d_ptr || d_ptr->key.empty();
}

bool ConstLmdbIterator::operator==(const ConstLmdbIterator &other) const
{
    if (isEnd()) {
        return other.isEnd();
    }
    if (other.isEnd()) {
        return false;
    }
    if (d_ptr->cursor != other.d_ptr->cursor) {
        return false;
    }
    return d_ptr->key == other.d_ptr->key;
}

ConstLmdbIterator &ConstLmdbIterator::operator++()
{
    if (isEnd()) {
        return *this;
    }
    d_ptr->load(MDB_NEXT);
    return *this;
}

ConstLmdbIterator &ConstLmdbIterator::operator--()
{
    if (!d_ptr->cursor) {
        return *this;
    }
    d_ptr->load(MDB_PREV);
    return *this;
}

LmdbIterator::~LmdbIterator()
{
    if (!d_ptr) {
        return;
    }
    mdb_cursor_close(d_ptr->cursor);
    delete d_ptr;
}

string LmdbIterator::key() const
{
    if (isEnd()) {
        return string();
    }
    return d_ptr->key;
}

string LmdbIterator::value() const
{
    if (isEnd()) {
        return string();
    }
    return string(static_cast<const char *>(d_ptr->mdbValue.mv_data), d_ptr->mdbValue.mv_size);
}

bool LmdbIterator::isEnd() const
{
    return !d_ptr || !d_ptr->cursor || d_ptr->key.empty();
}

char *LmdbIterator::data() const
{
    if (isEnd()) {
        return nullptr;
    }
    return static_cast<char *>(d_ptr->mdbValue.mv_data);
}

size_t LmdbIterator::size() const
{
    if (isEnd()) {
        return 0;
    }
    return d_ptr->mdbValue.mv_size;
}

bool LmdbIterator::operator==(const LmdbIterator &other) const
{
    if (isEnd()) {
        return other.isEnd();
    }
    if (other.isEnd()) {
        return false;
    }
    if (d_ptr->cursor != other.d_ptr->cursor) {
        return false;
    }
    return d_ptr->key == other.d_ptr->key;
}

LmdbIterator &LmdbIterator::operator++()
{
    if (isEnd()) {
        return *this;
    }
    d_ptr->load(MDB_NEXT);
    return *this;
}

LmdbIterator &LmdbIterator::operator--()
{
    if (!d_ptr->cursor) {
        return *this;
    }
    d_ptr->load(MDB_PREV);
    return *this;
}

Database::~Database()
{
    delete d_ptr;
}

Database::iterator Database::insert(const string &key, const string &value)
{
    Database::iterator itor = reserve(key, value.size());

    if (itor.isEnd()) {
        return itor;
    }
    if (static_cast<size_t>(value.size()) != itor.d_ptr->mdbValue.mv_size) {
        ngWarning() << "setting value size mismatch for lmdb reserved key-value.";
    }
    size_t minSize = min(static_cast<size_t>(value.size()), itor.d_ptr->mdbValue.mv_size);
    memcpy(itor.d_ptr->mdbValue.mv_data, value.data(), minSize);
    return itor;
}

Database::iterator Database::reserve(const string &key, size_t size)
{
    if (isNull() || d_ptr->readOnly) {
        return LmdbIterator(nullptr);
    }

    if (size == 0) {
        return LmdbIterator(nullptr);
    }

    MDB_cursor *cursor = d_ptr->makeCursor();
    if (!cursor) {
        return LmdbIterator(nullptr);
    }

    MDB_val mdbKey, mdbData;
    mdbKey.mv_size = key.size();
    mdbKey.mv_data = (void *) key.data();
    mdbData.mv_size = size;
    mdbData.mv_data = NULL;

    unsigned int flags = MDB_RESERVE;
    int rt = mdb_cursor_put(cursor, &mdbKey, &mdbData, flags);
    if (rt) {
#if QTLMDB_DEBUG
        ngWarning() << "can not put lmdb value:" << mdb_strerror(rt);
#endif
        mdb_cursor_close(cursor);
        return LmdbIterator(nullptr);
    }
    LmdbIteratorPrivate *d = new LmdbIteratorPrivate(key, cursor, mdbData);
    return LmdbIterator(d);
}

int64_t Database::insert(const Database &other)
{
    if (isNull() || d_ptr->readOnly) {
        return -1;
    }

    MDB_cursor *cursor = d_ptr->makeCursor();
    if (!cursor) {
        return -1;
    }

    int64_t count = 0;
    for (const_iterator itor = other.begin(); itor != other.end(); ++itor) {
        string &key = itor.d_ptr->key;

        MDB_val mdbKey;
        mdbKey.mv_size = key.size();
        mdbKey.mv_data = const_cast<char *>(key.data());

        int rt = mdb_cursor_put(cursor, &mdbKey, &itor.d_ptr->mdbValue, 0);
        if (rt) {
#if QTLMDB_DEBUG
            ngWarning() << "can not put lmdb value:" << mdb_strerror(rt);
#endif
            mdb_cursor_close(cursor);
            return -1;
        }
        ++count;
    }
    mdb_cursor_close(cursor);
    return count;
}

void Database::clear()
{
    if (isNull() || d_ptr->readOnly) {
        return;
    }

    int rt = mdb_drop(d_ptr->txn, d_ptr->dbi, 0);
    if (rt < 0) {
#if QTLMDB_DEBUG
        ngWarning() << "can not clear lmdb database:" << mdb_strerror(rt);
#endif
    }
}

vector<string> Database::keys() const
{
    if (isNull()) {
        return vector<string>();
    }

    MDB_cursor *cursor = d_ptr->makeCursor();
    if (!cursor) {
        return vector<string>();
    }

    MDB_val key, data;
    memset(&key, 0, sizeof(MDB_val));
    memset(&data, 0, sizeof(MDB_val));

    vector<string> ks;
    int rt;
    while ((rt = mdb_cursor_get(cursor, &key, &data, MDB_NEXT)) == 0) {
        ks.push_back(string(static_cast<const char *>(key.mv_data), key.mv_size));
    }

    mdb_cursor_close(cursor);

    if (rt && rt != MDB_NOTFOUND) {
#if QTLMDB_DEBUG
        ngWarning() << "can not iterate lmdb cursor:" << mdb_strerror(rt);
#endif
    }
    // if iterator was failed, we return the loaded keys anyway.
    // return vector<string>();
    return ks;
}

vector<string> Database::strKeys() const
{
    if (isNull()) {
        return vector<string>();
    }
    vector<string> ks = keys();
    vector<string> sl;
    sl.reserve(ks.size());
    for (const string &key : ks) {
        sl.push_back(key);
    }
    return sl;
}

int Database::remove(const string &key)
{
    if (isNull() || d_ptr->readOnly) {
        return -1;
    }

    MDB_val mdbKey, mdbData;
    string newKey(key);
    MDB_cursor *cursor = d_ptr->setCursor(newKey, mdbKey, mdbData, MDB_SET);
    if (!cursor) {
        return -1;
    }

    int rt = mdb_cursor_del(cursor, 0);
    mdb_cursor_close(cursor);
    if (rt) {
#if QTLMDB_DEBUG
        ngWarning() << "can not delete lmdb cursor:" << mdb_strerror(rt);
#endif
        return -1;
    }
    return 1;
}

Database::iterator Database::erase(Database::iterator &itor)
{
    if (itor.isEnd()) {
        return LmdbIterator(nullptr);
    }

    MDB_cursor *cursor = itor.d_ptr->cursor;
    itor.d_ptr->cursor = nullptr;
    const string& key = itor.d_ptr->key;

    int rt = mdb_cursor_del(cursor, 0);
    if (rt) {
#if QTLMDB_DEBUG
        ngWarning() << "can not delete lmdb cursor:" << mdb_strerror(rt) << key;
#endif
        mdb_cursor_close(cursor);
        return LmdbIterator(nullptr);
    }

    MDB_val mdbKey, mdbData;
    memset(&mdbKey, 0, sizeof(mdbKey));
    memset(&mdbData, 0, sizeof(mdbData));
    rt = mdb_cursor_get(cursor, &mdbKey, &mdbData, MDB_NEXT);

    if (rt) {
        // we delete the last item. there is no next item any more.
        mdb_cursor_close(cursor);
        return LmdbIterator(nullptr);
    }

    string newKey(static_cast<const char *>(mdbKey.mv_data), mdbKey.mv_size);
    LmdbIteratorPrivate *d = new LmdbIteratorPrivate(newKey, cursor, mdbData);
    return LmdbIterator(d);
}

string Database::take(const string &key)
{
    if (isNull() || d_ptr->readOnly) {
        return string();
    }

    MDB_val mdbKey, mdbData;
    string newKey(key);
    MDB_cursor *cursor = d_ptr->setCursor(newKey, mdbKey, mdbData, MDB_SET);
    if (!cursor) {
        return string();
    }

    const string &value = string(static_cast<const char *>(mdbData.mv_data), mdbData.mv_size);

    int rt = mdb_cursor_del(cursor, 0);
    mdb_cursor_close(cursor);
    if (rt) {
#if QTLMDB_DEBUG
        ngWarning() << "can not delete lmdb cursor:" << mdb_strerror(rt);
#endif
        return string();
    }
    return value;
}

bool Database::contains(const string &key) const
{
    if (isNull()) {
        return false;
    }
    return find(key) != end();
}

bool Database::isNull() const
{
    return d_ptr == nullptr;
}

bool Database::isEmpty() const
{
    return d_ptr == nullptr || size() <= 0;
}

int64_t Database::size() const
{
    if (isNull()) {
        return 0;
    }
    MDB_stat stat;
    int rt = mdb_stat(d_ptr->txn, d_ptr->dbi, &stat);
    if (rt) {
#if QTLMDB_DEBUG
        ngWarning() << "can not count lmdb:" << mdb_strerror(rt);
#endif
        return -1;
    }
    return stat.ms_entries;
}

Database::iterator Database::begin()
{
    if (isNull() || d_ptr->readOnly) {
        return LmdbIterator(nullptr);
    }

    MDB_cursor *cursor = d_ptr->makeCursor();
    if (!cursor) {
        return LmdbIterator(nullptr);
    }

    MDB_val mdbData;
    memset(&mdbData, 0, sizeof(MDB_val));
    LmdbIteratorPrivate *d = new LmdbIteratorPrivate(string(), cursor, mdbData);
    d->load(MDB_FIRST);
    return d;
}

Database::const_iterator Database::constBegin() const
{
    if (isNull()) {
        return ConstLmdbIterator(nullptr);
    }

    MDB_cursor *cursor = d_ptr->makeCursor();
    if (!cursor) {
        return ConstLmdbIterator(nullptr);
    }

    MDB_val mdbData;
    memset(&mdbData, 0, sizeof(MDB_val));
    LmdbIteratorPrivate *d = new LmdbIteratorPrivate(string(), cursor, mdbData);
    d->load(MDB_FIRST);
    return d;
}

Database::iterator Database::end()
{
    if (isNull() || d_ptr->readOnly) {
        return LmdbIterator(nullptr);
    }

    MDB_cursor *cursor = d_ptr->makeCursor();
    if (!cursor) {
        return LmdbIterator(nullptr);
    }

    MDB_val mdbData;
    memset(&mdbData, 0, sizeof(MDB_val));
    LmdbIteratorPrivate *d = new LmdbIteratorPrivate(string(), cursor, mdbData);
    return LmdbIterator(d);
}

Database::const_iterator Database::constEnd() const
{
    if (isNull()) {
        return ConstLmdbIterator(nullptr);
    }

    MDB_cursor *cursor = d_ptr->makeCursor();
    if (!cursor) {
        return ConstLmdbIterator(nullptr);
    }

    MDB_val mdbData;
    memset(&mdbData, 0, sizeof(MDB_val));
    LmdbIteratorPrivate *d = new LmdbIteratorPrivate(string(), cursor, mdbData);
    return ConstLmdbIterator(d);
}

Database::iterator Database::find(const string &key)
{
    if (isNull() || d_ptr->readOnly) {
        return LmdbIterator(nullptr);
    }

    MDB_val mdbKey, mdbData;
    string newKey(key);
    MDB_cursor *cursor = d_ptr->setCursor(newKey, mdbKey, mdbData, MDB_SET);
    if (!cursor) {
        return LmdbIterator(nullptr);
    }

    LmdbIteratorPrivate *d = new LmdbIteratorPrivate(newKey, cursor, mdbData);
    return LmdbIterator(d);
}

Database::const_iterator Database::constFind(const string &key) const
{
    if (isNull()) {
        return ConstLmdbIterator(nullptr);
    }

    MDB_val mdbKey, mdbData;
    string newKey(key);
    MDB_cursor *cursor = d_ptr->setCursor(newKey, mdbKey, mdbData, MDB_SET);
    if (!cursor) {
        return ConstLmdbIterator(nullptr);
    }

    LmdbIteratorPrivate *d = new LmdbIteratorPrivate(newKey, cursor, mdbData);
    return ConstLmdbIterator(d);
}

Database::const_iterator Database::lowerBound(const string &key) const
{
    if (isNull()) {
        return ConstLmdbIterator(nullptr);
    }

    MDB_val mdbKey, mdbData;
    string newKey(key);
    MDB_cursor *cursor = d_ptr->setCursor(newKey, mdbKey, mdbData, MDB_SET_RANGE);
    if (!cursor) {
        return ConstLmdbIterator(nullptr);
    }

    newKey = string(static_cast<const char *>(mdbKey.mv_data), mdbKey.mv_size);
    LmdbIteratorPrivate *d = new LmdbIteratorPrivate(newKey, cursor, mdbData);
    return ConstLmdbIterator(d);
}

Database::iterator Database::lowerBound(const string &key)
{
    if (isNull() || d_ptr->readOnly) {
        return LmdbIterator(nullptr);
    }

    MDB_val mdbKey, mdbData;
    string newKey(key);
    MDB_cursor *cursor = d_ptr->setCursor(newKey, mdbKey, mdbData, MDB_SET_RANGE);
    if (!cursor) {
        return LmdbIterator(nullptr);
    }

    newKey = string(static_cast<const char *>(mdbKey.mv_data), mdbKey.mv_size);
    LmdbIteratorPrivate *d = new LmdbIteratorPrivate(newKey, cursor, mdbData);
    return LmdbIterator(d);
}

Database::const_iterator Database::upperBound(const string &key) const
{
    Database::const_iterator itor = lowerBound(key);
    if (itor.key() == key) {
        ++itor;
    }
    return itor;
}

Database::iterator Database::upperBound(const string &key)
{
    Database::iterator itor = lowerBound(key);
    if (itor.key() == key) {
        ++itor;
    }
    return itor;
}

Database &TransactionPrivate::open(const string &name)
{
    static Database empty(nullptr);

    if (finished) {
        if (readOnly) {
            mdb_txn_renew(txn);
            finished = false;
        } else {
            return empty;
        }
    }

    map<string, shared_ptr<Database>>::const_iterator itor = dbs.find(name);
    if (itor != dbs.end()) {
        return *itor->second;
    }

    MDB_dbi dbi;
    unsigned int flags = readOnly ? 0 : MDB_CREATE;
    int rt = mdb_dbi_open(txn, name.c_str(), flags, &dbi);
    if (rt < 0) {
        return empty;
    }
    unique_ptr<DatabasePrivate> d(new DatabasePrivate(txn, dbi, readOnly));
    shared_ptr<Database> db(new Database(d.release()));
    dbs[name] = db;
    return *db;
}

Transaction::~Transaction()
{
    if (!d_ptr->finished) {
#if QTLMDB_DEBUG
        if (!d_ptr->readOnly) {
            ngWarning() << "lmdb transaction is not finished.";
        }
#endif
        commit();
    }
    delete d_ptr;
}

const Database &Transaction::db(const string &name) const
{
    return d_ptr->open(name);
}

Database &Transaction::db(const string &name)
{
    return d_ptr->open(name);
}

static bool isWriteMap(MDB_env *env)
{
    unsigned int flags;
    int rt = mdb_env_get_flags(env, &flags);
    assert(rt == MDB_SUCCESS);
    if (flags & MDB_WRITEMAP) {
#if QTLMDB_DEBUG
        ngWarning() << "sub transaction is not supported with MDB_WRITEMAP features.";
#endif
        return true;
    }
    return false;
}

shared_ptr<Transaction> Transaction::sub()
{
    if (isWriteMap(d_ptr->env)) {
        return shared_ptr<Transaction>();
    }
    MDB_txn *child_txn;
    unsigned int flags = 0;
    int rt = mdb_txn_begin(d_ptr->env, d_ptr->txn, flags, &child_txn);
    if (rt) {
#if QTLMDB_DEBUG
        ngWarning() << "can not begin sub lmdb transaction:" << mdb_strerror(rt);
#endif
        return shared_ptr<Transaction>();
    }
    TransactionPrivate *d = new TransactionPrivate(d_ptr->env, child_txn, false);
    return shared_ptr<Transaction>(new Transaction(d));
}

shared_ptr<const Transaction> Transaction::sub() const
{
    if (isWriteMap(d_ptr->env)) {
        return shared_ptr<const Transaction>();
    }
    MDB_txn *child_txn;
    unsigned int flags = MDB_RDONLY;
    int rt = mdb_txn_begin(d_ptr->env, d_ptr->txn, flags, &child_txn);
    if (rt) {
#if QTLMDB_DEBUG
        ngWarning() << "can not begin sub lmdb transaction:" << mdb_strerror(rt);
#endif
        return shared_ptr<const Transaction>();
    }
    TransactionPrivate *d = new TransactionPrivate(d_ptr->env, child_txn, true);
    return shared_ptr<Transaction>(new Transaction(d));
}

static TransactionPrivate *makePrivateToWrite(MDB_env * const env)
{
    MDB_txn *txn;
    unsigned int flags = 0;
    int rt = mdb_txn_begin(env, NULL, flags, &txn);
    if (rt) {
#if QTLMDB_DEBUG
        ngWarning() << "can not begin lmdb transaction:" << mdb_strerror(rt);
#endif
        return nullptr;
    }
    TransactionPrivate *d = new TransactionPrivate(env, txn, false);
    return d;
}

shared_ptr<Transaction> Transaction::fork()
{
    return shared_ptr<Transaction>(new Transaction(makePrivateToWrite(d_ptr->env)));
}

static TransactionPrivate *makePrivateToRead(MDB_env * const env)
{
    MDB_txn *txn;
    unsigned int flags = MDB_RDONLY;
    int rt = mdb_txn_begin(env, NULL, flags, &txn);
    if (rt) {
#if QTLMDB_DEBUG
        ngWarning() << "can not begin lmdb transaction:" << mdb_strerror(rt);
#endif
        return nullptr;
    }
    TransactionPrivate *d = new TransactionPrivate(env, txn, true);
    return d;
}

shared_ptr<const Transaction> Transaction::fork() const
{
    return shared_ptr<const Transaction>(new Transaction(makePrivateToRead(d_ptr->env)));
}

bool Transaction::commit()
{
    int rt = mdb_txn_commit(d_ptr->txn);
    d_ptr->dbs.clear();
    d_ptr->finished = true;
    if (rt) {
#if QTLMDB_DEBUG
        ngWarning() << "can not commit lmdb transaction:" << mdb_strerror(rt);
#endif
        return false;
    }
    return true;
}

void Transaction::abort()
{
    mdb_txn_abort(d_ptr->txn);
    d_ptr->dbs.clear();
    d_ptr->finished = true;
}

Lmdb::~Lmdb()
{
    mdb_env_close(d_ptr->env);
    delete d_ptr;
}

shared_ptr<const Transaction> Lmdb::toRead()
{
    TransactionPrivate *d = makePrivateToRead(d_ptr->env);
    if (!d) {
        return shared_ptr<const Transaction>();
    }
    return shared_ptr<const Transaction>(new Transaction(d));
}

shared_ptr<Transaction> Lmdb::toWrite()
{
    TransactionPrivate *d = makePrivateToWrite(d_ptr->env);
    if (!d) {
        return shared_ptr<Transaction>();
    }
    return shared_ptr<Transaction>(new Transaction(d));
}

string Lmdb::version() const
{
    char *s = mdb_version(NULL, NULL, NULL);
    return s;
}

void Lmdb::sync(bool force)
{
    int rt = mdb_env_sync(d_ptr->env, force);
    if (rt) {
#if QTLMDB_DEBUG
        ngWarning() << "can not sync lmdb env:" << mdb_strerror(rt);
#endif
    }
}

bool Lmdb::backupTo(const string &dirPath)
{
    unsigned int flags = 0;
    int rt = mdb_env_copy2(d_ptr->env, dirPath.c_str(), flags);
    if (rt) {
#if QTLMDB_DEBUG
        ngWarning() << "can not backup lmdb env:" << mdb_strerror(rt);
#endif
        return false;
    }
    return true;
}

LmdbBuilder::LmdbBuilder(const string &dirPath)
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

shared_ptr<Lmdb> LmdbBuilder::create()
{
    assert(!m_dirPath.empty());
    unique_ptr<LmdbPrivate> d(new LmdbPrivate());
    int rt = mdb_env_create(&d->env);
    if (rt) {
#if QTLMDB_DEBUG
        ngWarning() << "can not create lmdb env:" << mdb_strerror(rt);
#endif
        return shared_ptr<Lmdb>();
    }
    mdb_env_set_mapsize(d->env, m_maxMapSize);
    mdb_env_set_maxdbs(d->env, m_maxDbs);
    mdb_env_set_maxreaders(d->env, m_maxReaders);

    unsigned int flags = MDB_NOTLS;
    if (m_noSubDir) {
        flags |= MDB_NOSUBDIR;
    }
    if (m_noSync) {
        flags |= MDB_NOSYNC;
    }
    if (m_writeMap) {
        flags |= MDB_WRITEMAP;
        // Note that sync=False, writemap=True leaves the system with no hint for when to write transactions to disk,
        // unless sync() is called. map_async=True, writemap=True may be preferable.
        if (m_noSync) {
            flags |= MDB_MAPASYNC;
        }
    }
    mdb_mode_t mode = 0660;
    rt = mdb_env_open(d->env, m_dirPath.c_str(), flags, mode);
    if (rt) {
#if QTLMDB_DEBUG
        ngWarning() << "can not open lmdb env:" << mdb_strerror(rt) << m_dirPath << flags << mode;
#endif
        mdb_env_close(d->env);
        return shared_ptr<Lmdb>();
    }
    return shared_ptr<Lmdb>(new Lmdb(d.release()));
}

}  // namespace qtng
