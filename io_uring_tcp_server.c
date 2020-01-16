#include "liburing.h"
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <stdlib.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/poll.h>

struct connection_info
{
    int fd;
    int type;
};
const int POLL_LISTEN = 0, POLL_NEW_CONNECTION = 1, READ = 2, WRITE = 3;

int main(int argc, char *argv[]) {
    // some need variables
    int portno = strtol(argv[1], NULL, 10);
    struct sockaddr_in serv_addr, client_addr;
    int client_len = sizeof(client_addr);

    
    // initialize iovec
    struct iovec iov;
    char buf[1024];
    memset(buf, 0, sizeof(buf));
    iov.iov_base = buf;
    iov.iov_len = sizeof(buf);


    // setup socket
    int sock_listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    const int val = 1;
    setsockopt(sock_listen_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(portno);
    serv_addr.sin_addr.s_addr = INADDR_ANY;


    // bind socket and listen for connections
    if (bind(sock_listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        perror("Error binding socket\n");
        exit(-1);
    }
	if (listen(sock_listen_fd, 128) < 0) {
        perror("Error listening\n");
        exit(-1);
    }
    printf("listening for connections on port: %d\n", portno);


    // initialize io_uring
    struct io_uring ring;
    io_uring_queue_init(32, &ring, 0);


    // add first io_uring sqe, check when there will be data available on sock_listen_fd
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_poll_add(sqe, sock_listen_fd, POLLIN);
    struct connection_info conn_i = 
    {
        .fd = sock_listen_fd, 
        .type = POLL_LISTEN
    };
    io_uring_sqe_set_data(sqe, &conn_i);


    // tell kernel we have put a sqe on the submission ring
    io_uring_submit(&ring);
    

    // if incoming data then check if it is a new connection (listen_fd == the_fd_that_was_in_the_cqe)
    struct io_uring_cqe *cqe;
    struct io_uring_sqe *sqe_conn;
    int type;
    
    while (1)
    {
        // wait for new sqe to become available
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret != 0)
        {
            perror("Error io_uring_wait_cqe\n");
            exit(-1);
        }

        struct connection_info *user_data = (struct connection_info *)cqe->user_data;
        type = user_data->type;

        if (type == POLL_LISTEN)
        {
            printf("new connection, res: %d\n", cqe->res);
            io_uring_cqe_seen(&ring, cqe);

            // io_uring_prep_accept(sqe, sock_listen_fd, (struct sockaddr *)&client_addr, (socklen_t *)&client_len, 0);
            // while loop until all connections are emptied using accept
            int sock_conn_fd =
                accept4(sock_listen_fd, (struct sockaddr *)&client_addr, (socklen_t *)&client_len, SOCK_NONBLOCK);
            if (sock_conn_fd == -1)
            {
                perror("Accept socket error\n");
            }

            sqe = io_uring_get_sqe(&ring);
            io_uring_prep_poll_add(sqe, sock_listen_fd, POLLIN);
            struct connection_info conn_i_listen = {
                .fd = sock_listen_fd,
                .type = POLL_LISTEN};
            io_uring_sqe_set_data(sqe, &conn_i_listen);

            // add poll sqe for newly connected socket
            sqe_conn = io_uring_get_sqe(&ring);
            io_uring_prep_poll_add(sqe_conn, sock_conn_fd, POLLIN);
            struct connection_info conn_i_conn = {
                .fd = sock_conn_fd,
                .type = POLL_NEW_CONNECTION};
            io_uring_sqe_set_data(sqe_conn, &conn_i_conn);
            io_uring_submit(&ring);
        }
        else if (type == POLL_NEW_CONNECTION)
        {
            printf("new data on socket: %d\n", user_data->fd);
            io_uring_cqe_seen(&ring, cqe);

            sqe = io_uring_get_sqe(&ring);
            int fd = user_data->fd;
            io_uring_prep_readv(sqe, fd, &iov, 1, 0);
            struct connection_info conn_i = {
                .fd = fd,
                .type = READ};
            io_uring_sqe_set_data(sqe, &conn_i);
            io_uring_submit(&ring);
        }
        else if (type == READ)
        {
            // prep send to socket
            printf("amount of bytes received: %d\n", cqe->res);
            io_uring_cqe_seen(&ring, cqe);
            sqe = io_uring_get_sqe(&ring);
            int fd = user_data->fd;
            io_uring_prep_writev(sqe, fd, &iov, 1, 0);
            struct connection_info conn_i = {
                .fd = fd,
                .type = WRITE};
            io_uring_sqe_set_data(sqe, &conn_i);
            io_uring_submit(&ring);
        }
        else if (type == WRITE)
        {
            // read from socket completed
            io_uring_cqe_seen(&ring, cqe);
            shutdown(user_data->fd, 2);
            memset(buf, 0, sizeof(buf));
        }
    }
  
}