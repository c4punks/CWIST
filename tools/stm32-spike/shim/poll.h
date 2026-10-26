/* spike: poll() stub header */
#ifndef SPIKE_POLL_H
#define SPIKE_POLL_H
struct pollfd { int fd; short events; short revents; };
typedef unsigned int nfds_t;
#define POLLIN 1
#define POLLOUT 4
#define POLLERR 8
#define POLLHUP 16
#define POLLPRI 2
#define POLLNVAL 32
int poll(struct pollfd *, nfds_t, int);
#endif
