/**
 * @file sockopts.h
 * @brief Listen-socket option knobs shared by the bind sites (private).
 */

#ifndef CWIST_NET_HTTP_SOCKOPTS_H
#define CWIST_NET_HTTP_SOCKOPTS_H

#include <stdbool.h>

/**
 * Whether listen sockets get SO_REUSEPORT where the OS has it. Default on;
 * CWIST_REUSEPORT=0/false disables, which restores bind() failure on a port
 * conflict (and therefore requires a single worker process). Read at every
 * bind, so worker children re-created after fork honor the same environment.
 */
bool cwist_reuseport_enabled(void);

#endif /* CWIST_NET_HTTP_SOCKOPTS_H */
