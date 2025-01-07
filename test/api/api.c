#include "sim_api.h"

#define __STDC_LIMIT_MACROS
#define __STDC_CONSTANT_MACROS
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>

volatile long long value;

long long loop() {
	volatile int i;
	

	for (i = 0 ; i < 10000; i++) {
		value += i;
	}

	struct timespec ts;
	ts.tv_sec = 1;
	ts.tv_nsec = 0;
	nanosleep(&ts, NULL);
	asm volatile("mfence");
	return value;
}

int main() {

	SimRoiStart();

	value = 0;

	loop();

	SimRoiEnd();
}
