#include <iostream>
#include <functional>
#include <vector>
#include <random>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>

#ifdef ENABLE_ATC
#include <atomic>
#include <chrono>
#endif

#include "quill-runtime.h"

using namespace std;

#define MAX_SZ 128
#define MAX_THREADS 128

#ifdef ENABLE_ATC
#define MAX_LEVELS 64
#define DEFAULT_MAX_LEVEL 4
#endif

extern "C" 
{
    void profiler_init();
    void profiler_finalize();
    double calculate_JPI();
}

namespace quill
{
    typedef struct 
    {
        int key;
    } thread_args;            // argument marshalling for pthread_create
    typedef struct
    {
        std::function<void()> fxn;
#ifdef ENABLE_ATC
        int level;
#endif
    } task;
    typedef struct
    {
        task *arr[MAX_SZ];         // array data structure implemented as deque
        volatile int head, tail;                             // two task counters for two ends
        // pthread_mutex_t deque_mutex;                
    } worker_deque;
    typedef struct
    {
        task *arr[MAX_SZ];
        volatile int cnt;
    } mail_box;
    typedef struct
    {
        volatile int id; 
        pthread_mutex_t request_mutex;
        pthread_cond_t request_cond;
    } request_box;
    volatile bool shutdown = 0;
    volatile int finish_counter = 0;
    volatile int quill_workers = 1;
    pthread_mutex_t finish_mutex = PTHREAD_MUTEX_INITIALIZER;          // Lock for updating the finish counter
    vector<pthread_t> tid;                                      // Thread ids for all the worker threads
    pthread_key_t thread_key;                 
    worker_deque *deques;                                      // All worker deques
    mail_box *mail_boxes;
    request_box *request_boxes;
    thread_args *args;
#ifdef ENABLE_ATC
    int *thread_level;
    std::atomic<int> total_ready_tasks;
    std::atomic<bool> level_closed[MAX_LEVELS];
    std::atomic<int> level_ready_tasks[MAX_LEVELS];
    std::atomic<long long> level_estimate[MAX_LEVELS];
#endif
    volatile int dop;
    pthread_t daemon_tid;
    volatile int *worker_sleep;
    pthread_mutex_t *worker_mutex;
    pthread_cond_t *worker_cond;
    int dop_direction = -1;
    int dct_N = 8;
    int fixed_interval = 50000;
    int prev_dop;

    void check_dop(int key)
    {
        if(worker_sleep[key])
        {
            pthread_mutex_lock(&worker_mutex[key]);
            while(worker_sleep[key] && !shutdown) pthread_cond_wait(&worker_cond[key], &worker_mutex[key]);
            pthread_mutex_unlock(&worker_mutex[key]);
        }
    }

    void configure_dop(double JPI_prev, double JPI_curr)
    {
        if(JPI_prev != 0 && JPI_curr > JPI_prev)
        {
            dop_direction = -1 * dop_direction;
            dop = prev_dop;
        }else if(JPI_prev != 0 && JPI_curr <= JPI_prev)
        {
            prev_dop = dop;
            dop += (dop_direction * dct_N);
        }else if(JPI_prev == 0)
        {
            prev_dop = dop;
            dop += (dop_direction * dct_N);
        }

        if(dop < 1) dop = 1;
        else if(dop > quill_workers) dop = quill_workers;

        cout<< "Current JPI - " << JPI_curr << ", dop updated to - " << dop <<endl;

        for(int i = 0; i < quill_workers; i++)
        {
            if(i < dop)
            {
                if(worker_sleep[i])
                {
                    worker_sleep[i] = 0;
                    pthread_mutex_lock(&worker_mutex[i]);
                    pthread_cond_signal(&worker_cond[i]);
                    pthread_mutex_unlock(&worker_mutex[i]);
                }
            }else worker_sleep[i] = 1;
        }
    }

    void *daemon_profiler(void *args)
    {
        usleep(100000);
        double JPI_prev = 0;
        int elapsed_time_ms = 0;       // For plot

        while(!shutdown)
        {
            double JPI_curr = calculate_JPI();
            configure_dop(JPI_prev, JPI_curr);
            JPI_prev = JPI_curr;

            printf("DOP_PLOT_DATA,%d,%d\n", elapsed_time_ms, dop);     // For plot

            usleep(fixed_interval);
            elapsed_time_ms += (fixed_interval / 1000);
        }

        for(int i = 0; i < quill_workers; i++)
        {
            worker_sleep[i] = 0;
            pthread_mutex_lock(&worker_mutex[i]);
            pthread_cond_broadcast(&worker_cond[i]);
            pthread_mutex_unlock(&worker_mutex[i]);
        }

        return NULL;
    }

