#ifndef CWIST_CORE_ORM_SOCKET_H
#define CWIST_CORE_ORM_SOCKET_H

#include <cwist/core/orm/orm.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open a SQLite database behind an in-process SQL socket.
 *
 * Creates an AF_UNIX socket pair and starts a detached worker thread that
 * owns the SQLite connection on one end.  The other end is returned to the
 * caller, typically to be passed to cwist_orm_open_socket().
 *
 * The worker executes every SQL request it receives on the returned fd
 * as-is, with no escaping or filtering.  Treat the fd as full access to the
 * database: keep it inside the process and do not pass it to untrusted code
 * or processes (for example over SCM_RIGHTS, or by fork() without exec).
 * Both ends are close-on-exec.
 *
 * @param path SQLite database path, or ":memory:".
 * @return The caller's socket fd, or -1 with errno set on failure.
 */
int cwist_db_transfer_sqlite_to_socket(const char *path);
cwist_orm_t *cwist_orm_open_socket(int sock);
void cwist_orm_close_socket(cwist_orm_t *orm);

int cwist_orm_socketpair_cloexec(int domain, int type, int protocol, int sv[2]);

#ifdef __cplusplus
}
#endif

#endif /* CWIST_CORE_ORM_SOCKET_H */
