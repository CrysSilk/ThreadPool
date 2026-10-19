#include "threadpool.h"

#include <functional>
#include <thread>
#include <iostream>

/*----------- 线程池方法实现 -----------*/
const size_t TASK_MAX_THRESHHOLD    = 1024;
const size_t THREAD_MAX_THRESHHOLD  = 10;
const size_t THREAD_MAX_IDLE_TIME   = 10;   // 单位:s

// 线程池构造
ThreadPool::ThreadPool()
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
ThreadPool::~ThreadPool()
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
void ThreadPool::setMode(PoolMode mode)
{
    if (isPoolRunning_)
        return;
    poolMode_ = mode;
}

// 设置task任务队列上限阈值
void ThreadPool::setTaskQueMaxThreshHold(size_t threshhold)
{
    if (isPoolRunning_)
        return;
    taskQueMaxThreshHold_ = threshhold;
}

// 设置线程池cached模式下线程阈值
void ThreadPool::setThreadSizeThreshHold(size_t threshhold)
{
    if (isPoolRunning_)
        return;
    if (poolMode_ == PoolMode::MODE_CACHED)
    {
        threadSizeThreshHold_ = threshhold;
    }
}

// 给线程池提交任务     用户调用该接口，传入任务对象，生产任务
Result ThreadPool::submitTask(std::shared_ptr<Task> sp)
{
    // 1.提前创建返回值共享状态
    auto state = std::make_shared<ResultState>();

    // 2.在任务进入队列前 先绑定共享状态
    sp->setResult(state);

    // 获取锁
    std::unique_lock<std::mutex> lock(taskQueMtx_);

    // 线程的通信  等待任务队列有空余  wait   wait_for    wait_until
    // 用户提交任务 最长不能阻塞超过1s 否则判断提交任务失败 返回
    if (!notFull_.wait_for(lock, std::chrono::seconds(1), 
        [&]() -> bool { return taskQue_.size() < taskQueMaxThreshHold_ && isPoolRunning_; }))
    {
        // 表示notFull_等待1s后/线程池已经关闭
        std::cerr << "submit task failed: queue full or pool stopped." << std::endl;
        return Result(nullptr, false);
    }

    // 如果有空余 把任务放入任务队列中
    taskQue_.emplace(sp);
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
    return Result(state);
}

// 开启线程池  std::thread::hardware_concurrency(): 返回当前计算机CPU核心数量
void ThreadPool::start(size_t initThreadSize) 
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

// 定义线程函数  线程池的所有线程从任务队列里面消费任务
void ThreadPool::threadFunc(size_t threadid)
{
    // 当前线程开始时间
    auto lastTime = std::chrono::high_resolution_clock::now();
    for (;;)
    {
        std::shared_ptr<Task> task;

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
                task->exec();
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
bool ThreadPool::checkRunningState() const
{
    return isPoolRunning_;
}

/*----------- 线程方法实现 -----------*/
size_t Thread::generateId_ = 0;

Thread::Thread(ThreadFunc func)
    : func_(func)
    , threadId_(generateId_++)
{}

Thread::~Thread()
{}

// 启动线程
void Thread::start()
{
    // 创建一个线程来执行一个线程函数
    std::thread t(func_, threadId_);
    // 线程句柄立即分离 线程池通过活动线程计数等待其完成。
    t.detach();
}

// 获取线程ID
size_t Thread::getId() const
{
    return threadId_;
}

/*----------- Task方法实现 -----------*/
Task::Task()
    : resultState_(nullptr)
{}

void Task::exec()
{
    try
    {
        Any value = run();
        if (resultState_ != nullptr)
            resultState_->setVal(std::move(value));  // 这里发生多态调用
    }
    catch (...)
    {
        // 让结果状态在任务失败时也就绪 避免 Result::get() 永久等待
        if (resultState_ != nullptr)
            resultState_->setException(std::current_exception());
        else
            throw;
    }
}

void Task::setResult(std::shared_ptr<ResultState> state)
{
    resultState_ = std::move(state);
}

/*----------- Result方法实现 -----------*/
Result::Result(std::shared_ptr<ResultState> state, bool isValid)
    : state_(std::move(state))
    , isValid_(isValid)
{}

Any Result::get()
{
    if (!isValid_ || state_ == nullptr)
    {
        return "";
    }

    return state_->get();
}