    void check_request_box(int key)
    {
        if(request_boxes[key].id != -1)
        {
            pthread_mutex_lock(&request_boxes[key].request_mutex);
            int thief = request_boxes[key].id;
            if(thief != -1)
            {
                int num_tasks = (deques[key].tail - deques[key].head + MAX_SZ) % MAX_SZ;
                num_tasks /= 2;
                for(int i = 0; i < num_tasks; i++) 
                {
                    mail_boxes[thief].arr[i] = deques[key].arr[deques[key].head];
                    deques[key].head = (deques[key].head + 1) % MAX_SZ;
                }
                mail_boxes[thief].cnt = num_tasks;
                pthread_cond_signal(&request_boxes[key].request_cond);
                request_boxes[key].id = -1;
            }
            pthread_mutex_unlock(&request_boxes[key].request_mutex);
        }
    }

    void finalize_runtime()
    {
        shutdown = 1;                            // After this threads will stop spinning in the while loop while(!shutdown)
        pthread_join(daemon_tid, NULL);
        for(int i = 0; i < quill_workers - 1; i++) pthread_join(tid[i], NULL);
        pthread_key_delete(thread_key);
        profiler_finalize();
        free(deques);
        free(mail_boxes);
        free(request_boxes);
        free((void *)worker_sleep);
        free(worker_mutex);
        free(worker_cond);
        delete[] args;
    }

    void push_task_to_runtime(int key, task *t)
    {
        check_dop(key);
        check_request_box(key);
        if(((deques[key].tail + 1) % MAX_SZ) == deques[key].head)               // Check if deque is full
        {
            cout<< "Deque size exceeded" <<endl;
            exit(0);
        }
        deques[key].arr[deques[key].tail] = t;
        deques[key].tail = (deques[key].tail + 1) % MAX_SZ;
    }

    bool is_empty(worker_deque *dq) { return dq->head == dq->tail; }

    void steal_task(int key, int victim, task *&t)
    {
        check_dop(key);
        if(!is_empty(&deques[victim]))
        {
            pthread_mutex_lock(&request_boxes[victim].request_mutex);
            if(request_boxes[victim].id == -1)
            {
                request_boxes[victim].id = key;
                pthread_cond_wait(&request_boxes[victim].request_cond, &request_boxes[victim].request_mutex);
                if(mail_boxes[key].cnt != 0)
                {
                    t = mail_boxes[key].arr[0];
                    for(int i = 1; i < mail_boxes[key].cnt; i++)
                    {
                        deques[key].arr[deques[key].tail] = mail_boxes[key].arr[i];
                        deques[key].tail = (deques[key].tail + 1) % MAX_SZ;
                    }
                    mail_boxes[key].cnt = 0;
                }
            }
            pthread_mutex_unlock(&request_boxes[victim].request_mutex);
        }
    }

    void pop_task(int key, task *&t)
    {
        check_dop(key);
        check_request_box(key);
        if(!is_empty(&deques[key]))
        {
            deques[key].tail = ((deques[key].tail - 1) + MAX_SZ) % MAX_SZ;
            t = deques[key].arr[deques[key].tail];
        }
    }

    void find_and_execute_task()
    {
        int key = (int)(intptr_t)pthread_getspecific(thread_key);
        check_dop(key);
        task *t = nullptr;
        pop_task(key, t);
        if(t == nullptr && quill_workers > 1)                    // If own deque empty (no task found) then try stealing from someone else's deque
        {
            std::random_device rand_dev;
            std::mt19937 generator(rand_dev());
            std::uniform_int_distribution<int> distr(0, quill_workers - 2);
            int rnd = (int) distr(generator);                                        
            if(rnd >= key) rnd++;                                                 // This gives a random number between 0 and quill_workers - 1(both inclusive) and guarantees that it won't be equal to the worker's own index
            steal_task(key, rnd, t);
        }
        if(t != nullptr)
        {
#ifdef ENABLE_ATC
            total_ready_tasks--;
            level_ready_tasks[t->level]--;

            int prev = thread_level[key];
            thread_level[key] = t->level;
            
            auto start = std::chrono::high_resolution_clock::now();
#endif
            (t->fxn)();
#ifdef ENABLE_ATC
            auto end = std::chrono::high_resolution_clock::now();
            long long time_taken = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

            thread_level[key] = prev;
            long long estimate = level_estimate[t->level].load();
            if(estimate == -1) level_estimate[t->level].compare_exchange_strong(estimate, time_taken);
            else
            {
                long long est = (estimate * 3 + time_taken) / 4;
                level_estimate[t->level].compare_exchange_strong(estimate, est);
            }
            if(level_estimate[t->level].load() < (long long)1e6) level_closed[t->level].store(1);
#endif

            delete t;                                   // Free the memory as function was allocated on the heap
        
            pthread_mutex_lock(&finish_mutex);             // Thread safe updation of finish counter
            finish_counter--;
            pthread_mutex_unlock(&finish_mutex);
        }
    }

