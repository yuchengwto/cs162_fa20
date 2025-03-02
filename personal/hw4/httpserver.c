#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unistd.h>

#include "libhttp.h"
#include "wq.h"

/*
 * Global configuration variables.
 * You need to use these in your implementation of handle_files_request and
 * handle_proxy_request. Their values are set up in main() using the
 * command line arguments (already implemented for you).
 */
wq_t work_queue;  // Only used by poolserver
int num_threads;  // Only used by poolserver
int server_port;  // Default value: 8000
char *server_files_directory;
char *server_proxy_hostname;
int server_proxy_port;
int server_fd;
pthread_t *tids;



/*
 * Serves the contents the file stored at `path` to the client socket `fd`.
 * It is the caller's reponsibility to ensure that the file stored at `path` exists.
 */
void serve_file(int fd, char *path) {
  struct stat buf;
  stat(path, &buf);
  char filesize[20];
  sprintf(filesize, "%ld", buf.st_size);

  http_start_response(fd, 200);
  http_send_header(fd, "Content-Type", http_get_mime_type(path));
  http_send_header(fd, "Content-Length", filesize); // Change this too
  http_end_headers(fd);

  /* TODO: PART 2 */
  FILE *fp = fopen(path, "r");
  char buffer[1024];
  size_t nread;
  size_t nwrite;
  size_t fdwrite;
  while ((nread = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
    nwrite = 0;
    while ((fdwrite = write(fd, buffer + nwrite, nread - nwrite)) > 0) {
      nwrite += fdwrite;
    }
  }
  fclose(fp);
}

void serve_directory(int fd, char *path) {
  http_start_response(fd, 200);
  http_send_header(fd, "Content-Type", http_get_mime_type(".html"));
  http_end_headers(fd);

  /* TODO: PART 3 */
  DIR *dp = opendir(path);
  struct dirent *entry;
  char filelist[10240]; // for simplicity, assume filelist buffer is bit enough.
  char filepath[1024];
  memset(filelist, 0, sizeof(filelist));
  memset(filepath, 0, sizeof(filepath));
  while ((entry = readdir(dp)))
  {
    if (entry->d_type == 8) {
      if (strcmp(entry->d_name, "index.html") == 0) {
        sprintf(filepath, "%sindex.html", path);
        serve_file(fd, filepath);
        closedir(dp);
        return;
      } else {
        sprintf(filepath, "<a href=\"http://192.168.162.162:8000%s%s\">%s</a><br>", path+2, entry->d_name, entry->d_name);
        sprintf(filelist, "%s%s", filelist, filepath);
      }
    }
  }
  closedir(dp);
  size_t listlen = strlen(filelist);
  size_t nwrite = 0;
  size_t fdwrite;
  while ((fdwrite = write(fd, filelist + nwrite, listlen - nwrite)) > 0) {
    nwrite += fdwrite;
  }
}

size_t stream_request(char *stream, struct http_request *request) {
  size_t size = sprintf(stream, 
                        "%s %s HTTP/1.0\r\n"
                        "\r\n"
                        , request->method, request->path);
  return size;
}

struct request_handler_args {
  pthread_t *pid;
  void (*func)(int);
  int client_fd;
};


void *request_handler_wrapper(void *args) {
  pthread_detach(pthread_self());
  struct request_handler_args *args_ = (struct request_handler_args *) args;
  void (*request_handler)(int) = args_->func;
  pthread_t *pid = args_->pid;
  int client_fd = args_->client_fd;
  request_handler(client_fd);

  // Free malloc memory
  free(pid);
  free(args_);
  pthread_exit(0);
}


/*
 * Reads an HTTP request from client socket (fd), and writes an HTTP response
 * containing:
 *
 *   1) If user requested an existing file, respond with the file
 *   2) If user requested a directory and index.html exists in the directory,
 *      send the index.html file.
 *   3) If user requested a directory and index.html doesn't exist, send a list
 *      of files in the directory with links to each.
 *   4) Send a 404 Not Found response.
 *
 *   Closes the client socket (fd) when finished.
 */
void handle_files_request(int fd) {

  struct http_request *request = http_request_parse(fd);

  if (request == NULL || request->path[0] != '/') {
    http_start_response(fd, 400);
    http_send_header(fd, "Content-Type", "text/html");
    http_end_headers(fd);
    close(fd);
    return;
  }

  if (strstr(request->path, "..") != NULL) {
    http_start_response(fd, 403);
    http_send_header(fd, "Content-Type", "text/html");
    http_end_headers(fd);
    close(fd);
    return;
  }

  /* Remove beginning `./` */
  char *path = malloc(2 + strlen(request->path) + 1);
  path[0] = '.';
  path[1] = '/';
  memcpy(path + 2, request->path, strlen(request->path) + 1);

  /*
   * TODO: PART 2 is to serve files. If the file given by `path` exists,
   * call serve_file() on it. Else, serve a 404 Not Found error below.
   * The `stat()` syscall will be useful here.
   *
   * TODO: PART 3 is to serve both files and directories. You will need to
   * determine when to call serve_file() or serve_directory() depending
   * on `path`. Make your edits below here in this function.
   */
  struct stat buf;
  if (stat(path, &buf) == 0) {
    if (S_ISREG(buf.st_mode)) {
      serve_file(fd, path);
    } else if (S_ISDIR(buf.st_mode))
    {
      serve_directory(fd, path);
    }
  } else {
    http_start_response(fd, 404);
    http_send_header(fd, "Content-Type", "text/html");
    http_end_headers(fd);
  }

  free(path);
  close(fd);
  return;
}

/*
 * Opens a connection to the proxy target (hostname=server_proxy_hostname and
 * port=server_proxy_port) and relays traffic to/from the stream fd and the
 * proxy target_fd. HTTP requests from the client (fd) should be sent to the
 * proxy target (target_fd), and HTTP responses from the proxy target (target_fd)
 * should be sent to the client (fd).
 *
 *   +--------+     +------------+     +--------------+
 *   | client | <-> | httpserver | <-> | proxy target |
 *   +--------+     +------------+     +--------------+
 *
 *   Closes client socket (fd) and proxy target fd (target_fd) when finished.
 */
void handle_proxy_request(int fd) {

  /*
  * The code below does a DNS lookup of server_proxy_hostname and 
  * opens a connection to it. Please do not modify.
  */
  struct sockaddr_in target_address;
  memset(&target_address, 0, sizeof(target_address));
  target_address.sin_family = AF_INET;
  target_address.sin_port = htons(server_proxy_port);

  // Use DNS to resolve the proxy target's IP address
  struct hostent *target_dns_entry = gethostbyname2(server_proxy_hostname, AF_INET);

  // Create an IPv4 TCP socket to communicate with the proxy target.
  int target_fd = socket(PF_INET, SOCK_STREAM, 0);
  if (target_fd == -1) {
    fprintf(stderr, "Failed to create a new socket: error %d: %s\n", errno, strerror(errno));
    close(fd);
    exit(errno);
  }

  if (target_dns_entry == NULL) {
    fprintf(stderr, "Cannot find host: %s\n", server_proxy_hostname);
    close(target_fd);
    close(fd);
    exit(ENXIO);
  }

  char *dns_address = target_dns_entry->h_addr_list[0];

  // Connect to the proxy target.
  memcpy(&target_address.sin_addr, dns_address, sizeof(target_address.sin_addr));
  int connection_status = connect(target_fd, (struct sockaddr*) &target_address,
      sizeof(target_address));

  if (connection_status < 0) {
    /* Dummy request parsing, just to be compliant. */
    http_request_parse(fd);

    http_start_response(fd, 502);
    http_send_header(fd, "Content-Type", "text/html");
    http_end_headers(fd);
    close(target_fd);
    close(fd);
    return;

  }

  /* TODO: PART 4 */
  struct http_request *request = http_request_parse(fd);
  size_t requestlen = strlen(request->method) + strlen(request->path) + 1024; // 1024 for some redundant 0x0 bytes, also as a null termination.
  char requestline[requestlen];
  requestlen = stream_request(requestline, request);  // formatted string line length.
  
  size_t nwrite;
  size_t requestleft = requestlen + 1;// plus one for a null termination '\0'
  while ((nwrite = write(target_fd, requestline, requestleft)) > 0) {
    requestleft -= nwrite;
  }

  // wait response from target server and redirect to client.
  char buffer[4096];
  size_t nread;
  size_t nwriteleft;
  while ((nread = read(target_fd, buffer, sizeof(buffer))) > 0)
  {
    nwriteleft = nread;
    while ((nwrite = write(fd, buffer, nwriteleft)) > 0)
    {
      nwriteleft -= nwrite;
    }
    memset(buffer, 0x0, sizeof(buffer));
  }
  write(fd, buffer, 1); // write one 0x0 at final as a null termination

  close(target_fd);
  close(fd);
}

#ifdef POOLSERVER
/* 
 * All worker threads will run this function until the server shutsdown.
 * Each thread should block until a new request has been received.
 * When the server accepts a new connection, a thread should be dispatched
 * to send a response to the client.
 */
void *handle_clients(void *void_request_handler) {
  void (*request_handler)(int) = (void (*)(int)) void_request_handler;
  /* (Valgrind) Detach so thread frees its memory on completion, since we won't
   * be joining on it. */
  pthread_detach(pthread_self());

  /* TODO: PART 7 */
  struct tcp_info info;
  int len = sizeof(info);
  while (1) {
    getsockopt(server_fd, IPPROTO_TCP, TCP_INFO, &info, (socklen_t *) &len);
    if (info.tcpi_state == TCP_CLOSE) {
      break;
    }
    // pthread_mutex_lock(&work_queue.mutex);
    // while (work_queue.size == 0) {
    //   pthread_cond_wait(&work_queue.condvar, &work_queue.mutex);
    // }
    // wq_item_t *work = work_queue.head;
    int client_fd = wq_pop(&work_queue);
    request_handler(client_fd);
    // pthread_mutex_unlock(&work_queue.mutex);
  }
  pthread_exit(NULL);
}

/* 
 * Creates `num_threads` amount of threads. Initializes the work queue.
 */
void init_thread_pool(int num_threads, void (*request_handler)(int)) {

  /* TODO: PART 7 */
  wq_init(&work_queue);
  tids = malloc(sizeof(pthread_t) * num_threads);
  for (size_t i=0; i!=num_threads; ++i) {
    int err = pthread_create(&tids[i], NULL,
                            handle_clients, (void *)request_handler);
    if (err != 0) {
      printf("pthread create: %s\n", strerror(err));
      pthread_exit(NULL);
    }
  }
}
#endif

/*
 * Opens a TCP stream socket on all interfaces with port number PORTNO. Saves
 * the fd number of the server socket in *socket_number. For each accepted
 * connection, calls request_handler with the accepted fd number.
 */
void serve_forever(int *socket_number, void (*request_handler)(int)) {

  struct sockaddr_in server_address, client_address;
  size_t client_address_length = sizeof(client_address);
  int client_socket_number;

  // Creates a socket for IPv4 and TCP.
  *socket_number = socket(PF_INET, SOCK_STREAM, 0);
  if (*socket_number == -1) {
    perror("Failed to create a new socket");
    exit(errno);
  }

  int socket_option = 1;
  if (setsockopt(*socket_number, SOL_SOCKET, SO_REUSEADDR, &socket_option,
        sizeof(socket_option)) == -1) {
    perror("Failed to set socket options");
    exit(errno);
  }

  // Setup arguments for bind()
  memset(&server_address, 0, sizeof(server_address));
  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = INADDR_ANY;
  server_address.sin_port = htons(server_port);

  /* 
   * TODO: PART 1 BEGIN
   *
   * Given the socket created above, call bind() to give it
   * an address and a port. Then, call listen() with the socket.
   * An appropriate size of the backlog is 1024, though you may
   * play around with this value during performance testing.
   */
  bind(*socket_number, (struct sockaddr *)&server_address, sizeof(server_address));
  listen(*socket_number, 1024);

  /* PART 1 END */
  printf("Listening on port %d...\n", server_port);

#ifdef POOLSERVER
  /* 
   * The thread pool is initialized *before* the server
   * begins accepting client connections.
   */
  init_thread_pool(num_threads, request_handler);
#endif

  while (1) {
    client_socket_number = accept(*socket_number,
        (struct sockaddr *) &client_address,
        (socklen_t *) &client_address_length);
    if (client_socket_number < 0) {
      perror("Error accepting socket");
      continue;
    }

    printf("Accepted connection from %s on port %d\n",
        inet_ntoa(client_address.sin_addr),
        client_address.sin_port);

#ifdef BASICSERVER
    /*
     * This is a single-process, single-threaded HTTP server.
     * When a client connection has been accepted, the main
     * process sends a response to the client. During this
     * time, the server does not listen and accept connections.
     * Only after a response has been sent to the client can
     * the server accept a new connection.
     */
    request_handler(client_socket_number);
#elif FORKSERVER
    /* 
     * TODO: PART 5 BEGIN
     *
     * When a client connection has been accepted, a new
     * process is spawned. This child process will send
     * a response to the client. Afterwards, the child
     * process should exit. During this time, the parent
     * process should continue listening and accepting
     * connections.
     */
    pid_t handler_process = fork();
    if (handler_process == 0) {
      // subprocess
      close(*socket_number);
      request_handler(client_socket_number);
      close(client_socket_number);
      exit(EXIT_SUCCESS);
    } else {
      close(client_socket_number);
    }

    /* PART 5 END */
#elif THREADSERVER
    /* 
     * TODO: PART 6 BEGIN
     *
     * When a client connection has been accepted, a new
     * thread is created. This thread will send a response
     * to the client. The main thread should continue
     * listening and accepting connections. The main
     * thread will NOT be joining with the new thread.
     */
    pthread_t *handler_thread = malloc(sizeof(pthread_t));
    struct request_handler_args *args = malloc(sizeof(struct request_handler_args));
    args->func = request_handler;
    args->client_fd = client_socket_number;
    args->pid = handler_thread;
    int err = pthread_create(handler_thread, NULL,
                            request_handler_wrapper, (void *)args);
    if (err != 0) {
      printf("pthread create: %s\n", strerror(err));
      pthread_exit(NULL);
    }

    /* PART 6 END */
#elif POOLSERVER
    /* 
     * TODO: PART 7 BEGIN
     *
     * When a client connection has been accepted, add the
     * client's socket number to the work queue. A thread
     * in the thread pool will send a response to the client.
     */
    // pthread_mutex_lock(&work_queue.mutex);
    wq_push(&work_queue, client_socket_number);
    pthread_cond_signal(&work_queue.condvar);
    // pthread_mutex_unlock(&work_queue.mutex);
    /* PART 7 END */
#endif
  }

  // Free malloc memory
  free(tids);

  shutdown(*socket_number, SHUT_RDWR);
  close(*socket_number);
}

void signal_callback_handler(int signum) {
  printf("Caught signal %d: %s\n", signum, strsignal(signum));
  printf("Closing socket %d\n", server_fd);
  if (close(server_fd) < 0) perror("Failed to close server_fd (ignoring)\n");
  exit(0);
}

char *USAGE =
  "Usage: ./httpserver --files some_directory/ [--port 8000 --num-threads 5]\n"
  "       ./httpserver --proxy inst.eecs.berkeley.edu:80 [--port 8000 --num-threads 5]\n";

void exit_with_usage() {
  fprintf(stderr, "%s", USAGE);
  exit(EXIT_SUCCESS);
}

int main(int argc, char **argv) {
  signal(SIGINT, signal_callback_handler);
  signal(SIGPIPE, SIG_IGN);

  /* Default settings */
  server_port = 8000;
  void (*request_handler)(int) = NULL;

  int i;
  for (i = 1; i < argc; i++) {
    if (strcmp("--files", argv[i]) == 0) {
      request_handler = handle_files_request;
      server_files_directory = argv[++i];
      if (!server_files_directory) {
        fprintf(stderr, "Expected argument after --files\n");
        exit_with_usage();
      }
    } else if (strcmp("--proxy", argv[i]) == 0) {
      request_handler = handle_proxy_request;

      char *proxy_target = argv[++i];
      if (!proxy_target) {
        fprintf(stderr, "Expected argument after --proxy\n");
        exit_with_usage();
      }

      char *colon_pointer = strchr(proxy_target, ':');
      if (colon_pointer != NULL) {
        *colon_pointer = '\0';
        server_proxy_hostname = proxy_target;
        server_proxy_port = atoi(colon_pointer + 1);
      } else {
        server_proxy_hostname = proxy_target;
        server_proxy_port = 80;
      }
    } else if (strcmp("--port", argv[i]) == 0) {
      char *server_port_string = argv[++i];
      if (!server_port_string) {
        fprintf(stderr, "Expected argument after --port\n");
        exit_with_usage();
      }
      server_port = atoi(server_port_string);
    } else if (strcmp("--num-threads", argv[i]) == 0) {
      char *num_threads_str = argv[++i];
      if (!num_threads_str || (num_threads = atoi(num_threads_str)) < 1) {
        fprintf(stderr, "Expected positive integer after --num-threads\n");
        exit_with_usage();
      }
    } else if (strcmp("--help", argv[i]) == 0) {
      exit_with_usage();
    } else {
      fprintf(stderr, "Unrecognized option: %s\n", argv[i]);
      exit_with_usage();
    }
  }

  if (server_files_directory == NULL && server_proxy_hostname == NULL) {
    fprintf(stderr, "Please specify either \"--files [DIRECTORY]\" or \n"
                    "                      \"--proxy [HOSTNAME:PORT]\"\n");
    exit_with_usage();
  }

#ifdef POOLSERVER
  if (num_threads < 1) {
    fprintf(stderr, "Please specify \"--num-threads [N]\"\n");
    exit_with_usage();
  }
#endif

  chdir(server_files_directory);
  serve_forever(&server_fd, request_handler);

  return EXIT_SUCCESS;
}
