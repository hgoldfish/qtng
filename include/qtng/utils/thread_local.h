#ifndef QTNG_UTILS_THREAD_LOCAL_H
#define QTNG_UTILS_THREAD_LOCAL_H

namespace qtng {
namespace utils {

template<typename T>
class ThreadLocal
{
    struct Storage {
        T value{};
        bool valid = false;
    };
public:
    bool hasLocalData() const { return storage().valid; }
    T &localData()
    {
        storage().valid = true;
        return storage().value;
    }
    const T &localData() const { return const_cast<ThreadLocal *>(this)->localData(); }
    void setLocalData(const T &data)
    {
        storage().value = data;
        storage().valid = true;
    }
    void clean()
    {
        storage().value = T();
        storage().valid = false;
    }
private:
    static Storage &storage()
    {
        thread_local Storage s;
        return s;
    }
};

}  // namespace utils
}  // namespace qtng

#endif  // QTNG_UTILS_THREAD_LOCAL_H