    void *worker_routine(void *ptr)
    {
        int x = ((thread_args *)ptr)->key;
        pthread_setspecific(thread_key, (void *)(intptr_t)x);           // Set everyone's unique key that they can retrieve later (0 for main thread)
        while(!shutdown) find_and_execute_task();
        return NULL;
    }

    void init_runtime()
    {
        char *env = getenv("QUILL_WORKERS");                        // Read QUILL_WORKERS environment variable and parse it if it is correctly an integer less than 256
        if(env)
        {
            char *endptr = nullptr;
            long cnt = strtol(env, &endptr, 10);
            if(cnt > 0) quill_workers = min((int)cnt, 256);

            char *env_n = getenv("DCT_N");
            if(env_n) dct_N = atoi(env_n);

            char *env_int = getenv("DCT_INTERVAL");
            if(env_int) fixed_interval = atoi(env_int);
        }
        dop = quill_workers;
        prev_dop = quill_workers;
        deques = (worker_deque *) malloc(quill_workers * sizeof(worker_deque));        // allocate memory dynamically for all the deques after knowing how many deques are to be created
        mail_boxes = (mail_box *) malloc(quill_workers * sizeof(mail_box));
        request_boxes = (request_box *) malloc(quill_workers * sizeof(request_box));
        worker_sleep = (volatile int *) malloc(quill_workers * sizeof(int));
        worker_mutex = (pthread_mutex_t *) malloc(quill_workers * sizeof(pthread_mutex_t));
        worker_cond = (pthread_cond_t *) malloc(quill_workers * sizeof(pthread_cond_t));
        tid.resize(quill_workers - 1);
        for(int i = 0; i < quill_workers; i++)
        {
            deques[i].head = deques[i].tail = 0;
            mail_boxes[i].cnt = 0;
            request_boxes[i].id = -1;
            request_boxes[i].request_mutex = PTHREAD_MUTEX_INITIALIZER;
            pthread_cond_init(&request_boxes[i].request_cond, NULL);
            worker_sleep[i] = 0;
            pthread_mutex_init(&worker_mutex[i], NULL);
            pthread_cond_init(&worker_cond[i], NULL);
        }
#ifdef ENABLE_ATC
        thread_level = (int *)malloc(quill_workers * sizeof(int));
        for(int i = 0; i < MAX_LEVELS; i++)
        {
            level_estimate[i].store(-1);
            level_closed[i].store(0);
            level_ready_tasks[i].store(0);
        }
        total_ready_tasks.store(0);
#endif
        pthread_key_create(&thread_key, NULL);
        if(quill_workers > 1) args = new thread_args[quill_workers - 1];
        for(int i = 0; i < quill_workers - 1; i++)
        {
            args[i].key = i + 1;                                                     // args contains the key to be set later for each thread
            int status = pthread_create(&tid[i], NULL, worker_routine, (void *) &args[i]);                     
            if(status != 0) { cout << "Error while thread creation" << endl;}
        }
        profiler_init();
        int status = pthread_create(&daemon_tid, NULL, daemon_profiler, NULL);
        if(status != 0) { cout << "Error while daemon thread creation" << endl; }
    }

    void start_finish() { finish_counter = 0; }

    void async(std::function<void()> &&lambda)
    {
        int key = (int)(intptr_t)pthread_getspecific(thread_key);
#ifdef ENABLE_ATC
        int level = thread_level[key] + 1;
        bool aggregate = 1;
        if(level < MAX_LEVELS && !level_closed[level].load())
        {
            long long estimate = level_estimate[level].load();
            if(estimate == -1 && level_ready_tasks[level].load() < 2 * quill_workers && level < DEFAULT_MAX_LEVEL) aggregate = 0;
            else if(estimate > (long long)1e6 && total_ready_tasks.load() < 4 * quill_workers) aggregate = 0;
        }

        if(!aggregate)
        {
            pthread_mutex_lock(&finish_mutex);
            finish_counter++;
            pthread_mutex_unlock(&finish_mutex);
            total_ready_tasks++;
            level_ready_tasks[level]++;
            task *t = new task{std::move(lambda), level};
            push_task_to_runtime(key, t);
        }else
        {
            int prev = thread_level[key];
            thread_level[key] = level;
            lambda();
            thread_level[key] = prev;
        }
#else
        pthread_mutex_lock(&finish_mutex);
        finish_counter++;
        pthread_mutex_unlock(&finish_mutex);
        
        task *t = new task{std::move(lambda)};
        push_task_to_runtime(key, t);
#endif
    }

    void end_finish() 
    { 
        int x = 0;
        pthread_setspecific(thread_key, (void *)(intptr_t)x);
        while(finish_counter != 0) find_and_execute_task(); 
    }
}
