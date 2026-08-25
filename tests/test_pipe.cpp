// qtng core 层 Pipe 的白盒多线程测试。
//
// 覆盖本轮对 src/io_utils.cpp 的修改：
//   - PipePrivate::closed 改为 std::atomic<bool>
//   - FileToRead::close() 恢复 1.0 的 drain + bytesWrittenCallback 语义
//   - readMore() 返回 bool 表达 EOF，FileToRead 用每 reader 的 eof 标志
//   - 写端 localBuffer 积累、读端字节级 takeBytes 优化路径
//
// 每个测试用例都启动独立的读写线程，验证数据完整性、EOF 边界与
// close 竞态行为。

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "qtng/io_utils.h"

using namespace qtng;

namespace {

// 写 payload 字节并在末尾 close，返回成功写入的字节数。
int64_t producerWriteAll(const std::shared_ptr<FileLike> &w, const char *data, int32_t size)
{
    int64_t written = 0;
    while (written < size) {
        int32_t n = w->write(data + written, size - static_cast<int32_t>(written));
        if (n <= 0) {
            break;
        }
        written += n;
    }
    w->close();
    return written;
}

}  // namespace

TEST_CASE("pipe-core: 多线程大块传输完整性", "[pipe][core][threads]")
{
    const int payloadBytes = 70 * 1024;
    std::vector<char> payload(payloadBytes, 'x');
    for (int iter = 0; iter < 200; ++iter) {
        auto pipe = std::make_shared<Pipe>(1024 * 1024 * 64);
        std::atomic<bool> start(false);
        std::atomic<int64_t> written(0), read(0);
        std::thread producer([&] {
            auto w = pipe->fileToWrite();
            while (!start.load()) {
                std::this_thread::yield();
            }
            int32_t n = w->write(payload.data(), payloadBytes);
            if (n > 0) {
                written.fetch_add(n);
            }
            w->close();
        });
        std::thread consumer([&] {
            auto r = pipe->fileToRead();
            std::vector<char> buf(64 * 1024);
            while (true) {
                int32_t n = r->read(buf.data(), static_cast<int32_t>(buf.size()));
                if (n <= 0) {
                    break;
                }
                read.fetch_add(n);
            }
            r->close();
        });
        start = true;
        producer.join();
        consumer.join();
        if (written.load() != read.load()) {
            FAIL("iter " << iter << " mismatch w=" << written.load() << " r=" << read.load());
        }
    }
}

TEST_CASE("pipe-core: 写端逐字节 + 读端大块", "[pipe][core][threads]")
{
    const int payloadBytes = 64 * 1024;
    auto pipe = std::make_shared<Pipe>(1024 * 1024);
    std::atomic<int64_t> written(0), read(0);
    std::atomic<int64_t> badBytes(0);
    std::thread producer([&] {
        auto w = pipe->fileToWrite();
        char c = 'a';
        for (int i = 0; i < payloadBytes; ++i) {
            int32_t n = w->write(&c, 1);
            if (n != 1) {
                break;
            }
            written.fetch_add(1);
        }
        w->close();
    });
    std::thread consumer([&] {
        auto r = pipe->fileToRead();
        std::vector<char> buf(64 * 1024);
        while (true) {
            int32_t n = r->read(buf.data(), static_cast<int32_t>(buf.size()));
            if (n <= 0) {
                break;
            }
            for (int32_t i = 0; i < n; ++i) {
                if (buf[static_cast<size_t>(i)] != 'a') {
                    badBytes.fetch_add(1);
                }
            }
            read.fetch_add(n);
        }
        r->close();
    });
    producer.join();
    consumer.join();
    REQUIRE(written.load() == payloadBytes);
    REQUIRE(read.load() == payloadBytes);
    REQUIRE(badBytes.load() == 0);
}

TEST_CASE("pipe-core: 写端大块 + 读端逐字节", "[pipe][core][threads]")
{
    const int payloadBytes = 64 * 1024;
    auto pipe = std::make_shared<Pipe>(1024 * 1024);
    std::atomic<int64_t> written(0), read(0);
    std::atomic<int64_t> badBytes(0);
    std::thread producer([&] {
        auto w = pipe->fileToWrite();
        std::vector<char> payload(payloadBytes, 'b');
        int32_t n = w->write(payload.data(), payloadBytes);
        if (n > 0) {
            written.fetch_add(n);
        }
        w->close();
    });
    std::thread consumer([&] {
        auto r = pipe->fileToRead();
        char c;
        while (true) {
            int32_t n = r->read(&c, 1);
            if (n <= 0) {
                break;
            }
            if (c != 'b') {
                badBytes.fetch_add(1);
            }
            read.fetch_add(1);
        }
        r->close();
    });
    producer.join();
    consumer.join();
    REQUIRE(written.load() == payloadBytes);
    REQUIRE(read.load() == payloadBytes);
    REQUIRE(badBytes.load() == 0);
}

