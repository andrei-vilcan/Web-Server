#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h> /* Internet domain header */

#include "wrapsock.h"
#include "ws_helpers.h"

#include "sys/select.h"

#define MAXCLIENTS 10

int handleClient(struct clientstate *cs, char *line);

// You may want to use this function for initial testing
// void write_page(int fd);

int main(int argc, char **argv)
{

    if (argc != 2)
    {
        // fprintf(stderr, "Usage: wserver <port>\n");
        exit(1);
    }
    unsigned short port = (unsigned short)atoi(argv[1]);
    int listenfd;
    struct clientstate client[MAXCLIENTS];

    // Set up the socket to which the clients will connect
    listenfd = setupServerSocket(port);

    initClients(client, MAXCLIENTS);

    fd_set read_fds;
    fd_set shadow_fds;

    FD_ZERO(&shadow_fds);

    int max_fd = listenfd;
    // fprintf(stderr, "Server will listen on socket %d\n", listenfd);

    int n_connections = 0;
    int exit_flag = 0;
    while (!exit_flag)
    {

        // Set-up all descriptors "select" should inspect
        // This includes both socket descriptors and pipe descriptors
        FD_ZERO(&read_fds);
        FD_SET(listenfd, &read_fds);
        for (int k = 0; k < max_fd + 1; k++)
        {
            if (FD_ISSET(k, &shadow_fds))
            {
                FD_SET(k, &read_fds);
            }
        }
        FD_ZERO(&shadow_fds);

        struct timeval timeLimit = {300, 0};
        int num_active = Select(max_fd + 1, &read_fds, NULL, NULL, &timeLimit);
        if (num_active == 0)
        {
            // fprintf(stderr, "Timeout encountered. Will exit the program now\n");
            break; // Will exit the program
        }

        // fprintf(stderr, "select encountered  %d active file descriptor(s)\n", num_active);
        // : the following "for" loop can stop after processing numActive sockets
        for (int i = 0; i <= max_fd; i++)
        {
            int fd = FD_ISSET(i, &read_fds) ? i : -1;
            if (fd == -1)
            {
                continue;
            }
            // fprintf(stderr, "file descriptor %d is ready\n", fd);
            // Check which type of descriptor is ready. We have 3 possibilities:
            // (1) Listen socket for new connections
            // (2) Sockets for receiving http requests
            // (3) Pipes for receiving data from the CGI program
            if (fd == listenfd)
            {
                int newfd = Accept(listenfd, NULL, NULL);
                n_connections++;
                if (n_connections > MAXCLIENTS)
                {
                    // fprintf(stderr, "Reached maximum number of clients allowed. Exiting now.\n");
                    Close(newfd);
                    exit_flag = 1;
                    break;
                }
                // fprintf(stderr, "accepted new connection on socket %d\n", newfd);
                int j = 0;
                for (; j < MAXCLIENTS; j++)
                {
                    if (client[j].sock == -1)
                    { // available entry; re-use
                        client[j].sock = newfd;
                        // fprintf(stderr, "client %d connected to socket %d\n", j, newfd);
                        // Prepare for the next round of using "select"
                        FD_SET(newfd, &shadow_fds);
                        if (newfd > max_fd)
                            max_fd = newfd;
                        break;
                    }
                }
                if (j == MAXCLIENTS)
                {
                    // fprintf(stderr, "too many clients\n");
                    Close(newfd);
                }
            }
            else
            {
                // There is data to be read on file descriptor "fd"
                // Determine the right client for this descriptor and whether it's a socket or a pipe
                int client_id = get_client_for_sock_fd(fd, client, MAXCLIENTS);
                if (client_id == -1)
                {
                    // fprintf(stderr, "we are dealing with pipe fd %d\n", fd);
                    client_id = get_client_for_pipe_fd(fd, client, MAXCLIENTS);
                    if (client_id == -1)
                    {
                        // This case should only happen in case of a bug
                        // fprintf(stderr, "error getting client for pipe fd %d\n", fd);
                        exit(1);
                    }
                    struct clientstate *client_ptr = &client[client_id];
                    int ret_code = handle_pipe_data(client_ptr);
                    if (ret_code == -1)
                    {
                        // Server Error
                        // fprintf(stderr, "error handling pipe data\n");
                        printServerError(client_ptr->sock);
                        Close(fd);
                        Close(client_ptr->sock);
                        resetClient(client_ptr);
                    }
                    else if (ret_code == 1)
                    {
                        // There is more data to be read from the CGI program
                        FD_SET(fd, &shadow_fds);
                    }
                    else if (ret_code == 0)
                    {
                        // All data from the CGI program was received
                        printOK(client_ptr->sock, client_ptr->output, client_ptr->optr - client_ptr->output);
                        Close(fd);
                        Close(client_ptr->sock);
                        resetClient(client_ptr);
                        // fprintf(stderr, "CGI program executed successfully. All data read and sent back to the http client\n");
                    }
                    else if (ret_code == 100)
                    {
                        // The CGI program has not been found
                        printNotFound(client_ptr->sock);
                        Close(fd);
                        Close(client_ptr->sock);
                        resetClient(client_ptr);
                        // fprintf(stderr, "404 Not Found\n");
                    }
                }
                else
                {
                    // We have incoming data on socket fd for client_id
                    char line[MAXLINE + 1]; // Add one extra byte for null terminator
                    struct clientstate *client_ptr = &client[client_id];
                    int n = read(fd, line, MAXLINE);
                    if (n < 0)
                    {
                        perror("read error");
                        exit(-1);
                    }
                    else if (n == 0)
                    {
                        // fprintf(stderr, "client %d disconnected from socket %d\n", client_id, fd);
                        Close(fd); // Clean up if data in cs
                        resetClient(client_ptr);
                    }
                    else
                    {

                        // fprintf(stderr, "client %d read %d bytes from socket %d\n", client_id, n, fd);
                        line[n] = '\0'; // Add NULL terminator so that we can print it as string

                        // Put all the code above inside handleClient
                        int handle_code = handleClient(client_ptr, line);

                        if (handle_code == -1)
                        {
                            // fprintf(stderr, "error parsing request from client %d\n", client_id);
                            printINVALID(client_ptr->sock);
                            Close(fd);
                            resetClient(client_ptr);
                            continue;
                        }
                        else if (handle_code == 0)
                        {
                            FD_SET(fd, &shadow_fds);
                            continue;
                        }
                        else if (handle_code != 1)
                        {
                            // fprintf(stderr, "Bad Error Code");
                            exit(1);
                        }

                        // Open a pipe, fork/exec and allocate buffer for incoming data
                        int pipe_fd = do_pipe(client_ptr);

                        if (pipe_fd == -1)
                        {
                            // fprintf(stderr, "error creating pipe or forking\n");
                            printServerError(client_ptr->sock);
                            Close(fd);
                            resetClient(client_ptr);
                            continue;
                        }
                        // fprintf(stderr, "fork succeeded\n");

                        if (pipe_fd > max_fd)
                        {
                            max_fd = pipe_fd;
                        }
                        FD_SET(pipe_fd, &shadow_fds);
                    }
                } // End of handling incoming data on socket
            }
        } // end 'for' loop iterating over active file descriptors
    }     // end 'while' loop
    return 0;
}

