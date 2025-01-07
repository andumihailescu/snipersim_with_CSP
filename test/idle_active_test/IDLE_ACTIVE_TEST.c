/******************************************************************************
 * Custom "Active-Idle" benchmark, structured similarly to the Splash2 FFT code
 * so that Sniper treats it as a single application (generating a single trace).
 *
 * This benchmark spawns num_threads - 1 worker threads. The main thread acts
 * as thread 0 (so the total is num_threads threads), each alternating between:
 *   - A 1-second "busy" period (with CPU-bound branching),
 *   - Followed by a 1-second "idle" period (with light branching).
 *
 * The total run time is controlled by -t<seconds>.
 * The number of threads is controlled by -p<threads>.
 * 
 * It uses a global barrier (like FFT) and Sniper markers so that the simulator
 * sees a single ROI region, as is done in the FFT example.
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <string.h>
#include "sim_api.h"

/******************************************************************************
 * Global data structures & barrier, imitating the FFT example
 ******************************************************************************/
typedef struct {
    // Barrier structure
    struct {
        pthread_mutex_t mutex;
        pthread_cond_t  cv;
        unsigned long counter;
        unsigned long cycle;
    } start;
    
    // We can track start/finish times as done in FFT if desired
    uint64_t starttime;
    uint64_t finishtime;
} GlobalMemory;

static GlobalMemory *Global;

/******************************************************************************
 * Command-line options
 ******************************************************************************/
static int num_threads = 1;
static int total_seconds = 10;
static volatile int should_exit = 0;

/******************************************************************************
 * Barrier implementation (same as in FFT)
 ******************************************************************************/
static void Barrier(GlobalMemory *Global, int num_threads)
{
    unsigned long Error, Cycle;
    int Temp, Cancel;
    
    Error = pthread_mutex_lock(&(Global->start.mutex));
    if (Error != 0) {
        fprintf(stderr, "Error while trying to lock in barrier.\n");
        exit(-1);
    }
    
    Cycle = Global->start.cycle;
    if (++Global->start.counter != (unsigned long)num_threads) {
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &Cancel);
        while (Cycle == Global->start.cycle) {
            Error = pthread_cond_wait(&(Global->start.cv), &(Global->start.mutex));
            if (Error != 0) {
                break;
            }
        }
        pthread_setcancelstate(Cancel, &Temp);
    } else {
        Global->start.cycle = !(Global->start.cycle);
        Global->start.counter = 0;
        Error = pthread_cond_broadcast(&(Global->start.cv));
    }
    pthread_mutex_unlock(&(Global->start.mutex));
}

/******************************************************************************
 * Helper: random_range
 ******************************************************************************/
static int random_range(int min, int max) {
    return min + rand() % (max - min + 1);
}

/******************************************************************************
 * The busy portion, with branching
 ******************************************************************************/
static void busy_wait_one_second()
{
    time_t start_time = time(NULL);
    volatile unsigned long long sum = 0;
    volatile int array[1024];

    // Initialize array with random values
    for (int i = 0; i < 1024; i++) {
        array[i] = random_range(-100, 100);
    }

    // Run until one second has elapsed
    while (time(NULL) - start_time < 1) {
        // Complex branching pattern with calculations
        for (volatile int i = 0; i < 1024; i++) {
            if (array[i] > 0) {
                sum += array[i] * array[(i + 1) % 1024];
                if (sum > 1000000) {
                    sum = sum % 100;
                    array[i] = -array[i];
                }
            } else {
                sum -= array[i] * array[(i + 2) % 1024];
                if (sum < -1000000) {
                    sum = -sum % 100;
                    array[i] = -array[i];
                }
            }
            
            // Additional branching based on sum
            if (sum % 2 == 0) {
                array[(i + 3) % 1024] += 1;
            } else {
                array[(i + 3) % 1024] -= 1;
            }
        }
    }
}

/******************************************************************************
 * The idle portion, mostly sleeping/lighter branching
 ******************************************************************************/
static void idle_with_light_work_one_second()
{
    time_t start_time = time(NULL);
    volatile unsigned long long idle_sum = 0;

    while (time(NULL) - start_time < 1) {
        // Occasionally do some small work
        if (random_range(0, 100) < 5) {  // ~5% chance
            for (volatile int i = 0; i < 1000; i++) {
                if (i % 2 == 0) {
                    idle_sum += i * 3;
                } else {
                    idle_sum -= i * 2;
                }
            }
        }
        // Sleep for 1ms
        usleep(1000);
    }
}

