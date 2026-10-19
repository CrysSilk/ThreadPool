#include "threadpool.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if 0
// 测试任务：返回整数
class AddTask : public Task {
public:
    AddTask(int a, int b) : a_(a), b_(b) {}

    Any run() override {
        return a_ + b_;
    }

private:
    int a_;
    int b_;
};

// 测试任务：返回字符串
class StringTask : public Task {
public:
    explicit StringTask(std::string value) : value_(std::move(value)) {}

    Any run() override {
        return value_;
    }

private:
    std::string value_;
};

// 测试任务：执行时抛出异常，用于验证 Result::get() 能否重新抛出异常
class ThrowTask : public Task {
public:
    Any run() override {
        throw std::runtime_error("intentional test exception");
    }
};

// 测试任务：统计并发任务是否全部执行
class CounterTask : public Task {
public:
    explicit CounterTask(std::atomic<int>& counter) : counter_(counter) {}

    Any run() override {
        counter_.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }

private:
    std::atomic<int>& counter_;
};

static bool testReturnValues() {
    std::cout << "\n[TEST] task return values\n";

    ThreadPool pool;
    pool.start(3);

    auto intResult = pool.submitTask(std::make_shared<AddTask>(20, 22));
    auto stringResult =
        pool.submitTask(std::make_shared<StringTask>("thread-pool"));

    const int value = intResult.get().cast_<int>();
    const std::string text = stringResult.get().cast_<std::string>();

    const bool passed = (value == 42 && text == "thread-pool");
    std::cout << (passed ? "[PASS]" : "[FAIL]")
              << " integer result = " << value
              << ", string result = " << text << '\n';
    return passed;
}

static bool testConcurrentTasks() {
    std::cout << "\n[TEST] concurrent task execution\n";

    constexpr int taskCount = 100;
    std::atomic<int> completed{0};

    ThreadPool pool;
    pool.start(4);

    std::vector<Result> results;
    results.reserve(taskCount);

    for (int i = 0; i < taskCount; ++i) {
        results.emplace_back(
            pool.submitTask(std::make_shared<CounterTask>(completed)));
    }
    
    bool allResultsReady = true;
    for (auto& result : results) {
        try {
            (void)result.get().cast_<int>();
        } catch (...) {
            allResultsReady = false;
        }
    }

    const int actual = completed.load(std::memory_order_relaxed);
    const bool passed = allResultsReady && actual == taskCount;

    std::cout << (passed ? "[PASS]" : "[FAIL]")
              << " completed " << actual << " / " << taskCount
              << " tasks\n";
    return passed;
}

static bool testExceptionPropagation() {
    std::cout << "\n[TEST] task exception propagation\n";

    ThreadPool pool;
    pool.start(2);

    auto result = pool.submitTask(std::make_shared<ThrowTask>());

    try {
        (void)result.get();
        std::cout << "[FAIL] Result::get() did not throw\n";
        return false;
    } catch (const std::runtime_error& ex) {
        const bool passed =
            std::string(ex.what()) == "intentional test exception";
        std::cout << (passed ? "[PASS]" : "[FAIL]")
                  << " caught exception: " << ex.what() << '\n';
        return passed;
    } catch (...) {
        std::cout << "[FAIL] caught an unexpected exception type\n";
        return false;
    }
}

static bool testCachedMode() {
    std::cout << "\n[TEST] cached mode accepts and executes tasks\n";

    std::atomic<int> completed{0};

    ThreadPool pool;
    pool.setMode(PoolMode::MODE_CACHED);
    pool.setThreadSizeThreshHold(8);
    pool.start(2);

    constexpr int taskCount = 30;
    std::vector<Result> results;
    results.reserve(taskCount);

    for (int i = 0; i < taskCount; ++i) {
        results.emplace_back(
            pool.submitTask(std::make_shared<CounterTask>(completed)));
    }

    bool allResultsReady = true;
    for (auto& result : results) {
        try {
            (void)result.get().cast_<int>();
        } catch (...) {
            allResultsReady = false;
        }
    }

    const int actual = completed.load(std::memory_order_relaxed);
    const bool passed = allResultsReady && actual == taskCount;

    std::cout << (passed ? "[PASS]" : "[FAIL]")
              << " cached mode completed " << actual
              << " / " << taskCount << " tasks\n";
    return passed;
}

