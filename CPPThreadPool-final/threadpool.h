#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <iostream>
#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>
#include <exception>
#include <thread>
#include <future>

/*----------- 线程池方法实现 -----------*/
const size_t TASK_MAX_THRESHHOLD    = 4;
const size_t THREAD_MAX_THRESHHOLD  = 10;
const size_t THREAD_MAX_IDLE_TIME   = 10;   // 单位:s

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
    using ThreadFunc = std::function<void(size_t)>;

    // 线程构造
    Thread(ThreadFunc func)
        : func_(func)
        , threadId_(generateId_++)
    {}

    // 线程析构
    ~Thread() = default;

    // 启动线程
    void start()
    {
        // 创建一个线程来执行一个线程函数
        std::thread t(func_, threadId_);
        // 线程句柄立即分离 线程池通过活动线程计数等待其完成。
        t.detach();
    }

    // 获取线程id
    size_t getId() const
    {
        return threadId_;
    }

private:
    ThreadFunc func_;
    inline static size_t generateId_ = 0;
    size_t threadId_;   // 保存线程id
};

// 线程池类型
class ThreadPool
{
public:
    // 线程池构造
    ThreadPool()
        : initThreadSize_(0)
        , curThreadSize_(0)
        , threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
        , idleThreadSize_(0)
        , taskSize_(0)
        , taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
        , poolMode_(PoolMode::MODE_FIXED)
        , isPoolRunning_(false)
    {}

    // 线程池析构
    ~ThreadPool()
    {
        {
            // 用任务队列锁同步停止标志与工作线程退出检查。
            std::lock_guard<std::mutex> lock(taskQueMtx_);
            isPoolRunning_ = false;
        }

        // 唤醒等待任务的线程
        notEmpty_.notify_all();

        // 等待所有工作线程在锁内注销后再析构线程池成员
        std::unique_lock<std::mutex> lock(taskQueMtx_);
        exitCond_.wait(lock, [this]() -> bool { return curThreadSize_ == 0; });
    }

    // 设置线程池的工作模式
    void setMode(PoolMode mode)
    {
        if (isPoolRunning_)
            return;
        poolMode_ = mode;
    }

    // 设置task任务队列上限阈值
    void setTaskQueMaxThreshHold(size_t threshhold)
    {
        if (isPoolRunning_)
            return;
        taskQueMaxThreshHold_ = threshhold;
    }

    // 设置线程池cached模式下线程阈值
    void setThreadSizeThreshHold(size_t threshhold)
    {
        if (isPoolRunning_)
            return;
        if (poolMode_ == PoolMode::MODE_CACHED)
        {
            threadSizeThreshHold_ = threshhold;
        }
    }

    // 给线程池提交任务
    // 使用可变参模板 让submitTask可以接收任意函数和任意数量的参数
    template<typename Func, typename... Args>
    auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>
    {
        // 打包任务 放入任务队列
        using RType = decltype(func(args...));
        auto task = std::make_shared<std::packaged_task<RType()>>(
            std::bind(std::forward<Func>(func), std::forward<Args>(args)...)
        );

        std::future<RType> result = task->get_future();

        // 获取锁
        std::unique_lock<std::mutex> lock(taskQueMtx_);

        // 用户提交任务 最长不能阻塞超过1s 否则判断提交任务失败 返回
        if (!notFull_.wait_for(lock, std::chrono::seconds(1), 
            [&]() -> bool { return taskQue_.size() < taskQueMaxThreshHold_ && isPoolRunning_; }))
        {
            // 表示notFull_等待1s后/线程池已经关闭
            std::cerr << "submit task failed: queue full or pool stopped." << std::endl;

            auto task = std::make_shared<std::packaged_task<RType()>>(
                []() -> RType { return RType(); }
            );
            (*task)();
            return task->get_future();
        }

        // 如果有空余 把任务放入任务队列中
        taskQue_.emplace([task]() { (*task)(); });
        taskSize_++;

        // 因为新放了任务 任务队列肯定不为空 在notEmpty_通知 分配任务执行线程
        notEmpty_.notify_all();

        // Cached模式No2.需要根据任务数量和空闲线程的数量 判断是否需要新的线程出来
        // 处理场景：处理任务比较紧急 适合小而快的任务
        if (poolMode_ == PoolMode::MODE_CACHED 
            && taskSize_ > idleThreadSize_ 
            && curThreadSize_ < threadSizeThreshHold_)
        {        
            std::cout << ">>> create new thread <<<" << std::endl;

            // 创建新的线程对象
            std::unique_ptr<Thread> ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
            size_t threadId = ptr->getId();
            auto threadIt = threads_.emplace(threadId, std::move(ptr)).first;
            // 修改线程相关的线程对象
            curThreadSize_++;
            idleThreadSize_++;
            threadIt->second->start();      // 启动线程
        }

        // 返回任务的Result对象
        return result;
    }