/******************************************************************************
 * The thread start routine (similar to SlaveStart in FFT)
 ******************************************************************************/
static void ThreadStart(long thread_id)
{
    // Set thread name
    if (thread_id == 0) {
        SimSetThreadName("thread0");
    } else {
        char name[64];
        snprintf(name, sizeof(name), "thread%ld", thread_id);
        SimSetThreadName(name);
    }

    srand(time(NULL) + thread_id);

    // Initial barrier wait
    Barrier(Global, num_threads);

    time_t start_time = time(NULL);

    // Main loop
    while (!should_exit) {
        // If this is thread 0, check for timeout
        if (thread_id == 0 && (time(NULL) - start_time >= total_seconds)) {
            should_exit = 1;
            break;
        }

        SimMarker(1, thread_id);
        busy_wait_one_second();
        SimMarker(2, thread_id);

        SimMarker(1, thread_id);
        idle_with_light_work_one_second();
        SimMarker(2, thread_id);
    }
}

/******************************************************************************
 * The pthread routine just calls ThreadStart
 ******************************************************************************/
static void* worker_thread(void* arg)
{
    long thread_id = (long)arg;
    ThreadStart(thread_id);
    return NULL;
}

/******************************************************************************
 * main(), structured similarly to the FFT code:
 *  - The main thread calls ThreadStart(0)
 *  - We create the other (num_threads-1) pthreads with IDs 1..(num_threads-1)
 ******************************************************************************/
int main(int argc, char *argv[])
{
    int c;
    pthread_t *threads;
    struct timespec sleep_time;
    sleep_time.tv_sec = total_seconds;
    sleep_time.tv_nsec = 0;

    // Parse command line arguments
    while ((c = getopt(argc, argv, "p:t:")) != -1) {
        switch (c) {
            case 'p':
                num_threads = atoi(optarg);
                if (num_threads < 1) {
                    fprintf(stderr, "Number of threads must be >= 1\n");
                    exit(1);
                }
                break;
            case 't':
                total_seconds = atoi(optarg);
                if (total_seconds < 1) {
                    fprintf(stderr, "Total seconds must be >= 1\n");
                    exit(1);
                }
                break;
            default:
                fprintf(stderr, "Usage: %s -p<threads> -t<seconds>\n", argv[0]);
                exit(1);
        }
    }

    // Allocate global structure
    Global = (GlobalMemory*) calloc(1, sizeof(GlobalMemory));
    if (!Global) {
        fprintf(stderr, "Failed to allocate global memory\n");
        exit(1);
    }

    // Initialize barrier
    pthread_mutex_init(&(Global->start.mutex), NULL);
    pthread_cond_init(&(Global->start.cv), NULL);

    printf("Starting Active-Idle simulation with %d threads for %d seconds.\n",
           num_threads, total_seconds);

    // Start ROI
    SimRoiStart();
    SimNamedMarker(4, "begin");

    // Create (num_threads - 1) worker threads
    if (num_threads > 1) {
        threads = (pthread_t*)malloc((num_threads - 1) * sizeof(pthread_t));
        if (!threads) {
            fprintf(stderr, "Failed to allocate thread array\n");
            exit(1);
        }

        for (long i = 1; i < num_threads; i++) {
            if (pthread_create(&threads[i - 1], NULL, worker_thread, (void*)i) != 0) {
                fprintf(stderr, "Failed to create thread %ld\n", i);
                exit(1);
            }
        }
    }

    // Instead of a timer thread, we'll use the main thread to both:
    // 1. Participate in the work as thread 0
    // 2. Check for timeout
    time_t start_time = time(NULL);
    ThreadStart(0);  // This will run until should_exit becomes 1

    // When either the time is up or work finishes, join other threads
    if (num_threads > 1) {
        for (int i = 0; i < (num_threads - 1); i++) {
            pthread_join(threads[i], NULL);
        }
        free(threads);
    }

    // End ROI
    SimNamedMarker(5, "end");
    SimRoiEnd();

    // Cleanup
    pthread_mutex_destroy(&(Global->start.mutex));
    pthread_cond_destroy(&(Global->start.cv));
    free(Global);

    return 0;
}