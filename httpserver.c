/**
 * @file httpserver.c
 * @brief Multithreaded HTTP server handling GET and PUT requests with concurrency and synchronization.
 *
 * This server uses thread pools, read-write locks, and a URI store to process HTTP requests efficiently.
 * It supports multiple concurrent connections and ensures proper synchronization when accessing shared files.
 */

#include "iowrapper.h"
#include "listener_socket.h"
#include "connection.h"
#include "response.h"
#include "request.h"
#include "queue.h"
#include "rwlock.h"

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/stat.h>

#define OPTIONS "t:"
#define DEFAULT_THREADS 4   // Default thread count if not specified
#define MAX_URIS 1024       // Maximum number of unique URIs that can be handled

/**
 * @brief Hash function to generate an index for a given URI.
 *
 * This function implements a simple DJB2 hash algorithm to map URIs
 * to a bucket index within `uri_store_t`.
 *
 * @param uri The URI string to be hashed.
 * @return The computed hash index within the `MAX_URIS` range.
 */
static unsigned int hash_uri_key(const char *uri) {
    unsigned int hash = 5381;
    int c;
    while ((c = *uri++)) {
        hash = ((hash << 5) + hash) + c;  // Equivalent to hash * 33 + c
    }
    return hash % MAX_URIS;
}

/**
 * @struct uri_entry
 * @brief Stores metadata for each URI being accessed.
 *
 * Each `uri_entry_t` represents a unique resource (file) being requested
 * and maintains its own read-write lock to allow concurrent access control.
 */
typedef struct uri_entry {
    char *uri;                 // URI string
    rwlock_t *lock;            // Read-Write lock for concurrent access
    int ref_count;             // Reference count for tracking active operations
    struct uri_entry *next;    // Pointer to next entry (for collision handling)
} uri_entry_t;

/**
 * @struct uri_store_t
 * @brief Thread-safe data structure for tracking all active URIs.
 *
 * Implements a hash table with buckets storing `uri_entry_t` structures.
 * Each bucket contains a linked list of URIs mapped to the same hash index.
 */
typedef struct {
    uri_entry_t *buckets[MAX_URIS];  // Hash table for URI storage
    pthread_mutex_t mutex;           // Mutex to synchronize URI entry modifications
} uri_store_t;

// Global variables for the request queue, logging, and URI tracking
static queue_t *task_queue = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static uri_store_t *uri_store = NULL;

/**
 * @brief Logs an HTTP request in a thread-safe manner.
 *
 * @param operation Type of request (e.g., GET, PUT).
 * @param uri The requested resource URI.
 * @param status The HTTP response status code.
 * @param request_id The unique ID associated with the request.
 */
void log_http_request(const char *operation, const char *uri, int status, const char *request_id) {
    pthread_mutex_lock(&log_mutex);
    fprintf(stderr, "%s,%s,%d,%s\n", operation, uri, status, request_id);
    fflush(stderr);
    pthread_mutex_unlock(&log_mutex);
}

/**
 * @brief Retrieves the Request-Id header from a connection.
 *
 * If no Request-Id is found, it defaults to "0" to ensure logs are consistent.
 *
 * @param conn The active connection object.
 * @return The Request-Id string or "0" if missing.
 */
const char *get_request_identifier(conn_t *conn) {
    char *request_id = conn_get_header(conn, "Request-Id");
    return (request_id && strlen(request_id) > 0) ? request_id : "0";
}

/**
 * @brief Retrieves or creates a URI entry in the global `uri_store`.
 *
 * This function ensures that each URI has a corresponding `uri_entry_t` that
 * contains its read-write lock, allowing proper synchronization.
 *
 * @param uri The requested resource URI.
 * @return A pointer to the associated `uri_entry_t`, or NULL on failure.
 */
static uri_entry_t *retrieve_uri_entry(const char *uri) {
    unsigned int idx = hash_uri_key(uri);
    pthread_mutex_lock(&uri_store->mutex);
    
    uri_entry_t *entry = uri_store->buckets[idx];
    while (entry) {
        if (strcmp(entry->uri, uri) == 0) {
            entry->ref_count++;
            pthread_mutex_unlock(&uri_store->mutex);
            return entry;
        }
        entry = entry->next;
    }
    
    // Create a new entry if the URI doesn't exist
    entry = calloc(1, sizeof(uri_entry_t));
    if (!entry) {
        pthread_mutex_unlock(&uri_store->mutex);
        return NULL;
    }
    
    entry->uri = strdup(uri);
    entry->lock = rwlock_new(0, 0); 
    entry->ref_count = 1;
    entry->next = uri_store->buckets[idx];
    uri_store->buckets[idx] = entry;
    
    pthread_mutex_unlock(&uri_store->mutex);
    return entry;
}