    // 开启线程池
    void start(size_t initThreadSize = std::thread::hardware_concurrency()) 
    {
        // 设置线程池的运行状态
        isPoolRunning_ = true;

        // 记录初始线程个数
        initThreadSize_ = initThreadSize;
        curThreadSize_ = initThreadSize;

        // 创建线程对象
        for (int i = 0; i < initThreadSize_; ++i)
        {
            // 创建thread线程对象的时候，把线程函数给到thread线程对象
            std::unique_ptr<Thread> ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
            size_t threadId = ptr->getId();
            threads_.emplace(threadId, std::move(ptr));
        }

        // 启动所有线程
        std::unique_lock<std::mutex> lock(taskQueMtx_);
        for (auto& entry : threads_)
        {
            entry.second->start();
            ++idleThreadSize_;
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

private:
    // 定义线程函数
    void threadFunc(size_t threadid)
    {
        // 当前线程开始时间
        auto lastTime = std::chrono::high_resolution_clock::now();
        for (;;)
        {
            Task task;

            {
                // 先获取锁
                std::unique_lock<std::mutex> lock(taskQueMtx_);

                std::cout << "tid: " << std::this_thread::get_id() << "尝试获取任务..." << std::endl;

                // Cached模式No3.如何在设定的空闲时间超时情况下 把多余的线程进行回收
                // 结束回收线程 (超过initThreadSize_数量的线程要进行回收)
                while (taskQue_.empty() && isPoolRunning_)
                {
                    if (poolMode_ == PoolMode::MODE_CACHED) {
                        // 每一秒钟返回一次     怎么区分: 超时返回？任务待返回
                        /*
                        条件变量超时返回
                        enum class cv_status { no_timeout, timeout };
                        */
                        const bool awakened = notEmpty_.wait_for(
                            lock,
                            std::chrono::seconds(1),
                            [this]() -> bool { return !taskQue_.empty() || !isPoolRunning_; });
                        if (!awakened)
                        {
                            auto now = std::chrono::high_resolution_clock::now();
                            auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
                            if (taskQue_.empty()
                                && isPoolRunning_
                                && dur.count() >= THREAD_MAX_IDLE_TIME
                                && curThreadSize_ > initThreadSize_)
                            {
                                /*
                                开始回收当前线程
                                记录线程数量相关变量的值
                                修改：detach 模式下注销 Thread 包装对象，但不能 join 自己。
                                */
                                threads_.erase(threadid);
                                curThreadSize_--;
                                idleThreadSize_--;

                                std::cout << "threadid: " << std::this_thread::get_id() << " exit!" << std::endl;
                                // 修改：通知析构函数重新检查活动线程计数。
                                exitCond_.notify_all();
                                return;
                            }
                        }
                    } else {
                        // 等待notEmpty条件
                        notEmpty_.wait(lock, [&]() -> bool { return !taskQue_.empty() || !isPoolRunning_; });
                    }
                }

                // 停止后队列清空时 工作线程在锁内注销并通知析构函数
                if (taskQue_.empty() && !isPoolRunning_)
                {
                    threads_.erase(threadid);
                    --curThreadSize_;
                    --idleThreadSize_;
                    std::cout << "threadid: " << std::this_thread::get_id() << " exit!" << std::endl;
                    exitCond_.notify_all();
                    return;
                }

                idleThreadSize_--;

                // 从任务队列中取一个任务出来
                task = taskQue_.front();
                taskQue_.pop();
                --taskSize_;

                std::cout << "tid: " << std::this_thread::get_id() << "获取任务成功!" << std::endl;

                // 如果依然有剩余任务 继续通知其它线程执行任务
                if (taskQue_.size() > 0)
                {
                    notEmpty_.notify_all();
                }
                
                // 取出一个任务进行通知
                notFull_.notify_all();
            }   // 出作用域自动释放锁

            // 当前线程负责执行这个任务
            if (task != nullptr) {
                // task->run();    // 执行任务: 把任务的返回值setVal方法给到Result
                // 修改：兜底捕获任务之外的异常，避免异常逃出 detached 线程入口导致 terminate。
                try
                {
                    task();
                }
                catch (const std::exception& ex)
                {
                    std::cerr << "task execution failed: " << ex.what() << std::endl;
                }
                catch (...)
                {
                    std::cerr << "task execution failed with an unknown exception." << std::endl;
                }
            }

            lastTime = std::chrono::high_resolution_clock::now();     // 更新线程执行完任务的时间
            // 空闲线程计数与提交任务读取使用同一把锁保护。
            {
                std::lock_guard<std::mutex> lock(taskQueMtx_);
                ++idleThreadSize_;
            }
        }
    }

    // 检查pool的运行状态
    bool checkRunningState() const
    {
        return isPoolRunning_;
    }

private:
    // std::vector<std::unique_ptr<Thread>> threads_;      // 线程列表
    std::unordered_map<size_t, std::unique_ptr<Thread>> threads_; // 线程列表
    size_t initThreadSize_;                                     // 初始的线程数量
    std::atomic_uint curThreadSize_;                            // 当前线程池里的线程总数量
    size_t threadSizeThreshHold_;                               // 线程数量上限阈值
    std::atomic_uint idleThreadSize_;                           // 空闲线程的数量

    // Task任务 -> 函数对象
    using Task = std::function<void()>;
    std::queue<Task> taskQue_; // 任务队列
    std::atomic_uint taskSize_;                 // 任务的数量
    size_t taskQueMaxThreshHold_;               // 任务队列数量上限阈值

    std::mutex taskQueMtx_;              // 保证任务队列的线程安全
    std::condition_variable notFull_;    // 表示任务队列不满
    std::condition_variable notEmpty_;   // 表示任务队列不空   
    std::condition_variable exitCond_;   // 等待线程资源全部回收

    PoolMode poolMode_;                 // 当前线程池的工作模式
    std::atomic_bool isPoolRunning_;    // 表示当前线程池的启动状态
};

#endif