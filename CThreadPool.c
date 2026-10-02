#include "CThreadPool.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>

#define NUMBER 2  // 每次添加/销毁线程数

// 任务结构体
typedef struct Task
{
    void (*function)(void *arg);
    void *arg;
} Task;

// 线程池结构体
struct ThreadPool
{
    // 任务队列
    Task *taskQ;
    int queueCapacity;
    int queueSize;
    int queueFront;
    int queueRear;

    // 线程信息
    pthread_t managerID;
    pthread_t *threadIDs;
    int minNum;
    int maxNum;
    int busyNum;
    int liveNum;
    int exitNum;

    // 锁和条件变量
    pthread_mutex_t mutexpool;
    pthread_mutex_t mutexBusy;
    pthread_cond_t notFull;
    pthread_cond_t notEmpty;

    bool shutdown;
};

// 函数声明
void *worker(void *arg);
void *manager(void *arg);
void threadExit(ThreadPool *pool);

// 创建线程池
ThreadPool *threadPoolCreate(int min, int max, int queueSize)
{
    if (min <= 0 || max < min || queueSize <= 0)
        return NULL;

    ThreadPool *pool = calloc(1, sizeof(ThreadPool));
    if (pool == NULL)
        return NULL;

    pool->threadIDs = calloc(max, sizeof(pthread_t));
    pool->taskQ = malloc(sizeof(Task) * queueSize);

    if (pool->threadIDs == NULL || pool->taskQ == NULL)
        goto FAIL;

    pool->minNum = min;
    pool->maxNum = max;
    pool->queueCapacity = queueSize;

    // 初始化锁和条件变量
    if (pthread_mutex_init(&pool->mutexpool, NULL) != 0)
        goto FAIL;

    if (pthread_mutex_init(&pool->mutexBusy, NULL) != 0)
        goto FAIL_MUTEX_POOL;

    if (pthread_cond_init(&pool->notEmpty, NULL) != 0)
        goto FAIL_MUTEX_BUSY;

    if (pthread_cond_init(&pool->notFull, NULL) != 0)
        goto FAIL_COND_EMPTY;

    // 创建管理线程
    if (pthread_create(&pool->managerID, NULL, manager, pool) != 0)
        goto FAIL_COND_FULL;

    // 创建最小数量的工作线程
    for (int i = 0; i < min; ++i) {
        if (pthread_create(&pool->threadIDs[i],
                           NULL, worker, pool) != 0) {

            pthread_mutex_lock(&pool->mutexpool);
            pool->shutdown = true;
            pthread_cond_broadcast(&pool->notEmpty);
            pthread_mutex_unlock(&pool->mutexpool);

            pthread_join(pool->managerID, NULL);

            for (int j = 0; j < i; ++j)
                pthread_join(pool->threadIDs[j], NULL);

            goto FAIL_COND_FULL;
        }

        pool->liveNum++;
    }

    return pool;

FAIL_COND_FULL:
    pthread_cond_destroy(&pool->notFull);

FAIL_COND_EMPTY:
    pthread_cond_destroy(&pool->notEmpty);

FAIL_MUTEX_BUSY:
    pthread_mutex_destroy(&pool->mutexBusy);

FAIL_MUTEX_POOL:
    pthread_mutex_destroy(&pool->mutexpool);

FAIL:
    free(pool->threadIDs);
    free(pool->taskQ);
    free(pool);

    return NULL;
}

// 销毁线程池
int threadPoolDestroy(ThreadPool *pool)
{
    if (pool == NULL)
        return -1;

    pthread_mutex_lock(&pool->mutexpool);

    pool->shutdown = true;

    // 唤醒所有工作线程
    pthread_cond_broadcast(&pool->notEmpty);
    pthread_cond_broadcast(&pool->notFull);

    pthread_mutex_unlock(&pool->mutexpool);

    // 等待管理线程退出
    pthread_join(pool->managerID, NULL);

    // 等待工作线程退出
    for (int i = 0; i < pool->maxNum; ++i) {
        if (pool->threadIDs[i] != 0)
            pthread_join(pool->threadIDs[i], NULL);
    }

    // 销毁同步资源
    pthread_cond_destroy(&pool->notEmpty);
    pthread_cond_destroy(&pool->notFull);
    pthread_mutex_destroy(&pool->mutexpool);
    pthread_mutex_destroy(&pool->mutexBusy);

    free(pool->threadIDs);
    free(pool->taskQ);
    free(pool);

    return 0;
}

