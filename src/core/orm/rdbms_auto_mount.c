/**
 * @file rdbms_auto_mount.c
 * @brief Automatic RDBMS provider detection and runtime mounting.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

#include <cwist/sys/app/app.h>
#include <cwist/core/log.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/mem/gc.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static void rdbms_no_sigpipe(int sock) {
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)sock;
#endif
}

/**
 * @brief Send a minimal MS-SQL TDS Pre-Login packet and check for response.
 *
 * @param sock Connected socket.
 * @return true if server responds with a valid TDS response packet (type 0x04).
 */
static bool probe_mssql(int sock) {
    static const unsigned char tds_prelogin[] = {
        0x12, 0x01, 0x00, 0x2f, 0x00, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x1a, 0x00, 0x06, 0x01, 0x00, 0x20,
        0x00, 0x01, 0x02, 0x00, 0x21, 0x00, 0x01, 0x03,
        0x00, 0x22, 0x00, 0x04, 0x04, 0x00, 0x26, 0x00,
        0x01, 0xff, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    if (send(sock, tds_prelogin, sizeof(tds_prelogin), MSG_NOSIGNAL) != (ssize_t)sizeof(tds_prelogin)) {
        return false;
    }
    unsigned char resp_hdr[8];
    ssize_t n = recv(sock, resp_hdr, sizeof(resp_hdr), 0);
    return (n >= 4 && (resp_hdr[0] == 0x04 || resp_hdr[0] == 0x12));
}

/**
 * @brief Send a minimal Oracle TNS Connect packet and check for response.
 *
 * @param sock Connected socket.
 * @return true if server responds with a valid TNS Accept/Refuse/Resend packet.
 */
static bool probe_oracle(int sock) {
    static const unsigned char tns_connect[] = {
        0x00, 0x3a, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x39, 0x01, 0x2c, 0x00, 0x00, 0x08, 0x00,
        0x7f, 0xff, 0x7f, 0x08, 0x00, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00
    };
    if (send(sock, tns_connect, sizeof(tns_connect), MSG_NOSIGNAL) != (ssize_t)sizeof(tns_connect)) {
        return false;
    }
    unsigned char resp[8];
    ssize_t n = recv(sock, resp, sizeof(resp), 0);
    return (n >= 5 && (resp[4] == 0x02 || resp[4] == 0x04 || resp[4] == 0x05 || resp[4] == 0x0b));
}

/**
 * @brief Send a minimal PostgreSQL StartupMessage and check for 'R' response.
 *
 * @param sock Connected socket.
 * @return true if server responds with an AuthenticationRequest ('R').
 */
static bool probe_postgresql(int sock) {
    char startup[64];
    memset(startup, 0, sizeof(startup));

    /* "user\0postgres\0\0" payload after version */
    const char *user_key = "user";
    const char *user_val = "postgres";
    size_t payload = strlen(user_key) + 1 + strlen(user_val) + 1 + 1; /* +1 terminator */
    uint32_t len = htonl((uint32_t)(4 + 4 + payload));
    uint32_t version = htonl(196608); /* 3.0 */

    memcpy(startup, &len, 4);
    memcpy(startup + 4, &version, 4);
    strcpy(startup + 8, user_key);
    strcpy(startup + 8 + strlen(user_key) + 1, user_val);
    /* trailing null already present from memset */

    size_t total = 8 + payload;
    if (send(sock, startup, total, MSG_NOSIGNAL) != (ssize_t)total) return false;

    char resp = 0;
    ssize_t n = recv(sock, &resp, 1, 0);
    return (n == 1 && resp == 'R');
}

/**
 * @brief Inspect a MySQL/MariaDB handshake initiation packet.
 *
 * @param buf  Received bytes.
 * @param len  Number of valid bytes in @p buf.
 * @return Detected provider (MYSQL or MARIADB) or UNKNOWN.
 */
static cwist_rdbms_provider_t classify_mysql(const char *buf, size_t len) {
    if (len < 5) return CWIST_RDBMS_NONE;

    unsigned char protocol = (unsigned char)buf[4];
    if (protocol != 0x0a) return CWIST_RDBMS_NONE;

    /* Look for "MariaDB" in the server version string that follows */
    for (size_t i = 5; i + 6 < len; ++i) {
        if (memcmp(&buf[i], "MariaDB", 7) == 0) return CWIST_RDBMS_MARIADB;
    }
    return CWIST_RDBMS_MYSQL;
}

/**
 * @brief Probe a TCP port on 127.0.0.1 to detect the RDBMS provider.
 *
 * Connects to the given port with a 2-second send/recv timeout, inspects
 * any unsolicited handshake bytes as a MySQL/MariaDB handshake, and if that
 * does not match, sends a PostgreSQL StartupMessage and waits for an
 * AuthenticationRequest response.
 *
 * @param port TCP port on localhost to probe.
 * @return Detected provider (@c CWIST_RDBMS_MYSQL, @c CWIST_RDBMS_MARIADB,
 *         or @c CWIST_RDBMS_POSTGRES) or @c CWIST_RDBMS_NONE if detection
 *         fails. The socket is always closed before returning.
 */
cwist_rdbms_provider_t cwist_rdbms_probe_port(int port) {
    if (port < 1 || port > 65535) return CWIST_RDBMS_NONE;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        CWIST_LOG_ERROR("RDBMS probe: socket creation failed: %s", strerror(errno));
        return CWIST_RDBMS_NONE;
    }

    rdbms_no_sigpipe(sock);
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        CWIST_LOG_ERROR("RDBMS probe: connect to 127.0.0.1:%d failed: %s", port, strerror(errno));
        close(sock);
        return CWIST_RDBMS_NONE;
    }

    char buf[512];
    ssize_t n = recv(sock, buf, sizeof(buf), 0);
    if (n > 0) {
        cwist_rdbms_provider_t p = classify_mysql(buf, (size_t)n);
        if (p != CWIST_RDBMS_NONE) {
            close(sock);
            return p;
        }
        /* Data was received but did not look like MySQL; could be noise.
         * Close and re-probe as PostgreSQL below. */
        close(sock);
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return CWIST_RDBMS_NONE;
        rdbms_no_sigpipe(sock);
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(sock);
            return CWIST_RDBMS_NONE;
        }
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        CWIST_LOG_ERROR("RDBMS probe: recv error on port %d: %s", port, strerror(errno));
        close(sock);
        return CWIST_RDBMS_NONE;
    }

    /* Try provider-specific probes */
    if (port == 1433) {
        if (probe_mssql(sock)) {
            close(sock);
            return CWIST_RDBMS_MSSQL;
        }
    } else if (port == 1521) {
        if (probe_oracle(sock)) {
            close(sock);
            return CWIST_RDBMS_ORACLE;
        }
    }

    if (probe_postgresql(sock)) {
        close(sock);
        return CWIST_RDBMS_POSTGRES;
    }

    /* Check MS-SQL or Oracle if port was generic */
    if (port != 1433 && port != 1521) {
        close(sock);
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock >= 0) {
            rdbms_no_sigpipe(sock);
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                if (probe_mssql(sock)) {
                    close(sock);
                    return CWIST_RDBMS_MSSQL;
                }
            }
            close(sock);
        }
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock >= 0) {
            rdbms_no_sigpipe(sock);
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                if (probe_oracle(sock)) {
                    close(sock);
                    return CWIST_RDBMS_ORACLE;
                }
            }
            close(sock);
        }
        return CWIST_RDBMS_NONE;
    }

    close(sock);
    return CWIST_RDBMS_NONE;
}