/* Update the client state cs with the request input in line.
 * Intializes cs->request if this is the first read call from the socket.
 * Note that line must be null-terminated string.
 *
 * Return 0 if the get request message is not complete and we need to wait for
 *     more data
 * Return -1 if there is an error and the socket should be closed
 *     - Request is not a GET request
 *     - The first line of the GET request is poorly formatted (getPath, getQuery)
 *
 * Return 1 if the get request message is complete and ready for processing
 *     cs->request will hold the complete request
 *     cs->path will hold the executable path for the CGI program
 *     cs->query will hold the query string
 *     cs->output will be allocated to hold the output of the CGI program
 *     cs->optr will point to the beginning of cs->output
 */
int handleClient(struct clientstate *cs, char *line)
{

    int n = strlen(line);
    // fprintf(stderr, "line: %s\n", line);
    if (cs->request == NULL)
    { // First read operation for this socket
        cs->request = (char *)malloc((n + 1) * sizeof(char));
        memcpy(cs->request, line, n + 1); // Copy received data, including the NULL terminator
    }
    else
    {
        // We need to append the data we just received to the older data
        int previous_size = strlen(cs->request);
        int new_size = previous_size + n + 1;
        char *new_mem = (char *)malloc(new_size * sizeof(char));
        memcpy(new_mem, cs->request, previous_size);  // Copy old data
        memcpy(new_mem + previous_size, line, n + 1); // Append the new data, including the NULL terminator
        free(cs->request);                            // Free the old buffer which is no longer needed
        cs->request = new_mem;                        // Attach the new buffer to this client
    }
    // Check whether we have a full HTTP request
    char *end_ptr = strstr(cs->request, "\r\n\r\n");
    if (end_ptr == NULL)
    {
        // fprintf(stderr, "Request not received fully. Will arm this socket for the next 'select' operation\n");
        return 0;
    }
    // fprintf(stderr, "server read entire HTTP request for client\n");
    // Parse the HTTP request and make sure it meets all acceptance criteria
    int parsable = parse_http_request(cs);
    if (parsable == -1)
    {
        return -1;
    }

    // fprintf(stderr, "handleClient called with socket: %d\n", cs->sock);

    // If the resource is favicon.ico we will ignore the request
    if (strcmp("favicon.ico", cs->path) == 0)
    {
        // A suggestion for debugging output
        // fprintf(stderr, "Client: sock = %d\n", cs->sock);
        // fprintf(stderr, "        path = %s (ignoring)\n", cs->path);
        printNotFound(cs->sock);
        return -1;
    }

    // A suggestion for printing some information about each client.
    // You are welcome to modify or remove these print statements
    // fprintf(stderr, "Client: sock = %d\n", cs->sock);
    // fprintf(stderr, "        path = %s\n", cs->path);
    // fprintf(stderr, "        query_string = %s\n", cs->query_string);

    return 1;
}
