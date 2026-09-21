#include "qtng/utils/slice_ring_buffer.h"

#include <cstring>
#include <new>
#include <stdexcept>
#include <vector>

#ifdef NG_OS_WIN
#  include <windows.h>
#else
#  ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#  endif
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

using namespace std;

namespace qtng {
namespace utils {

namespace {

#ifdef NG_OS_WIN
size_t allocationGranularity()
{
    static SYSTEM_INFO si{};
    GetSystemInfo(&si);
    return static_cast<size_t>(si.dwAllocationGranularity);
}
#else
size_t pageSize()
{
    static const size_t ps = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    return ps ? ps : 4096;
}
#endif

size_t roundUp(size_t value, size_t align)
{
    return (value + align - 1) / align * align;
}

}  // namespace

// A double-mapped buffer: a 2N virtual region whose two halves both map the
// same N-byte physical backing, so the live window [head, tail) is always
// contiguous in virtual address space regardless of physical wraparound.
struct SliceRingBufferPrivate
{
    explicit SliceRingBufferPrivate(size_t cap)
    {
        const size_t unit = initCapacityUnit();
        const size_t want = cap ? roundUp(cap, unit) : unit;
        if (!map(want)) {
            throw bad_alloc();
        }
        resetWindow();
    }

    ~SliceRingBufferPrivate()
    {
        unmap();
    }

    size_t size() const { return static_cast<size_t>(tail - head); }
    bool empty() const { return head == tail; }
    size_t capacity() const { return m_capacity; }
    char *windowPtr() const { return static_cast<char *>(head); }
    char *windowEnd() const { return static_cast<char *>(tail); }

    void pushBack(const char *data, size_t n)
    {
        if (n == 0) {
            return;
        }
        reserve(size() + n);
        if (tail + n > base + m_capacity * 2) {
            center();
        }
        memcpy(tail, data, n);
        tail += n;
    }

    void pushFront(const char *data, size_t n)
    {
        if (n == 0) {
            return;
        }
        reserve(size() + n);
        if (head - n < base) {
            center();
        }
        head -= n;
        memcpy(head, data, n);
    }

    void popFront(size_t n)
    {
        const size_t c = n < size() ? n : size();
        head += c;
        if (empty()) {
            resetWindow();
        }
    }

    void popBack(size_t n)
    {
        const size_t c = n < size() ? n : size();
        tail -= c;
        if (empty()) {
            resetWindow();
        }
    }

    void clear()
    {
        resetWindow();
    }

private:
    size_t initCapacityUnit() const
    {
#ifdef NG_OS_WIN
        return allocationGranularity();
#else
        return pageSize();
#endif
    }

    // Park the (empty) window at the center so the next push on either side
    // starts with balanced free space.
    void resetWindow()
    {
        head = tail = base + (m_capacity * 2) / 2;
    }

    // Growth path: allocate a larger mapping, copy the live window into its
    // center, then release the old mapping and adopt the new one. If the new
    // allocation fails, the object is left untouched.
    void reserve(size_t need)
    {
        if (need <= m_capacity) {
            return;
        }
        size_t newCap = m_capacity ? m_capacity : initCapacityUnit();
        while (newCap < need) {
            newCap *= 2;
        }
        newCap = roundUp(newCap, initCapacityUnit());
        if (newCap == m_capacity) {
            return;
        }

        const size_t s = size();
        const size_t p = (newCap * 2 - s) / 2;

        char *newBase = nullptr;
#ifdef NG_OS_WIN
        HANDLE newSection = nullptr;
        char *newReserve = nullptr;
        void *newView1 = nullptr;
        void *newView2 = nullptr;
        if (!mapWin(newCap, &newBase, &newSection, &newReserve, &newView1, &newView2)) {
            return;  // out of memory; keep current state
        }
#else
        int newFd = -1;
        if (!mapPosix(newCap, &newBase, &newFd)) {
            return;
        }
#endif

        if (s) {
            memcpy(newBase + p, head, s);
        }

        unmap();
        base = newBase;
        head = newBase + p;
        tail = head + s;
        m_capacity = newCap;
#ifdef NG_OS_WIN
        m_section = newSection;
        m_reserve = newReserve;
        m_view1 = newView1;
        m_view2 = newView2;
#else
        m_backingFd = newFd;
#endif
    }

