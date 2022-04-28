#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>

#include "wrapsock.h"
#include "ws_helpers.h"


void initClients(struct clientstate *client, int size) {
    // Initialize client array
    for (int i = 0; i < size; i++){
        client[i].sock = -1;	/* -1 indicates available entry */
        client[i].fd[0] = -1;
        client[i].request = NULL;
        client[i].path = NULL;
        client[i].query_string = NULL;
        client[i].output = NULL;
        client[i].optr = client[i].output;
    }
}


/* Reset the client state cs.
 * Free the dynamically allocated fields
 */
void resetClient(struct clientstate *cs){
    cs->sock = -1;
    cs->fd[0] = -1;

    if(cs->path != NULL) {
        free(cs->path);
        cs->path = NULL;
    }
    if(cs->request != NULL) {
        free(cs->request);
        cs->request = NULL;
    }
    if(cs->output != NULL) {
        free(cs->output);
        cs->output = NULL;
    }
    if(cs->query_string != NULL) {
        free(cs->query_string);
        cs->query_string = NULL;
    }
}

int reset_client_for_fd(int fd, struct clientstate *client, int size) {
    for (int i = 0; i < size; i++) {
        if (client[i].sock == fd) {
            resetClient(&client[i]);
            return 0;
        }
    }
    return -1;
}


int handle_pipe_data(struct clientstate *client) {
    int already_read = client->optr - client->output;
    int bytes_read = read(client->fd[0], client->optr, MAXPAGE - already_read);
    if (bytes_read < 0) {
        perror("read");
        return -1;
    } else if (bytes_read == 0) { // external program closed pipe
        int status = 33;
        fprintf(stderr, "External CGI program closed pipe %d\n", client->fd[0]);
        // Check that the external CGI program finished successfuly

        int rc = waitpid(client->cgi_pid, &status, WNOHANG);
        fprintf(stderr, "waitpid returned %d\n", rc);
        fprintf(stderr, "status: %d\n", status);
        if (rc < 0) {
            perror("waitpid failed");
            return -1;
        } else if (rc == 0) {
            // Program has not exited yet
            return -1;
        } else {
            // External program has finished; inspect its status
            if (status == 100<<8) {
                // Success
                fprintf(stderr, "CGI program not found\n");
                return 100;
            } else if (status == 0) {
                // Success
                fprintf(stderr, "CGI program exited successfully\n");
                return 0;
            } else {
                fprintf(stderr, "External program has finished with an error. Don't send anything to client.\n");
                return -1;
            }
        }
    } else {
        client->optr += bytes_read;
        fprintf(stderr, "Read %d bytes from pipe %d\n", bytes_read, client->fd[0]);
        return 1;
    }
}

int get_client_for_pipe_fd(int fd, struct clientstate *client, int size) {
    for (int i = 0; i < size; i++) {
        if (client[i].fd[0] == fd) {
            return i;
        }
    }
    return -1;
}

int get_client_for_sock_fd(int fd, struct clientstate *client, int size) {
    for (int i = 0; i < size; i++) {
        if (client[i].sock == fd) {
            return i;
        }
    }
    return -1;
}

int parse_http_request(struct clientstate *client) {
    int get = strncmp(client->request, "GET ", 4);
    if (get != 0) {
        fprintf(stderr, "Not a GET request\n");
        return -1;
    }
    // Test for valid path
    if (client->request[4] != '/') {
        fprintf(stderr, "Bad request2\n");
        return -1;
    }
    const int path_start = 5;
    int path_end = strcspn(client->request + path_start, "\r ?");

    if (path_end == 0) {
        fprintf(stderr, "Bad request1\n");
        return -1;
    }

    // Get path
    client->path = (char*) malloc((path_end) * sizeof(char));
    memcpy(client->path, client->request + path_start, path_end);
    client->path[path_end] = '\0';

    // Test query string
    int code = validResource(client->path);
    if (code == 0) {
        fprintf(stderr, "Wrong program to execute: %s\n", client->path);
        return -1;
    }
    fprintf(stderr, "Path: %s\n", client->path);
    
    if (client->request[path_start + path_end] == '?') {
        
        // Get query string
        int query_start = path_start + path_end + 1;
        int query_end = strcspn(client->request + query_start, " \r");
        if (query_end == 0) {
            fprintf(stderr, "Bad request3\n");
            return -1;
        }
        client->query_string = (char*) malloc((query_end) * sizeof(char));
        memcpy(client->query_string, client->request + query_start, query_end);
        client->query_string[query_end] = '\0';
        fprintf(stderr, "Query string is: %s\n", client->query_string);
    }
    return 0;
}

