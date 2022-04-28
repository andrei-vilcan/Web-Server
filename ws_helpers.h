#define MAXLINE 1024
#define MAXPAGE 1048576  /* 1MB max page size */

/* Assumptions you can make about the client state:
 *   request: An HTTP request will be no bigger than MAXLINE bytes
 *   resource: The resources string will be no bigger than MAXLINE bytes
 *   query_string: This string is part of the resource so can be dynamically
 *           allocated with the correct size.
 *   output: The output from the CGI program will be no bigger than
 *           MAXPAGE bytes
 */

struct clientstate {
    int sock; /* Socket to write to */
    int fd[2]; /* The pipe descriptors for the child to write to parent */
    char *request; /* pointer to the beginning of a request message */
    char *path; /* program to run - not including the query string */
    char *query_string;
    char *output; /* pointer to the beginning of the response data */
    char *optr; /* pointer to the current end of the response data */
    int cgi_pid; /* pid of the external CGI executable that is launched */
};

void printNotFound(int sock);
void printServerError(int sock);
void printOK(int sock, char *output, int length);
void printINVALID(int fd);
int reset_client_for_fd(int fd, struct clientstate *client, int size);
int handle_pipe_data(struct clientstate *client);
int get_client_for_pipe_fd(int fd, struct clientstate *client, int size);
int get_client_for_sock_fd(int fd, struct clientstate *client, int size);
int parse_http_request(struct clientstate *client);
int do_pipe(struct clientstate *client);

char *getPath(char *str);
char *getQuery(char *str);
void initClients(struct clientstate *client, int size);
void resetClient(struct clientstate *cs);

int validResource(char *str);
char *getPath(char *str);
char *getQuery(char *str);
int processRequest(struct clientstate *cs);
