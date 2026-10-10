#include <iostream>
#include <chrono>
#include <thread>

#include "threadpool.h"

/*
有些场景需要获取线程执行任务得到的返回值
举例:
1 + .... + 10000
thread1 1 + ..... + 1000
thread2 1001 + ..... + 2000
.......

main thread: 给每一个线程分配计算的区间 并等待他们算完返回结构 合并最终的结果即可 
*/

class MyTask : public Task
{
public:
    MyTask(int begin, int end)
        : begin_(begin)
        , end_(end)
    {}

    // No1.怎么设计run函数的返回值 可以表示任意的类型
    // Java Python    Object 是所有其他类类型的基类
    // C++17 Any类型
    Any run()
    {
        std::cout << "tid: " << std::this_thread::get_id() << " begin!" << std::endl;
        
        int sum = 0;
        for (int i = begin_; i <= end_; ++i)
            sum += i;

        std::cout << "tid: " << std::this_thread::get_id() << " end!" << std::endl;

        return sum;
    }

private:
    int begin_;
    int end_;
};


int main()
{
    ThreadPool pool;
    pool.start(4);

    // No2.如何设计这里的Result机制
    Result res = pool.submitTask(std::make_shared<MyTask>());

    // get返回一个Any类型 怎么转成具体的类型呢
    int sum = res.get(),cast_<int>();
    
    pool.submitTask(std::make_shared<MyTask>());
    pool.submitTask(std::make_shared<MyTask>());
    pool.submitTask(std::make_shared<MyTask>());
    pool.submitTask(std::make_shared<MyTask>());
    pool.submitTask(std::make_shared<MyTask>());
    pool.submitTask(std::make_shared<MyTask>());
    pool.submitTask(std::make_shared<MyTask>());
    pool.submitTask(std::make_shared<MyTask>());
    pool.submitTask(std::make_shared<MyTask>());


    getchar();
}