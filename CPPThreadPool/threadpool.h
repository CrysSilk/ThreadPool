#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>

class Result;

// Any类型: 可以接收任意数据的类型
class Any
{
public:
    Any() = default;
    ~Any() = default;
    Any(const Any&) = delete;
    Any& operator=(const Any&) = delete;
    Any(Any&&) = default;
    Any& operator=(Any&&) = default;

    // 接收Any任意的其它数据类型
    template<typename T>
    Any(T data) : base_(std::make_unique<Derive<T>>(data))
    {}

    // 提取出Any存储的data数据类型
    template<typename T>
    T cast_()
    {
        // 如何从base_找到指向它的Derive对象
        // 基类指针 -> 派生类指针   RTTI
        Derive<T> *pd = dynamic_cast<Derive<T>*>(base_.get());
        if (pd == nullptr)
        {
            throw "type is unmatch!";
        }
        return pd->data_;
    }

private:
    // 基类类型
    class Base
    {
    public:
        virtual ~Base() = default;
    };

    // 派生类类型
    template<typename T>
    class Derive : public Base
    {
    public:
        Derive(T data) : data_(data)
        {}
        T data_;
    };

private:
    // 定义一个基类的指针
    std::unique_ptr<Base> base_;
};

// 实现一个信号量类
class Semaphore
{
public:
    Semaphore(int limit = 0) : resLimit_()
    {}

    ~Semaphore() = default;

    // 获取一个信号量资源
    void wait()
    {
        std::unique_lock<std::mutex> lock(mtx_);
        // 等待信号量有资源 没有资源的话 会阻塞当前线程
        cond_.wait(lock, [&]() -> bool { return resLimit_ > 0; });
        resLimit_--;
    }

    // 增加一个信号量资源
    void post()
    {
        std::unique_lock<std::mutex> lock(mtx_);
        resLimit_++;
        cond_.notify_all();
    }
private:
    size_t resLimit_;
    std::mutex mtx_;
    std::condition_variable cond_;
};

// 任务抽象基类
class Task
{
public:
    Task();

    ~Task() = default;

    void exec();

    void setResult(Result* res);

    // 用户可以自定义任意任务类型，从Task继承，重写run方法，实现自定义任务处理
    virtual Any run() = 0;

private:
    Result* result_;    // Result对象的生存周期 >>> Task
};

// 实现接收提交到线程池的task任务执行完后的返回值类型Result
class Result
{
public:
    Result(std::shared_ptr<Task> task, bool isValid = true);

    ~Result() = default;

    // No1.setVal方法 获取任务执行完的返回值
    void setVal(Any any);

    // No2.get方法 用户调用这个方法获取task的返回值
    Any get();

private:
    Any any_;                       // 存储信号的返回值
    Semaphore sem_;                 // 线程通信信号量
    std::shared_ptr<Task> task_;    // 指向对应获取返回值的任务对象
    std::atomic_bool isValid_;      // 返回值是否有效
};

// 线程池支持的模式
enum class PoolMode
{
    MODE_FIXED,     // 固定数量的线程
    MODE_CACHED,    // 线程数量可动态增长
};

// 线程类型
class Thread
{
public:
    using ThreadFunc = std::function<void()>;

    // 线程构造
    Thread(ThreadFunc func);

    // 线程析构
    ~Thread();

    // 启动线程
    void start();

private:
    ThreadFunc func_;
};

/*
example:
ThreadPool pool;
pool.start(4);

class MyTask : public Task
{
public:
    void run() { // 线程代码... }
};

pool.submitTask(std::make_shared<MyTask>());
*/

// 线程池类型
class ThreadPool
{
public:
    // 线程池构造
    ThreadPool();

    // 线程池析构
    ~ThreadPool();

    // 设置线程池的工作模式
    void setMode(PoolMode mode);

    // 设置task任务队列上限阈值
    void setTaskQueMaxThreshHold(size_t threshhold);

    // 给线程池提交任务
    Result submitTask(std::shared_ptr<Task> sp);

    // 开启线程池
    void start(int initThreadSize = 4);

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

private:
    // 定义线程函数
    void threadFunc();

private:
    std::vector<std::unique_ptr<Thread>> threads_;      // 线程列表
    size_t initThreadSize_;             // 初始的线程数量

    std::queue<std::shared_ptr<Task>> taskQue_; // 任务队列
    std::atomic_uint taskSize_;                 // 任务的数量
    size_t taskQueMaxThreshHold_;               // 任务队列数量上限阈值

    std::mutex taskQueMtx_;              // 保证任务队列的线程安全
    std::condition_variable notFull_;    // 表示任务队列不满
    std::condition_variable notEmpty_;   // 表示任务队列不空   

    PoolMode poolMode_;     // 当前线程池的工作模式
};

#endif