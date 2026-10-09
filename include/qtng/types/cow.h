#ifndef QTNG_TYPES_COW_H
#define QTNG_TYPES_COW_H

// Common facilities for implicit sharing (copy-on-write).
//
// This header depends only on the C++11 standard library: it includes no other qtng header
// and uses no NG_* macro. One reason is to let these types be moved elsewhere wholesale
// later; another is to keep coroutine/platform details out of value types.
//
// ---------------------------------------------------------------------------
// Reference-counting protocol (the only copy of it in the library)
// ---------------------------------------------------------------------------
//
// Every shared block (String::Data, a container's CowData<T>) embeds a
// `std::atomic<std::uint32_t> ref` member that may only be manipulated through cowIncref /
// cowDecref. The atomic incref/decref logic exists in exactly one place in the library, so
// the code most likely to produce a use-after-free is not copied into every container.
//
// incref uses relaxed and decref uses acq_rel, deciding that the count hit zero from
// fetch_sub's return value -- the standard shared_ptr approach:
//   - incref needs no barrier: the caller already holds the object (visibility is handled
//     by the acquisition path);
//   - decref uses acq_rel: writes before the release must not be reordered after it
//     (release), while the last reference must see every write other threads made before
//     their release (acquire).
//
// Thread-safety contract (the one correct premise; misuse causes data races):
//   COW's detach test is based on the reference count and is isomorphic to Qt's implicit
//   sharing; it only holds under "at most one writer and read-only readers at any moment".
//   A shared block already published to readers must never be modified in place by a
//   writer; the standard approach is for the writer to modify a private copy and then
//   publish it.

#include <atomic>
#include <cstdint>
#include <new>
#include <utility>

namespace qtng {

typedef std::atomic<std::uint32_t> CowRef;

inline void cowIncref(CowRef &ref)
{
    ref.fetch_add(1, std::memory_order_relaxed);
}

// A true return means the reference count reached zero and the caller must free the block.
inline bool cowDecref(CowRef &ref)
{
    return ref.fetch_sub(1, std::memory_order_acq_rel) == 1;
}

inline void cowInitRef(CowRef &ref)
{
    ref.store(1, std::memory_order_relaxed);
}

// For assertions/tests and diagnostics only; do not use it for control flow (it has an
// inherent read-then-use race).
inline bool cowIsShared(const CowRef &ref)
{
    return ref.load(std::memory_order_acquire) > 1;
}

inline long cowUseCount(const CowRef &ref)
{
    return static_cast<long>(ref.load(std::memory_order_acquire));
}

// ---------------------------------------------------------------------------
// Container shared block: { ref, underlying STL container }
// ---------------------------------------------------------------------------
//
// The copy constructor deliberately copies only the underlying container and sets ref to
// 1: the copy is "newly created", not "another reference to the original shared block".
// Copying a reference count must go through the handle (CowContainerBase), never here.
template<typename Underlying>
struct CowData
{
    CowRef ref;
    Underlying m;

    CowData()
    {
        cowInitRef(ref);
    }

    explicit CowData(const Underlying &v)
        : m(v)
    {
        cowInitRef(ref);
    }

    explicit CowData(Underlying &&v)
        : m(std::move(v))
    {
        cowInitRef(ref);
    }

    CowData(const CowData &other)
        : m(other.m)
    {
        cowInitRef(ref);
    }

    // Copying a shared block may only go through the handle (CowContainerBase): that is
    // what "bump the reference count" means. Allowing assignment here would mean "create a
    // new copy and set the count to 1", which conflicts with the handle semantics.
    CowData &operator=(const CowData &) = delete;
};

// ---------------------------------------------------------------------------
// Container handle: a copy bumps the reference count; every write detaches first
// ---------------------------------------------------------------------------
//
// detach() copies the underlying container in full when the reference count is > 1, after
// which the handle owns it exclusively.
//
// Usage constraints (see the top of containers.h):
//   - non-const member functions detach first, even when they look read-only (such as a
//     non-const find/begin), because they return mutable iterators/references that could
//     bypass detach and modify someone else's data if they pointed into a shared buffer;
//   - after a detach, every iterator/reference obtained earlier is invalidated (it points
//     at the old buffer).
template<typename Underlying>
class CowContainerBase
{
public:
    CowContainerBase()
        : d(new CowData<Underlying>())
    {
    }

    CowContainerBase(const CowContainerBase &other)
        : d(other.d)
    {
        cowIncref(d->ref);
    }

    ~CowContainerBase()
    {
        if (cowDecref(d->ref)) {
            delete d;
        }
    }

    CowContainerBase &operator=(const CowContainerBase &other)
    {
        // Increment before decrementing: on self-assignment the two cancel out, so we never
        // free ourselves.
        cowIncref(other.d->ref);
        if (cowDecref(d->ref)) {
            delete d;
        }
        d = other.d;
        return *this;
    }

public:
    // Whether another handle shares the same underlying container.
    bool isShared() const { return cowIsShared(d->ref); }
    long useCount() const { return cowUseCount(d->ref); }

    // Make exclusive. A no-op when already exclusive.
    void detach()
    {
        if (!cowIsShared(d->ref)) {
            return;
        }
        CowData<Underlying> *clone = new CowData<Underlying>(*d);
        if (cowDecref(d->ref)) {
            delete d;
        }
        d = clone;
    }

protected:
    // Read-only access: does not detach.
    const Underlying &constRef() const { return d->m; }
    // Mutable access: detaches first.
    Underlying &mutableRef()
    {
        detach();
        return d->m;
    }

    // Construct a handle from an existing shared block (taking over one reference). For the
    // derived classes in this file only.
    explicit CowContainerBase(CowData<Underlying> *data)
        : d(data)
    {
    }

    CowData<Underlying> *d;
};

}  // namespace qtng

#endif  // QTNG_TYPES_COW_H
