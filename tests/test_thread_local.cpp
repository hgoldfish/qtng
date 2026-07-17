using namespace std;

#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <thread>
#include <vector>

#include "qtng/utils/thread_local.h"

using namespace qtng::utils;


TEST_CASE("ThreadLocal starts empty", "[thread_local]")
{
    ThreadLocal<int> storage;
    REQUIRE_FALSE(storage.hasLocalData());
}

TEST_CASE("ThreadLocal set and get", "[thread_local]")
{
    ThreadLocal<string> storage;
    storage.setLocalData("hello");
    REQUIRE(storage.hasLocalData());
    REQUIRE(storage.localData() == "hello");

    storage.localData() = "world";
    REQUIRE(storage.localData() == "world");
}

TEST_CASE("ThreadLocal clean resets state", "[thread_local]")
{
    ThreadLocal<int> storage;
    storage.setLocalData(99);
    storage.clean();
    REQUIRE_FALSE(storage.hasLocalData());
    REQUIRE(storage.localData() == 0);
}

TEST_CASE("ThreadLocal isolates per thread", "[thread_local]")
{
    ThreadLocal<int> storage;
    storage.setLocalData(1);

    atomic<bool> otherSeen{false};
    thread worker([&]() {
        REQUIRE_FALSE(storage.hasLocalData());
        storage.setLocalData(2);
        REQUIRE(storage.localData() == 2);
        otherSeen = true;
    });
    worker.join();

    REQUIRE(otherSeen);
    REQUIRE(storage.localData() == 1);
}

TEST_CASE("ThreadLocal const access", "[thread_local]")
{
    ThreadLocal<int> storage;
    storage.setLocalData(7);
    const ThreadLocal<int> &cstorage = storage;
    REQUIRE(cstorage.localData() == 7);
}

TEST_CASE("ThreadLocal concurrent writers", "[thread_local]")
{
    ThreadLocal<int> storage;
    storage.clean();
    constexpr int kThreads = 8;
    vector<thread> workers;
    atomic<int> ready{0};

    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([&, i]() {
            storage.setLocalData(i * 10);
            ready.fetch_add(1);
            REQUIRE(storage.localData() == i * 10);
        });
    }
    for (thread &worker : workers) {
        worker.join();
    }
    REQUIRE(ready == kThreads);
    REQUIRE(storage.localData() == 0);
}
