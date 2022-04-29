/*
 * proxy.c - CS:APP Web proxy
 *
 * TEAM MEMBERS:
 *     Cody Johnson, johnscl1@sewanee.edu
 *     Lane Kilbride, kilbrla0@sewanee.edu
 * 
 * IMPORTANT: A proxy server that intercepts client web requests, changes them,
 * and creates a log entry of the time of the request, IP address of the client,
 * the web domain the request was sent to, and the size of the server's response. 
 */ 

#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

extern int errno;


/* Struct definitions */
struct format_args {
    struct sockaddr_in sock;
    int fd;
};


/* Function prototypes */
void *thread(void *vargp);
void build_hdr(char *http_header, char *hostname, char *path, int port, rio_t *client_rio);
int parse_uri(char *uri, char *hostname, char *pathname, int *port);
int conn_end_server(char *hostname, int port);
void format_log_entry(char *logstring, struct sockaddr_in sockaddr, 
		      char *uri, int size);
void print_log(char* log);



/* 
 * main - The main routine for the proxy program
 *
 * Uses the command line argument of a specified port number to connect the client
 * to the server and push their request through the threaded function. 
 */
int main(int argc, char **argv)
{
    int listenfd;
    char hostname[MAXLINE], port[MAXLINE];
    pthread_t tid;
    socklen_t clientlen;

    struct format_args *formargs; 
    formargs = malloc(sizeof(struct sockaddr_in) + sizeof(int));

    /* Ignores SIGPIPE signal */
    Signal(SIGPIPE, SIG_IGN);

    /* Check arguments */
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port number>\n", argv[0]);
	    exit(0);
    }

    listenfd = Open_listenfd(argv[1]);
    while (1) {
	clientlen = sizeof(formargs->sock);
	formargs->fd = Accept(listenfd, (SA *)&(formargs->sock), &clientlen);

        Getnameinfo((SA *) &(formargs->sock), clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);

        /* concurrent request */
        Pthread_create(&tid, NULL, thread, (void *)formargs);
    }
    
    return 0;
}


/* 
 * thread - handles multithreading for concurrent requests.
 *
 * Given a struct containing the connection file descriptor (fd) and the 
 * socket the user connected to for the request (clientaddr), handle multithreading
 * and function calls for each threaded connection request.
 */
void *thread(void *vargp)
{
    Pthread_detach(pthread_self());
    
    struct format_args *formargs = (struct format_args *)vargp;
    int fd = formargs->fd;
    struct sockaddr_in clientaddr = formargs->sock;

    int end_serverfd = 0;/*the end server file descriptor*/

    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char endserver_http_hdr[MAXLINE];
    char logdata[MAXLINE];

    /* store the request line arguments */
    char hostname[MAXLINE], path[MAXLINE];
    int port;

    rio_t rio, server_rio;/* Create client and server rio */

    rio_readinitb(&rio, fd); /* Can't error check, void function */
    
    if((rio_readlineb(&rio, buf, MAXLINE)) == -1) {
        close(fd);
        close(end_serverfd);
        fprintf(stderr, "\n\nReading through pipe failed: %s.\n\n", strerror(errno));
        return (void *)0;
    }
    sscanf(buf, "%s %s %s", method, uri, version); /* read the client request line */

    if(strcasecmp(method, "GET") != 0){
        printf("\nProxy does not implement the %s method.\n", method);
        return (void *)0;
    }

    /* parse the URI to get hostname, file path, and port */
    if((parse_uri(uri, hostname, path, &port)) == -1) {
        printf("\nAddress was not of type http://\n");
        return (void *)0;
    }

    /* build the http header which will send to the end server */
    build_hdr(endserver_http_hdr, hostname, path, port, &rio);

    /* connect to the end server */
    if((end_serverfd = conn_end_server(hostname, port)) < 0) {
        close(fd);
        close(end_serverfd);
        fprintf(stderr, "Connection to server failed: %s.\n", strerror(errno));
        return (void *)0;
    }

    rio_readinitb(&server_rio, end_serverfd);/* Can't error check, void function */

    /* write the http header to endserver */
    if((rio_writen(end_serverfd, endserver_http_hdr, strlen(endserver_http_hdr))) == -1) {
        close(fd);
        close(end_serverfd);
        fprintf(stderr, "\n\nWriting through pipe failed: %s.\n\n", strerror(errno));
        return (void *)0;
    }

    /* receive message from end server and send to the client */
    int sizebuf = 0;
    size_t n;
    while((n = rio_readlineb(&server_rio, buf, MAXLINE)) != 0) {
        sizebuf += n;
        
        if((rio_writen(fd, buf, n)) == -1) {
            close(fd);
            close(end_serverfd);
            fprintf(stderr, "\n\nWriting through pipe failed: %s.\n\n", strerror(errno));
            return (void *)0;
        }
    }

    /* Formats the log entry of the request and sends it to the print_log() function */
    format_log_entry(logdata, clientaddr, uri, sizebuf);

    printf("\n\nLogged: %s\n\n", logdata);
    
    Close(end_serverfd); 
    Close(fd);

    return (void *)0;
}


