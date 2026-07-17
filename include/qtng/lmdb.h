#ifndef QTNG_LMDB_H
#define QTNG_LMDB_H

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "qtng/utils/platform.h"

namespace qtng {

class DatabasePrivate;
class Database;
class TransactionPrivate;
class Transaction;
class LmdbIteratorPrivate;
class LmdbIterator;
class ConstLmdbIterator;

class ConstLmdbIterator
{
public:
    typedef std::bidirectional_iterator_tag iterator_category;
    typedef std::ptrdiff_t difference_type;
    typedef std::string value_type;
    typedef const std::string *pointer;
    typedef const std::string &reference;
public:
    ~ConstLmdbIterator();
    inline ConstLmdbIterator(LmdbIterator &&other);
    inline ConstLmdbIterator(ConstLmdbIterator &&other)
        : d_ptr(other.d_ptr)
    {
        other.d_ptr = nullptr;
    }
public:
    std::string key() const;
    std::string value() const;
    bool isEnd() const;
public:
    const char *data() const;
    size_t size() const;
public:
    inline const std::string strKey() const { return std::string(key()); }
public:
    bool operator==(const ConstLmdbIterator &other) const;
    inline bool operator!=(const ConstLmdbIterator &other) const { return !(*this == other); }
    bool operator!() const { return isEnd(); }
    explicit operator bool() const { return !isEnd(); }
    ConstLmdbIterator &operator++();
    ConstLmdbIterator &operator--();
private:
    inline ConstLmdbIterator(LmdbIteratorPrivate *d)
        : d_ptr(d)
    {
    }
    LmdbIteratorPrivate *d_ptr;
    friend class Database;
};

class LmdbIterator
{
public:
    typedef std::bidirectional_iterator_tag iterator_category;
    typedef std::ptrdiff_t difference_type;
    typedef std::string value_type;
    typedef const std::string *pointer;
    typedef const std::string &reference;
public:
    ~LmdbIterator();
    inline LmdbIterator(LmdbIterator &&other);
public:
    std::string key() const;
    std::string value() const;
    bool isEnd() const;
public:
    char *data() const;  // use this at your risk! you must known what you're doing.
    size_t size() const;
public:
    inline std::string strKey() const { return std::string(key()); }
public:
    inline bool operator==(const ConstLmdbIterator &) const { return false; }
    inline bool operator!=(const ConstLmdbIterator &) const { return true; }
    bool operator==(const LmdbIterator &other) const;
    inline bool operator!=(const LmdbIterator &other) const { return !(*this == other); }
    bool operator!() const { return isEnd(); }
    LmdbIterator &operator++();
    LmdbIterator &operator--();
private:
    inline LmdbIterator(LmdbIteratorPrivate *d)
        : d_ptr(d)
    {
    }
    LmdbIteratorPrivate *d_ptr;
    friend class Database;
    friend class ConstLmdbIterator;
};

ConstLmdbIterator::ConstLmdbIterator(LmdbIterator &&other)
    : d_ptr(other.d_ptr)
{
    other.d_ptr = nullptr;
}

LmdbIterator::LmdbIterator(LmdbIterator &&other)
    : d_ptr(other.d_ptr)
{
    other.d_ptr = nullptr;
}

class Database
{
public:
    typedef LmdbIterator iterator;
    typedef ConstLmdbIterator const_iterator;
    typedef std::string key_type;
    typedef std::string mapped_type;
    typedef std::ptrdiff_t difference_type;
    typedef std::int64_t size_type;
    ~Database();
public:
    inline std::string value(const std::string &key) const { return constFind(key).value(); }
    iterator insert(const std::string &key, const std::string &value);
    iterator reserve(const std::string &key, size_t size); // insert value using itor.data() and itor.size()
    std::int64_t insert(const Database &other);
    void clear();
    std::vector<std::string> keys() const;
    std::vector<std::string> strKeys() const;
    int remove(const std::string &key);
    std::string take(const std::string &key);
    bool contains(const std::string &key) const;
    bool isNull() const;
    bool isEmpty() const;
    std::int64_t size() const;
public:
    iterator begin();
    const_iterator constBegin() const;
    iterator end();
    const_iterator constEnd() const;
    iterator erase(iterator &itor);
    iterator find(const std::string &key);
    const_iterator constFind(const std::string &key) const;
    const_iterator lowerBound(const std::string &key) const;
    iterator lowerBound(const std::string &key);
    const_iterator upperBound(const std::string &key) const;
    iterator upperBound(const std::string &key);
    inline std::string firstKey() const
    {
        assert(!isEmpty());
        return constBegin().key();
    }
    inline std::string firstValue() const
    {
        assert(!isEmpty());
        return constBegin().value();
    }
    inline std::string lastKey() const
    {
        assert(!isEmpty());
        const_iterator it = constEnd();
        --it;
        return it.key();
    }
    inline std::string lastValue() const
    {
        assert(!isEmpty());
        const_iterator it = constEnd();
        --it;
        return it.value();
    }
public:
    inline std::int64_t count() const { return size(); }
    inline const_iterator begin() const { return constBegin(); }
    inline const_iterator cbegin() const { return constBegin(); }
    inline const_iterator end() const { return constEnd(); }
    inline const_iterator cend() const { return constEnd(); }
    inline const_iterator find(const std::string &key) const { return constFind(key); }
    inline std::string operator[](const std::string &key) const { return value(key); }
private:
    Database(DatabasePrivate *d)
        : d_ptr(d)
    {
    }
    friend class DatabasePrivate;
    friend class LmdbIteratorPrivate;
    friend class TransactionPrivate;
private:
    DatabasePrivate * const d_ptr;
    NG_DECLARE_PRIVATE(Database)
private:
    NG_DISABLE_COPY(Database)
    Database(Database &&) = delete;
    Database &operator=(Database &&) = delete;
};

class TransactionPrivate;
class Transaction
{
public:
    ~Transaction();
public:
    const Database &db(const std::string &name) const;
    Database &db(const std::string &name);
    std::shared_ptr<Transaction> sub();
    std::shared_ptr<const Transaction> sub() const;
    std::shared_ptr<Transaction> fork();
    std::shared_ptr<const Transaction> fork() const;
    bool commit();
    void abort();
private:
    Transaction(TransactionPrivate *d)
        : d_ptr(d)
    {
    }
    TransactionPrivate * const d_ptr;
    friend class TransactionPrivate;
    friend class Lmdb;
private:
    NG_DISABLE_COPY(Transaction)
    Transaction(Transaction &&) = delete;
    Transaction &operator=(Transaction &&) = delete;
};

class LmdbPrivate;
class LmdbBuilder;
class Lmdb
{
public:
    typedef LmdbBuilder Builder;
    ~Lmdb();
public:
    std::shared_ptr<const Transaction> toRead();
    std::shared_ptr<Transaction> toWrite();
    std::string version() const;
    void sync(bool force = false);
    bool backupTo(const std::string &dirPath);
private:
    Lmdb(LmdbPrivate *d)
        : d_ptr(d)
    {
    }
    LmdbPrivate * const d_ptr;
    NG_DECLARE_PRIVATE_D(d_ptr, Lmdb)
    friend class LmdbBuilder;
private:
    NG_DISABLE_COPY(Lmdb)
    Lmdb(Lmdb &&) = delete;
    Lmdb &operator=(Lmdb &&) = delete;
};

class LmdbBuilder
{
public:
    LmdbBuilder(const std::string &dirPath);
    LmdbBuilder &maxMapSize(size_t size);
    LmdbBuilder &maxReaders(int readers);
    LmdbBuilder &maxDbs(int maxDbs);
    LmdbBuilder &noSync(bool noSync);
    LmdbBuilder &noSubDir(bool noSubDir);
    LmdbBuilder &writeMap(bool writable);
    std::shared_ptr<Lmdb> create();
private:
    size_t m_maxMapSize = 1024 * 1024 * 16;
    int m_maxReaders = 256;
    int m_maxDbs = 1024;
    std::string m_dirPath;
    bool m_noSync = false;
    bool m_writeMap = false;
    bool m_noSubDir = true;
private:
    NG_DISABLE_COPY(LmdbBuilder)
    LmdbBuilder(LmdbBuilder &&) = delete;
    LmdbBuilder &operator=(LmdbBuilder &&) = delete;
};

}  // namespace qtng

#endif  // QTNG_LMDB_H
