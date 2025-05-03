#include "rwlock.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>

struct rwlock {
    pthread_mutex_t mutex;
    pthread_cond_t readers_cv;
    pthread_cond_t writers_cv;
    int readers;
    int writers;
    int readers_waiting;
    int nway_readers;
    int writers_waiting;
    int n;
    int rw_flag;
    int total_readers;
    PRIORITY priority;
};

rwlock_t *rwlock_new(PRIORITY p, uint32_t n) {

    rwlock_t *rw = (rwlock_t *) malloc(sizeof(rwlock_t));
    if (rw == NULL) {
        return NULL; //if allocation failed
    }

    pthread_mutex_init(&rw->mutex, NULL);
    pthread_cond_init(&rw->readers_cv, NULL);
    pthread_cond_init(&rw->writers_cv, NULL);
    rw->priority = p;
    rw->n = n;
    rw->rw_flag = 0;
    rw->writers = 0;
    rw->readers_waiting = 0;
    rw->nway_readers = 0;
    rw->writers_waiting = 0;
    rw->readers = 0;
    rw->total_readers = 0;
    return rw;
}

void rwlock_delete(rwlock_t **rw) {
    if (rw != NULL) {
        pthread_mutex_destroy(&(*rw)->mutex);
        pthread_cond_destroy(&(*rw)->readers_cv);
        pthread_cond_destroy(&(*rw)->writers_cv);
        free(*rw);
        *rw = NULL;
    }
}

//if reader priority, only return active writers
//if writer priority, active or waiting writers

int readerwait(rwlock_t *rw) {
    if (rw->priority == WRITERS) {
        return rw->writers || rw->writers_waiting;
    } else if (rw->priority == READERS) {
        return rw->writers;
    } else if (rw->priority == N_WAY) {
        // If N_WAY priority
        if (rw->readers > 0) {
            if (rw->writers_waiting > 0) {
                if ((rw->nway_readers < rw->n) && (rw->rw_flag == 1)) {
                    // return rw->writers || rw->writers_waiting;
                    return 0;
                } else {
                    if (rw->rw_flag == 0) {
                        return rw->writers || rw->writers_waiting;
                    } else {
                        return 0;
                    }
                }
            }
        } else if (rw->writers > 0) {
            return rw->writers;
        } else {
            if (rw->rw_flag == 0) {
                return rw->writers_waiting;
            }
        }
    }
    return 0;
}
int writerwait(rwlock_t *rw) {
    if (rw->priority == WRITERS) {
        if (rw->writers > 0 || rw->readers > 0) {
            return rw->writers > 0 || rw->readers > 0;
        }
    } else if (rw->priority == READERS) {
        return rw->readers > 0 || rw->writers > 0;
    } else if (rw->priority == N_WAY) {
        if (rw->readers > 0) {
            return rw->readers > 0;
        } else if (rw->writers > 0) {
            return rw->writers;
        } else {
            if (rw->rw_flag == 1) {
                return rw->readers_waiting; // Writers wait if rw_flag is set
            } else {
                return 0;
            }
        }
    }
    return 0;
}

// if n-way flag is zero and n-way readers <n whenever in nway & check nway redears = n
void reader_lock(rwlock_t *rw) {
    pthread_mutex_lock(&rw->mutex);
    rw->total_readers++;
    rw->readers_waiting++;
    while (readerwait(rw)) {
        pthread_cond_wait(&rw->readers_cv, &rw->mutex);
    }
    rw->readers_waiting--;
    rw->readers++;
    if (rw->priority == N_WAY) {
        if (rw->nway_readers < rw->n) {
            rw->nway_readers++;
            if (rw->nway_readers == rw->n) {
                rw->rw_flag = 0;
                rw->nway_readers = 0;
            }
        }
    }
    pthread_mutex_unlock(&rw->mutex);
}

void reader_unlock(rwlock_t *rw) {
    pthread_mutex_lock(&rw->mutex);
    if (rw->total_readers == rw->n) {
        rw->total_readers = 0;
    }
    rw->readers--;
    pthread_cond_broadcast(&rw->readers_cv);
    pthread_cond_signal(&rw->writers_cv);
    pthread_mutex_unlock(&rw->mutex);
}

void writer_lock(rwlock_t *rw) {
    pthread_mutex_lock(&rw->mutex);
    rw->writers_waiting++;
    while (writerwait(rw)) {
        pthread_cond_wait(&rw->writers_cv, &rw->mutex);
    }
    rw->writers_waiting--;
    rw->writers++;
    pthread_mutex_unlock(&rw->mutex);
}

void writer_unlock(rwlock_t *rw) {
    pthread_mutex_lock(&rw->mutex);
    rw->writers--;
    if (rw->priority == N_WAY) {
        if (rw->rw_flag == 0) {
            rw->rw_flag = 1;
            rw->nway_readers = 0;
        }
    }
    pthread_cond_broadcast(&rw->readers_cv);
    pthread_cond_broadcast(&rw->writers_cv);
    pthread_mutex_unlock(&rw->mutex);
}

