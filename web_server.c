#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <getopt.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <errno.h>

#define BUFFER_SIZE 8192
#define DEFAULT_PORT 80
#define MAX_PATH_LENGTH 256
#define NUM_WORKERS 20

// Stats structure in shared memory
struct shared_stats {
    unsigned long total_requests;
    unsigned long bytes_received;
    unsigned long bytes_sent;
};

static struct shared_stats *stats = NULL;
static volatile sig_atomic_t server_running = 1;
static pid_t *worker_pids = NULL;

// Function to URL decode a string
void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if (*src == '%' && ((a = src[1]) && (b = src[2])) && isxdigit(a) && isxdigit(b)) {
            if (a >= 'a') a -= 'a'-'A';
            if (b >= 'a') b -= 'a'-'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            *dst++ = 16 * a + b;
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

// Function to extract query parameters
void get_query_params(const char *query, int *a, int *b) {
    char *param;
    char query_copy[256];
    strncpy(query_copy, query, sizeof(query_copy) - 1);

    param = strtok(query_copy, "&");
    while (param) {
        if (strncmp(param, "a=", 2) == 0) {
            *a = atoi(param + 2);
        } else if (strncmp(param, "b=", 2) == 0) {
            *b = atoi(param + 2);
        }
        param = strtok(NULL, "&");
    }
}

// Function to handle /static requests
void handle_static_request(int client_socket, const char *path) {
    char full_path[MAX_PATH_LENGTH];
    char buffer[BUFFER_SIZE];
    int fd;
    ssize_t bytes_read;

    printf("Requested path: %s\n", path);

    const char *file_path = (*path == '/') ? path + 1 : path;
    snprintf(full_path, sizeof(full_path), "./%s", file_path);

    // Debug print
    printf("Full path: %s\n", full_path);

    if (access(full_path, R_OK) != 0) {
        printf("File access error: %s\n", strerror(errno));
        const char *response = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nNot Found";
        send(client_socket, response, strlen(response), 0);
        __sync_fetch_and_add(&stats->bytes_sent, strlen(response));
        return;
    }

    fd = open(full_path, O_RDONLY);
    if (fd == -1) {
        printf("File open error: %s\n", strerror(errno));
        const char *response = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nNot Found";
        send(client_socket, response, strlen(response), 0);
        __sync_fetch_and_add(&stats->bytes_sent, strlen(response));
        return;
    }

    const char *content_type = "application/octet-stream";  // default
    char *ext = strrchr(path, '.');
    if (ext != NULL) {
        ext++;  // Skip the dot
        if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0)
            content_type = "image/jpeg";
        else if (strcasecmp(ext, "png") == 0)
            content_type = "image/png";
        else if (strcasecmp(ext, "gif") == 0)
            content_type = "image/gif";
        else if (strcasecmp(ext, "html") == 0 || strcasecmp(ext, "htm") == 0)
            content_type = "text/html";
        else if (strcasecmp(ext, "css") == 0)
            content_type = "text/css";
        else if (strcasecmp(ext, "js") == 0)
            content_type = "application/javascript";
    }

    // Get file size
    struct stat file_stat;
    fstat(fd, &file_stat);

    // Send HTTP headers
    char header[512];
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %ld\r\n"
             "\r\n",
             content_type, file_stat.st_size);

    send(client_socket, header, strlen(header), 0);
    __sync_fetch_and_add(&stats->bytes_sent, strlen(header));

    // Send file content
    while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0) {
        send(client_socket, buffer, bytes_read, 0);
        __sync_fetch_and_add(&stats->bytes_sent, bytes_read);
    }

    close(fd);
}

// Function to handle /stats requests
void handle_stats_request(int client_socket) {
    char response[BUFFER_SIZE];
    char content[BUFFER_SIZE];

    unsigned long total_reqs = __sync_fetch_and_add(&stats->total_requests, 0);
    unsigned long bytes_recv = __sync_fetch_and_add(&stats->bytes_received, 0);
    unsigned long bytes_sent = __sync_fetch_and_add(&stats->bytes_sent, 0);

    snprintf(content, sizeof(content),
             "<html><body>"
             "<h1>Server Statistics</h1>"
             "<p>Total Requests: %lu</p>"
             "<p>Bytes Received: %lu</p>"
             "<p>Bytes Sent: %lu</p>"
             "</body></html>",
             total_reqs, bytes_recv, bytes_sent);

    snprintf(response, sizeof(response),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: text/html\r\n"
             "Content-Length: %zu\r\n"
             "\r\n%s",
             strlen(content), content);

    send(client_socket, response, strlen(response), 0);
    __sync_fetch_and_add(&stats->bytes_sent, strlen(response));
}