    // Re-center the live window so there is room on both ends. With a 2N
    // region and a window of size s <= N, centering leaves (2N - s)/2 free
    // bytes on each side, which is always enough for a push of n <= N - s.
    void center()
    {
        const size_t s = size();
        const size_t p = (m_capacity * 2 - s) / 2;
        char *newHead = base + p;
        if (s && newHead != head) {
            // Inside the double-mapping the source and destination virtual
            // addresses alias the same physical pages; a linear memmove whose
            // range straddles the virtual middle boundary (base + m_capacity)
            // wraps physically and copies bytes out of order, corrupting the
            // window. Copy through a temporary buffer to break the alias. This
            // runs only on a recenter-triggering push, so the O(s) temp is fine.
            vector<char> tmp(s);
            memcpy(tmp.data(), head, s);
            memcpy(newHead, tmp.data(), s);
        }
        head = newHead;
        tail = head + s;
    }

    bool map(size_t cap)
    {
        m_capacity = cap;
#ifdef NG_OS_WIN
        return mapWin(cap, &base, &m_section, &m_reserve, &m_view1, &m_view2);
#else
        return mapPosix(cap, &base, &m_backingFd);
#endif
    }

#ifdef NG_OS_WIN
    bool mapWin(size_t cap, char **outBase, HANDLE *outSection, char **outReserve,
                void **outView1, void **outView2)
    {
        const size_t region = cap * 2;
        char *reserve = static_cast<char *>(
                VirtualAlloc(nullptr, region, MEM_RESERVE, PAGE_NOACCESS));
        if (!reserve) {
            return false;
        }
        HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                            PAGE_READWRITE, 0, cap, nullptr);
        if (!section) {
            VirtualFree(reserve, 0, MEM_RELEASE);
            return false;
        }
        void *view1 = MapViewOfFileEx(section, FILE_MAP_ALL_ACCESS, 0, 0, cap, reserve);
        void *view2 = nullptr;
        if (view1) {
            view2 = MapViewOfFileEx(section, FILE_MAP_ALL_ACCESS, 0, 0, cap,
                                    reserve + cap);
        }
        if (!view2) {
            if (view1) {
                UnmapViewOfFile(view1);
            }
            CloseHandle(section);
            VirtualFree(reserve, 0, MEM_RELEASE);
            return false;
        }
        *outBase = reserve;
        *outSection = section;
        *outReserve = reserve;
        *outView1 = view1;
        *outView2 = view2;
        return true;
    }
#else
    bool mapPosix(size_t cap, char **outBase, int *outFd)
    {
        const size_t region = cap * 2;
        int fd = createBacking(cap);
        if (fd < 0) {
            return false;
        }
        char *reserve = static_cast<char *>(
                mmap(nullptr, region, PROT_NONE, MAP_PRIVATE | anonymousFlag(), -1, 0));
        if (reserve == MAP_FAILED) {
            close(fd);
            return false;
        }
        void *view1 = mmap(reserve, cap, PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_FIXED, fd, 0);
        void *view2 = view1 == MAP_FAILED
                ? MAP_FAILED
                : mmap(reserve + cap, cap, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_FIXED, fd, 0);
        if (view2 == MAP_FAILED) {
            munmap(reserve, region);
            close(fd);
            return false;
        }
        *outBase = reserve;
        *outFd = fd;
        return true;
    }
#endif

    void unmap()
    {
        if (!base) {
            return;
        }
#ifdef NG_OS_WIN
        UnmapViewOfFile(m_view1);
        UnmapViewOfFile(m_view2);
        CloseHandle(m_section);
        VirtualFree(m_reserve, 0, MEM_RELEASE);
        m_section = nullptr;
        m_reserve = nullptr;
        m_view1 = nullptr;
        m_view2 = nullptr;
#else
        munmap(base, m_capacity * 2);
        if (m_backingFd >= 0) {
            close(m_backingFd);
            m_backingFd = -1;
        }
#endif
        base = nullptr;
        head = nullptr;
        tail = nullptr;
    }