/**
 * @brief Releases a URI entry, freeing memory if it is no longer needed.
 *
 * This function decrements the reference count and removes the URI entry
 * from the `uri_store` when it is no longer in use.
 *
 * @param entry The URI entry to release.
 */
static void release_uri_data(uri_entry_t *entry) {
    if (!entry) return;
    
    unsigned int idx = hash_uri_key(entry->uri);
    pthread_mutex_lock(&uri_store->mutex);
    
    entry->ref_count--;
    if (entry->ref_count == 0) {
        uri_entry_t *curr = uri_store->buckets[idx];
        uri_entry_t *prev = NULL;
        
        while (curr) {
            if (curr == entry) {
                if (prev)
                    prev->next = curr->next;
                else
                    uri_store->buckets[idx] = curr->next;
                
                rwlock_delete(&curr->lock);
                free(curr->uri);
                free(curr);
                break;
            }
            prev = curr;
            curr = curr->next;
        }
    }
    
    pthread_mutex_unlock(&uri_store->mutex);
}

/**
 * @brief Handles HTTP GET requests.
 *
 * Reads a file from the server's directory and sends its content back to the client.
 * Implements read-locks to allow concurrent GET requests while preventing PUT conflicts.
 *
 * @param conn The active connection.
 */
void handle_get_request(conn_t *conn) {
    const char *uri = conn_get_uri(conn);
    const Response_t *response = NULL;
    const char *request_id = get_request_identifier(conn);

    uri_entry_t *entry = retrieve_uri_entry(uri);
    if (!entry) {
        response = &RESPONSE_INTERNAL_SERVER_ERROR;
        conn_send_response(conn, response);
        log_http_request("GET", uri, 500, request_id);
        return;
    }

    reader_lock(entry->lock);

    int fd = open(uri, O_RDONLY);
    if (fd < 0) {
        reader_unlock(entry->lock);
        release_uri_data(entry);

        response = (errno == EACCES) ? &RESPONSE_FORBIDDEN :
                   (errno == ENOENT) ? &RESPONSE_NOT_FOUND :
                                       &RESPONSE_INTERNAL_SERVER_ERROR;
        conn_send_response(conn, response);
        log_http_request("GET", uri, response_get_code(response), request_id);
        return;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || S_ISDIR(st.st_mode)) {
        close(fd);
        reader_unlock(entry->lock);
        release_uri_data(entry);
        response = &RESPONSE_FORBIDDEN;
        conn_send_response(conn, response);
        log_http_request("GET", uri, 403, request_id);
        return;
    }

    response = conn_send_file(conn, fd, st.st_size);
    close(fd);
    reader_unlock(entry->lock);
    release_uri_data(entry);

    log_http_request("GET", uri, response ? 500 : 200, request_id);
}

/**
 * @brief Handles HTTP PUT requests.
 *
 * Receives file content from the client and writes it to the server's directory.
 * Implements write-locks to prevent race conditions.
 *
 * @param conn The active connection.
 */

void handle_put_request(conn_t *conn) {
    const char *uri = conn_get_uri(conn);
    const Response_t *response = NULL;
    const char *request_id = get_request_identifier(conn);
    bool file_exists = false;

    // Retrieve or create the lock for the URI
    uri_entry_t *entry = retrieve_uri_entry(uri);
    if (!entry) {
        response = &RESPONSE_INTERNAL_SERVER_ERROR;
        conn_send_response(conn, response);
        log_http_request("PUT", uri, 500, request_id);
        return;
    }

    // Acquire write lock
    writer_lock(entry->lock);

    file_exists = (access(uri, F_OK) == 0);// Check if the file exists before writing
					   //
    int fd = open(uri, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) {
        writer_unlock(entry->lock);
	release_uri_data(entry);

        if (errno == EACCES || errno == EISDIR) {
            response = &RESPONSE_FORBIDDEN;
            conn_send_response(conn, response);
            log_http_request("PUT", uri, 403, request_id);
        } else {
            response = &RESPONSE_INTERNAL_SERVER_ERROR;
            conn_send_response(conn, response);
            log_http_request("PUT", uri, 500, request_id);
        }
        return;
    }

    // Receive file data from the client
    response = conn_recv_file(conn, fd);

    if (fsync(fd) < 0) {  // Ensure file data is flushed
        response = &RESPONSE_INTERNAL_SERVER_ERROR;
    }

    close(fd);

    if (response == NULL) {
        response = file_exists ? &RESPONSE_OK : &RESPONSE_CREATED;
        conn_send_response(conn, response);
        log_http_request("PUT", uri, file_exists ? 200 : 201, request_id);
    } else {
        unlink(uri);  // Remove file if receiving failed
        conn_send_response(conn, response);
        log_http_request("PUT", uri, 500, request_id);
    }

    writer_unlock(entry->lock);
    release_uri_data(entry);

}

