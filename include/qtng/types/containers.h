#ifndef QTNG_TYPES_CONTAINERS_H
#define QTNG_TYPES_CONTAINERS_H

// qtng::Map / Vector / List / Set / Deque / UnorderedMap / UnorderedSet
//
// Implicitly shared (copy-on-write) wrappers around common STL containers: the underlying
// data structures are not rewritten; a copy is just a reference-count bump, and a write
// detaches first.
//
// ---------------------------------------------------------------------------
// Contract: please read this to the end before changing any line here
// ---------------------------------------------------------------------------
//
// 1. **Every non-const member function calls detach() first**, including the ones that
//    look read-only (find / lower_bound / begin / end / at / operator[] / front / back /
//    data). Reason: those non-const overloads return **mutable iterators or references**,
//    and if they pointed into a shared buffer the caller could bypass detach and modify
//    another handle's data, breaking the sharing semantics on the spot. The detach test
//    must therefore be "is this a non-const call", not "does the name read like a read".
//
// 2. **detach costs a full copy**. So
//        m.find(k)             // non-const handle: copies the whole map, then searches!
//        constM.find(k)        // const reference: O(log n), no copy
//    differ by an order of magnitude while shared. On hot paths, bind a const reference
//    before reading, or use cbegin/cend/cfind. This is inherent to COW (Qt's QMap/QVector
//    behave the same way); it is not an implementation defect.
//
// 3. **Iterators/references are invalidated by detach**: they point at the old shared
//    block. Holding a mutable iterator across a detach is a misuse.
//
// 4. **Both non-const begin() and end() detach**, so the two are guaranteed to land on the
//    same buffer; otherwise comparing iterators is undefined behavior. After the first
//    detach the reference count is back to 1, so the second call is a no-op and costs
//    nothing extra.
//
// 5. **Do not count on COW for large containers**: with tens of thousands of elements, any
//    accidental detach is O(n) with n allocations. Such cases should use the "explicit
//    private copy + empty flag" idiom (see BatchInsertPrivate::objectEdits in hugeload)
//    instead of asking a reference count "do I have a private copy?".
//
// 6. **The API does not try to cover every STL member**, only the common intersection; for
//    the rest, get() returns a reference / const reference to the underlying container.
//
// 7. **Qt-named convenience methods** (isEmpty / removeAll / removeOne / removeAt /
//    indexOf / lastIndexOf / contains / count / value / take / keys / values / unite /
//    intersect / subtract / removeIf / fill / ...) follow exactly the same rule as the
//    rest: a const overload reads through constRef() and never detaches, a mutating
//    overload goes through mutableRef(). "Not found" is npos (size_type(-1)), not Qt's
//    int -1, to stay consistent with String/ByteArray in this library. Bounds are always
//    defined (a no-op or a sentinel) rather than UB, again like String/ByteArray -- a
//    value type has no assert channel and aborting a process over a bad index is worse.
//    `isEmpty()` is only the Qt spelling of the existing `empty()`, defined as
//    `return empty();` so the two cannot drift apart.

#include <algorithm>
#include <deque>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "qtng/types/cow.h"