    int createBacking(size_t size)
    {
#if defined(NG_OS_LINUX)
        int lfd = memfd_create("slice-ring", 0);
        if (lfd >= 0 && ftruncate(lfd, static_cast<off_t>(size)) == 0) {
            return lfd;
        }
        if (lfd >= 0) {
            close(lfd);
        }
#endif
#if defined(NG_OS_LINUX) || defined(NG_OS_MACOS) || defined(NG_OS_FREEBSD) \
        || defined(NG_OS_NETBSD) || defined(NG_OS_OPENBSD) || defined(NG_OS_ANDROID) \
        || defined(NG_OS_IOS)
        // POSIX fallback: an anonymous shared-memory object, unlinked
        // immediately so it self-cleans on close/crash.
        static unsigned long counter = 0;
        char name[64];
        snprintf(name, sizeof(name), "/qtng-slice-%lu-%lu",
                 static_cast<unsigned long>(getpid()), counter++);
        int sfd = shm_open(name, O_CREAT | O_RDWR, 0600);
        if (sfd >= 0) {
            shm_unlink(name);
            if (ftruncate(sfd, static_cast<off_t>(size)) == 0) {
                return sfd;
            }
            close(sfd);
        }
#else
        (void)size;
#endif
        return -1;
    }

    size_t m_capacity = 0;
#ifdef NG_OS_WIN
    HANDLE m_section = nullptr;
    char *m_reserve = nullptr;
    void *m_view1 = nullptr;
    void *m_view2 = nullptr;
#else
    int m_backingFd = -1;

    int anonymousFlag() const
    {
#ifdef MAP_ANONYMOUS
        return MAP_ANONYMOUS;
#else
        return MAP_ANON;
#endif
    }
#endif
    char *base = nullptr;
    char *head = nullptr;
    char *tail = nullptr;
};

// --- public interface -----------------------------------------------------

SliceRingBuffer::SliceRingBuffer(size_t capacity)
    : d_ptr(new SliceRingBufferPrivate(capacity))
{
}

SliceRingBuffer::~SliceRingBuffer()
{
    delete d_ptr;
}

SliceRingBuffer::SliceRingBuffer(SliceRingBuffer &&other) noexcept
    : d_ptr(other.d_ptr)
{
    other.d_ptr = nullptr;
}

SliceRingBuffer &SliceRingBuffer::operator=(SliceRingBuffer &&other) noexcept
{
    if (this != &other) {
        delete d_ptr;
        d_ptr = other.d_ptr;
        other.d_ptr = nullptr;
    }
    return *this;
}

size_t SliceRingBuffer::size() const
{
    return d_ptr ? d_ptr->size() : 0;
}

bool SliceRingBuffer::empty() const
{
    return !d_ptr || d_ptr->empty();
}

size_t SliceRingBuffer::capacity() const
{
    return d_ptr ? d_ptr->capacity() : 0;
}

const char *SliceRingBuffer::data() const
{
    return d_ptr ? d_ptr->windowPtr() : nullptr;
}

char *SliceRingBuffer::data()
{
    return d_ptr ? d_ptr->windowPtr() : nullptr;
}

char SliceRingBuffer::front() const
{
    return empty() ? 0 : *d_ptr->windowPtr();
}

char SliceRingBuffer::back() const
{
    return empty() ? 0 : *(d_ptr->windowEnd() - 1);
}

void SliceRingBuffer::push_back(const char *data, size_t n)
{
    d_ptr->pushBack(data, n);
}

void SliceRingBuffer::push_front(const char *data, size_t n)
{
    d_ptr->pushFront(data, n);
}

void SliceRingBuffer::pop_front(size_t n)
{
    d_ptr->popFront(n);
}

void SliceRingBuffer::pop_back(size_t n)
{
    d_ptr->popBack(n);
}

void SliceRingBuffer::clear()
{
    d_ptr->clear();
}

}  // namespace utils
}  // namespace qtng
