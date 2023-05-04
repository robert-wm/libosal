# libosal
[![Build Status](https://rmc-jenkins.robotic.dlr.de/jenkins/buildStatus/icon?job=common%2libosal%2Fmaster)](https://rmc-jenkins.robotic.dlr.de/jenkins/job/common/job/libosal/job/master/)

libosal is an <ins>**o**</ins>perating <ins>**s**</ins>ystem <ins>**a**</ins>bstraction <ins>**l**</ins>ayer Library. It's purpose is to write os-independent code for easy portability between different systems and architectures.

Features:

## Timers

The timer framework of libosal provides functions for sleeping and waiting for some defined amount of time. It is also used by the other components of libosal e.g. specifying a timeout while waiting on some events.

Here are some common example on how to use the timer frameworks:

**wait for timeout example**

This example can be used to do some work until a specified timeout occured or is finished otherwise.

```c
osal_timer_t timeout;
osal_timer_init(&timeout, 1000000000); // values are in nanoseconds

do {
  // your work goes here
} while (osal_timer_expired(&timeout) != OSAL_ERR_TIMEOUT);
```
**deterministic 1 ms loop example**

This example implements a loop which can be used as deterministic (with appropriate priority) clock source for a control appllication.

```c
osal_uint64_t cycle_rate = 1000000; // values are in nanoseconds
osal_uint64_t abs_timeout = osal_timer_gettime_nsec();
osal_retval_t ret;

do { 
  // your work goes here
  
  abs_timeout += cycle_rate;
  ret = osal_sleep_until_nsec(abs_timeout);
} while (ret == OSAL_OK);
```

## Mutexes

The mutexes are mutual exclusion locks which are commonly used to protect shared memory structures from concurrent access.

```c
osal_mutex_t mtx;
some_struct_protected_by_mutex_t obj;

void *task_1(void *) {
  while (1) {
    osal_mutex_lock(&mtx);
    // wait for event, do some stuff on 'obj'
    osal_mutex_unlock(&mtx);
    
    // do other work
  }
  return NULL;
}

void *task_2(void *) {
  while (1) {
    osal_mutex_lock(&mtx);
    // wait for event, do some stuff on 'obj'
    osal_mutex_unlock(&mtx);
    
    // do other work
  }
  return NULL;
}

void main(int argc, char **argv) {
  osal_mutex_init(&mtx, NULL);
  // create tasks, do other things...
  
  return 0;
}
```

## Semaphores

## Binary Semaphores

Binary semaphores are a special case of a semaphore for signalling one event to one waiter. The state of the semaphore should be preserved until a waiter has consumed it. It is guaranteed that no event will be missed e.g. because of no one was waiting on the semaphore while it was posted.


```c
osal_binary_semaphore_t binsem;

void *task_1(void *) {
  while (1) {
    osal_binary_semaphore_wait(&binsem);
    // do other work
  }
  return NULL;
}

void *task_2(void *) {
  while (1) {   
    // wait for event, do some stuff
    osal_binary_semaphore_post(&binsem);
  }
  return NULL;
}
```

## Conditions


## Spinlocks


## Tasks

## Trace


Task/Thread abstraction.
