/******************************************************************************
 * Simplified single-threaded "Active-Idle" benchmark
 *
 * This benchmark uses only the main thread, alternating between:
 *   - A 1-second "busy" period (with CPU-bound branching),
 *   - Followed by a 1-second "idle" period (with light branching).
 *
 * The total run time is controlled by -t<seconds>.
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include "sim_api.h"

/******************************************************************************
 * Command-line options
 ******************************************************************************/
static int total_seconds = 10;

/******************************************************************************
 * Helper: random_range
 ******************************************************************************/
static int random_range(int min, int max) {
    return min + rand() % (max - min + 1);
}

/******************************************************************************
 * The busy portion, with branching
 ******************************************************************************/
static volatile uint64_t iteration_count = 0;
static const uint64_t ITERATIONS_PER_SECOND = 1;  // Each iteration is one busy+idle cycle
static const uint64_t TOTAL_ITERATIONS = ITERATIONS_PER_SECOND * 10; // For 10 seconds = 10 cycles

static void busy_wait_one_second()
{
    // Do one iteration worth of work
    volatile unsigned long long sum = 0;
    volatile int array[1024];
    volatile double floating_result = 1.0;
    volatile uint64_t bit_result = 0xFFFFFFFFFFFFFFFF;

    // Initialize array with random values
    for (int i = 0; i < 1024; i++) {
        array[i] = random_range(-100, 100);
    }

    // Complex branching pattern with calculations
    for (volatile int i = 0; i < 1024; i++) {
        // Floating point operations
        floating_result *= (array[i] / 100.0 + 1.0);
        floating_result = floating_result > 1e10 ? 1.0 : floating_result;
        
        // Bit manipulation operations
        bit_result = (bit_result << (array[i] & 0x3F)) | (bit_result >> (64 - (array[i] & 0x3F)));
        
        if (array[i] > 0) {
            sum += array[i] * array[(i + 1) % 1024];
            sum ^= bit_result & 0xFFFF;
            if (sum > 1000000) {
                sum = sum % 100;
                array[i] = ~array[i];  // Bitwise NOT
            }
        } else {
            sum -= array[i] * array[(i + 2) % 1024];
            sum ^= (bit_result >> 32) & 0xFFFF;
            if (sum < -1000000) {
                sum = -sum % 100;
                array[i] = array[i] << 1;  // Bit shift
            }
        }
        
        // Division and multiplication
        if ((sum & 0xFF) > 128) {
            floating_result /= 1.01;
        } else {
            floating_result *= 1.01;
        }
    }
    
    // Print results to prevent optimization
    printf("Busy calculations: sum=%llu, floating=%f, bits=0x%lx\n", 
           sum, floating_result, bit_result);
}

/******************************************************************************
 * The idle portion, mostly sleeping/lighter branching
 ******************************************************************************/
static void idle_with_light_work_one_second()
{
    // Do one iteration worth of idle behavior
    volatile unsigned long long idle_sum = 0;
    volatile double idle_float = 1.0;

    // More complex occasional work
    if (random_range(0, 100) < 50) {
        for (volatile int i = 0; i < 1000; i++) {
            // Insert CPU NOPs (using inline assembly)
            asm volatile("nop");
            asm volatile("nop");
            
            if (i % 2 == 0) {
                idle_sum += i * 3;
                idle_float *= 1.000001;
            } else {
                idle_sum -= i * 2;
                idle_float /= 1.000001;
            }
            
            // More NOPs and memory barrier
            asm volatile("nop");
            asm volatile("mfence");
        }
    }

    struct timespec ts;
    ts.tv_sec = 1;
    ts.tv_nsec = 0;
    nanosleep(&ts, NULL);
    asm volatile("mfence");

    // Print result to prevent optimization
    printf("Idle calculations: sum=%llu, float=%f\n", idle_sum, idle_float);
}

/******************************************************************************
 * main()
 ******************************************************************************/
int main(int argc, char *argv[])
{
    int c;

    // Parse command line arguments
    while ((c = getopt(argc, argv, "t:")) != -1) {
        switch (c) {
            case 't':
                total_seconds = atoi(optarg);
                if (total_seconds < 1) {
                    fprintf(stderr, "Total seconds must be >= 1\n");
                    exit(1);
                }
                break;
            default:
                fprintf(stderr, "Usage: %s -t<seconds>\n", argv[0]);
                exit(1);
        }
    }

    printf("Starting Single-threaded Active-Idle simulation for %d cycles (%d seconds).\n",
           (int)TOTAL_ITERATIONS, total_seconds);

    SimSetThreadName("main");
    
    // Start ROI
    SimRoiStart();
    SimNamedMarker(5, "begin");

    // Main loop - now using iteration count instead of time
    iteration_count = 0;
    while (iteration_count < TOTAL_ITERATIONS) {  // Will run exactly 10 times
        SimMarker(1, 0);
        busy_wait_one_second();
        SimMarker(2, 0);

        SimMarker(3, 0);
        idle_with_light_work_one_second();
        SimMarker(4, 0);

        iteration_count++;
        printf("Completed cycle %lu of %lu\n", iteration_count, TOTAL_ITERATIONS);
    }

    // End ROI
    SimNamedMarker(6, "end");
    SimRoiEnd();

    return 0;
}