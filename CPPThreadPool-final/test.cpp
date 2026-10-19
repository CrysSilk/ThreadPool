#include <iostream>
#include <chrono>

#include "threadpool.h"

int sum(int a, int b)
{
    return a + b;
}

int take(int a, int b)
{
    return a * b;
}

int main()
{
    ThreadPool pool;
    pool.start(2);

    std::future<bool> r7 = pool.submitTask([](int time) -> bool {
        std::this_thread::sleep_for(std::chrono::seconds(time));
        return true;
    }, 5);
    std::future<bool> r8 = pool.submitTask([](int time) -> bool {
        std::this_thread::sleep_for(std::chrono::seconds(time));
        return true;
    }, 6);
    std::future<int> r1 = pool.submitTask(sum, 1, 2);
    std::future<int> r2 = pool.submitTask(take, 5, 10);
    std::future<int> r3 = pool.submitTask(sum, 8, 41);
    std::future<int> r4 = pool.submitTask(sum, 22, 44);
    std::future<int> r5 = pool.submitTask(take, 52, 36);
    std::future<int> r6 = pool.submitTask(sum, 52, 36);
    

    std::cout << r1.get() << std::endl;
    std::cout << r2.get() << std::endl;
    std::cout << r3.get() << std::endl;
    std::cout << r4.get() << std::endl;
    std::cout << r5.get() << std::endl;
    std::cout << r6.get() << std::endl;
    std::cout << r7.get() << std::endl;
    std::cout << r8.get() << std::endl;
}