namespace qtng {

// The handles do not need hand-written copy/assignment: the derived classes have no data
// members of their own, so the implicitly generated copy constructor and copy assignment
// call the base class, which is exactly the behavior we want (a reference-count bump).
// Writing them out by hand would only add forwarding code that can be wrong. Here only the
// default constructor is needed -- it is suppressed by the declaration of the
// initializer_list constructor.
#define QTNG_COW_PLUMBING(Class) \
public:                          \
    Class() = default;

// Comparison forwards to the underlying container, so reader handles compare without
// detaching (get() is the read-only view). Needed because these handles end up inside
// std::variant (see hugeload's Value), and std::variant's relational operators require
// every alternative to support the same operators.
//
// Ordered containers get ordering; unordered ones deliberately get equality only --
// std::unordered_map/std::unordered_set have no operator< to forward to, and inventing
// an order for them would be a lie.
#define QTNG_COW_EQUALITY(Class)                                                            \
    bool operator==(const Class &other) const { return get() == other.get(); }              \
    bool operator!=(const Class &other) const { return get() != other.get(); }

#define QTNG_COW_ORDERING(Class)                                                            \
    QTNG_COW_EQUALITY(Class)                                                                \
    bool operator<(const Class &other) const { return get() < other.get(); }

namespace detail {

// Shared read-only search helpers for the sequence containers (Vector / Deque / List) so
// that indexOf / lastIndexOf / count / contains have one implementation instead of three
// copies. They read through a const reference; the caller passes a container obtained from
// constRef(), so nothing here can detach.
//
// "Not found" is size_type(-1) (the container's npos), matching the String/ByteArray
// convention in this library rather than Qt's int -1.
template<typename Container>
inline typename Container::size_type seqIndexOf(const Container &c,
                                                typename Container::size_type from,
                                                const typename Container::value_type &value)
{
    typedef typename Container::size_type SizeType;
    typedef typename Container::const_iterator ConstIterator;
    typedef typename std::iterator_traits<ConstIterator>::difference_type DifferenceType;
    if (from > c.size()) {
        return static_cast<SizeType>(-1);
    }
    ConstIterator it = c.begin();
    std::advance(it, static_cast<DifferenceType>(from));
    for (SizeType i = from; it != c.end(); ++it, ++i) {
        if (*it == value) {
            return i;
        }
    }
    return static_cast<SizeType>(-1);
}

template<typename Container>
inline typename Container::size_type seqLastIndexOf(const Container &c,
                                                    typename Container::size_type from,
                                                    const typename Container::value_type &value)
{
    typedef typename Container::size_type SizeType;
    typedef typename Container::const_iterator ConstIterator;
    typedef typename std::iterator_traits<ConstIterator>::difference_type DifferenceType;
    const SizeType n = c.size();
    if (n == 0) {
        return static_cast<SizeType>(-1);
    }
    // from == npos (>= n) means "start from the end"; any value past the end is clamped the
    // same way, so the caller can pass npos or size() without special-casing.
    const SizeType start = (from >= n) ? n - 1 : from;
    ConstIterator it = c.begin();
    std::advance(it, static_cast<DifferenceType>(start));
    for (SizeType i = start;; --i, --it) {
        if (*it == value) {
            return i;
        }
        if (i == 0) {
            break;
        }
    }
    return static_cast<SizeType>(-1);
}

template<typename Container>
inline typename Container::size_type seqCount(const Container &c, const typename Container::value_type &value)
{
    return static_cast<typename Container::size_type>(std::count(c.begin(), c.end(), value));
}

template<typename Container>
inline bool seqContains(const Container &c, const typename Container::value_type &value)
{
    return std::find(c.begin(), c.end(), value) != c.end();
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Vector: std::vector<T, Alloc>
// ---------------------------------------------------------------------------
template<typename T, typename Alloc = std::allocator<T> >
class Vector : public CowContainerBase<std::vector<T, Alloc> >
{
    typedef CowContainerBase<std::vector<T, Alloc> > Base;

public:
    typedef std::vector<T, Alloc> StlType;
    typedef typename StlType::iterator Iterator;
    typedef typename StlType::const_iterator ConstIterator;
    typedef typename StlType::reverse_iterator ReverseIterator;
    typedef typename StlType::const_reverse_iterator ConstReverseIterator;
    typedef typename StlType::value_type ValueType;
    typedef typename StlType::size_type SizeType;

    QTNG_COW_PLUMBING(Vector)

    QTNG_COW_ORDERING(Vector)

    // Qt's indexOf()/lastIndexOf() "not found" sentinel. An enumerator with a fixed
    // underlying type, exactly like String::npos / ByteArray::npos: an enumerator is a
    // prvalue and can never be odr-used, so no out-of-class definition is needed in any
    // standard -- unlike a static constexpr member, whose out-of-line definition is
    // deprecated in C++17. size_type(-1) is out of reach of any element index.
    enum : SizeType
    {
        npos = static_cast<SizeType>(-1),
    };

    Vector(std::initializer_list<T> l)
        : Base(new CowData<StlType>(StlType(l)))
    {
    }

public:
    // -- read-only
    SizeType size() const { return Base::constRef().size(); }
    bool empty() const { return Base::constRef().empty(); }
    bool isEmpty() const { return empty(); }
    SizeType capacity() const { return Base::constRef().capacity(); }
    const T &operator[](SizeType i) const { return Base::constRef()[i]; }
    const T &at(SizeType i) const { return Base::constRef().at(i); }
    const T &front() const { return Base::constRef().front(); }
    const T &back() const { return Base::constRef().back(); }
    const T *data() const { return Base::constRef().data(); }
    ConstIterator begin() const { return Base::constRef().begin(); }
    ConstIterator end() const { return Base::constRef().end(); }
    ConstIterator cbegin() const { return Base::constRef().begin(); }
    ConstIterator cend() const { return Base::constRef().end(); }
    ConstReverseIterator rbegin() const { return Base::constRef().rbegin(); }
    ConstReverseIterator rend() const { return Base::constRef().rend(); }
    ConstReverseIterator crbegin() const { return Base::constRef().rbegin(); }
    ConstReverseIterator crend() const { return Base::constRef().rend(); }
    const StlType &get() const { return Base::constRef(); }

    // Qt-named read-only helpers. All of them go through constRef(), so calling them on a
    // shared handle must not detach -- see the test that locks this in.
    const T &first() const { return Base::constRef().front(); }
    const T &last() const { return Base::constRef().back(); }
    bool contains(const T &v) const { return detail::seqContains(Base::constRef(), v); }
    SizeType count(const T &v) const { return detail::seqCount(Base::constRef(), v); }
    SizeType indexOf(const T &v, SizeType from = 0) const { return detail::seqIndexOf(Base::constRef(), from, v); }
    SizeType lastIndexOf(const T &v, SizeType from = npos) const
    {
        return detail::seqLastIndexOf(Base::constRef(), from, v);
    }
    bool startsWith(const T &v) const { return !Base::constRef().empty() && Base::constRef().front() == v; }
    bool endsWith(const T &v) const { return !Base::constRef().empty() && Base::constRef().back() == v; }
    // Out of range yields defaultValue instead of reading past the end (QList::value()'s
    // contract, but without asserting).
    T value(SizeType i, const T &defaultValue = T()) const
    {
        return i < Base::constRef().size() ? Base::constRef()[i] : defaultValue;
    }

    // -- read/write
    T &operator[](SizeType i) { return Base::mutableRef()[i]; }
    T &at(SizeType i) { return Base::mutableRef().at(i); }
    T &front() { return Base::mutableRef().front(); }
    T &back() { return Base::mutableRef().back(); }
    T &first() { return Base::mutableRef().front(); }
    T &last() { return Base::mutableRef().back(); }
    T *data() { return Base::mutableRef().data(); }
    Iterator begin() { return Base::mutableRef().begin(); }
    Iterator end() { return Base::mutableRef().end(); }
    ReverseIterator rbegin() { return Base::mutableRef().rbegin(); }
    ReverseIterator rend() { return Base::mutableRef().rend(); }
    void push_back(const T &v) { Base::mutableRef().push_back(v); }
    void pop_back() { Base::mutableRef().pop_back(); }
    void append(const T &v) { Base::mutableRef().push_back(v); }
    void resize(SizeType n) { Base::mutableRef().resize(n); }
    void resize(SizeType n, const T &v) { Base::mutableRef().resize(n, v); }
    void reserve(SizeType n) { Base::mutableRef().reserve(n); }
    void clear() { Base::mutableRef().clear(); }
    Iterator insert(Iterator pos, const T &v) { return Base::mutableRef().insert(pos, v); }
    Iterator erase(Iterator pos) { return Base::mutableRef().erase(pos); }
    Iterator erase(Iterator first, Iterator last) { return Base::mutableRef().erase(first, last); }
    StlType &get() { return Base::mutableRef(); }

    // Qt-named mutating helpers.
    void prepend(const T &v)
    {
        StlType &c = Base::mutableRef();
        c.insert(c.begin(), v);
    }
    // Out-of-range index means "append", matching QList::insert(); a no-op would silently
    // drop the value, which is a worse failure mode than appending it.
    void insert(SizeType i, const T &v)
    {
        StlType &c = Base::mutableRef();
        if (i >= c.size()) {
            c.push_back(v);
        } else {
            c.insert(c.begin() + i, v);
        }
    }
    void replace(SizeType i, const T &v)
    {
        StlType &c = Base::mutableRef();
        if (i < c.size()) {
            c[i] = v;
        }
    }
    void removeAt(SizeType i)
    {
        StlType &c = Base::mutableRef();
        if (i < c.size()) {
            c.erase(c.begin() + i);
        }
    }
    void removeFirst()
    {
        StlType &c = Base::mutableRef();
        if (!c.empty()) {
            c.erase(c.begin());
        }
    }
    void removeLast()
    {
        StlType &c = Base::mutableRef();
        if (!c.empty()) {
            c.pop_back();
        }
    }
    // Empty container yields T(), so a bad call cannot read an element that is not there.
    T takeAt(SizeType i)
    {
        StlType &c = Base::mutableRef();
        if (i >= c.size()) {
            return T();
        }
        T v = c[i];
        c.erase(c.begin() + i);
        return v;
    }
    T takeFirst()
    {
        StlType &c = Base::mutableRef();
        if (c.empty()) {
            return T();
        }
        T v = c.front();
        c.erase(c.begin());
        return v;
    }
    T takeLast()
    {
        StlType &c = Base::mutableRef();
        if (c.empty()) {
            return T();
        }
        T v = c.back();
        c.pop_back();
        return v;
    }
    SizeType removeAll(const T &v)
    {
        StlType &c = Base::mutableRef();
        const SizeType before = c.size();
        c.erase(std::remove(c.begin(), c.end(), v), c.end());
        return before - c.size();
    }
    bool removeOne(const T &v)
    {
        StlType &c = Base::mutableRef();
        Iterator it = std::find(c.begin(), c.end(), v);
        if (it == c.end()) {
            return false;
        }
        c.erase(it);
        return true;
    }
    template<typename Predicate>
    SizeType removeIf(Predicate pred)
    {
        StlType &c = Base::mutableRef();
        const SizeType before = c.size();
        c.erase(std::remove_if(c.begin(), c.end(), pred), c.end());
        return before - c.size();
    }
    // fill(value) fills the whole container; fill(value, n) resizes it to n first, like
    // QVector::fill(value, size).
    void fill(const T &v, SizeType n = npos)
    {
        StlType &c = Base::mutableRef();
        c.assign(n == npos ? c.size() : n, v);
    }
    void swapItemsAt(SizeType i, SizeType j)
    {
        StlType &c = Base::mutableRef();
        if (i < c.size() && j < c.size()) {
            std::swap(c[i], c[j]);
        }
    }
    // Move the element at `from` to index `to`, shifting the rest; a no-op when either index
    // is out of range or they are equal.
    void move(SizeType from, SizeType to)
    {
        StlType &c = Base::mutableRef();
        if (from == to || from >= c.size() || to >= c.size()) {
            return;
        }
        if (from < to) {
            std::rotate(c.begin() + from, c.begin() + from + 1, c.begin() + to + 1);
        } else {
            std::rotate(c.begin() + to, c.begin() + from, c.begin() + from + 1);
        }
    }
    void squeeze() { Base::mutableRef().shrink_to_fit(); }
};

// ---------------------------------------------------------------------------
// Deque: std::deque<T, Alloc>
// ---------------------------------------------------------------------------
template<typename T, typename Alloc = std::allocator<T> >
class Deque : public CowContainerBase<std::deque<T, Alloc> >
{
    typedef CowContainerBase<std::deque<T, Alloc> > Base;

public:
    typedef std::deque<T, Alloc> StlType;
    typedef typename StlType::iterator Iterator;
    typedef typename StlType::const_iterator ConstIterator;
    typedef typename StlType::reverse_iterator ReverseIterator;
    typedef typename StlType::const_reverse_iterator ConstReverseIterator;
    typedef typename StlType::value_type ValueType;
    typedef typename StlType::size_type SizeType;

    QTNG_COW_PLUMBING(Deque)

    QTNG_COW_ORDERING(Deque)

    enum : SizeType
    {
        npos = static_cast<SizeType>(-1),
    };

    Deque(std::initializer_list<T> l)
        : Base(new CowData<StlType>(StlType(l)))
    {
    }

public:
    // -- read-only
    SizeType size() const { return Base::constRef().size(); }
    bool empty() const { return Base::constRef().empty(); }
    bool isEmpty() const { return empty(); }
    const T &operator[](SizeType i) const { return Base::constRef()[i]; }
    const T &at(SizeType i) const { return Base::constRef().at(i); }
    const T &front() const { return Base::constRef().front(); }
    const T &back() const { return Base::constRef().back(); }
    ConstIterator begin() const { return Base::constRef().begin(); }
    ConstIterator end() const { return Base::constRef().end(); }
    ConstIterator cbegin() const { return Base::constRef().begin(); }
    ConstIterator cend() const { return Base::constRef().end(); }
    ConstReverseIterator rbegin() const { return Base::constRef().rbegin(); }
    ConstReverseIterator rend() const { return Base::constRef().rend(); }
    ConstReverseIterator crbegin() const { return Base::constRef().rbegin(); }
    ConstReverseIterator crend() const { return Base::constRef().rend(); }
    const StlType &get() const { return Base::constRef(); }

    // Qt-named read-only helpers, same contract as Vector's.
    const T &first() const { return Base::constRef().front(); }
    const T &last() const { return Base::constRef().back(); }
    bool contains(const T &v) const { return detail::seqContains(Base::constRef(), v); }
    SizeType count(const T &v) const { return detail::seqCount(Base::constRef(), v); }
    SizeType indexOf(const T &v, SizeType from = 0) const { return detail::seqIndexOf(Base::constRef(), from, v); }
    SizeType lastIndexOf(const T &v, SizeType from = npos) const
    {
        return detail::seqLastIndexOf(Base::constRef(), from, v);
    }
    bool startsWith(const T &v) const { return !Base::constRef().empty() && Base::constRef().front() == v; }
    bool endsWith(const T &v) const { return !Base::constRef().empty() && Base::constRef().back() == v; }
    T value(SizeType i, const T &defaultValue = T()) const
    {
        return i < Base::constRef().size() ? Base::constRef()[i] : defaultValue;
    }

    // -- read/write
    T &operator[](SizeType i) { return Base::mutableRef()[i]; }
    T &at(SizeType i) { return Base::mutableRef().at(i); }
    T &front() { return Base::mutableRef().front(); }
    T &back() { return Base::mutableRef().back(); }
    T &first() { return Base::mutableRef().front(); }
    T &last() { return Base::mutableRef().back(); }
    Iterator begin() { return Base::mutableRef().begin(); }
    Iterator end() { return Base::mutableRef().end(); }
    ReverseIterator rbegin() { return Base::mutableRef().rbegin(); }
    ReverseIterator rend() { return Base::mutableRef().rend(); }
    void push_back(const T &v) { Base::mutableRef().push_back(v); }
    void push_front(const T &v) { Base::mutableRef().push_front(v); }
    void pop_back() { Base::mutableRef().pop_back(); }
    void pop_front() { Base::mutableRef().pop_front(); }
    void append(const T &v) { Base::mutableRef().push_back(v); }
    void prepend(const T &v) { Base::mutableRef().push_front(v); }
    void clear() { Base::mutableRef().clear(); }
    Iterator insert(Iterator pos, const T &v) { return Base::mutableRef().insert(pos, v); }
    Iterator erase(Iterator pos) { return Base::mutableRef().erase(pos); }
    Iterator erase(Iterator first, Iterator last) { return Base::mutableRef().erase(first, last); }
    StlType &get() { return Base::mutableRef(); }

    // Qt-named mutating helpers (see Vector for the bounds rationale).
    void insert(SizeType i, const T &v)
    {
        StlType &c = Base::mutableRef();
        if (i >= c.size()) {
            c.push_back(v);
        } else {
            c.insert(c.begin() + i, v);
        }
    }
    void replace(SizeType i, const T &v)
    {
        StlType &c = Base::mutableRef();
        if (i < c.size()) {
            c[i] = v;
        }
    }
    void removeAt(SizeType i)
    {
        StlType &c = Base::mutableRef();
        if (i < c.size()) {
            c.erase(c.begin() + i);
        }
    }
    void removeFirst()
    {
        StlType &c = Base::mutableRef();
        if (!c.empty()) {
            c.pop_front();
        }
    }
    void removeLast()
    {
        StlType &c = Base::mutableRef();
        if (!c.empty()) {
            c.pop_back();
        }
    }
    T takeAt(SizeType i)
    {
        StlType &c = Base::mutableRef();
        if (i >= c.size()) {
            return T();
        }
        T v = c[i];
        c.erase(c.begin() + i);
        return v;
    }
    T takeFirst()
    {
        StlType &c = Base::mutableRef();
        if (c.empty()) {
            return T();
        }
        T v = c.front();
        c.pop_front();
        return v;
    }
    T takeLast()
    {
        StlType &c = Base::mutableRef();
        if (c.empty()) {
            return T();
        }
        T v = c.back();
        c.pop_back();
        return v;
    }
    SizeType removeAll(const T &v)
    {
        StlType &c = Base::mutableRef();
        const SizeType before = c.size();
        c.erase(std::remove(c.begin(), c.end(), v), c.end());
        return before - c.size();
    }
    bool removeOne(const T &v)
    {
        StlType &c = Base::mutableRef();
        Iterator it = std::find(c.begin(), c.end(), v);
        if (it == c.end()) {
            return false;
        }
        c.erase(it);
        return true;
    }
    template<typename Predicate>
    SizeType removeIf(Predicate pred)
    {
        StlType &c = Base::mutableRef();
        const SizeType before = c.size();
        c.erase(std::remove_if(c.begin(), c.end(), pred), c.end());
        return before - c.size();
    }
    void fill(const T &v, SizeType n = npos)
    {
        StlType &c = Base::mutableRef();
        c.assign(n == npos ? c.size() : n, v);
    }
    void swapItemsAt(SizeType i, SizeType j)
    {
        StlType &c = Base::mutableRef();
        if (i < c.size() && j < c.size()) {
            std::swap(c[i], c[j]);
        }
    }
    void move(SizeType from, SizeType to)
    {
        StlType &c = Base::mutableRef();
        if (from == to || from >= c.size() || to >= c.size()) {
            return;
        }
        if (from < to) {
            std::rotate(c.begin() + from, c.begin() + from + 1, c.begin() + to + 1);
        } else {
            std::rotate(c.begin() + to, c.begin() + from, c.begin() + from + 1);
        }
    }
    // std::deque has no shrink_to_fit(): swap with a tight copy of the contents, so the
    // old (over-allocated) blocks are freed when the temporary dies. This is O(n) -- it
    // copies the elements -- which is the price of squeezing a node-based container.
    void squeeze()
    {
        StlType &c = Base::mutableRef();
        StlType(c).swap(c);
    }
};

// ---------------------------------------------------------------------------
// List: std::list<T, Alloc>
//
// Note: COW benefits node-based containers the least. A detach allocates and copies node
// by node, and a linked list is cache-unfriendly to begin with. Do not rely on the
// implicit sharing here once a list gets large.
//
// List deliberately omits the index-based methods (value / removeAt / takeAt / insert(i) /
// replace / swapItemsAt / move / squeeze): std::list has no random access, so those would
// be O(n) traps dressed up as O(1) calls. Use indexOf() when you need a position.
// ---------------------------------------------------------------------------
template<typename T, typename Alloc = std::allocator<T> >
class List : public CowContainerBase<std::list<T, Alloc> >
{
    typedef CowContainerBase<std::list<T, Alloc> > Base;

public:
    typedef std::list<T, Alloc> StlType;
    typedef typename StlType::iterator Iterator;
    typedef typename StlType::const_iterator ConstIterator;
    typedef typename StlType::reverse_iterator ReverseIterator;
    typedef typename StlType::const_reverse_iterator ConstReverseIterator;
    typedef typename StlType::value_type ValueType;
    typedef typename StlType::size_type SizeType;

    QTNG_COW_PLUMBING(List)

    QTNG_COW_ORDERING(List)

    enum : SizeType
    {
        npos = static_cast<SizeType>(-1),
    };

    List(std::initializer_list<T> l)
        : Base(new CowData<StlType>(StlType(l)))
    {
    }

public:
    // -- read-only
    SizeType size() const { return Base::constRef().size(); }
    bool empty() const { return Base::constRef().empty(); }
    bool isEmpty() const { return empty(); }
    const T &front() const { return Base::constRef().front(); }
    const T &back() const { return Base::constRef().back(); }
    const T &first() const { return Base::constRef().front(); }
    const T &last() const { return Base::constRef().back(); }
    ConstIterator begin() const { return Base::constRef().begin(); }
    ConstIterator end() const { return Base::constRef().end(); }
    ConstIterator cbegin() const { return Base::constRef().begin(); }
    ConstIterator cend() const { return Base::constRef().end(); }
    ConstReverseIterator rbegin() const { return Base::constRef().rbegin(); }
    ConstReverseIterator rend() const { return Base::constRef().rend(); }
    ConstReverseIterator crbegin() const { return Base::constRef().rbegin(); }
    ConstReverseIterator crend() const { return Base::constRef().rend(); }
    const StlType &get() const { return Base::constRef(); }

    bool contains(const T &v) const { return detail::seqContains(Base::constRef(), v); }
    SizeType count(const T &v) const { return detail::seqCount(Base::constRef(), v); }
    SizeType indexOf(const T &v, SizeType from = 0) const { return detail::seqIndexOf(Base::constRef(), from, v); }
    SizeType lastIndexOf(const T &v, SizeType from = npos) const
    {
        return detail::seqLastIndexOf(Base::constRef(), from, v);
    }
    bool startsWith(const T &v) const { return !Base::constRef().empty() && Base::constRef().front() == v; }
    bool endsWith(const T &v) const { return !Base::constRef().empty() && Base::constRef().back() == v; }

    // -- read/write
    T &front() { return Base::mutableRef().front(); }
    T &back() { return Base::mutableRef().back(); }
    T &first() { return Base::mutableRef().front(); }
    T &last() { return Base::mutableRef().back(); }
    Iterator begin() { return Base::mutableRef().begin(); }
    Iterator end() { return Base::mutableRef().end(); }
    ReverseIterator rbegin() { return Base::mutableRef().rbegin(); }
    ReverseIterator rend() { return Base::mutableRef().rend(); }
    void push_back(const T &v) { Base::mutableRef().push_back(v); }
    void push_front(const T &v) { Base::mutableRef().push_front(v); }
    void append(const T &v) { Base::mutableRef().push_back(v); }
    void prepend(const T &v) { Base::mutableRef().push_front(v); }
    void pop_back() { Base::mutableRef().pop_back(); }
    void pop_front() { Base::mutableRef().pop_front(); }
    void clear() { Base::mutableRef().clear(); }
    Iterator erase(Iterator it) { return Base::mutableRef().erase(it); }
    Iterator erase(Iterator first, Iterator last) { return Base::mutableRef().erase(first, last); }
    Iterator insert(Iterator pos, const T &v) { return Base::mutableRef().insert(pos, v); }
    StlType &get() { return Base::mutableRef(); }

    // Qt-named mutating helpers. The erase-all/erase-if variants use std::list's own
    // remove()/remove_if(), which relink nodes instead of shuffling elements.
    void removeFirst()
    {
        StlType &c = Base::mutableRef();
        if (!c.empty()) {
            c.pop_front();
        }
    }
    void removeLast()
    {
        StlType &c = Base::mutableRef();
        if (!c.empty()) {
            c.pop_back();
        }
    }
    T takeFirst()
    {
        StlType &c = Base::mutableRef();
        if (c.empty()) {
            return T();
        }
        T v = c.front();
        c.pop_front();
        return v;
    }
    T takeLast()
    {
        StlType &c = Base::mutableRef();
        if (c.empty()) {
            return T();
        }
        T v = c.back();
        c.pop_back();
        return v;
    }
    SizeType removeAll(const T &v)
    {
        StlType &c = Base::mutableRef();
        const SizeType before = c.size();
        c.remove(v);
        return before - c.size();
    }
    bool removeOne(const T &v)
    {
        StlType &c = Base::mutableRef();
        Iterator it = std::find(c.begin(), c.end(), v);
        if (it == c.end()) {
            return false;
        }
        c.erase(it);
        return true;
    }
    template<typename Predicate>
    SizeType removeIf(Predicate pred)
    {
        StlType &c = Base::mutableRef();
        const SizeType before = c.size();
        c.remove_if(pred);
        return before - c.size();
    }
    void fill(const T &v, SizeType n = npos)
    {
        StlType &c = Base::mutableRef();
        c.assign(n == npos ? c.size() : n, v);
    }
};

// ---------------------------------------------------------------------------
// Map: std::map<K, V, Compare, Alloc>
// ---------------------------------------------------------------------------
template<typename K, typename V, typename Compare = std::less<K>, typename Alloc = std::allocator<std::pair<const K, V> > >
class Map : public CowContainerBase<std::map<K, V, Compare, Alloc> >
{
    typedef CowContainerBase<std::map<K, V, Compare, Alloc> > Base;

public:
    typedef std::map<K, V, Compare, Alloc> StlType;
    typedef typename StlType::iterator Iterator;
    typedef typename StlType::const_iterator ConstIterator;
    typedef typename StlType::value_type ValueType;
    typedef typename StlType::key_type KeyType;
    typedef typename StlType::mapped_type MappedType;
    typedef typename StlType::size_type SizeType;

    QTNG_COW_PLUMBING(Map)

    QTNG_COW_ORDERING(Map)

    Map(std::initializer_list<ValueType> l)
        : Base(new CowData<StlType>(StlType(l)))
    {
    }

public:
    // -- read-only (never detaches)
    SizeType size() const { return Base::constRef().size(); }
    bool empty() const { return Base::constRef().empty(); }
    bool isEmpty() const { return empty(); }
    SizeType count(const K &k) const { return Base::constRef().count(k); }
    bool contains(const K &k) const { return Base::constRef().find(k) != Base::constRef().end(); }
    ConstIterator begin() const { return Base::constRef().begin(); }
    ConstIterator end() const { return Base::constRef().end(); }
    ConstIterator cbegin() const { return Base::constRef().begin(); }
    ConstIterator cend() const { return Base::constRef().end(); }
    ConstIterator find(const K &k) const { return Base::constRef().find(k); }
    ConstIterator lowerBound(const K &k) const { return Base::constRef().lower_bound(k); }
    ConstIterator upperBound(const K &k) const { return Base::constRef().upper_bound(k); }
    std::pair<ConstIterator, ConstIterator> equalRange(const K &k) const { return Base::constRef().equal_range(k); }
    const V &at(const K &k) const { return Base::constRef().at(k); }
    const StlType &get() const { return Base::constRef(); }

    // Qt-named read-only helpers. keys()/values() return a Vector (declared above), so a
    // caller can indexOf()/contains() the result with the same vocabulary.
    V value(const K &k, const V &defaultValue = V()) const
    {
        ConstIterator it = Base::constRef().find(k);
        return it == Base::constRef().end() ? defaultValue : it->second;
    }
    Vector<K> keys() const
    {
        const StlType &c = Base::constRef();
        Vector<K> out;
        out.reserve(c.size());
        for (ConstIterator it = c.begin(); it != c.end(); ++it) {
            out.push_back(it->first);
        }
        return out;
    }
    Vector<V> values() const
    {
        const StlType &c = Base::constRef();
        Vector<V> out;
        out.reserve(c.size());
        for (ConstIterator it = c.begin(); it != c.end(); ++it) {
            out.push_back(it->second);
        }
        return out;
    }
    K key(const V &v, const K &defaultKey = K()) const
    {
        const StlType &c = Base::constRef();
        for (ConstIterator it = c.begin(); it != c.end(); ++it) {
            if (it->second == v) {
                return it->first;
            }
        }
        return defaultKey;
    }
    // Preconditions: the map is not empty (the ordered container is what makes these cheap;
    // an empty call would dereference end()).
    const K &firstKey() const { return Base::constRef().begin()->first; }
    const K &lastKey() const { return Base::constRef().rbegin()->first; }
    const V &first() const { return Base::constRef().begin()->second; }
    const V &last() const { return Base::constRef().rbegin()->second; }

    // -- read/write (detaches first)
    Iterator begin() { return Base::mutableRef().begin(); }
    Iterator end() { return Base::mutableRef().end(); }
    Iterator find(const K &k) { return Base::mutableRef().find(k); }
    Iterator lowerBound(const K &k) { return Base::mutableRef().lower_bound(k); }
    Iterator upperBound(const K &k) { return Base::mutableRef().upper_bound(k); }
    std::pair<Iterator, Iterator> equalRange(const K &k) { return Base::mutableRef().equal_range(k); }
    V &operator[](const K &k) { return Base::mutableRef()[k]; }
    V &at(const K &k) { return Base::mutableRef().at(k); }
    std::pair<Iterator, bool> insert(const ValueType &v) { return Base::mutableRef().insert(v); }
    // A real range overload: the iterator must dereference to something convertible to
    // ValueType. The constraint is what keeps `insert(k, v)` (below) from being hijacked by
    // this overload -- with a string-keyed map, `insert("a", 1)` would otherwise deduce
    // It = const char* and then fail inside std::map. Foreign iterators (say a
    // std::vector<std::pair<K, V>>::iterator) still satisfy it.
    template<typename It>
    typename std::enable_if<std::is_convertible<decltype(*std::declval<It>()), const ValueType &>::value, void>::type
    insert(It first, It last)
    {
        Base::mutableRef().insert(first, last);
    }
    SizeType erase(const K &k) { return Base::mutableRef().erase(k); }
    Iterator erase(Iterator it) { return Base::mutableRef().erase(it); }
    void clear() { Base::mutableRef().clear(); }
    StlType &get() { return Base::mutableRef(); }

    // Qt-named mutating helpers.
    // insert(key, value) overwrites, where STL insert() refuses an existing key. Uses
    // insert() + assignment instead of operator[] so V need not be default-constructible.
    void insert(const K &k, const V &v)
    {
        StlType &c = Base::mutableRef();
        std::pair<Iterator, bool> r = c.insert(ValueType(k, v));
        if (!r.second) {
            r.first->second = v;
        }
    }
    SizeType remove(const K &k) { return Base::mutableRef().erase(k); }
    V take(const K &k)
    {
        StlType &c = Base::mutableRef();
        Iterator it = c.find(k);
        if (it == c.end()) {
            return V();
        }
        V v = it->second;
        c.erase(it);
        return v;
    }
    // Existing keys keep this container's value (STL range-insert semantics), matching
    // QMap::unite().
    void unite(const Map &other)
    {
        if (this == &other) {
            return;
        }
        Base::mutableRef().insert(other.get().begin(), other.get().end());
    }
    template<typename Predicate>
    SizeType removeIf(Predicate pred)
    {
        StlType &c = Base::mutableRef();
        SizeType removed = 0;
        for (Iterator it = c.begin(); it != c.end();) {
            if (pred(*it)) {
                it = c.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        return removed;
    }
};

// ---------------------------------------------------------------------------
// Set: std::set<K, Compare, Alloc>
// ---------------------------------------------------------------------------
template<typename K, typename Compare = std::less<K>, typename Alloc = std::allocator<K> >
class Set : public CowContainerBase<std::set<K, Compare, Alloc> >
{
    typedef CowContainerBase<std::set<K, Compare, Alloc> > Base;

public:
    typedef std::set<K, Compare, Alloc> StlType;
    typedef typename StlType::iterator Iterator;
    typedef typename StlType::const_iterator ConstIterator;
    typedef typename StlType::value_type ValueType;
    typedef typename StlType::size_type SizeType;

    QTNG_COW_PLUMBING(Set)

    QTNG_COW_ORDERING(Set)

    Set(std::initializer_list<K> l)
        : Base(new CowData<StlType>(StlType(l)))
    {
    }

public:
    // -- read-only
    SizeType size() const { return Base::constRef().size(); }
    bool empty() const { return Base::constRef().empty(); }
    bool isEmpty() const { return empty(); }
    SizeType count(const K &k) const { return Base::constRef().count(k); }
    bool contains(const K &k) const { return Base::constRef().find(k) != Base::constRef().end(); }
    ConstIterator begin() const { return Base::constRef().begin(); }
    ConstIterator end() const { return Base::constRef().end(); }
    ConstIterator cbegin() const { return Base::constRef().begin(); }
    ConstIterator cend() const { return Base::constRef().end(); }
    ConstIterator find(const K &k) const { return Base::constRef().find(k); }
    ConstIterator lowerBound(const K &k) const { return Base::constRef().lower_bound(k); }
    ConstIterator upperBound(const K &k) const { return Base::constRef().upper_bound(k); }
    const StlType &get() const { return Base::constRef(); }

    // Qt-named read-only helper.
    Vector<K> values() const
    {
        const StlType &c = Base::constRef();
        Vector<K> out;
        out.reserve(c.size());
        for (ConstIterator it = c.begin(); it != c.end(); ++it) {
            out.push_back(*it);
        }
        return out;
    }

    // -- read/write
    Iterator begin() { return Base::mutableRef().begin(); }
    Iterator end() { return Base::mutableRef().end(); }
    Iterator find(const K &k) { return Base::mutableRef().find(k); }
    Iterator lowerBound(const K &k) { return Base::mutableRef().lower_bound(k); }
    Iterator upperBound(const K &k) { return Base::mutableRef().upper_bound(k); }
    std::pair<Iterator, bool> insert(const ValueType &v) { return Base::mutableRef().insert(v); }
    template<typename It>
    void insert(It first, It last)
    {
        Base::mutableRef().insert(first, last);
    }
    SizeType erase(const K &k) { return Base::mutableRef().erase(k); }
    Iterator erase(Iterator it) { return Base::mutableRef().erase(it); }
    void clear() { Base::mutableRef().clear(); }
    StlType &get() { return Base::mutableRef(); }

    // Qt-named mutating helpers.
    SizeType remove(const K &k) { return Base::mutableRef().erase(k); }
    void unite(const Set &other)
    {
        if (this == &other) {
            return;
        }
        Base::mutableRef().insert(other.get().begin(), other.get().end());
    }
    // Keep only keys also present in `other`. Erase-as-you-go is safe here: the erased node
    // is the one the iterator already passed, and the comparison only reads `other` (which,
    // after the detach below, never aliases this container's buffer).
    void intersect(const Set &other)
    {
        if (this == &other) {
            return;
        }
        StlType &c = Base::mutableRef();
        Iterator it = c.begin();
        while (it != c.end()) {
            if (other.get().find(*it) == other.get().end()) {
                it = c.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Drop every key also present in `other`.
    void subtract(const Set &other)
    {
        if (this == &other) {
            Base::mutableRef().clear();
            return;
        }
        StlType &c = Base::mutableRef();
        Iterator it = c.begin();
        while (it != c.end()) {
            if (other.get().find(*it) != other.get().end()) {
                it = c.erase(it);
            } else {
                ++it;
            }
        }
    }
    template<typename Predicate>
    SizeType removeIf(Predicate pred)
    {
        StlType &c = Base::mutableRef();
        SizeType removed = 0;
        for (Iterator it = c.begin(); it != c.end();) {
            if (pred(*it)) {
                it = c.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        return removed;
    }
};

// ---------------------------------------------------------------------------
// UnorderedMap: std::unordered_map<K, V, Hash, Equal, Alloc>
//
// Note: a detach rehashes node by node, which is more expensive than Vector's bulk copy.
// ---------------------------------------------------------------------------
template<typename K, typename V, typename Hash = std::hash<K>, typename Equal = std::equal_to<K>,
         typename Alloc = std::allocator<std::pair<const K, V> > >
class UnorderedMap : public CowContainerBase<std::unordered_map<K, V, Hash, Equal, Alloc> >
{
    typedef CowContainerBase<std::unordered_map<K, V, Hash, Equal, Alloc> > Base;

public:
    typedef std::unordered_map<K, V, Hash, Equal, Alloc> StlType;
    typedef typename StlType::iterator Iterator;
    typedef typename StlType::const_iterator ConstIterator;
    typedef typename StlType::value_type ValueType;
    typedef typename StlType::size_type SizeType;

    QTNG_COW_PLUMBING(UnorderedMap)

    QTNG_COW_EQUALITY(UnorderedMap)

    UnorderedMap(std::initializer_list<ValueType> l)
        : Base(new CowData<StlType>(StlType(l)))
    {
    }

public:
    // -- read-only
    SizeType size() const { return Base::constRef().size(); }
    bool empty() const { return Base::constRef().empty(); }
    bool isEmpty() const { return empty(); }
    SizeType count(const K &k) const { return Base::constRef().count(k); }
    bool contains(const K &k) const { return Base::constRef().find(k) != Base::constRef().end(); }
    ConstIterator begin() const { return Base::constRef().begin(); }
    ConstIterator end() const { return Base::constRef().end(); }
    ConstIterator cbegin() const { return Base::constRef().begin(); }
    ConstIterator cend() const { return Base::constRef().end(); }
    ConstIterator find(const K &k) const { return Base::constRef().find(k); }
    const V &at(const K &k) const { return Base::constRef().at(k); }
    const StlType &get() const { return Base::constRef(); }

    V value(const K &k, const V &defaultValue = V()) const
    {
        ConstIterator it = Base::constRef().find(k);
        return it == Base::constRef().end() ? defaultValue : it->second;
    }
    Vector<K> keys() const
    {
        const StlType &c = Base::constRef();
        Vector<K> out;
        out.reserve(c.size());
        for (ConstIterator it = c.begin(); it != c.end(); ++it) {
            out.push_back(it->first);
        }
        return out;
    }
    Vector<V> values() const
    {
        const StlType &c = Base::constRef();
        Vector<V> out;
        out.reserve(c.size());
        for (ConstIterator it = c.begin(); it != c.end(); ++it) {
            out.push_back(it->second);
        }
        return out;
    }
    K key(const V &v, const K &defaultKey = K()) const
    {
        const StlType &c = Base::constRef();
        for (ConstIterator it = c.begin(); it != c.end(); ++it) {
            if (it->second == v) {
                return it->first;
            }
        }
        return defaultKey;
    }

    // -- read/write
    Iterator begin() { return Base::mutableRef().begin(); }
    Iterator end() { return Base::mutableRef().end(); }
    Iterator find(const K &k) { return Base::mutableRef().find(k); }
    V &operator[](const K &k) { return Base::mutableRef()[k]; }
    V &at(const K &k) { return Base::mutableRef().at(k); }
    std::pair<Iterator, bool> insert(const ValueType &v) { return Base::mutableRef().insert(v); }
    void insert(const K &k, const V &v)
    {
        StlType &c = Base::mutableRef();
        std::pair<Iterator, bool> r = c.insert(ValueType(k, v));
        if (!r.second) {
            r.first->second = v;
        }
    }
    SizeType erase(const K &k) { return Base::mutableRef().erase(k); }
    Iterator erase(Iterator it) { return Base::mutableRef().erase(it); }
    void clear() { Base::mutableRef().clear(); }
    void reserve(SizeType n) { Base::mutableRef().reserve(n); }
    StlType &get() { return Base::mutableRef(); }

    SizeType remove(const K &k) { return Base::mutableRef().erase(k); }
    V take(const K &k)
    {
        StlType &c = Base::mutableRef();
        Iterator it = c.find(k);
        if (it == c.end()) {
            return V();
        }
        V v = it->second;
        c.erase(it);
        return v;
    }
    void unite(const UnorderedMap &other)
    {
        if (this == &other) {
            return;
        }
        Base::mutableRef().insert(other.get().begin(), other.get().end());
    }
    template<typename Predicate>
    SizeType removeIf(Predicate pred)
    {
        StlType &c = Base::mutableRef();
        SizeType removed = 0;
        for (Iterator it = c.begin(); it != c.end();) {
            if (pred(*it)) {
                it = c.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        return removed;
    }
};

// ---------------------------------------------------------------------------
// UnorderedSet: std::unordered_set<K, Hash, Equal, Alloc>
// ---------------------------------------------------------------------------
template<typename K, typename Hash = std::hash<K>, typename Equal = std::equal_to<K>, typename Alloc = std::allocator<K> >
class UnorderedSet : public CowContainerBase<std::unordered_set<K, Hash, Equal, Alloc> >
{
    typedef CowContainerBase<std::unordered_set<K, Hash, Equal, Alloc> > Base;

public:
    typedef std::unordered_set<K, Hash, Equal, Alloc> StlType;
    typedef typename StlType::iterator Iterator;
    typedef typename StlType::const_iterator ConstIterator;
    typedef typename StlType::size_type SizeType;

    QTNG_COW_PLUMBING(UnorderedSet)

    QTNG_COW_EQUALITY(UnorderedSet)

    UnorderedSet(std::initializer_list<K> l)
        : Base(new CowData<StlType>(StlType(l)))
    {
    }

public:
    // -- read-only
    SizeType size() const { return Base::constRef().size(); }
    bool empty() const { return Base::constRef().empty(); }
    bool isEmpty() const { return empty(); }
    SizeType count(const K &k) const { return Base::constRef().count(k); }
    bool contains(const K &k) const { return Base::constRef().find(k) != Base::constRef().end(); }
    ConstIterator begin() const { return Base::constRef().begin(); }
    ConstIterator end() const { return Base::constRef().end(); }
    ConstIterator cbegin() const { return Base::constRef().begin(); }
    ConstIterator cend() const { return Base::constRef().end(); }
    ConstIterator find(const K &k) const { return Base::constRef().find(k); }
    const StlType &get() const { return Base::constRef(); }

    Vector<K> values() const
    {
        const StlType &c = Base::constRef();
        Vector<K> out;
        out.reserve(c.size());
        for (ConstIterator it = c.begin(); it != c.end(); ++it) {
            out.push_back(*it);
        }
        return out;
    }

    // -- read/write
    Iterator begin() { return Base::mutableRef().begin(); }
    Iterator end() { return Base::mutableRef().end(); }
    Iterator find(const K &k) { return Base::mutableRef().find(k); }
    std::pair<Iterator, bool> insert(const K &v) { return Base::mutableRef().insert(v); }
    SizeType erase(const K &k) { return Base::mutableRef().erase(k); }
    Iterator erase(Iterator it) { return Base::mutableRef().erase(it); }
    void clear() { Base::mutableRef().clear(); }
    void reserve(SizeType n) { Base::mutableRef().reserve(n); }
    StlType &get() { return Base::mutableRef(); }

    // Qt-named mutating helpers. std::unordered_set inserts are not invalidated by erase of
    // a different element in the same bucket, so erase-as-you-go is safe.
    SizeType remove(const K &k) { return Base::mutableRef().erase(k); }
    void unite(const UnorderedSet &other)
    {
        if (this == &other) {
            return;
        }
        Base::mutableRef().insert(other.get().begin(), other.get().end());
    }
    void intersect(const UnorderedSet &other)
    {
        if (this == &other) {
            return;
        }
        StlType &c = Base::mutableRef();
        Iterator it = c.begin();
        while (it != c.end()) {
            if (other.get().find(*it) == other.get().end()) {
                it = c.erase(it);
            } else {
                ++it;
            }
        }
    }
    void subtract(const UnorderedSet &other)
    {
        if (this == &other) {
            Base::mutableRef().clear();
            return;
        }
        StlType &c = Base::mutableRef();
        Iterator it = c.begin();
        while (it != c.end()) {
            if (other.get().find(*it) != other.get().end()) {
                it = c.erase(it);
            } else {
                ++it;
            }
        }
    }
    template<typename Predicate>
    SizeType removeIf(Predicate pred)
    {
        StlType &c = Base::mutableRef();
        SizeType removed = 0;
        for (Iterator it = c.begin(); it != c.end();) {
            if (pred(*it)) {
                it = c.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        return removed;
    }
};

#undef QTNG_COW_PLUMBING

}  // namespace qtng

#endif  // QTNG_TYPES_CONTAINERS_H