/*
 * build_hdr - Build the HTTP header to be sent to the end server from client
 *             requests.
 * 
 * Given a string to hold the HTTP header, the hostname, the path of the website,
 * the port number of the client, and the client's rio, buid the reformatted HTTP header
 * into the desired HTTP/1.0 request.
 */
void build_hdr(char *http_header, char *hostname, char *path, int port, rio_t *client_rio)
{
    char buf[MAXLINE], request_hdr[MAXLINE], other_hdr[MAXLINE], host_hdr[MAXLINE];
    
    /* Predefined request parts that we check and build properly */
    const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
    const char *conn_hdr = "Connection: close\r\n";
    const char *proxy_hdr = "Proxy-Connection: close\r\n";
    const char *endof_hdr = "\r\n";
    
    const char *connection_key = "Connection";
    const char *user_agent_key= "User-Agent";
    const char *proxy_connection_key = "Proxy-Connection";
    const char *host_key = "Host";

    /* copy the path to the proper request line */
    sprintf(request_hdr, "GET %s HTTP/1.0\r\n", path);

    /* get other request header for client rio and change it */
    while(rio_readlineb(client_rio, buf, MAXLINE) > 0)
    {
        /* If we have an EOF, break */
        if(strcmp(buf, endof_hdr) == 0) break; 

        /* Check if buf contains Host: */
        if(!strncasecmp(buf, host_key, strlen(host_key))) 
        {
            //Copy buf to host_hdr
            strcpy(host_hdr, buf);
            continue;
        }

        /* 
         * Checking for extra request headers found after 
         * Connection:, Proxy-Connectiion:, and User-Agent: 
        */
        if(!strncasecmp(buf, connection_key, strlen(connection_key))
            && !strncasecmp(buf, proxy_connection_key, strlen(proxy_connection_key))
            && !strncasecmp(buf, user_agent_key, strlen(user_agent_key)))
        {
            /* Copy those extra requests in buf to other_hdr */
            strcat(other_hdr, buf);
        }
    }

    /* Check to see if host_hdr is empty */
    if(strlen(host_hdr) == 0)
    {
        /* Copy the host info into host_hdr */
        sprintf(host_hdr, "Host: %s\r\n", hostname);
    }

    /* Create the HTTP header from the changed, proper request lines */
    sprintf(http_header,"%s%s%s%s%s%s%s",
            request_hdr,
            host_hdr,
            conn_hdr,
            proxy_hdr,
            user_agent_hdr,
            other_hdr,
            endof_hdr);

    return;
}


/*
 * parse_uri - URI parser
 * 
 * Given a URI from an HTTP proxy GET request (i.e., a URL), extract
 * the host name, path name, and port.  The memory for hostname and
 * pathname must already be allocated and should be at least MAXLINE
 * bytes. Return -1 if there are any problems.
 */