int do_pipe(struct clientstate *client) {
    int pipe_status = pipe(client->fd);
    if (pipe_status == -1) {
        fprintf(stderr, "pipe failed\n");
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork failed\n");
        return -1;
    }
    if (pid > 0) {
        // Parent
        close(client->fd[1]);
        client->output = (char*) malloc(MAXPAGE * sizeof(char));
        client->optr = client->output;
        client->cgi_pid = pid;
        return client->fd[0];
    }
    if (pid == 0) {
        // Child
        close(client->fd[0]);
        dup2(client->fd[1], STDOUT_FILENO);
        close(client->fd[1]);
        execl(client->path, client->path, NULL);
        fprintf(stderr, "Exec failed\n");
        // return 100;
        exit(100);
    }
    return 0;
}

/* Write the 404 Not Found error message on the file descriptor fd
 */
void printNotFound(int fd) {

    char *error_str = "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/html\r\n\r\n"
        "<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\n"
        "<html><head>\n"
        "<title>404 Not Found  </title>\n"
        "</head><body>\n"
        "<h1>Not Found (CSC209)</h1>\n"
        "<hr>\n</body>The server could not satisfy the request.</html>\n";

    int result = write(fd, error_str, strlen(error_str));
    if(result != strlen(error_str)) {
        perror("write");
    }
}

/* Write the 500 error message on the file descriptor fd
 */
void printServerError(int fd) {

    char *error_str = "HTTP/1.1 500 Internal Server Error\r\n"
        "Content-Type: text/html\r\n\r\n"
        "<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\n"
        "<html><head>\n"
        "<title>500 Internal Server Error</title>\n"
        "</head><body>\n"
        "<h1>Internal Server Error (CSC209) </h1>\n"
        "The server encountered an internal error or\n"
        "misconfiguration and was unable to complete your request.<p>\n"
        "</body></html>\n";

    int result = write(fd, error_str, strlen(error_str));
    if(result != strlen(error_str)) {
        perror("write");
    }
}

/* Write the 200 OK response on the file descriptor fd, and write the
 * content of the response from the string output. The string in output
 * is expected to be correctly formatted.
 */
void printOK(int fd, char *output, int length) {
    int nbytes = strlen("HTTP/1.1 200 OK\r\n");
    if(write(fd, "HTTP/1.1 200 OK\r\n", nbytes) != nbytes) {
        perror("write");
    }
    int n;
    while(length > MAXLINE) {
        n = write(fd, output, MAXLINE);
        length -= n;
        output += n;
    }
    n = write(fd, output, length);
    if(n != length) {
        perror("write");
    }
}


void printINVALID(int fd) {
    char *error_str = "HTTP/1.1 400 Bad Request\r\n"
        "Content-Type: text/html\r\n\r\n"
        "<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\n"
        "<html><head>\n"
        "<title>400 Bad Request</title>\n"
        "</head><body>\n"
        "<h1>Bad Request (CSC209) </h1>\n"
        "Bad Request<p>\n"
        "</body></html>\n";
    
    fprintf(stderr, "im here\n");

    int result = write(fd, error_str, strlen(error_str));
    if(result != strlen(error_str)) {
        perror("write");
    }
}