const char *cwist_rdbms_provider_name(cwist_rdbms_provider_t provider) {
    switch (provider) {
        case CWIST_RDBMS_POSTGRES: return "PostgreSQL";
        case CWIST_RDBMS_MYSQL: return "MySQL";
        case CWIST_RDBMS_MARIADB: return "MariaDB";
        case CWIST_RDBMS_ORACLE: return "OracleDB";
        case CWIST_RDBMS_MSSQL: return "MS-SQL";
        case CWIST_RDBMS_NONE:
        default: return "None";
    }
}

/**
 * @brief Allocate and attach an RDBMS runtime record to an app.
 *
 * Idempotent: if @p app already has an RDBMS runtime mounted, this returns
 * true without allocating. On success the new runtime is stored in
 * @c app->rdbms with @c ready set to true.
 *
 * @param app     App to attach the runtime to; must not be NULL.
 * @param provider Detected provider stored in the runtime.
 * @param port    Port the provider was detected on.
 * @return true if a runtime is mounted, false on NULL app or allocation failure.
 */
bool cwist_rdbms_mount_runtime(cwist_app *app, cwist_rdbms_provider_t provider, int port) {
    if (!app || provider <= CWIST_RDBMS_NONE || provider > CWIST_RDBMS_MSSQL ||
        port < 1 || port > 65535)
        return false;

    if (app->rdbms) {
        CWIST_LOG_INFO("RDBMS runtime already mounted");
        return true;
    }

    struct cwist_rdbms_runtime *rt =
        (struct cwist_rdbms_runtime *)cwist_alloc_array(1, sizeof(*rt));
    if (!rt) {
        CWIST_LOG_ERROR("RDBMS mount: out of memory");
        return false;
    }
    if (cwist_full_gc_enabled()) cwist_gc_scope_disown(rt);

    rt->provider = provider;
    rt->port = port;
    rt->ready = true;

    app->rdbms = rt;
    CWIST_LOG_INFO("RDBMS runtime mounted: provider=%d port=%d", (int)provider, port);
    return true;
}

/**
 * @brief Auto-detect an RDBMS on a local port and mount its runtime on an app.
 *
 * Probes @p port via cwist_rdbms_probe_port(); on success the detected
 * provider's runtime is mounted with cwist_rdbms_mount_runtime().
 *
 * @param app  App to attach the runtime to; must not be NULL.
 * @param port TCP port on localhost to probe.
 * @return true if detection and mounting succeed, false otherwise.
 */
bool cwist_app_auto_rdbms(cwist_app *app, int port) {
    if (!app) {
        CWIST_LOG_ERROR("RDBMS auto-mount: app is NULL");
        return false;
    }

    cwist_rdbms_provider_t provider = cwist_rdbms_probe_port(port);
    if (provider == CWIST_RDBMS_NONE) {
        CWIST_LOG_ERROR("RDBMS auto-detection failed on port %d", port);
        return false;
    }

    return cwist_rdbms_mount_runtime(app, provider, port);
}