int main() {
    int passed = 0;
    int failed = 0;

    const auto run = [&](const char* name, bool (*test)()) {
        bool result = false;
        try {
            result = test();
        } catch (const std::exception& ex) {
            std::cout << "[FAIL] " << name
                      << " threw std::exception: " << ex.what() << '\n';
        } catch (...) {
            std::cout << "[FAIL] " << name
                      << " threw an unknown exception\n";
        }

        if (result) {
            ++passed;
        } else {
            ++failed;
        }
    };

    run("return values", testReturnValues);
    run("concurrent tasks", testConcurrentTasks);
    run("exception propagation", testExceptionPropagation);
    run("cached mode", testCachedMode);

    std::cout << "\n========== Test summary ==========\n"
              << "Passed: " << passed << '\n'
              << "Failed: " << failed << '\n';

    return failed == 0 ? 0 : 1;
}
#endif

// 测试任务：返回字符串
class StringTask : public Task {
public:
    explicit StringTask(std::string value) : value_(std::move(value)) {}

    Any run() override {
        return value_;
    }

private:
    std::string value_;
};

static bool testReturnValues() {
    std::cout << "\n[TEST] task return values\n";

    ThreadPool pool;
    pool.setMode(PoolMode::MODE_CACHED);
    pool.start(3);

    auto stringResult_1 =
        pool.submitTask(std::make_shared<StringTask>("thread-pool-1"));

    auto stringResult_2 =
        pool.submitTask(std::make_shared<StringTask>("thread-pool-2"));

    auto stringResult_3 =
        pool.submitTask(std::make_shared<StringTask>("thread-pool-3"));

    auto stringResult_4 =
        pool.submitTask(std::make_shared<StringTask>("thread-pool-4"));

    auto stringResult_5 =
        pool.submitTask(std::make_shared<StringTask>("thread-pool-5"));

    auto stringResult_6 =
        pool.submitTask(std::make_shared<StringTask>("thread-pool-6"));

    const std::string text_1 = stringResult_1.get().cast_<std::string>();
    const std::string text_2 = stringResult_2.get().cast_<std::string>();
    const std::string text_3 = stringResult_3.get().cast_<std::string>();


    const bool passed_1 = (text_1 == "thread-pool");
    std::cout << (passed_1 ? "[PASS]" : "[FAIL]")
              << "string result = " << text_1 << '\n';

    const bool passed_2 = (text_2 == "thread-pool");
    std::cout << (passed_2 ? "[PASS]" : "[FAIL]")
              << "string result = " << text_2 << '\n';
            
    const bool passed_3 = (text_3 == "thread-pool");
    std::cout << (passed_3 ? "[PASS]" : "[FAIL]")
              << "string result = " << text_3 << '\n';

    return passed_1 || passed_2 || passed_3;
}

int main()
{
    int passed = 0;
    int failed = 0;

    const auto run = [&](const char* name, bool (*test)()) {
        bool result = false;
        try {
            result = test();
        } catch (const std::exception& ex) {
            std::cout << "[FAIL] " << name
                        << " threw std::exception: " << ex.what() << '\n';
        } catch (...) {
            std::cout << "[FAIL] " << name
                        << " threw an unknown exception\n";
        }

        if (result) {
            ++passed;
        } else {
            ++failed;
        }
    };

    run("return values", testReturnValues);

    std::cout << "\n========== Test summary ==========\n"
            << "Passed: " << passed << '\n'
            << "Failed: " << failed << '\n';

    return failed == 0 ? 0 : 1;
}