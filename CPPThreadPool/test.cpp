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

using ULong = unsigned long long;

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
        
        ULong sum = 0;
        for (ULong i = begin_; i <= end_; ++i)
            sum += i;

        std::cout << "tid: " << std::this_thread::get_id() << " end!" << std::endl;

        return sum;
    }

private:
    ULong begin_;
    ULong end_;
};


int main()
{
    ThreadPool pool;
    pool.start(4);

    // No2.如何设计这里的Result机制
    Result res1 = pool.submitTask(std::make_shared<MyTask>(1, 100000));
    Result res2 = pool.submitTask(std::make_shared<MyTask>(100001, 200000));
    Result res3 = pool.submitTask(std::make_shared<MyTask>(200001, 300000));

    // get返回一个Any类型 怎么转成具体的类型呢
    // 随着task被执行完 task对象没了 依赖于task对象的Result对象也没了
    ULong sum1 = res1.get().cast_<ULong>();
    ULong sum2 = res2.get().cast_<ULong>();
    ULong sum3 = res3.get().cast_<ULong>();

    // Master - Slave线程模型
    // Master线程用来分解任务 然后给各个Salve线程分配任务
    // 等待各个Slave线程线程执行完任务 返回结果
    // Master线程合并各个任务结果 输出
    std::cout << (sum1 + sum2 + sum3) << std::endl;
    
    // pool.submitTask(std::make_shared<MyTask>());
    // pool.submitTask(std::make_shared<MyTask>());
    // pool.submitTask(std::make_shared<MyTask>());
    // pool.submitTask(std::make_shared<MyTask>());
    // pool.submitTask(std::make_shared<MyTask>());
    // pool.submitTask(std::make_shared<MyTask>());
    // pool.submitTask(std::make_shared<MyTask>());
    // pool.submitTask(std::make_shared<MyTask>());
    // pool.submitTask(std::make_shared<MyTask>());

    getchar();
}