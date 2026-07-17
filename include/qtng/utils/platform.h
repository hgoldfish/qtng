#ifndef QTNG_UTILS_PLATFORM_H
#define QTNG_UTILS_PLATFORM_H

#include <cstdint>
#include <memory>
#include <utility>

#if __cplusplus < 201402L

namespace std {

template<typename T, typename... Args>
inline unique_ptr<T> make_unique(Args &&... args)
{
    return unique_ptr<T>(new T(std::forward<Args>(args)...));
}

}  // namespace std

#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#  define NG_OS_WIN
#elif defined(__ANDROID__)
#  define NG_OS_ANDROID
#elif defined(__APPLE__) && (defined(__GNUC__) || defined(__xlC__) || defined(__xlc__))
#  include <TargetConditionals.h>
#  if TARGET_OS_IPHONE
#    define NG_OS_IOS
#  elif TARGET_OS_MAC
#    define NG_OS_MACOS
#  endif
#elif defined(__linux__)
#  define NG_OS_LINUX
#elif defined(__FreeBSD__)
#  define NG_OS_FREEBSD
#elif defined(__NetBSD__)
#  define NG_OS_NETBSD
#elif defined(__OpenBSD__)
#  define NG_OS_OPENBSD
#elif defined(__unix__) || defined(__unix)
#  define NG_OS_UNIX
#endif

#if defined(NG_OS_LINUX) || defined(NG_OS_FREEBSD) || defined(NG_OS_NETBSD) || defined(NG_OS_OPENBSD) || defined(NG_OS_MACOS) || defined(NG_OS_ANDROID)
#  ifndef NG_OS_UNIX
#    define NG_OS_UNIX
#  endif
#endif

#ifdef NG_OS_WIN
#  define NG_SOCKLEN_T int
#  define NG_SOCKOPTLEN_T int
#else
#  include <sys/types.h>
#  define NG_SOCKLEN_T socklen_t
#  define NG_SOCKOPTLEN_T socklen_t
#endif

#define NG_DECLARE_OPERATORS_FOR_FLAGS(Flags) \
    inline Flags operator|(Flags f1, Flags f2) noexcept { return Flags(int(f1) | int(f2)); } \
    inline Flags operator&(Flags f1, Flags f2) noexcept { return Flags(int(f1) & int(f2)); } \
    inline Flags operator^(Flags f1, Flags f2) noexcept { return Flags(int(f1) ^ int(f2)); } \
    inline Flags operator~(Flags f) noexcept { return Flags(~int(f)); } \
    inline Flags &operator|=(Flags &f1, Flags f2) noexcept { return f1 = (f1 | f2); } \
    inline Flags &operator&=(Flags &f1, Flags f2) noexcept { return f1 = (f1 & f2); } \
    inline Flags &operator^=(Flags &f1, Flags f2) noexcept { return f1 = (f1 ^ f2); }

#define NG_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define NG_UNREACHABLE() __builtin_unreachable()


template<typename T>
inline T ngFromBigEndian(const void *src)
{
    T result = 0;
    const unsigned char *s = static_cast<const unsigned char *>(src);
    for (size_t i = 0; i < sizeof(T); ++i) {
        result = static_cast<T>((static_cast<std::uint64_t>(result) << 8) | s[i]);
    }
    return result;
}

template<typename T>
inline void ngToBigEndian(T src, void *dest)
{
    static_assert(sizeof(T) <= sizeof(std::uint64_t), "ngToBigEndian supports up to 8-byte types");
    unsigned char *d = static_cast<unsigned char *>(dest);
    // Promote through uint64_t so the >>= 8 below is well-defined for 1-byte T
    // (e.g. uint8_t), which would otherwise trigger -Wshift-count-overflow.
    std::uint64_t value = static_cast<std::uint64_t>(src);
    for (int i = static_cast<int>(sizeof(T)) - 1; i >= 0; --i) {
        d[i] = static_cast<unsigned char>(value & 0xFFu);
        value >>= 8;
    }
}

template<typename T>
inline T ngToBigEndian(T src)
{
    T result = 0;
    ngToBigEndian(src, &result);
    return result;
}

#define NG_DISABLE_COPY(Class) \
    Class(const Class &) = delete; \
    Class &operator=(const Class &) = delete;

#define NG_DECLARE_PRIVATE(Class) \
    friend class Class##Private; \
    inline Class##Private *d_func() \
    { \
        return reinterpret_cast<Class##Private *>(d_ptr); \
    } \
    inline const Class##Private *d_func() const \
    { \
        return reinterpret_cast<const Class##Private *>(d_ptr); \
    }

#define NG_D(Class) Class##Private * const d = d_func();
#define NG_Q(Class) Class * const q = q_func();

#define NG_DECLARE_PRIVATE_D(Ptr, Class) \
    inline Class##Private *d_func() \
    { \
        return reinterpret_cast<Class##Private *>(Ptr); \
    } \
    inline const Class##Private *d_func() const \
    { \
        return reinterpret_cast<const Class##Private *>(Ptr); \
    }

#if defined(_MSC_VER)
#  define NG_DEPRECATED __declspec(deprecated)
#elif defined(__GNUC__) || defined(__clang__)
#  define NG_DEPRECATED __attribute__((deprecated))
#else
#  define NG_DEPRECATED
#endif

#define NG_DECLARE_PUBLIC(Class) \
    inline Class *q_func() \
    { \
        return static_cast<Class *>(q_ptr); \
    } \
    inline const Class *q_func() const \
    { \
        return static_cast<const Class *>(q_ptr); \
    }

#define NG_GLOBAL_STATIC(TYPE, NAME) \
    static TYPE &NAME() \
    { \
        static TYPE instance; \
        return instance; \
    }

#define NG_GLOBAL_STATIC_WITH_ARGS(TYPE, NAME, ARGS) \
    static TYPE &NAME() \
    { \
        static TYPE instance ARGS; \
        return instance; \
    }

#endif  // QTNG_UTILS_PLATFORM_H
