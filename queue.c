#include "queue.h"
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>

struct queue {
    int size;
    int in;
    int out;
    int count;
    void **buf;
    pthread_mutex_t mutex; //mutex to protect shared queue access
    pthread_cond_t empty; //condition variable to signal when queue is not full
    pthread_cond_t full; //condition variable to signal when queue is not empty
};

/**
 * @brief Creates and initializes a new queue.
 * 
 * This function allocates memory for the queue structure and buffer,
 * initializes synchronization primitives, and sets queue parameters.
 *
 * @param size The maximum number of elements the queue can hold.
 * @return Pointer to the newly created queue, or NULL if allocation fails or size is invalid.
 */

queue_t *queue_new(int size) {
    if (size <= 0)
        return NULL;
    queue_t *q = (queue_t *) malloc(sizeof(queue_t));
    q->buf = (void **) malloc(size * sizeof(void *));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->empty, NULL);
    pthread_cond_init(&q->full, NULL);
    q->size = size;
    q->in = 0;
    q->out = 0;
    q->count = 0;
    return q;
}

/**
 * @brief Deletes a queue and releases allocated resources.
 * 
 * This function frees memory for the queue and its buffer and destroys
 * the mutex and condition variables to prevent resource leaks.
 *
 * @param q Pointer to the queue pointer that will be deleted.
 */

void queue_delete(queue_t **q) {
    pthread_mutex_destroy(&(*q)->mutex);
    pthread_cond_destroy(&(*q)->empty);
    pthread_cond_destroy(&(*q)->full);
    free((*q)->buf);
    (*q)->buf = NULL;
    free(*q);
    *q = NULL;
}

/**
 * @brief Pushes an element into the queue (enqueue operation).
 *
 * If the queue is full, the function blocks until space becomes available.
 * The queue maintains a circular buffer, so the insertion index wraps around.
 *
 * @param q Pointer to the queue.
 * @param elem Pointer to the element to enqueue.
 * @return true if the element was successfully enqueued, false if queue is NULL.
 */

bool queue_push(queue_t *q, void *elem) {
    if (q == NULL) {
        return false;
    }
    pthread_mutex_lock(&q->mutex);
    while (q->count == q->size) {
        pthread_cond_wait(&q->empty, &q->mutex);
    }
    q->buf[q->in] = elem;
    q->in = (q->in + 1) % (q->size);
    q->count++;

    // Signal that the queue is not empty
    pthread_cond_signal(&q->full);
    pthread_mutex_unlock(&q->mutex);
    return true;
}

/**
 * @brief Removes an element from the queue (dequeue operation).
 *
 * If the queue is empty, the function blocks until an element becomes available.
 * The queue maintains a circular buffer, so the removal index wraps around.
 *
 * @param q Pointer to the queue.
 * @param elem Pointer to store the dequeued element.
 * @return true if an element was successfully dequeued, false if queue is NULL.
 */

bool queue_pop(queue_t *q, void **elem) {
    if (q == NULL) {
        return false;
    }

    pthread_mutex_lock(&q->mutex);
    while (q->count == 0) {
        pthread_cond_wait(&q->full, &q->mutex);
    }

// Remove element from queue
    *elem = q->buf[q->out];
    q->out = (q->out + 1) % q->size; //circular increment
    q->count--;
    pthread_cond_signal(&q->empty);
    pthread_mutex_unlock(&q->mutex);
    return true;
}

