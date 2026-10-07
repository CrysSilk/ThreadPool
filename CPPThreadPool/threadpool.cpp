#include "threadpool.h"

#include <functional>
#include <thread>
#include <iostream>

/*----------- 线程池方法实现 -----------*/
const int TASK_MAX_THRESHHOLD = 1024;

// 线程池构造
ThreadPool::ThreadPool()
    : initThreadSize_(0)
    , taskSize_(0)
    , taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
    , poolMode_(PoolMode::MODE_FIXED)
{}

// 线程池析构
ThreadPool::~ThreadPool()
{}

// 设置线程池的工作模式
void ThreadPool::setMode(PoolMode mode)
{
    poolMode_ = mode;
}

// 设置task任务队列上限阈值
void ThreadPool::setTaskQueMaxThreshHold(size_t threshhold)
{
    taskQueMaxThreshHold_ = threshhold;
}

// 给线程池提交任务     用户调用该接口，传入任务对象，生产任务
void ThreadPool::submitTask(std::shared_ptr<Task> sp)
{

}

// 开启线程池
void ThreadPool::start(int initThreadSize)
{
    // 记录初始线程个数
    initThreadSize_ = initThreadSize;

    // 创建线程对象
    for (int i = 0; i < initThreadSize_; ++i)
    {
        // 创建thread线程对象的时候，把线程函数给到thread线程对象
        threads_.emplace_back(new Thread(std::bind(&ThreadPool::threadFunc, this)));
    }

    // 启动所有线程
    for (int i = 0; i < initThreadSize_; ++i)
    {
        threads_[i]->start();   // 需要去执行一个线程函数
    }
}

// 定义线程函数  线程池的所有线程从任务队列里面消费任务
void ThreadPool::threadFunc()
{
    std::cout << "begin threadFunc tid:" << std::this_thread::get_id() << std::endl;
    std::cout << "end threadFunc tid:" << std::this_thread::get_id() << std::endl;
}

/*----------- 线程方法实现 -----------*/
Thread::Thread(ThreadFunc func)
    : func_(func)
{}

Thread::~Thread()
{}

// 启动线程
void Thread::start()
{
    // 创建一个线程来执行一个线程函数
    std::thread t(func_);  // C++11来说，线程对象t和线程函数func_
    t.detach();            // 设置分离线程
}
