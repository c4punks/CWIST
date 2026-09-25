#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <cjson/cJSON.h>
#include <cwist/core/orm/orm.h>
#include <cwist/core/orm/orm_socket.h>

/* cwist_db_transfer_sqlite_to_socket() must hand back a usable SQL socket:
 * the ORM helpers round-trip rows through the SQLite worker behind it. */
static void test_transfer_sqlite_to_socket(void) {
    errno = 0;
    assert(cwist_db_transfer_sqlite_to_socket(NULL) == -1);
    assert(errno == EINVAL);

    int sock = cwist_db_transfer_sqlite_to_socket(":memory:");
    assert(sock >= 0);
    int flags = fcntl(sock, F_GETFD);
    assert(flags != -1);
    assert((flags & FD_CLOEXEC) != 0);

    cwist_orm_t *orm = cwist_orm_open_socket(sock);
    assert(orm != NULL);
    cwist_orm_immediate_commit(true);

    assert(cwist_orm_exec(orm, "CREATE TABLE posts(id INTEGER PRIMARY KEY, title TEXT);")
               .error.err_i16 == 0);

    cJSON *row = cJSON_Parse("{\"title\":\"first\"}");
    assert(row != NULL);
    assert(cwist_orm_insert(orm, "posts", row).error.err_i16 == 0);
    cJSON_Delete(row);

    cJSON *upd = cJSON_Parse("{\"title\":\"renamed\"}");
    assert(upd != NULL);
    assert(cwist_orm_update(orm, "posts", upd, "id = 1").error.err_i16 == 0);
    cJSON_Delete(upd);

    cJSON *rows = NULL;
    assert(cwist_orm_select(orm, "posts", "id, title", NULL, &rows).error.err_i16 == 0);
    assert(rows != NULL);
    assert(cJSON_GetArraySize(rows) == 1);
    cJSON *title = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(rows, 0), "title");
    assert(cJSON_IsString(title));
    assert(strcmp(title->valuestring, "renamed") == 0);
    cJSON_Delete(rows);

    assert(cwist_orm_delete(orm, "posts", "id = 1").error.err_i16 == 0);
    rows = NULL;
    assert(cwist_orm_select(orm, "posts", "id", NULL, &rows).error.err_i16 == 0);
    assert(rows != NULL);
    assert(cJSON_GetArraySize(rows) == 0);
    cJSON_Delete(rows);

    cwist_orm_close_socket(orm);
    printf("cwist_db_transfer_sqlite_to_socket round-trip test passed.\n");
}

int main(void) {
    int sv[2] = {-1, -1};
    int res = cwist_orm_socketpair_cloexec(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(res == 0);
    assert(sv[0] >= 0);
    assert(sv[1] >= 0);

    int flags0 = fcntl(sv[0], F_GETFD);
    assert(flags0 != -1);
    assert((flags0 & FD_CLOEXEC) != 0);

    int flags1 = fcntl(sv[1], F_GETFD);
    assert(flags1 != -1);
    assert((flags1 & FD_CLOEXEC) != 0);

    const char *msg = "test_payload";
    ssize_t w = write(sv[0], msg, strlen(msg));
    assert(w == (ssize_t)strlen(msg));

    char buf[32] = {0};
    ssize_t r = read(sv[1], buf, sizeof(buf));
    assert(r == (ssize_t)strlen(msg));
    assert(strcmp(buf, msg) == 0);

    close(sv[0]);
    close(sv[1]);

    printf("cwist_orm_socketpair_cloexec unit test passed.\n");

    test_transfer_sqlite_to_socket();
    return 0;
}