// 添加任务
void threadPoolAdd(ThreadPool *pool,
                    void (*func)(void *),
                    void *arg)
{
    if (pool == NULL || func == NULL)
        return;

    pthread_mutex_lock(&pool->mutexpool);

    // 队列满则等待
    while (pool->queueSize == pool->queueCapacity &&
           !pool->shutdown) {
        pthread_cond_wait(&pool->notFull, &pool->mutexpool);
    }

    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->mutexpool);
        return;
    }

    // 添加任务
    pool->taskQ[pool->queueRear].function = func;
    pool->taskQ[pool->queueRear].arg = arg;

    pool->queueRear =
        (pool->queueRear + 1) % pool->queueCapacity;

    pool->queueSize++;

    pthread_cond_signal(&pool->notEmpty);

    pthread_mutex_unlock(&pool->mutexpool);
}

// 获取工作线程数
int threadPoolBusyNum(ThreadPool *pool)
{
    if (pool == NULL)
        return -1;

    pthread_mutex_lock(&pool->mutexBusy);

    int busyNum = pool->busyNum;

    pthread_mutex_unlock(&pool->mutexBusy);

    return busyNum;
}

// 获取存活线程数
int threadPoolAliveNum(ThreadPool *pool)
{
    if (pool == NULL)
        return -1;

    pthread_mutex_lock(&pool->mutexpool);

    int liveNum = pool->liveNum;

    pthread_mutex_unlock(&pool->mutexpool);

    return liveNum;
}

// 工作线程
void *worker(void *arg)
{
    ThreadPool *pool = (ThreadPool *)arg;

    while (1) {
        pthread_mutex_lock(&pool->mutexpool);

        // 队列为空则等待
        while (pool->queueSize == 0 && !pool->shutdown) {
            pthread_cond_wait(&pool->notEmpty,
                              &pool->mutexpool);

            // 检查是否需要销毁线程
            if (pool->exitNum > 0 &&
                pool->queueSize == 0 &&
                pool->liveNum > pool->minNum) {

                pool->exitNum--;
                pool->liveNum--;

                pthread_mutex_unlock(&pool->mutexpool);

                threadExit(pool);
            }
        }

        // 检查线程池是否关闭
        if (pool->shutdown) {
            pool->liveNum--;

            pthread_mutex_unlock(&pool->mutexpool);

            threadExit(pool);
        }

        // 获取任务
        Task task = pool->taskQ[pool->queueFront];

        pool->queueFront =
            (pool->queueFront + 1) % pool->queueCapacity;

        pool->queueSize--;

        // 唤醒生产者
        pthread_cond_signal(&pool->notFull);

        pthread_mutex_unlock(&pool->mutexpool);

        // 执行任务
        pthread_mutex_lock(&pool->mutexBusy);
        pool->busyNum++;
        pthread_mutex_unlock(&pool->mutexBusy);

        task.function(task.arg);
        free(task.arg);

        pthread_mutex_lock(&pool->mutexBusy);
        pool->busyNum--;
        pthread_mutex_unlock(&pool->mutexBusy);
    }

    return NULL;
}

// 管理线程
void *manager(void *arg)
{
    ThreadPool *pool = (ThreadPool *)arg;

    while (1) {
        sleep(3);

        pthread_mutex_lock(&pool->mutexpool);

        // 检查线程池是否关闭
        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->mutexpool);
            break;
        }

        int queueSize = pool->queueSize;
        int liveNum = pool->liveNum;

        pthread_mutex_unlock(&pool->mutexpool);

        // 获取工作线程数
        pthread_mutex_lock(&pool->mutexBusy);
        int busyNum = pool->busyNum;
        pthread_mutex_unlock(&pool->mutexBusy);

        // 扩容
        if (queueSize > liveNum &&
            liveNum < pool->maxNum) {

            pthread_mutex_lock(&pool->mutexpool);

            int counter = 0;

            for (int i = 0;
                 i < pool->maxNum &&
                 counter < NUMBER &&
                 pool->liveNum < pool->maxNum;
                 ++i) {

                if (pool->threadIDs[i] == 0) {
                    if (pthread_create(&pool->threadIDs[i],
                                       NULL,
                                       worker,
                                       pool) == 0) {

                        pool->liveNum++;
                        counter++;
                    }
                }
            }

            pthread_mutex_unlock(&pool->mutexpool);
        }

        // 缩容
        if (busyNum * 2 < liveNum &&
            liveNum > pool->minNum) {

            pthread_mutex_lock(&pool->mutexpool);

            int n = pool->liveNum - pool->minNum;

            if (n > NUMBER)
                n = NUMBER;

            pool->exitNum = n;

            pthread_cond_broadcast(&pool->notEmpty);

            pthread_mutex_unlock(&pool->mutexpool);
        }
    }

    return NULL;
}

// 工作线程退出
void threadExit(ThreadPool *pool)
{
    pthread_t tid = pthread_self();

    pthread_mutex_lock(&pool->mutexpool);

    for (int i = 0; i < pool->maxNum; ++i) {
        if (pthread_equal(pool->threadIDs[i], tid)) {
            pool->threadIDs[i] = 0;
            break;
        }
    }

    pthread_mutex_unlock(&pool->mutexpool);

    pthread_exit(NULL);
}