TEST_CASE("pipe-core: writer 先 close，reader 之后读完并正确到达 EOF", "[pipe][core][threads]")
{
    const int payloadBytes = 100 * 1024;
    auto pipe = std::make_shared<Pipe>(1024 * 1024 * 4);
    std::atomic<int> writeErrors(0);
    std::thread producer([&] {
        auto w = pipe->fileToWrite();
        std::vector<char> payload(payloadBytes, 'c');
        int32_t n = w->write(payload.data(), payloadBytes);
        if (n != payloadBytes) {
            writeErrors.fetch_add(1);
        }
        // 写完立即 close：EOF sentinel 与 closed 标志的翻转顺序是竞态关键路径。
        w->close();
    });
    int64_t total = 0;
    {
        auto r = pipe->fileToRead();
        std::vector<char> buf(64 * 1024);
        while (true) {
            int32_t n = r->read(buf.data(), static_cast<int32_t>(buf.size()));
            if (n <= 0) {
                break;
            }
            total += n;
        }
        // FileLike 语义：管道关闭且数据耗尽后 read 返回非正数（0 或 -1），
        // sendfile 等调用方用 `<= 0` 判断终止。
        char c;
        REQUIRE(r->read(&c, 1) <= 0);
        r->close();
    }
    producer.join();
    REQUIRE(writeErrors.load() == 0);
    REQUIRE(total == payloadBytes);
}

TEST_CASE("pipe-core: reader 提前 close 时 drain 队列并通过 bytesWrittenCallback 确认", "[pipe][core][threads]")
{
    const int payloadBytes = 100 * 1024;
    auto pipe = std::make_shared<Pipe>(1024 * 1024 * 4);
    std::atomic<int64_t> confirmed(0);
    pipe->setBytesWrittenCallback([&confirmed](int64_t bytes) {
        confirmed.fetch_add(bytes);
    });
    std::atomic<bool> writerDone(false);
    std::atomic<int> writeErrors(0);
    std::thread producer([&] {
        auto w = pipe->fileToWrite();
        std::vector<char> payload(payloadBytes, 'd');
        int32_t n = w->write(payload.data(), payloadBytes);
        if (n != payloadBytes) {
            writeErrors.fetch_add(1);
        }
        // 等 writer 的 localBuffer 全部 flush 进队列再通知 reader。
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        writerDone = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        w->close();
    });
    while (!writerDone.load()) {
        std::this_thread::yield();
    }
    {
        auto r = pipe->fileToRead();
        std::vector<char> buf(1024);
        int32_t n = r->read(buf.data(), static_cast<int32_t>(buf.size()));
        REQUIRE(n == 1024);
        // reader 提前关闭：未消费的 100KB-1KB 必须被 drain 并上报。
        r->close();
    }
    producer.join();
    REQUIRE(writeErrors.load() == 0);
    REQUIRE(confirmed.load() >= payloadBytes - 1024);
}

TEST_CASE("pipe-core: 写端写一半后 close，剩余 localBuffer 与 EOF 不丢", "[pipe][core][threads]")
{
    const int payloadBytes = 64 * 1024;
    auto pipe = std::make_shared<Pipe>(1024 * 1024 * 4);
    std::atomic<int> writeErrors(0);
    std::thread producer([&] {
        auto w = pipe->fileToWrite();
        std::vector<char> payload(payloadBytes, 'e');
        // 逐块写，确保有部分数据积压在 writer 的 localBuffer 里。
        for (int i = 0; i < payloadBytes; i += 1024) {
            int32_t n = w->write(payload.data() + i, 1024);
            if (n != 1024) {
                writeErrors.fetch_add(1);
                break;
            }
        }
        w->close();
    });
    int64_t total = 0;
    {
        auto r = pipe->fileToRead();
        std::vector<char> buf(64 * 1024);
        while (true) {
            int32_t n = r->read(buf.data(), static_cast<int32_t>(buf.size()));
            if (n <= 0) {
                break;
            }
            total += n;
        }
        r->close();
    }
    producer.join();
    REQUIRE(writeErrors.load() == 0);
    REQUIRE(total == payloadBytes);
}