/**
 * @brief Handles unsupported HTTP requests.
 *
 * If a request type other than GET or PUT is received, this function responds
 * with a `501 Not Implemented` status.
 *
 * @param conn The active connection object.
 */

void handle_unsupported_request(conn_t *conn) {
  const char *request_id = get_request_identifier(conn);
  const char *uri = conn_get_uri(conn);

  //the 501 response to be sent and logged as an unsupported request attempt
  conn_send_response(conn, &RESPONSE_NOT_IMPLEMENTED);
  log_http_request("UNSUPPORTED", uri, 501, request_id);
}

/**
 * @brief Processes an incoming client connection.
 *
 * This function determines the type of HTTP request (GET, PUT, or unsupported)
 * and dispatches it to the appropriate handler. It ensures proper parsing
 * and response handling.
 *
 * @param connfd The file descriptor for the client connection.
 */

void process_connection(int connfd) {
    if (connfd < 0) return;

    //create the new connection object from the file descriptor 
    conn_t *conn = conn_new(connfd);
    if (!conn) return;


    //parse the request to check for errors
    const Response_t *response = conn_parse(conn);
    if (response) {
        conn_send_response(conn, response);
    } else {
	//determine the request type and dispatch it to the appropriate handler
        const Request_t *request = conn_get_request(conn);
        if (request == &REQUEST_GET) {
            handle_get_request(conn);
        } else if (request == &REQUEST_PUT) {
            handle_put_request(conn);
        } else {
            handle_unsupported_request(conn);
        }
    }

    conn_delete(&conn);  // Ensure connection is properly closed after processing
}

/**
 * @brief Worker thread function for handling incoming connections.
 *
 * Each worker thread continuously retrieves client connections from the request queue,
 * processes them, and closes the connection upon completion.
 *
 * @param arg Unused argument (required for pthread compatibility).
 * @return Always returns NULL.
 */

void *worker_thread(void *arg) {
    (void)arg;
    while (1) {
        uintptr_t connfd;

	//get the next client connection from the queue
        queue_pop(task_queue, (void **)&connfd);

	//process that connection
	//
        process_connection((int)connfd);
        close((int)connfd);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    int opt = 0;
    int num_threads = DEFAULT_THREADS;  // Default to 4 worker threads

    // Parse command-line arguments for thread count
    while ((opt = getopt(argc, argv, OPTIONS)) != -1) {
        switch (opt) {
        case 't':
            num_threads = atoi(optarg);
            if (num_threads <= 0) {
                fprintf(stderr, "Error: Invalid thread count '%s'. Must be > 0.\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        default:
            fprintf(stderr, "Usage: %s [-t threads] <port>\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    // Ensure port number is provided
    if (optind >= argc) {
        fprintf(stderr, "Error: Port number is required.\n");
        return EXIT_FAILURE;
    }

    // Parse and validate port number
    char *endptr = NULL;
    size_t port = (size_t) strtoull(argv[optind], &endptr, 10);
    if (endptr && *endptr != '\0') {
        fprintf(stderr, "Error: Invalid port number '%s'. Must be between 1-65535.\n", argv[optind]);
        return EXIT_FAILURE;
    }

    // Ignore SIGPIPE to prevent server crashes when clients disconnect abruptly
    signal(SIGPIPE, SIG_IGN);

    // Initialize URI store (for thread-safe locking on resources)
    uri_store = calloc(1, sizeof(uri_store_t));
    if (!uri_store) {
        fprintf(stderr, "Error: Failed to allocate URI store.\n");
        return EXIT_FAILURE;
    }
    pthread_mutex_init(&uri_store->mutex, NULL);

    // Initialize the queue for handling incoming connections
    task_queue = queue_new(num_threads * 2);
    if (!task_queue) {
        fprintf(stderr, "Error: Failed to create request queue.\n");
        return EXIT_FAILURE;
    }

    // Initialize listener socket
    Listener_Socket_t *listener = ls_new(port);
    if (!listener) {
        fprintf(stderr, "Error: Failed to initialize listener socket.\n");
        return EXIT_FAILURE;
    }

    pthread_t threads[num_threads];
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&threads[i], NULL, worker_thread, NULL) != 0) {
            fprintf(stderr, "Failed to create thread\n");
            return EXIT_FAILURE;
        }
    }

    while (1) {
        int connfd = ls_accept(listener);
        if (connfd < 0) continue;
        queue_push(task_queue, (void *)(uintptr_t)connfd);
    }
//poop
    return EXIT_SUCCESS;

}

