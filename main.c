#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#include "CThreadPool.h"

void taskFunc(void *arg)
{
    int num = *(int *)arg;

    printf("thread tid = %lu is working, number = %d\n",
           (unsigned long)pthread_self(),
           num);

    usleep(1000);
}

int main(void)
{
    // 创建线程池
    ThreadPool *pool =
        threadPoolCreate(3, 10, 100);

    if (pool == NULL) {
        fprintf(stderr, "threadPoolCreate failed\n");
        return 1;
    }

    // 添加 100 个任务
    for (int i = 0; i < 100; ++i) {

        int *num = malloc(sizeof(int));

        if (num == NULL) {
            fprintf(stderr, "malloc failed\n");
            break;
        }

        *num = i + 100;

        threadPoolAdd(pool, taskFunc, num);
    }

    // 等待任务执行
    sleep(30);

    // 销毁线程池
    threadPoolDestroy(pool);

    return 0;
}