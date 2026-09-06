#ifndef QTNG_UTILS_STRING_UTILS_H
#define QTNG_UTILS_STRING_UTILS_H

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace qtng {
namespace utils {

// ByteBuffer: a cursor-based byte buffer for stream decoders that need to "peek"
// a region without consuming it, then consume a prefix later.
//
// This is the "least-move" form: consuming a prefix only advances a read offset
// (O(1)); a compact (which moves the unconsumed window to the front) runs
// lazily, only when the consumed prefix has grown past kCompactThreshold, so the
// amortized cost is O(1) per byte.
//
// Semantics (offsets below are relative to the current unconsumed head):
//   - available(): number of unconsumed bytes.
//   - read(offset, out, n): copy `n` bytes at `offset` WITHOUT consuming.
//   - consume(n): drop the first `n` unconsumed bytes (advance the offset). O(1),
//     except when it trips the compact threshold.
//   - append(data, n): append at the tail; compacts first if the offset is large.
//
// The buffer only grows when the tail really needs space beyond capacity; it
// never shrinks unless clear() is called, so there is no repeated malloc/free
// in steady state.
class ByteBuffer
{
public:
    explicit ByteBuffer(std::size_t /*capacity*/ = 0) {}

    std::size_t available() const { return m_buf.size() - m_off; }
    bool empty() const { return m_off >= m_buf.size(); }
    std::size_t capacity() const { return m_buf.capacity(); }

    // Copy `n` bytes at logical offset `offset` (from the unconsumed head) into
    // `out`. `offset + n` must be <= available(). Does not consume.
    void read(std::size_t offset, void *out, std::size_t n) const
    {
        if (n == 0) {
            return;
        }
        std::memcpy(out, m_buf.data() + m_off + offset, n);
    }

    // Drop the first `n` unconsumed bytes, advancing the read offset. O(1),
    // unless it trips the compact threshold (rare).
    void consume(std::size_t n)
    {
        const std::size_t c = std::min(n, available());
        m_off += c;
        maybeCompact();
    }

    // Append at the tail; compacts first if the consumed prefix is large.
    void append(const char *data, std::size_t n)
    {
        if (n == 0) {
            return;
        }
        maybeCompact();
        m_buf.append(data, n);
    }

    // Discard all bytes and snap the offset back to 0.
    void clear()
    {
        m_buf.clear();
        m_off = 0;
    }

private:
    static constexpr std::size_t kCompactThreshold = 64 * 1024;

    // Re-home the unconsumed window to the front when the consumed prefix has
    // grown large enough that continuing would waste memory or drift the offset.
    void maybeCompact()
    {
        if (m_off >= kCompactThreshold) {
            m_buf.erase(0, m_off);
            m_off = 0;
        }
    }

    std::string m_buf;
    std::size_t m_off = 0;
};

std::vector<std::string> split(const std::string &text, char separator);
std::vector<std::string> split(const std::string &text, const std::string &separator);
std::string trimmed(const std::string &text);
std::string toLower(const std::string &text);
std::string toUpper(const std::string &text);
bool startsWith(const std::string &text, const std::string &prefix);
bool endsWith(const std::string &text, const std::string &suffix);
std::string number(int value);
std::string number(long long value);
// Format an integer in the given base (base=16 is used for HTTP chunked block sizes, etc.).
std::string number(int value, int base);
std::string number(long long value, int base);
std::string number(double value, int precision = 6);
int parseInt(const std::string &text, bool *ok = nullptr);
long long parseLongLong(const std::string &text, bool *ok = nullptr);
float parseFloat(const std::string &text, bool *ok = nullptr);
double parseDouble(const std::string &text, bool *ok = nullptr);
std::string fromLatin1(const char *data, std::size_t size);
std::string join(const std::vector<std::string> &parts, const std::string &separator);
bool equalsIgnoreCase(const std::string &a, const std::string &b);
std::string htmlEscape(const std::string &text);
std::string formatMessage(const std::string &pattern, const std::vector<std::string> &args);
bool isValidUtf8(const std::string &text);
std::string toAce(const std::string &domain);
std::string fromAce(const std::string &domain);
std::string bytesToHex(const std::string &data);
std::string bytesToBase64(const std::string &data);
// Decode base64; whitespace and invalid characters in the input are tolerated and skipped, returning the
// decoded result (possibly empty).
std::string base64ToBytes(const std::string &data);
std::string join(const std::vector<std::string> &parts, char separator);

}  // namespace utils
}  // namespace qtng

#endif  // QTNG_UTILS_STRING_UTILS_H
