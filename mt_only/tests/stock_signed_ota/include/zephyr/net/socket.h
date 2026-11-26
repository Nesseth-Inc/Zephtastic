#pragma once
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#define NET_IPV4_ADDR_LEN INET_ADDRSTRLEN
#define ZSOCK_MSG_DONTWAIT MSG_DONTWAIT
#define ZSOCK_POLLIN POLLIN
#define zsock_close close
#define zsock_send send
#define zsock_recv recv
#define zsock_inet_pton inet_pton
#define zsock_socket socket
#define zsock_listen listen
#define zsock_bind bind
#define zsock_sendto sendto
#define zsock_pollfd pollfd
#define zsock_poll ::poll
#define zsock_accept accept
