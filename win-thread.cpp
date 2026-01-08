#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <windows.h>
#include <process.h>  // 用于 _beginthreadex

// 方式1: 使用 C++11 std::thread（推荐，跨平台）
namespace Cpp11Thread {
    std::mutex g_mutex;
    int g_counter = 0;

    // 线程函数
    void worker_thread(int thread_id, int iterations) {
        for (int i = 0; i < iterations; ++i) {
            // 使用互斥锁保护共享资源
            std::lock_guard<std::mutex> lock(g_mutex);
            g_counter++;
            std::cout << "Thread " << thread_id << " iteration " << i + 1
                      << ", counter = " << g_counter << std::endl;

            // 模拟一些工作
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void demo() {
        std::cout << "\n=== C++11 std::thread Example ===" << std::endl;
        g_counter = 0;

        const int num_threads = 3;
        const int iterations_per_thread = 5;
        std::vector<std::thread> threads;

        // 创建多个线程
        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back(worker_thread, i + 1, iterations_per_thread);
        }

        // 等待所有线程完成
        for (auto& t : threads) {
            t.join();
        }

        std::cout << "Final counter value: " << g_counter << std::endl;
    }
}

// 方式2: 使用 Windows API CreateThread
namespace WindowsAPI {
    HANDLE g_mutex_handle = NULL;
    int g_counter = 0;

    // 线程函数（必须是 DWORD WINAPI 格式）
    DWORD WINAPI worker_thread(LPVOID lpParam) {
        int thread_id = *(int*)lpParam;
        int iterations = 10;

        for (int i = 0; i < iterations; ++i) {
            // 等待互斥锁
            WaitForSingleObject(g_mutex_handle, INFINITE);

            g_counter++;
            std::cout << "Thread " << thread_id << " iteration " << i + 1
                      << ", counter = " << g_counter << std::endl;

            // 释放互斥锁
            ReleaseMutex(g_mutex_handle);

            // 模拟一些工作
            Sleep(100);
        }

        return 0;
    }

    void demo() {
        std::cout << "\n=== Windows API CreateThread Example ===" << std::endl;
        g_counter = 0;

        // 创建互斥锁
        g_mutex_handle = CreateMutex(NULL, FALSE, NULL);
        if (g_mutex_handle == NULL) {
            std::cerr << "Failed to create mutex" << std::endl;
            return;
        }

        const int num_threads = 3;
        std::vector<HANDLE> thread_handles;
        std::vector<int> thread_ids(num_threads);

        // 创建多个线程
        for (int i = 0; i < num_threads; ++i) {
            thread_ids[i] = i + 1;
            HANDLE hThread = CreateThread(
                NULL,                   // 安全属性
                0,                      // 栈大小（0表示使用默认）
                worker_thread,          // 线程函数
                &thread_ids[i],        // 传递给线程的参数
                0,                      // 创建标志（0表示立即运行）
                NULL                    // 线程ID（不需要）
            );

            if (hThread != NULL) {
                thread_handles.push_back(hThread);
            } else {
                std::cerr << "Failed to create thread " << i + 1 << std::endl;
            }
        }

        // 等待所有线程完成
        WaitForMultipleObjects(
            thread_handles.size(),
            thread_handles.data(),
            TRUE,                      // 等待所有线程
            INFINITE                   // 无限等待
        );

        // 关闭线程句柄
        for (HANDLE h : thread_handles) {
            CloseHandle(h);
        }

        // 关闭互斥锁句柄
        CloseHandle(g_mutex_handle);

        std::cout << "Final counter value: " << g_counter << std::endl;
    }
}

// 方式3: 使用 _beginthreadex（推荐用于 C 代码，更安全）
namespace BeginThreadEx {
    HANDLE g_mutex_handle = NULL;
    int g_counter = 0;

    // 线程函数（unsigned __stdcall 格式）
    unsigned __stdcall worker_thread(void* param) {
        int thread_id = *(int*)param;
        int iterations = 10;

        for (int i = 0; i < iterations; ++i) {
            WaitForSingleObject(g_mutex_handle, INFINITE);

            g_counter++;
            std::cout << "Thread " << thread_id << " iteration " << i + 1
                      << ", counter = " << g_counter << std::endl;

            ReleaseMutex(g_mutex_handle);
            Sleep(100);
        }

        return 0;
    }

    void demo() {
        std::cout << "\n=== _beginthreadex Example ===" << std::endl;
        g_counter = 0;

        g_mutex_handle = CreateMutex(NULL, FALSE, NULL);
        if (g_mutex_handle == NULL) {
            std::cerr << "Failed to create mutex" << '\n';
            return;
        }

        const int num_threads = 3;
        std::vector<HANDLE> thread_handles;
        std::vector<int> thread_ids(num_threads);

        for (int i = 0; i < num_threads; ++i) {
            thread_ids[i] = i + 1;
            HANDLE hThread = (HANDLE)_beginthreadex(
                NULL,                   // 安全属性
                0,                      // 栈大小
                worker_thread,          // 线程函数
                &thread_ids[i],        // 参数
                0,                      // 创建标志
                NULL                    // 线程ID
            );

            if (hThread != NULL) {
                thread_handles.push_back(hThread);
            }
        }

        WaitForMultipleObjects(thread_handles.size(), thread_handles.data(), TRUE, INFINITE);

        for (HANDLE h : thread_handles) {
            CloseHandle(h);
        }

        CloseHandle(g_mutex_handle);
        std::cout << "Final counter value: " << g_counter << '\n';
    }
}

// 高级示例：使用条件变量进行线程同步
namespace AdvancedSync {
    std::mutex mtx;
    std::condition_variable cv;
    bool ready = false;
    int data = 0;

    void producer() {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        std::lock_guard<std::mutex> lock(mtx);
        data = 42;
        ready = true;
        std::cout << "Producer: Data is ready" << std::endl;
        cv.notify_one();  // 通知等待的线程
    }

    void consumer() {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [] { return ready; });  // 等待条件满足
        std::cout << "Consumer: Received data = " << data << std::endl;
    }

    void demo() {
        std::cout << "\n=== Condition Variable Example (Producer-Consumer) ===" << std::endl;

        std::thread t1(producer);
        std::thread t2(consumer);

        t1.join();
        t2.join();
    }
}

int main() {
    std::cout << "Windows Multi-threading Implementation Examples\n" << std::endl;

    // 方式1: C++11 std::thread（推荐）
    //Cpp11Thread::demo();
    
    // 方式2: Windows API CreateThread
    //WindowsAPI::demo();

    // 方式3: _beginthreadex
    //BeginThreadEx::demo();

    // 高级示例：条件变量
    AdvancedSync::demo();

    return 0;
}