int parse_uri(char *uri, char *hostname, char *pathname, int *port)
{
    char *hostbegin;
    char *hostend;
    char *pathbegin;
    int len;

    if (strncasecmp(uri, "http://", 7) != 0) {
	    hostname[0] = '\0';
	    return -1;
   }
       
    /* Extract the host name */
    hostbegin = uri + 7;
    hostend = strpbrk(hostbegin, " :/\r\n\0");
    len = hostend - hostbegin;
    strncpy(hostname, hostbegin, len);
    hostname[len] = '\0';
    
    /* Extract the port number */
    *port = 80; /* default port */
    if (*hostend == ':')   
	    *port = atoi(hostend + 1);
    
    /* Extract the path */
    pathbegin = strchr(hostbegin, '/');
    if (pathbegin == NULL) {
	    pathname[0] = '\0';
    }
    else {
	    pathbegin++;	
	    strcpy(pathname, hostend);
   }

    return 0;
}


/* 
 * conn_end_server - Establish a connection to the end server 
 *
 * Given the hostname of the client and the port number they are connected to, 
 * connect that client to the end server they are attempting to request information
 * from.  Returns -1 or -2 on error (see csapp.c open_clientfd() function definition).
 */
int conn_end_server(char *hostname, int port) 
{

    char portStr[100];

    /* Copy the port number to a string */
    sprintf(portStr, "%d", port);

    /* Return the int representing if the connection was successfully made */
    return open_clientfd(hostname, portStr);

}


/*
 * format_log_entry - Create a formatted log entry in logstring. 
 * 
 * The inputs are the socket address of the requesting client
 * (sockaddr), the URI from the request (uri), and the size in bytes
 * of the response from the server (size).
 */
void format_log_entry(char *logstring, struct sockaddr_in sockaddr, 
		      char *uri, int size)
{
    time_t now;
    char time_str[MAXLINE];
    unsigned long host;
    unsigned char a, b, c, d;

    /* Get a formatted time string */
    now = time(NULL);
    strftime(time_str, MAXLINE, "%a %d %b %Y %H:%M:%S %Z", localtime(&now));

    /* 
     * Next, convert the IP address in network byte order to dotted decimal
     * form. Note that we could have used inet_ntoa, but chose not to
     * because inet_ntoa is a Class 3 thread unsafe function that
     * returns a pointer to a static variable (Ch 13, CS:APP).
     */

    /* Calculate the parts of the URI */
    char *pos = strstr(uri,"//");
    pos = pos != NULL ? pos+2: uri;
    char *pos2 = strstr(pos,"/");

    /* Find the size of the needed part of the URI and parse it */
    int buffersize = (strlen(uri) - strlen(pos2)) + 2; 
    char uribase[buffersize];
    strncpy(uribase, uri, buffersize + 1);
    uribase[sizeof(uribase) - 1] = '\0';

    /* Retrieve client IP address and change from network to host byte order */
    host = ntohl(sockaddr.sin_addr.s_addr);
    a = host >> 24;
    b = (host >> 16) & 0xff;
    c = (host >> 8) & 0xff;
    d = host & 0xff;

    /* Store the formatted log entry string in logstring */
    sprintf(logstring, "[%s] %d.%d.%d.%d %s %d\n", time_str, a, b, c, d, uribase, size);
    
    /* Log the string in the proxy.log file */
    print_log(logstring);

    return;
}


/* 
 * print_log - Logs the log string from format_log_entry to the proxy.log file.
 *
 * Given the log string, copies the string to the proxy.log log file, accounting for
 * possible thread problems accessing the file with a semaphore (mutex) and 
 * corresponding P() and V() wait and pass functions.
 */
void print_log(char* log)
{
    sem_t mutex;
    if(sem_init(&mutex, 0, 1) < 0) {
        fprintf(stderr, "Could not initialize semaphore in print_log(): %s\n", strerror(errno));
        return;
    }
	
    FILE *proxy_log;
    char *file = "proxy.log";

    P(&mutex); // wait here, only one file at a time
    if(access(file, F_OK) == 0) {
        // file exists, append logs to it
        proxy_log = fopen(file, "a");
    } else {
        // file doesn't exist, create it and add logs to it
        proxy_log = fopen(file, "w+");
    }
    
    fwrite(log, 1, strlen(log), proxy_log);
    fclose(proxy_log);
    V(&mutex); // pass, this thread is done with proxy.log
}



