/* spike: socket declarations only; every call is a link-time ENOSYS stub */
#ifndef SPIKE_SYS_SOCKET_H
#define SPIKE_SYS_SOCKET_H
#include <stddef.h>
#include <sys/types.h>
#include <sys/uio.h>
typedef unsigned int socklen_t;
struct sockaddr { unsigned short sa_family; char sa_data[14]; };
struct msghdr { void *msg_name; socklen_t msg_namelen; struct iovec *msg_iov; int msg_iovlen; void *msg_control; socklen_t msg_controllen; int msg_flags; };
#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0x4000
#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2
ssize_t recv(int, void *, size_t, int);
ssize_t send(int, const void *, size_t, int);
ssize_t sendmsg(int, const struct msghdr *, int);
int shutdown(int, int);
#endif