// Function to handle /calc requests
void handle_calc_request(int client_socket, const char *query) {
    int a = 0, b = 0;
    char response[BUFFER_SIZE];
    char content[BUFFER_SIZE];

    get_query_params(query, &a, &b);
    int sum = a + b;

    snprintf(content, sizeof(content),
             "<html><body>"
             "<h1>Calculator Result</h1>"
             "<p>%d + %d = %d</p>"
             "</body></html>",
             a, b, sum);

    snprintf(response, sizeof(response),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: text/html\r\n"
             "Content-Length: %zu\r\n"
             "\r\n%s",
             strlen(content), content);

    send(client_socket, response, strlen(response), 0);
    __sync_fetch_and_add(&stats->bytes_sent, strlen(response));
}

// Worker process function
void worker_process(int server_fd) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    char buffer[BUFFER_SIZE];

    while (server_running) {
        int client_socket = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_socket < 0) {
            if (errno == EINTR) continue;
            perror("accept failed");
            continue;
        }

        ssize_t bytes = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
        if (bytes > 0) {
            buffer[bytes] = '\0';
            __sync_fetch_and_add(&stats->bytes_received, bytes);
            __sync_fetch_and_add(&stats->total_requests, 1);

            char method[16], path[256], version[16], query[256];
            query[0] = '\0';

            sscanf(buffer, "%s %s %s", method, path, version);

            char *query_start = strchr(path, '?');
            if (query_start) {
                *query_start = '\0';
                strncpy(query, query_start + 1, sizeof(query) - 1);
            }

            if (strncmp(path, "/static/", 8) == 0) {
                handle_static_request(client_socket, path);
            } else if (strcmp(path, "/stats") == 0) {
                handle_stats_request(client_socket);
            } else if (strcmp(path, "/calc") == 0) {
                handle_calc_request(client_socket, query);
            } else {
                const char *response = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nNot Found";
                send(client_socket, response, strlen(response), 0);
                __sync_fetch_and_add(&stats->bytes_sent, strlen(response));
            }
        }

        close(client_socket);
    }
}

void handle_signal(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        server_running = 0;

        // Terminate all worker processes
        for (int i = 0; i < NUM_WORKERS; i++) {
            if (worker_pids[i] > 0) {
                kill(worker_pids[i], SIGTERM);
            }
        }
    }
}

int main(int argc, char *argv[]) {
    int server_fd;
    struct sockaddr_in address;
    int opt;
    int port = DEFAULT_PORT;

    while ((opt = getopt(argc, argv, "p:")) != -1) {
        switch (opt) {
            case 'p':
                port = atoi(optarg);
                break;
            default:
                fprintf(stderr, "Usage: %s [-p port]\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    stats = mmap(NULL, sizeof(struct shared_stats),
                PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (stats == MAP_FAILED) {
        perror("mmap failed");
        exit(EXIT_FAILURE);
    }
    memset(stats, 0, sizeof(struct shared_stats));

    worker_pids = malloc(sizeof(pid_t) * NUM_WORKERS);
    if (!worker_pids) {
        perror("malloc failed");
        exit(EXIT_FAILURE);
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // Create socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    int opt_val = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof(opt_val))) {
        perror("setsockopt failed");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Multi-process server listening on port %d with %d worker processes\n",
           port, NUM_WORKERS);

    for (int i = 0; i < NUM_WORKERS; i++) {
        pid_t pid = fork();
        if (pid == -1) {
            perror("fork failed");
            exit(EXIT_FAILURE);
        }
        if (pid == 0) {
            free(worker_pids);
            worker_process(server_fd);
            exit(0);
        } else {
            worker_pids[i] = pid;
        }
    }

    while (server_running) {
        int status;
        pid_t pid = wait(&status);
        if (pid > 0) {
            for (int i = 0; i < NUM_WORKERS; i++) {
                if (worker_pids[i] == pid && server_running) {
                    pid_t new_pid = fork();
                    if (new_pid == 0) {
                        free(worker_pids);
                        worker_process(server_fd);
                        exit(0);
                    } else if (new_pid > 0) {
                        worker_pids[i] = new_pid;
                    }
                    break;
                }
            }
        }
    }

    printf("\nShutting down server...\n");

    for (int i = 0; i < NUM_WORKERS; i++) {
        if (worker_pids[i] > 0) {
            waitpid(worker_pids[i], NULL, 0);
        }
    }

    free(worker_pids);
    munmap(stats, sizeof(struct shared_stats));
    close(server_fd);

    return 0;
}