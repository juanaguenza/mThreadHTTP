// Asgn 4: A multi-threaded HTTP server.
// By: Juan Aguenza

#include "asgn2_helper_funcs.h"
#include "connection.h"
// #include "debug.h"
#include "response.h"
#include "request.h"
#include "queue.h"

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>

void *startThread();

void handle_connection(int);

void handle_get(conn_t *);
void handle_put(conn_t *);
void handle_unsupported(conn_t *);

queue_t *q;

pthread_mutex_t mutex;

extern int errno;

int main(int argc, char **argv) {
    if (argc < 2) { // should have ./httpserver (1) -t (2) #threads (3) #port (4)
        warnx("wrong arguments: %s -t threads_count port_num", argv[0]);
        fprintf(stderr, "usage: %s [-t threads] <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int num_threads;
    size_t port;

    if (argc == 2) {
        // parse as if there is only a port number
        char *endptr = NULL;
        port = (size_t) strtoull(argv[optind], &endptr, 10);
        if (endptr && *endptr != '\0') {
            warnx("invalid port number: %s", argv[1]);
            return EXIT_FAILURE;
        }
        num_threads = 4;
    }
    // case where there is a thread # as well as port num
    else if (argc == 4) {
        // to get the number of threads
        int opt;
        char *num_threads_str;
        opt = getopt(argc, argv, "t:");

        switch (opt) {
        case 't':
            if (optarg == NULL) {
                return EXIT_FAILURE;
            } else {
                num_threads_str = optarg;
            }
        }

        // convert the number of threads into an int
        // if nothing was passed then we set it to the default of 4
        num_threads = atoi(num_threads_str);

        char *endptr = NULL;
        port = (size_t) strtoull(argv[optind], &endptr, 10);
        if (endptr && *endptr != '\0') {
            warnx("invalid port number: %s", argv[1]);
            return EXIT_FAILURE;
        }
    } else {
        warnx("wrong arguments: %s -t threads_count port_num", argv[0]);
        fprintf(stderr, "usage: %s [-t threads] <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    // create the bounded buffer with size num_threads
    q = queue_new(num_threads);

    pthread_t threads[num_threads];

    for (int i = 0; i < num_threads; i++) {
        pthread_create(&threads[i], NULL, &startThread, NULL);
    }

    signal(SIGPIPE, SIG_IGN);
    Listener_Socket sock;
    listener_init(&sock, port);

    while (1) {
        uintptr_t connfd = listener_accept(&sock);
        queue_push(q, (void *) connfd);
    }

    return EXIT_SUCCESS;
}

void *startThread() {
    while (1) {
        int next_job_fd = 0;
        queue_pop(q, (void **) &next_job_fd);
        handle_connection(next_job_fd);
        close(next_job_fd);
    }
}

void handle_connection(int connfd) {

    conn_t *conn = conn_new(connfd);

    const Response_t *res = conn_parse(conn);

    if (res != NULL) {
        conn_send_response(conn, res);
    } else {
        // debug("%s", conn_str(conn));
        const Request_t *req = conn_get_request(conn);
        if (req == &REQUEST_GET) {
            handle_get(conn);
        } else if (req == &REQUEST_PUT) {
            handle_put(conn);
        } else {
            handle_unsupported(conn);
        }
    }

    conn_delete(&conn);
}

void handle_get(conn_t *conn) {

    char *uri = conn_get_uri(conn);
    const Response_t *res = NULL;

    pthread_mutex_lock(&mutex);

    bool error = false;

    // 1. Open the file.
    int fd = open(uri, O_RDONLY, 0600);

    flock(fd, LOCK_SH);

    pthread_mutex_unlock(&mutex);

    // If the file doesn't open
    if (fd < 0) {
        error = true;
        // debug("%s: %d", uri, errno);
        if (errno == EACCES) {
            conn_send_response(conn, &RESPONSE_FORBIDDEN);
        } else {
            conn_send_response(conn, &RESPONSE_NOT_FOUND);
        }
    }

    // 2. Get the size of the file.
    // (hint: checkout the function fstat)!

    // initialize a stat buffer
    struct stat *buf_stat;
    buf_stat = malloc(sizeof(struct stat));

    // place the results into the buffer
    fstat(fd, buf_stat);

    // get the total bytes of the file;
    uint64_t total_bytes = buf_stat->st_size;

    // free the stat buffer
    free(buf_stat);

    // 4. Send the file
    // (hint: checkout the conn_send_file function!)
    if (error == false) {
        res = conn_send_file(conn, fd, total_bytes);
    }

    if (res == NULL) {
        res = &RESPONSE_OK;
    }

    fprintf(stderr, "%s,%s,%hu,%s\n", request_get_str(&REQUEST_GET), uri, response_get_code(res),
        conn_get_header(conn, "Request-Id"));

    close(fd);
}

void handle_unsupported(conn_t *conn) {
    // debug("handling unsupported request");

    // send responses
    conn_send_response(conn, &RESPONSE_NOT_IMPLEMENTED);
}

void handle_put(conn_t *conn) {
    pthread_mutex_lock(&mutex);

    char *uri = conn_get_uri(conn);
    const Response_t *res = NULL;

    // Check if file already exists before opening it.
    bool existed = access(uri, F_OK) == 0;

    // Open the file..
    int fd = open(uri, O_CREAT | O_WRONLY, 0600);
    if (fd < 0) {
        // debug("%s: %d", uri, errno);
        if (errno == EACCES || errno == EISDIR || errno == ENOENT) {
            res = &RESPONSE_FORBIDDEN;
            goto out;
        } else {
            res = &RESPONSE_INTERNAL_SERVER_ERROR;
            goto out;
        }
    }

    flock(fd, LOCK_EX);

    pthread_mutex_unlock(&mutex);

    // TRUNC ONLY AFTER WE LOCK THE FILE

    ftruncate(fd, 0);

    res = conn_recv_file(conn, fd);

    if (res == NULL && existed) {
        res = &RESPONSE_OK;
    } else {
        res = &RESPONSE_CREATED;
    }

    fprintf(stderr, "%s,%s,%hu,%s\n", request_get_str(&REQUEST_PUT), uri, response_get_code(res),
        conn_get_header(conn, "Request-Id"));

    close(fd);

out:
    conn_send_response(conn, res);
}
