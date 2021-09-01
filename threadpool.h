#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <thread>
#include <mutex>
#include <condition_variable>
#include <list>
#include <vector>
#include <exception>
#include <functional>
// #include <pthread.h>
#include <iostream>

// 线程池类，将它定义为模板类是为了代码复用，模板参数T是任务类
template <typename Task>
class threadPool 
{
public:
    /*threadNum是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadPool(int threadNum = 8, int max_requests = 10000);
    ~threadPool();
    bool addTask(Task* task);
private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    void threadFunc();
private:
    int m_threadNum;  // 工作线程的数量
    std::vector<std::shared_ptr<std::thread>> m_threads; // pthread_t * m_threads;  // 描述线程池的数组，大小为m_threadNum 
    int m_max_requests;     // 任务队列中最多允许的、等待处理的请求的数量
    std::list<std::shared_ptr<Task>> m_taskList; // std::list<Task*> m_taskList;   // 任务队列
    std::mutex m_mutex;    // 保护任务队列和条件变量的互斥锁 
    std::condition_variable m_cv;   // 是否有任务需要处理   
    bool m_stop;    // 是否结束线程                  
};

template <typename Task>
threadPool<Task>::threadPool(int threadNum, int max_requests) : 
        m_threadNum(threadNum), m_max_requests(max_requests), 
        m_stop(false) 
{
    if((threadNum <= 0) || (max_requests <= 0) ) 
        throw std::exception();

    // 创建thread_number 个线程，并将他们设置为脱离线程。
    for(int i = 0; i < threadNum; ++i)
    {
        std::cout << "create the " << i << "th thread" << std::endl;
        std::shared_ptr<std::thread> spThread;
        spThread.reset(new std::thread(std::bind(&threadPool::threadFunc, this)));
        if(!spThread)
            throw std::exception();
        m_threads.push_back(spThread);
        spThread->detach();
    }
}

template <typename Task>
threadPool<Task>::~threadPool() 
{
    m_stop = true;
    {
        std::unique_lock<std::mutex> guard(m_mutex);
        for(auto& iter : m_taskList)
            iter.reset();
        m_taskList.clear();
    }
}

template <typename Task>
bool threadPool<Task>::addTask(Task* task)
{
    std::shared_ptr<Task> spTask;
    spTask.reset(task);
    // 操作工作队列时一定要加锁，因为它被所有线程共享。
    {
        std::lock_guard<std::mutex> guard(m_mutex);   
        if (m_taskList.size() > m_max_requests) 
            return false;
        m_taskList.push_back(spTask);
    }
    m_cv.notify_one();
    return true;
}

template <typename Task>
void threadPool<Task>::threadFunc()
{
    std::shared_ptr<Task> spTask;
    while(1) 
    {
        {
            std::unique_lock<std::mutex> guard(m_mutex);
            while(m_taskList.empty()) 
            {
                if(m_stop)
                    break;
                m_cv.wait(guard);
            }
            if(m_stop)
                break;
            spTask = m_taskList.front();
            m_taskList.pop_front();
        }
        if (spTask == NULL) 
            continue;
        spTask->process();
        // spTask.reset();
    }
}

#endif // THREADPOOL_H
