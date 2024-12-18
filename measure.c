#include <sys/sysctl.h>
#ifdef __APPLE__

#ifndef PTHREAD_BARRIER_H_
#define PTHREAD_BARRIER_H_

#include <pthread.h>
#include <errno.h>

typedef int pthread_barrierattr_t;
typedef struct
{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int count;
    int tripCount;
} pthread_barrier_t;


int pthread_barrier_init(pthread_barrier_t *barrier, const pthread_barrierattr_t *attr, unsigned int count)
{
    if(count == 0)
    {
        errno = EINVAL;
        return -1;
    }
    if(pthread_mutex_init(&barrier->mutex, 0) < 0)
    {
        return -1;
    }
    if(pthread_cond_init(&barrier->cond, 0) < 0)
    {
        pthread_mutex_destroy(&barrier->mutex);
        return -1;
    }
    barrier->tripCount = count;
    barrier->count = 0;

    return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *barrier)
{
    pthread_cond_destroy(&barrier->cond);
    pthread_mutex_destroy(&barrier->mutex);
    return 0;
}

int pthread_barrier_wait(pthread_barrier_t *barrier)
{
    pthread_mutex_lock(&barrier->mutex);
    ++(barrier->count);
    if(barrier->count >= barrier->tripCount)
    {
        barrier->count = 0;
        pthread_cond_broadcast(&barrier->cond);
        pthread_mutex_unlock(&barrier->mutex);
        return 1;
    }
    else
    {
        pthread_cond_wait(&barrier->cond, &(barrier->mutex));
        pthread_mutex_unlock(&barrier->mutex);
        return 0;
    }
}

#endif // PTHREAD_BARRIER_H_
#endif // __APPLE__

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>

#define BILLION (1000*1000*1000)
#define NTRIALS 3000
#define NITER 1000

/* the difference in nanoseconds between times recorded in a and b */
#define NSDELTA(a,b) ((b).tv_nsec-(a).tv_nsec + BILLION*((b).tv_sec-(a).tv_sec))

/* Keep trying until the value at x is equal to a, then replace it with b. */
#define compare_exchange(x, a, b) \
	while(!atomic_compare_exchange_strong_explicit(x, a, b, \
	 memory_order_relaxed, memory_order_relaxed))

/* Two threads will be trying to flip the value of x atomically using
 * compare_exchange.  We'll record the amount of time it takes both threads to
 * go through NITER iterations of that. */
typedef _Atomic bool atomicbool;
atomicbool x = true;

int j_c = -1;
int i_c = -1;
/* Before we let each thread run, we make them sync to make sure both are
 * initialized and ready. */
pthread_barrier_t barrier;

/* This thread waits for x to become true, then flips the value to false. */
void *tf(void *ptr) {
	int core = i_c;
	sysctlbyname("kern.pin_core", NULL, NULL, &core, sizeof(core));
	(void)ptr;
	/* wait until both tf and ft are ready */
	pthread_barrier_wait(&barrier);
	for (int i=0, n=NITER; i<n; ++i)
		compare_exchange(&x, (bool[]){true}, false);
	return NULL;
}


/* This thread waits for x to become false, then flips the value to true. */
void *ft(void *ptr) {
	struct timespec start, end;
	double *result = ptr;

	int core = j_c;
	sysctlbyname("kern.pin_core", NULL, NULL, &core, sizeof(core));

	/* wait until both tf and ft are ready */
	pthread_barrier_wait(&barrier);

	/* measure how much time it takes to run NITER iterations */
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (int i=0, n=NITER; i<n; ++i)
		compare_exchange(&x, (bool[]){false}, true);
	clock_gettime(CLOCK_MONOTONIC, &end);

	/* x starts out as true and in each iteration above we wait for it to
	* turn false, then flip it back to true. The time it takes to complete
	* one iteration can thus be considered the communication round trip
	* latency. We divide by 2 to obtain the one way latency. */
	*result = NSDELTA(start,end)/(double)NITER/2.0;
	return NULL;
}

int main(void) {
	pthread_t ti, tj;
	pthread_attr_t attri, attrj;

	/* Attribute objects are used to configure threads. In our case we use
	* them to pin each thread to a specific core. */
	pthread_attr_init(&attri);
	pthread_attr_init(&attrj);
	pthread_barrier_init(&barrier, NULL, 2);

	/* How many cores do we have on this machine? Note on x86 with SMT the
	* core count includes all hardware threads, so most typically you will
	* see twice the number of actual physical cores. Communication between
	* two hardware threads on the same physical core is the fastest in my
	* experience. */
	int nprocs = 8;
	double result[nprocs][nprocs];
	memset(result, 0, sizeof result);

	/* For each ordered pair of visible cores. */
	for (int i=0; i<nprocs; ++i)
	for (int j=0; j<nprocs; ++j) {
		/* Not interested in measuring how long it takes for the kernel
		* to context switch a hardware thread. */
		if (i == j)
			continue;

		/* Pin thread ti to core i. */
		i_c = i;

		/* Pin thread tj to core j. */
		j_c = j;

		for (int t=0; t<NTRIALS; ++t) {
			double res;
			pthread_create(&ti, &attri, tf, NULL);
			pthread_create(&tj, &attrj, ft, &res);

			pthread_join(ti, NULL);
			pthread_join(tj, NULL);

			result[i][j] += res;
		}
		/* csv */
		printf("%d,%d,%g\n", i, j, result[i][j]/NTRIALS);
	}
}
