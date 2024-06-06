#include "libosal/mq.h"
#include "libosal/osal.h"
#include "test_utils.h"
#include "gtest/gtest.h"
#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

namespace test_messagequeue {

int verbose = 0;

using testutils::set_deadline;
using testutils::wait_nanoseconds;

/*
  This is a test which tests the messagequeue with
  multiple producers, multiple consumers.

  The concept is as follows: N producer threads
  write a series of pseudo-random values to
  K destinations. These randon values are
  generated by hashing the sequence of natural
  numbers.

  In order to ensure that the sequence is
  in the right order, the counter is locked for
  each destination, by a per-destination mutex.

  These values are used to form messages,
  which are all sent to the same message queue.

  Then, M consumer threads read the messages from the
  queue, and append them to the destination.
  Appending happens by hashing the previous
  value together with the received message
  value.

  To compare, the sender threads computes the same
  sequence of hashing, and when the threads finish,
  the two resulting hashes are compared. If the message
  queue works correctly (preserving both content
  and order of messages), the hashes have to match.


*/

namespace multiwriter_multireader {
const uint N_PRODUCERS = 30;
const uint M_CONSUMERS = 20;
const uint K_ENDPOINTS = 10;

const ulong NUM_MESSAGES = 1000 * N_PRODUCERS * M_CONSUMERS;
const ulong NUM_MESSAGES_PER_PRODUCER = NUM_MESSAGES / N_PRODUCERS;
const ulong NUM_MESSAGES_PER_CONSUMER = NUM_MESSAGES / M_CONSUMERS;
const ulong MIN_WAIT_TIME_NS = 1000;
const ulong MAX_WAIT_TIME_NS = 100000;

typedef struct {
  uint32_t counter;
  size_t hash;
  pthread_mutex_t source_mutex;
} source_t;

typedef struct {
  uint32_t counter;
  size_t hash;
  pthread_mutex_t dest_mutex;
} dest_t;

typedef struct {
  uint dest_id;
  uint32_t payload;
} message_t;

typedef struct {
  source_t source[K_ENDPOINTS];
  dest_t dest[K_ENDPOINTS];
  pthread_mutex_t receive_lock;
  osal_mq_t queue;
} shared_t;

typedef struct {
  uint32_t thread_id;
  shared_t *pshared;
} thread_data_t;

// combine two hash values (as in a HMAC,
// just not cryptographically secure). */

size_t gethash(uint32_t const n) { return std::hash<uint32_t>{}(n); }

size_t combine_hash(size_t const oldhash, uint32_t const payload) {
  size_t new_hash = std::hash<uint32_t>{}(payload);
  return (oldhash << 4) ^ new_hash;
}

void *run_producer(void *p_params) {

  shared_t *pshared = ((thread_data_t *)p_params)->pshared;
  uint32_t const thread_id = ((thread_data_t *)p_params)->thread_id;

  osal_retval_t orv;
  int rv;

  message_t msg;
  if (verbose) {
    printf("started: producer # %u\n", thread_id);
  }

  for (ulong i = 0; i < NUM_MESSAGES_PER_PRODUCER; i++) {

    // draw a random message id
    msg.dest_id = rand() % K_ENDPOINTS;
    source_t *source = &pshared->source[msg.dest_id];

    // the lock is needed here for two things:
    //
    // 1. to protect the per-endpoint counter, so that we can check
    // ordering later
    //
    // 2. to protect the ordering of messages in respect to that endpoint
    rv = pthread_mutex_lock(&source->source_mutex);
    EXPECT_EQ(rv, OSAL_OK) << "pthread_mutex_lock()[dest] failed";

    source->counter++;
    msg.payload = 0xFFFFFFFF & gethash(source->counter);
    source->hash = combine_hash(source->hash, msg.payload);

    if (verbose) {
      printf("sending from producer thread_id %u to endpoint %u\n", thread_id,
             msg.dest_id);
    }
    osal_uint32_t const prio = 0;
    orv = osal_mq_send(&pshared->queue, (char *)&msg, sizeof(msg), prio);
    EXPECT_EQ(orv, OSAL_OK) << "osal_mq_send() failed";

    if (verbose) {
      printf("sending from producer thread_id %u to endpoint %u .. OK\n",
             thread_id, msg.dest_id);
    }

    // return dest lock
    rv = pthread_mutex_unlock(&source->source_mutex);
    EXPECT_EQ(rv, OSAL_OK) << "pthread_mutex_unlock()[source] failed";

    // wait a bit to retain some queue capacity
    wait_nanoseconds(rand() % MAX_WAIT_TIME_NS);
  }

  if (verbose) {
    printf("exiting: producer # %u\n", thread_id);
  }
  return nullptr;
}

void *run_consumer(void *p_params) {

  shared_t *pshared = ((thread_data_t *)p_params)->pshared;
  uint32_t const thread_id = ((thread_data_t *)p_params)->thread_id;

  osal_retval_t orv;
  int rv;

  message_t msg;

  if (verbose) {
    printf("started: consumer # %u\n", thread_id);
  }

  for (ulong i = 0; i < NUM_MESSAGES_PER_CONSUMER; i++) {

    if (verbose) {
      printf("consumer thread_id %u : locking\n", thread_id);
    }
    rv = pthread_mutex_lock(&pshared->receive_lock);
    EXPECT_EQ(rv, OSAL_OK) << "pthread_mutex_lock() [mq] failed";
    if (verbose) {
      printf("wait/receive from consumer thread_id %u\n", thread_id);
    }
    osal_uint32_t rprio = 0;
    orv = osal_mq_receive(&pshared->queue, (char *)&msg, sizeof(msg), &rprio);
    EXPECT_EQ(orv, OSAL_OK) << "osal_mq_receive() failed";

    dest_t *dest = &pshared->dest[msg.dest_id];

    if (verbose) {
      printf("received from consumer thread_id %u for endpoint %u\n", thread_id,
             msg.dest_id);
    }

    rv = pthread_mutex_lock(&dest->dest_mutex);
    EXPECT_EQ(rv, OSAL_OK) << "pthread_mutex_lock()[dest] failed";

    dest->counter++;
    dest->hash = combine_hash(dest->hash, msg.payload);

    // return dest lock
    rv = pthread_mutex_unlock(&dest->dest_mutex);
    EXPECT_EQ(rv, OSAL_OK) << "pthread_mutex_unlock()[dest] failed";

    // return mq lock
    rv = pthread_mutex_unlock(&pshared->receive_lock);
    EXPECT_EQ(rv, OSAL_OK) << "pthread_mutex_unlock()[mq] failed";
  }

  if (verbose) {
    printf("exiting: consumer # %u\n", thread_id);
  }
  return nullptr;
}

TEST(MessageQueue, MultiSendMultiReceive) {

  int rv;
  osal_retval_t orv;

  shared_t shared;

  pthread_t producers[N_PRODUCERS];
  thread_data_t prod_data[N_PRODUCERS];
  pthread_t consumers[M_CONSUMERS];
  thread_data_t cons_data[M_CONSUMERS];

  // initialize sources
  for (uint i = 0; i < K_ENDPOINTS; i++) {
    shared.source[i].counter = 0;
    shared.source[i].hash = 0;
    rv = pthread_mutex_init(&shared.source[i].source_mutex, nullptr);
    ASSERT_EQ(rv, 0) << "pthread_mutex_init()[source] failed";
  }

  // initialize destinations
  for (uint i = 0; i < K_ENDPOINTS; i++) {
    shared.dest[i].counter = 0;
    shared.dest[i].hash = 0;
    rv = pthread_mutex_init(&shared.dest[i].dest_mutex, nullptr);
    ASSERT_EQ(rv, 0) << "pthread_mutex_init()[dest] failed";
  }

  rv = pthread_mutex_init(&shared.receive_lock, nullptr);
  ASSERT_EQ(rv, 0) << "pthread_mutex_init()[rlock] failed";

  // initialize message queue
  osal_mq_attr_t attr = {};
  attr.oflags = OSAL_MQ_ATTR__OFLAG__RDWR | OSAL_MQ_ATTR__OFLAG__CREAT;
  attr.max_messages = 10; /* system default, won't work with larger
                           * number without adjustment */
  ASSERT_GE(attr.max_messages, 0u);
  attr.max_message_size = sizeof(message_t);
  ASSERT_GE(attr.max_message_size, 0u);
  attr.mode = S_IRUSR | S_IWUSR;
  // unlink message queue if it exists.
  // Note: the return value is intentionally not checked.
  mq_unlink("/test1");

  orv = osal_mq_open(&shared.queue, "/test1", &attr);
  if (orv != 0) {
    perror("failed to open mq:");
  }
  ASSERT_EQ(orv, OSAL_OK) << "osal_mq_open() failed";

  // initialize consumers
  if (verbose) {
    printf("starting consumers\n");
  }
  for (uint i = 0; i < M_CONSUMERS; i++) {
    cons_data[i].pshared = &shared;
    cons_data[i].thread_id = i;
    rv = pthread_create(/*thread*/ &(consumers[i]),
                        /*pthread_attr*/ nullptr,
                        /* start_routine */ run_consumer,
                        /* arg */ (void *)(&cons_data[i]));
    ASSERT_EQ(rv, 0) << "pthread_create()[consumers] failed";
  }

  // initialize producers
  if (verbose) {
    printf("starting producers\n");
  }
  for (uint i = 0; i < N_PRODUCERS; i++) {
    prod_data[i].pshared = &shared;
    prod_data[i].thread_id = i;

    rv = pthread_create(/*thread*/ &(producers[i]),
                        /*pthread_attr*/ nullptr,
                        /* start_routine */ run_producer,
                        /* arg */ (void *)(&prod_data[i]));
    ASSERT_EQ(rv, 0) << "pthread_create()[producers] failed";
  }

  if (verbose) {
    printf("joining producers\n");
  }
  // the following waits for all producers to finish first
  // join producers
  for (uint i = 0; i < N_PRODUCERS; i++) {
    rv = pthread_join(/*thread*/ producers[i],
                      /*retval*/ nullptr);
    ASSERT_EQ(rv, 0) << "pthread_join()[producers] failed";
  }

  // join consumers
  if (verbose) {
    printf("joining consumers\n");
  }
  for (uint i = 0; i < M_CONSUMERS; i++) {
    rv = pthread_join(/*thread*/ consumers[i],
                      /*retval*/ nullptr);
    ASSERT_EQ(rv, 0) << "pthread_join()[consumers] failed";
  }

  // destroy message queue
  orv = osal_mq_close(&shared.queue);
  ASSERT_EQ(orv, OSAL_OK) << "osal_mq_close() failed";

  rv = pthread_mutex_destroy(&shared.receive_lock);
  ASSERT_EQ(rv, 0) << "pthread_mutex_destroy()[rlock] failed";

  // destroy destinations mutexes
  for (uint i = 0; i < K_ENDPOINTS; i++) {
    rv = pthread_mutex_destroy(&shared.dest[i].dest_mutex);
    ASSERT_EQ(rv, 0) << "pthread_mutex_destroy[dest]() failed";
  }

  // destroy sources mutexes
  for (uint i = 0; i < K_ENDPOINTS; i++) {
    rv = pthread_mutex_destroy(&shared.source[i].source_mutex);
    ASSERT_EQ(rv, 0) << "pthread_mutex_destroy[source]() failed";
  }

  // compare results for correctness

  for (ulong i = 0; i < K_ENDPOINTS; i++) {
    EXPECT_EQ(shared.source[i].counter, shared.dest[i].counter)
        << "counters do not match";
  }
  if (verbose) {
    for (ulong i = 0; i < K_ENDPOINTS; i++) {
      printf("hashed values: source = 0x%zx - dest = 0x%zx\n",
             shared.source[i].hash, shared.dest[i].hash);
    }
  }
  for (ulong i = 0; i < K_ENDPOINTS; i++) {
    EXPECT_EQ(shared.source[i].hash, shared.dest[i].hash)
        << "hashes do not match";
  }
}
} // namespace multiwriter_multireader

namespace readonly_writeonly {
const uint N_PRODUCERS = 30;
const uint M_CONSUMERS = 20;
const uint K_ENDPOINTS = 10;

const ulong NUM_MESSAGES = 1000 * N_PRODUCERS * M_CONSUMERS;
const ulong NUM_MESSAGES_PER_PRODUCER = NUM_MESSAGES / N_PRODUCERS;
const ulong NUM_MESSAGES_PER_CONSUMER = NUM_MESSAGES / M_CONSUMERS;
const ulong MIN_WAIT_TIME_NS = 1000;
const ulong MAX_WAIT_TIME_NS = 100000;

typedef struct {
  uint32_t counter;
  size_t hash;
  pthread_mutex_t source_mutex;
} source_t;

typedef struct {
  uint32_t counter;
  size_t hash;
  pthread_mutex_t dest_mutex;
} dest_t;

typedef struct {
  uint dest_id;
  uint32_t payload;
} message_t;

typedef struct {
  source_t source[K_ENDPOINTS];
  dest_t dest[K_ENDPOINTS];
  pthread_mutex_t receive_lock;
  osal_mq_t wqueue;
  osal_mq_t rqueue;
} shared_t;

typedef struct {
  uint32_t thread_id;
  shared_t *pshared;
} thread_data_t;

// combine two hash values (as in a HMAC,
// just not cryptographically secure). */

size_t gethash(uint32_t const n) { return std::hash<uint32_t>{}(n); }

size_t combine_hash(size_t const oldhash, uint32_t const payload) {
  size_t new_hash = std::hash<uint32_t>{}(payload);
  return (oldhash << 4) ^ new_hash;
}

void *run_wproducer(void *p_params) {

  shared_t *pshared = ((thread_data_t *)p_params)->pshared;
  uint32_t const thread_id = ((thread_data_t *)p_params)->thread_id;

  osal_retval_t orv;
  int rv;

  message_t msg;
  if (verbose) {
    printf("started: producer # %u\n", thread_id);
  }

  for (ulong i = 0; i < NUM_MESSAGES_PER_PRODUCER; i++) {

    // draw a random message id
    msg.dest_id = rand() % K_ENDPOINTS;
    source_t *source = &pshared->source[msg.dest_id];

    // the lock is needed here for two things:
    //
    // 1. to protect the per-endpoint counter, so that we can check
    // ordering later
    //
    // 2. to protect the ordering of messages in respect to that endpoint
    rv = pthread_mutex_lock(&source->source_mutex);
    EXPECT_EQ(rv, OSAL_OK) << "pthread_mutex_lock()[dest] failed";

    source->counter++;
    msg.payload = 0xFFFFFFFF & gethash(source->counter);
    source->hash = combine_hash(source->hash, msg.payload);

    if (verbose) {
      printf("sending from producer thread_id %u to endpoint %u\n", thread_id,
             msg.dest_id);
    }
    osal_uint32_t const prio = 0;
    orv = osal_mq_send(&pshared->wqueue, (char *)&msg, sizeof(msg), prio);
    EXPECT_EQ(orv, OSAL_OK) << "osal_mq_send() failed";

    if (verbose) {
      printf("sending from producer thread_id %u to endpoint %u .. OK\n",
             thread_id, msg.dest_id);
    }

    // return dest lock
    rv = pthread_mutex_unlock(&source->source_mutex);
    EXPECT_EQ(rv, OSAL_OK) << "pthread_mutex_unlock()[source] failed";

    // wait a bit to retain some queue capacity
    wait_nanoseconds(rand() % MAX_WAIT_TIME_NS);
  }

  if (verbose) {
    printf("exiting: producer # %u\n", thread_id);
  }
  return nullptr;
}

void *run_rconsumer(void *p_params) {

  shared_t *pshared = ((thread_data_t *)p_params)->pshared;
  uint32_t const thread_id = ((thread_data_t *)p_params)->thread_id;

  osal_retval_t orv;
  int rv;

  message_t msg;

  if (verbose) {
    printf("started: consumer # %u\n", thread_id);
  }

  for (ulong i = 0; i < NUM_MESSAGES_PER_CONSUMER; i++) {

    if (verbose) {
      printf("consumer thread_id %u : locking\n", thread_id);
    }
    rv = pthread_mutex_lock(&pshared->receive_lock);
    EXPECT_EQ(rv, OSAL_OK) << "pthread_mutex_lock() [mq] failed";
    if (verbose) {
      printf("wait/receive from consumer thread_id %u\n", thread_id);
    }
    osal_uint32_t rprio = 0;
    orv = osal_mq_receive(&pshared->rqueue, (char *)&msg, sizeof(msg), &rprio);
    EXPECT_EQ(orv, OSAL_OK) << "osal_mq_receive() failed";

    dest_t *dest = &pshared->dest[msg.dest_id];

    if (verbose) {
      printf("received from consumer thread_id %u for endpoint %u\n", thread_id,
             msg.dest_id);
    }

    rv = pthread_mutex_lock(&dest->dest_mutex);
    EXPECT_EQ(rv, OSAL_OK) << "pthread_mutex_lock()[dest] failed";

    dest->counter++;
    dest->hash = combine_hash(dest->hash, msg.payload);

    // return dest lock
    rv = pthread_mutex_unlock(&dest->dest_mutex);
    EXPECT_EQ(rv, OSAL_OK) << "pthread_mutex_unlock()[dest] failed";

    // return mq lock
    rv = pthread_mutex_unlock(&pshared->receive_lock);
    EXPECT_EQ(rv, OSAL_OK) << "pthread_mutex_unlock()[mq] failed";
  }

  if (verbose) {
    printf("exiting: consumer # %u\n", thread_id);
  }
  return nullptr;
}

TEST(MessageQueue, ReadonlyWriteonly) {

  int rv;
  osal_retval_t orv;

  shared_t shared;

  pthread_t producers[N_PRODUCERS];
  thread_data_t prod_data[N_PRODUCERS];
  pthread_t consumers[M_CONSUMERS];
  thread_data_t cons_data[M_CONSUMERS];

  // initialize sources
  for (uint i = 0; i < K_ENDPOINTS; i++) {
    shared.source[i].counter = 0;
    shared.source[i].hash = 0;
    rv = pthread_mutex_init(&shared.source[i].source_mutex, nullptr);
    ASSERT_EQ(rv, 0) << "pthread_mutex_init()[source] failed";
  }

  // initialize destinations
  for (uint i = 0; i < K_ENDPOINTS; i++) {
    shared.dest[i].counter = 0;
    shared.dest[i].hash = 0;
    rv = pthread_mutex_init(&shared.dest[i].dest_mutex, nullptr);
    ASSERT_EQ(rv, 0) << "pthread_mutex_init()[dest] failed";
  }

  rv = pthread_mutex_init(&shared.receive_lock, nullptr);
  ASSERT_EQ(rv, 0) << "pthread_mutex_init()[rlock] failed";

  // initialize message queue
  osal_mq_attr_t attr_w = {};
  attr_w.oflags = OSAL_MQ_ATTR__OFLAG__WRONLY | OSAL_MQ_ATTR__OFLAG__CREAT;
  attr_w.max_messages = 10; /* system default, won't work with larger
                             * number without adjustment */
  ASSERT_GE(attr_w.max_messages, 0u);
  attr_w.max_message_size = sizeof(message_t);
  ASSERT_GE(attr_w.max_message_size, 0u);
  attr_w.mode = S_IRUSR | S_IWUSR;
  // unlink message queue if it exists.
  // Note: the return value is intentionally not checked.
  mq_unlink("/test2");

  orv = osal_mq_open(&shared.wqueue, "/test2", &attr_w);
  if (orv != 0) {
    perror("failed to open mq:");
  }
  ASSERT_EQ(orv, OSAL_OK) << "osal_mq_open() failed";

  osal_mq_attr_t attr_r = {};
  attr_r.oflags = OSAL_MQ_ATTR__OFLAG__RDONLY;
  attr_r.max_messages = 10; /* system default, won't work with larger
                             * number without adjustment */
  ASSERT_GE(attr_r.max_messages, 0u);
  attr_r.max_message_size = sizeof(message_t);
  ASSERT_GE(attr_r.max_message_size, 0u);
  attr_r.mode = S_IRUSR | S_IWUSR;

  orv = osal_mq_open(&shared.rqueue, "/test2", &attr_r);
  if (orv != 0) {
    perror("failed to open mq:");
  }
  ASSERT_EQ(orv, OSAL_OK) << "osal_mq_open() failed";

  // initialize consumers
  if (verbose) {
    printf("starting consumers\n");
  }
  for (uint i = 0; i < M_CONSUMERS; i++) {
    cons_data[i].pshared = &shared;
    cons_data[i].thread_id = i;
    rv = pthread_create(/*thread*/ &(consumers[i]),
                        /*pthread_attr*/ nullptr,
                        /* start_routine */ run_rconsumer,
                        /* arg */ (void *)(&cons_data[i]));
    ASSERT_EQ(rv, 0) << "pthread_create()[consumers] failed";
  }

  // initialize producers
  if (verbose) {
    printf("starting producers\n");
  }
  for (uint i = 0; i < N_PRODUCERS; i++) {
    prod_data[i].pshared = &shared;
    prod_data[i].thread_id = i;

    rv = pthread_create(/*thread*/ &(producers[i]),
                        /*pthread_attr*/ nullptr,
                        /* start_routine */ run_wproducer,
                        /* arg */ (void *)(&prod_data[i]));
    ASSERT_EQ(rv, 0) << "pthread_create()[producers] failed";
  }

  if (verbose) {
    printf("joining producers\n");
  }
  // the following waits for all producers to finish first
  // join producers
  for (uint i = 0; i < N_PRODUCERS; i++) {
    rv = pthread_join(/*thread*/ producers[i],
                      /*retval*/ nullptr);
    ASSERT_EQ(rv, 0) << "pthread_join()[producers] failed";
  }

  // join consumers
  if (verbose) {
    printf("joining consumers\n");
  }
  for (uint i = 0; i < M_CONSUMERS; i++) {
    rv = pthread_join(/*thread*/ consumers[i],
                      /*retval*/ nullptr);
    ASSERT_EQ(rv, 0) << "pthread_join()[consumers] failed";
  }

  // destroy message queues
  orv = osal_mq_close(&shared.rqueue);
  ASSERT_EQ(orv, OSAL_OK) << "osal_mq_close() failed";

  orv = osal_mq_close(&shared.wqueue);
  ASSERT_EQ(orv, OSAL_OK) << "osal_mq_close() failed";

  rv = pthread_mutex_destroy(&shared.receive_lock);
  ASSERT_EQ(rv, 0) << "pthread_mutex_destroy()[rlock] failed";

  // destroy destinations mutexes
  for (uint i = 0; i < K_ENDPOINTS; i++) {
    rv = pthread_mutex_destroy(&shared.dest[i].dest_mutex);
    ASSERT_EQ(rv, 0) << "pthread_mutex_destroy[dest]() failed";
  }

  // destroy sources mutexes
  for (uint i = 0; i < K_ENDPOINTS; i++) {
    rv = pthread_mutex_destroy(&shared.source[i].source_mutex);
    ASSERT_EQ(rv, 0) << "pthread_mutex_destroy[source]() failed";
  }

  // compare results for correctness

  for (ulong i = 0; i < K_ENDPOINTS; i++) {
    EXPECT_EQ(shared.source[i].counter, shared.dest[i].counter)
        << "counters do not match";
  }
  if (verbose) {
    for (ulong i = 0; i < K_ENDPOINTS; i++) {
      printf("hashed values: source = 0x%zx - dest = 0x%zx\n",
             shared.source[i].hash, shared.dest[i].hash);
    }
  }
  for (ulong i = 0; i < K_ENDPOINTS; i++) {
    EXPECT_EQ(shared.source[i].hash, shared.dest[i].hash)
        << "hashes do not match";
  }
}
} // namespace readonly_writeonly

namespace test_invalidparams {
TEST(MessageQueue, InvalidParamsAccess) {

  int rv;
  osal_retval_t orv;
  osal_mq_t fqueue;
  osal_mq_t gqueue;

  // initialize message queue
  osal_mq_attr_t attr = {};
  attr.oflags = OSAL_MQ_ATTR__OFLAG__WRONLY | OSAL_MQ_ATTR__OFLAG__CREAT;
  attr.max_messages = 10; /* system default, won't work with larger
                           * number without adjustment */
  ASSERT_GE(attr.max_messages, 0u);
  attr.max_message_size = 256;
  ASSERT_GE(attr.max_message_size, 0u);
  attr.mode = S_IRUSR | S_IWUSR;
  // unlink message queue if it exists.
  // Note: the return value is intentionally not checked.
  mq_unlink("/test3");

  orv = osal_mq_open(&fqueue, "/test3", &attr);
  if (orv != 0) {
    perror("failed to open mq:");
  }
  ASSERT_EQ(orv, OSAL_OK) << "osal_mq_open() failed";

  // destroy message queues
  orv = osal_mq_close(&fqueue);
  ASSERT_EQ(orv, OSAL_OK) << "osal_mq_close() failed";

  rv = chmod("/dev/mqueue/test3", S_IROTH);
  ASSERT_EQ(rv, 0) << "chmod() failed";

  attr.oflags = OSAL_MQ_ATTR__OFLAG__WRONLY;
  orv = osal_mq_open(&fqueue, "/test3", &attr);
  if (orv != OSAL_OK) {
    perror("correctly failed to fail opening mq:");
  }
  ASSERT_EQ(orv, OSAL_ERR_PERMISSION_DENIED)
      << "osal_mq_open() succeeded wrongly";

  rv = chmod("/dev/mqueue/test3", S_IRUSR | S_IWUSR);
  ASSERT_EQ(rv, 0) << "chmod() failed";
  attr.oflags = (OSAL_MQ_ATTR__OFLAG__RDWR | OSAL_MQ_ATTR__OFLAG__CREAT |
                 OSAL_MQ_ATTR__OFLAG__EXCL);

  mq_unlink("/test4");

  orv = osal_mq_open(&fqueue, "/test4", &attr);
  if (orv != OSAL_OK) {
    perror("failed to open mq /test4:");
  }
  ASSERT_EQ(orv, OSAL_OK) << "osal_mq_open() failed";

  orv = osal_mq_open(&gqueue, "/test4", &attr);
  if (orv == OSAL_OK) {
    perror("failed to check O_EXCL when opening mq /test4:");
  }

  ASSERT_EQ(orv, OSAL_ERR_PERMISSION_DENIED)
      << "osal_mq_open() succeeded wrongly";
}

TEST(MessageQueue, InvalidParamValues) {

  osal_retval_t orv;
  osal_mq_t fqueue;

  // initialize message queue
  osal_mq_attr_t attr = {};
  attr.oflags = OSAL_MQ_ATTR__OFLAG__WRONLY | OSAL_MQ_ATTR__OFLAG__CREAT;
  attr.max_messages = 10; /* system default, won't work with larger
                           * number without adjustment */
  ASSERT_GE(attr.max_messages, 0u);
  attr.max_message_size = 1 << 31;
  ASSERT_GE(attr.max_message_size, 0u);
  attr.mode = S_IRUSR | S_IWUSR;
  // unlink message queue if it exists.
  // Note: the return value is intentionally not checked.
  mq_unlink("/test5");

  orv = osal_mq_open(&fqueue, "/test5", &attr);
  if (orv != 0) {
    perror("failed to open mq:");
  }
  ASSERT_EQ(orv, OSAL_ERR_INVALID_PARAM)
      << "osal_mq_open() failed to check invalid message size";
}

TEST(MessageQueue, NonExistingName) {

  osal_retval_t orv;
  osal_mq_t fqueue;

  // initialize message queue
  osal_mq_attr_t attr = {};
  attr.oflags = OSAL_MQ_ATTR__OFLAG__WRONLY;
  attr.max_messages = 10; /* system default, won't work with larger
                           * number without adjustment */
  ASSERT_GE(attr.max_messages, 0u);
  attr.max_message_size = 256;
  ASSERT_GE(attr.max_message_size, 0u);
  attr.mode = S_IRUSR | S_IWUSR;
  // unlink message queue if it exists.
  // Note: the return value is intentionally not checked.
  mq_unlink("/test6");

  orv = osal_mq_open(&fqueue, "/test6", &attr);
  if (orv != 0) {
    perror("failed to open mq:");
  }
  ASSERT_EQ(orv, OSAL_ERR_NOT_FOUND)
      << "osal_mq_open() failed to check non-existant mq name";
}

TEST(MessageQueue, OverlyLongName) {

  osal_retval_t orv;
  osal_mq_t fqueue;

  // initialize message queue
  osal_mq_attr_t attr = {};
  attr.oflags = OSAL_MQ_ATTR__OFLAG__WRONLY | OSAL_MQ_ATTR__OFLAG__CREAT;
  attr.max_messages = 10; /* system default, won't work with larger
                           * number without adjustment */
  ASSERT_GE(attr.max_messages, 0u);
  attr.max_message_size = 256;
  ASSERT_GE(attr.max_message_size, 0u);
  attr.mode = S_IRUSR | S_IWUSR;
  // unlink message queue if it exists.
  // Note: the return value is intentionally not checked.
  const size_t name_len = 10000;
  char queue_name[name_len];
  queue_name[0] = '/';
  for (size_t i = 1; i < (name_len - 1); i++) {
    queue_name[i] = 'a';
  }
  queue_name[name_len - 1] = '\0';

  mq_unlink(queue_name);
  errno = 0;
  orv = osal_mq_open(&fqueue, queue_name, &attr);
  if (orv != 0) {
    perror("failed to open mq:");
  }
  ASSERT_EQ(orv, OSAL_ERR_INVALID_PARAM)
      << "osal_mq_open() failed to check overly long mq name";
}

TEST(MessageQueue, ExceedingSizeLimit) {

  osal_retval_t orv;
  osal_mq_t fqueue;

  // initialize message queue
  osal_mq_attr_t attr = {};
  attr.oflags = OSAL_MQ_ATTR__OFLAG__WRONLY | OSAL_MQ_ATTR__OFLAG__CREAT;
  attr.max_messages = 10000; /* system default, won't work with larger
                              * number without adjustment */
  ASSERT_GE(attr.max_messages, 0u);
  attr.max_message_size = 4096;
  ASSERT_GE(attr.max_message_size, 0u);
  attr.mode = S_IRUSR | S_IWUSR;
  // unlink message queue if it exists.
  // Note: the return value is intentionally not checked.
  mq_unlink("/test7");

  orv = osal_mq_open(&fqueue, "/test7", &attr);
  if (orv != 0) {
    perror("failed to open mq:");
  }
  ASSERT_EQ(orv, OSAL_ERR_INVALID_PARAM)
      << "osal_mq_open() failed to check memory limit";
}

} // namespace test_invalidparams

namespace test_maxresources {

TEST(MessageQueue, TestMessageNumber) {

  int rv;
  osal_retval_t orv;
  osal_mq_t mqueue;

  // initialize message queue
  osal_mq_attr_t attr = {};
  attr.oflags = OSAL_MQ_ATTR__OFLAG__RDWR | OSAL_MQ_ATTR__OFLAG__CREAT;
  attr.max_messages = 10; /* system default, won't work with larger
                           * number without adjustment */
  ASSERT_GE(attr.max_messages, 0u);
  attr.max_message_size = 256;
  ASSERT_GE(attr.max_message_size, 0u);
  attr.mode = S_IRUSR | S_IWUSR;
  // unlink message queue if it exists.
  // Note: the return value is intentionally not checked.

  struct rlimit lim;
  struct rlimit old_lim;

  /* test limit on message queue number */
  mq_unlink("/test8");
  getrlimit(RLIMIT_MSGQUEUE, &lim);
  old_lim = lim;
  lim.rlim_cur = 0;
  rv = setrlimit(RLIMIT_MSGQUEUE, &lim);
  ASSERT_EQ(rv, 0) << "setrlimit failed";

  orv = osal_mq_open(&mqueue, "/test8", &attr);
  if (orv != 0) {
    perror("failed to open mq:");
  }
  EXPECT_EQ(orv, OSAL_ERR_SYSTEM_LIMIT_REACHED) << "osal_mq_open() failed";

  rv = setrlimit(RLIMIT_MSGQUEUE, &old_lim);
  ASSERT_EQ(rv, 0) << "setrlimit failed";
}

TEST(MessageQueue, TestFileLimit) {

  int rv;
  osal_retval_t orv;
  osal_mq_t mqueue;

  // initialize message queue
  osal_mq_attr_t attr = {};
  attr.oflags = OSAL_MQ_ATTR__OFLAG__RDWR | OSAL_MQ_ATTR__OFLAG__CREAT;
  attr.max_messages = 10; /* system default, won't work with larger
                           * number without adjustment */
  ASSERT_GE(attr.max_messages, 0u);
  attr.max_message_size = 256;
  ASSERT_GE(attr.max_message_size, 0u);
  attr.mode = S_IRUSR | S_IWUSR;
  // unlink message queue if it exists.
  // Note: the return value is intentionally not checked.

  struct rlimit lim;
  struct rlimit old_lim;

  /* test limit on  file number */
  mq_unlink("/test8");
  getrlimit(RLIMIT_NOFILE, &lim);
  old_lim = lim;
  lim.rlim_cur = 0;
  rv = setrlimit(RLIMIT_NOFILE, &lim);
  ASSERT_EQ(rv, 0) << "setrlimit failed";

  orv = osal_mq_open(&mqueue, "/test8", &attr);
  if (orv != 0) {
    perror("failed to open mq:");
  }
  EXPECT_EQ(orv, OSAL_ERR_SYSTEM_LIMIT_REACHED) << "osal_mq_open() failed";

  rv = setrlimit(RLIMIT_NOFILE, &old_lim);
  ASSERT_EQ(rv, 0) << "setrlimit failed";
}

TEST(MessageQueue, TestResourceOversubscription) {

  int rv;
  osal_retval_t orv;
  osal_mq_t mqueue;

  // initialize message queue
  osal_mq_attr_t attr = {};
  attr.oflags = OSAL_MQ_ATTR__OFLAG__RDWR | OSAL_MQ_ATTR__OFLAG__CREAT;
  attr.max_messages = 10; /* system default, won't work with larger
                           * number without adjustment */
  ASSERT_GE(attr.max_messages, 0u);
  attr.max_message_size = 256;
  ASSERT_GE(attr.max_message_size, 0u);
  attr.mode = S_IRUSR | S_IWUSR;
  // unlink message queue if it exists.
  // Note: the return value is intentionally not checked.

  struct rlimit lim;
  struct rlimit old_lim;

#if 0
  // this test is unfortunately very system-dependent,
  // because it depends on value of host system limits
  // and how exceeding the limit is handled. On some
  // RMC OSL machines, this may exceed the limit and
  // cause a core dump.
  /* test limit on file size */
  mq_unlink("/test8");
  getrlimit(RLIMIT_FSIZE, &lim);
  old_lim = lim;
  lim.rlim_cur = 0;
  rv = setrlimit(RLIMIT_FSIZE, &lim);
  ASSERT_EQ(rv, 0) << "setrlimit failed";

  osal_mq_attr_t attr2 = attr;
  attr2.max_messages = 1000; /* system default, won't work with larger
                              * number without adjustment */
  attr2.max_message_size = 256;
  orv = osal_mq_open(&mqueue, "/test8", &attr2);
  if (orv != 0) {
    perror("failed to open mq:");
  }
  EXPECT_EQ(orv, OSAL_ERR_INVALID_PARAM) << "osal_mq_open() failed";

  rv = setrlimit(RLIMIT_FSIZE, &old_lim);
  ASSERT_EQ(rv, 0) << "setrlimit failed";
#endif

  /* test limit on data size */
  mq_unlink("/test8");
  getrlimit(RLIMIT_DATA, &lim);
  old_lim = lim;
  lim.rlim_cur = 0;
  rv = setrlimit(RLIMIT_DATA, &lim);
  ASSERT_EQ(rv, 0) << "setrlimit failed";

  osal_mq_attr_t attr3 = attr;
  attr3.max_messages = 100; /* system default, won't work with larger
                             * number without adjustment */
  attr3.max_message_size = 4000;
  orv = osal_mq_open(&mqueue, "/test8", &attr3);
  if (orv != 0) {
    perror("failed to open mq:");
  }
  EXPECT_EQ(orv, OSAL_ERR_INVALID_PARAM) << "osal_mq_open() failed";

  rv = setrlimit(RLIMIT_DATA, &old_lim);
  ASSERT_EQ(rv, 0) << "setrlimit failed";

  // try to mq_close invalid descriptor

  memset(&mqueue, 255, sizeof(mqueue));
  orv = osal_mq_close(&mqueue);
  if (orv != 0) {
    perror("failed to close mq:");
  }
  EXPECT_EQ(orv, OSAL_ERR_INVALID_PARAM) << "osal_mq_close() failed";

  // trying to run against max number of queue limit
  const int num_queues = 5000;
  osal_mq_t queue_arr[num_queues];

  int cutoff = -1;
  for (int i = 0; i < num_queues; i++) {
    orv = osal_mq_open(&queue_arr[i], "/test8", &attr);
    if (orv != 0) {
      perror("failed to open mq:");
      break;
    }
    cutoff = i;
  }
  EXPECT_EQ(orv, OSAL_ERR_SYSTEM_LIMIT_REACHED) << "osal_mq_open() failed";
  for (int i = 0; i <= cutoff; i++) {
    orv = osal_mq_close(&queue_arr[i]);
    ASSERT_EQ(orv, OSAL_OK) << "osal_mq_close() failed";
  }
}

} // namespace test_maxresources

namespace test_send_errors {

TEST(MessageQueue, TestSendErrors) {

  // int rv;
  osal_retval_t orv;
  osal_mq_t mqueue;
  unsigned char buf[256];

  // initialize message queue
  osal_mq_attr_t attr = {};
  attr.oflags = OSAL_MQ_ATTR__OFLAG__RDWR | OSAL_MQ_ATTR__OFLAG__CREAT;
  attr.max_messages = 1; /* system default, won't work with larger
                          * number without adjustment */
  ASSERT_GE(attr.max_messages, 0u);
  attr.max_message_size = 16;
  ASSERT_GE(attr.max_message_size, 0u);
  attr.mode = S_IRUSR | S_IWUSR;

  /* test with a too large buffer */

  // unlink message queue if it exists.
  // Note: the return value is intentionally not checked.
  mq_unlink("/test9");

  orv = osal_mq_open(&mqueue, "/test9", &attr);
  if (orv != 0) {
    perror("failed to open mq:");
  }
  EXPECT_EQ(orv, OSAL_OK) << "osal_mq_open() failed";

  memset(&buf, 1, sizeof(buf));
  orv = osal_mq_send(&mqueue, (const osal_char_t *)&buf, sizeof(buf), 1);
  // we expect this to fail since the message buffer is larger
  // than the configured queue message size
  EXPECT_EQ(orv, OSAL_ERR_INVALID_PARAM) << "osal_mq_send() failed";

  osal_timer_t deadline = set_deadline(1, 0);

  orv = osal_mq_timedsend(&mqueue, (const osal_char_t *)&buf, sizeof(buf), 1,
                          &deadline);
  // expect invalid because buffer is larger than configured in mq
  EXPECT_EQ(orv, OSAL_ERR_INVALID_PARAM) << "osal_mq_send() failed";

  // check with invalid deadline
  deadline.sec = -1;
  orv = osal_mq_timedsend(&mqueue, (const osal_char_t *)&buf, 16, 1, &deadline);
  EXPECT_EQ(orv, OSAL_ERR_INVALID_PARAM) << "osal_mq_send() failed";

  // test with invalid descriptor
  osal_mq_t mqueue2;
  memset(&mqueue2, 0, sizeof(mqueue2));
  orv = osal_mq_send(&mqueue2, (const osal_char_t *)&buf, sizeof(buf), 1);
  // we excpect to fail because the descriptor is invalid
  EXPECT_EQ(orv, OSAL_ERR_INVALID_PARAM) << "osal_mq_send() failed";

  // check timed send with invalid descriptor
  orv = osal_mq_timedsend(&mqueue2, (const osal_char_t *)&buf, sizeof(buf), 1,
                          &deadline);
  EXPECT_EQ(orv, OSAL_ERR_INVALID_PARAM) << "osal_mq_send() failed";
}
} // namespace test_send_errors

namespace test_receive_errors {

TEST(MessageQueue, TestReceiveErrors) {

  // int rv;
  osal_retval_t orv;
  osal_mq_t mqueue;
  unsigned char buf[256];

  // initialize message queue
  osal_mq_attr_t attr = {};
  attr.oflags = OSAL_MQ_ATTR__OFLAG__RDWR | OSAL_MQ_ATTR__OFLAG__CREAT;
  attr.max_messages = 10; /* system default, won't work with larger
                           * number without adjustment */
  ASSERT_GE(attr.max_messages, 0u);
  attr.max_message_size = 16;
  ASSERT_GE(attr.max_message_size, 0u);
  attr.mode = S_IRUSR | S_IWUSR;

  /* test with a too large buffer */

  // unlink message queue if it exists.
  // Note: the return value is intentionally not checked.
  mq_unlink("/test9");

  orv = osal_mq_open(&mqueue, "/test9", &attr);
  if (orv != 0) {
    perror("failed to open mq:");
  }
  EXPECT_EQ(orv, OSAL_OK) << "osal_mq_open() failed";

  memset(&buf, 1, sizeof(buf));
  osal_uint32_t prio = {};

  printf("provoke timeout\n");
  osal_timer_t deadline = set_deadline(1, 0);

  orv =
      osal_mq_timedreceive(&mqueue, (osal_char_t *)&buf, 16, &prio, &deadline);
  // expect timeout, no message is present
  EXPECT_EQ(orv, OSAL_ERR_TIMEOUT) << "osal_mq_send() failed";

  // send some message
  printf("small buffer: prepare\n");
  orv = osal_mq_send(&mqueue, (const osal_char_t *)&buf, 16, 1);
  EXPECT_EQ(orv, OSAL_OK) << "osal_mq_send() failed";

  printf("small buffer: recv\n");
  orv = osal_mq_receive(&mqueue, (osal_char_t *)&buf, 10, &prio);
  // we expect this to fail since the message buffer is larger
  // than the configured queue message size
  EXPECT_EQ(orv, OSAL_ERR_INVALID_PARAM) << "osal_mq_send() failed";

  // send some message
  printf("small buffer, timed: prepare\n");
  orv = osal_mq_send(&mqueue, (const osal_char_t *)&buf, 16, 1);
  EXPECT_EQ(orv, OSAL_OK) << "osal_mq_send() failed";

  printf("small buffer, timed: recv\n");
  osal_timer_t deadline1 = set_deadline(1, 0);

  orv =
      osal_mq_timedreceive(&mqueue, (osal_char_t *)&buf, 10, &prio, &deadline1);
  // expect invalid because buffer is larger than configured in mq
  EXPECT_EQ(orv, OSAL_ERR_INVALID_PARAM) << "osal_mq_send() failed";

  printf("invalid deadline: prepare\n");
  // send some message
  // orv = osal_mq_send(&mqueue, (const osal_char_t*)&buf, 16, 1);
  // EXPECT_EQ(orv, OSAL_OK) << "osal_mq_send() failed";

  // check with invalid deadline
  printf("invalid deadline: recv\n");
  osal_timer_t deadlinei = set_deadline(1, 0);
  deadlinei.sec = -1;
  orv =
      osal_mq_timedreceive(&mqueue, (osal_char_t *)&buf, 16, &prio, &deadlinei);
  EXPECT_EQ(orv, OSAL_ERR_INVALID_PARAM) << "osal_mq_send() failed";

  // send some message
  printf("invalid descriptor: prepare\n");
  orv = osal_mq_send(&mqueue, (const osal_char_t *)&buf, 16, 1);
  EXPECT_EQ(orv, OSAL_OK) << "osal_mq_send() failed";

  // test with invalid descriptor
  printf("invalid descriptor: recv\n");
  osal_mq_t mqueue2;
  memset(&mqueue2, 0, sizeof(mqueue2));
  orv = osal_mq_receive(&mqueue2, (osal_char_t *)&buf, sizeof(buf), &prio);
  // we excpect to fail because the descriptor is invalid
  EXPECT_EQ(orv, OSAL_ERR_INVALID_PARAM) << "osal_mq_send() failed";

  // check timed send with invalid descriptor
  // send some message
  printf("invalid descriptor, timed: prepare\n");
  orv = osal_mq_send(&mqueue, (const osal_char_t *)&buf, 16, 1);
  EXPECT_EQ(orv, OSAL_OK) << "osal_mq_send() failed";

  printf("invalid descriptor, timed: recv\n");
  osal_timer_t deadline2 = set_deadline(1, 0);
  orv = osal_mq_timedreceive(&mqueue2, (osal_char_t *)&buf, sizeof(buf), &prio,
                             &deadline2);
  EXPECT_EQ(orv, OSAL_ERR_INVALID_PARAM) << "osal_mq_send() failed";
}
} // namespace test_receive_errors

} // namespace test_messagequeue

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  if (getenv("VERBOSE")) {
    test_messagequeue::verbose = 1;
  }

  return RUN_ALL_TESTS();
}
