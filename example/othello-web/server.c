#define _POSIX_C_SOURCE 200809L
#include <cwist/https.h>
#include <cwist/http.h>
#include <cwist/sstring.h>
#include <cwist/sql.h>
#include <cwist/query.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cjson/cJSON.h>

#define PORT 8443
#define SIZE 8
#define BLACK 1
#define WHITE 2

// Global DB Connection
cwist_db *db_conn = NULL;

// --- Helper: Read File ---
char *read_file_content(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);
    return buf;
}

// --- DB Logic ---

void init_db() {
    cwist_db_open(&db_conn, "example/othello-web/othello.db");
    
    const char *schema = "CREATE TABLE IF NOT EXISTS games (room_id INTEGER PRIMARY KEY, board TEXT, turn INTEGER, status TEXT, players INTEGER);";
    
    cwist_db_exec(db_conn, schema);
}

// Helper to serialize board to CSV string
void serialize_board(int board[SIZE][SIZE], char *out) {
    out[0] = '\0';
    char buf[16];
    for (int r=0; r<SIZE; r++) {
        for (int c=0; c<SIZE; c++) {
            sprintf(buf, "%d,", board[r][c]);
            strcat(out, buf);
        }
    }
    // remove last comma
    if(strlen(out) > 0) out[strlen(out)-1] = '\0';
}

// Helper to deserialize
void deserialize_board(const char *in, int board[SIZE][SIZE]) {
    if (!in) return;
    char *dup = strdup(in);
    char *token = strtok(dup, ",");
    int r=0, c=0;
    while (token) {
        board[r][c] = atoi(token);
        c++;
        if (c >= SIZE) { c=0; r++; }
        token = strtok(NULL, ",");
    }
    free(dup);
}

// Get or Create Game
void get_game_state(int room_id, int board[SIZE][SIZE], int *turn, char *status, int *players) {
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT board, turn, status, players FROM games WHERE room_id = %d;", room_id);
    
    cJSON *res = NULL;
    cwist_db_query(db_conn, sql, &res);
    
    if (cJSON_GetArraySize(res) == 0) {
        // Create new game
        memset(board, 0, sizeof(int)*SIZE*SIZE);
        board[3][3] = WHITE; board[3][4] = BLACK;
        board[4][3] = BLACK; board[4][4] = WHITE;
        *turn = BLACK;
        strcpy(status, "waiting");
        *players = 0;
        
        char board_str[1024];
        serialize_board(board, board_str);
        
        char insert[2048];
        snprintf(insert, sizeof(insert), 
            "INSERT INTO games (room_id, board, turn, status, players) VALUES (%d, '%s', %d, '%s', %d);",
            room_id, board_str, *turn, status, *players);
        cwist_db_exec(db_conn, insert);
    } else {
        cJSON *row = cJSON_GetArrayItem(res, 0);
        cJSON *b = cJSON_GetObjectItem(row, "board");
        if(b && b->valuestring) deserialize_board(b->valuestring, board);
        
        cJSON *t = cJSON_GetObjectItem(row, "turn");
        if(t && t->valuestring) *turn = atoi(t->valuestring);
        else *turn = BLACK;

        cJSON *s = cJSON_GetObjectItem(row, "status");
        if(s && s->valuestring) strcpy(status, s->valuestring);
        else strcpy(status, "waiting");

        cJSON *p = cJSON_GetObjectItem(row, "players");
        if(p && p->valuestring) *players = atoi(p->valuestring);
        else *players = 0;
    }
    cJSON_Delete(res);
}

void update_game_state(int room_id, int board[SIZE][SIZE], int turn, const char *status, int players) {
    char board_str[1024];
    serialize_board(board, board_str);
    
    char sql[2048];
    snprintf(sql, sizeof(sql), 
        "UPDATE games SET board='%s', turn=%d, status='%s', players=%d WHERE room_id=%d;",
        board_str, turn, status, players, room_id);
    cwist_db_exec(db_conn, sql);
}

// --- Handler ---

void handle_client(cwist_https_connection *conn) {
    cwist_http_request *req = cwist_https_receive_request(conn);
    if (!req) return;

    cwist_http_response *res = cwist_http_response_create();
    
    printf("Request: %s %s (Params: %s)\n", cwist_http_method_to_string(req->method), req->path->data, req->query->data);

    // Default room 1
    int room_id = 1;
    const char *room_str = cwist_query_map_get(req->query_params, "room");
    if (room_str && strlen(room_str) > 0) {
        room_id = atoi(room_str);
    }

    if (strcmp(req->path->data, "/") == 0) {
        char *content = read_file_content("example/othello-web/index.html");
        if (content) {
            cwist_sstring_assign(res->body, content);
            free(content);
            cwist_http_header_add(&res->headers, "Content-Type", "text/html");
        } else {
            res->status_code = CWIST_HTTP_NOT_FOUND;
        }
    } else if (strcmp(req->path->data, "/style.css") == 0) {
        char *content = read_file_content("example/othello-web/style.css");
        if (content) {
            cwist_sstring_assign(res->body, content);
            free(content);
            cwist_http_header_add(&res->headers, "Content-Type", "text/css");
        }
    } else if (strcmp(req->path->data, "/script.js") == 0) {
        char *content = read_file_content("example/othello-web/script.js");
        if (content) {
            cwist_sstring_assign(res->body, content);
            free(content);
            cwist_http_header_add(&res->headers, "Content-Type", "application/javascript");
        }
    } 
    // API: /join?room=X (POST)
    else if (strcmp(req->path->data, "/join") == 0 && req->method == CWIST_HTTP_POST) {
        int board[SIZE][SIZE];
        int turn, players;
        char status[32];
        
        get_game_state(room_id, board, &turn, status, &players);

        if (players >= 2) {
             res->status_code = CWIST_HTTP_FORBIDDEN;
             cwist_sstring_assign(res->body, "{\"error\": \"Room full\"}");
             goto send;
        }
        
        players++;
        int pid = players;
        if (players == 2) strcpy(status, "active");
        
        update_game_state(room_id, board, turn, status, players);

        cJSON *json = cJSON_CreateObject();
        cJSON_AddNumberToObject(json, "player_id", pid);
        cJSON_AddNumberToObject(json, "room_id", room_id);
        char *str = cJSON_Print(json);
        cwist_sstring_assign(res->body, str);
        free(str);
        cJSON_Delete(json);
        cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    }
    // API: /state?room=X (GET)
    else if (strcmp(req->path->data, "/state") == 0) {
        int board[SIZE][SIZE];
        int turn, players;
        char status[32];
        get_game_state(room_id, board, &turn, status, &players);

        cJSON *json = cJSON_CreateObject();
        cJSON_AddStringToObject(json, "status", status);
        cJSON_AddNumberToObject(json, "turn", turn);
        
        cJSON *board_arr = cJSON_CreateArray();
        for(int r=0; r<SIZE; r++) {
            for(int c=0; c<SIZE; c++) {
                cJSON_AddItemToArray(board_arr, cJSON_CreateNumber(board[r][c]));
            }
        }
        cJSON_AddItemToObject(json, "board", board_arr);
        
        char *str = cJSON_Print(json);
        cwist_sstring_assign(res->body, str);
        free(str);
        cJSON_Delete(json);
        cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    }
    // API: /move?room=X (POST)
    else if (strcmp(req->path->data, "/move") == 0 && req->method == CWIST_HTTP_POST) {
        cJSON *json = cJSON_Parse(req->body->data);
        if (json) {
            int r = cJSON_GetObjectItem(json, "r")->valueint;
            int c = cJSON_GetObjectItem(json, "c")->valueint;
            int p = cJSON_GetObjectItem(json, "player")->valueint;
            
            int board[SIZE][SIZE];
            int turn, players;
            char status[32];
            get_game_state(room_id, board, &turn, status, &players);

            if (strcmp(status, "active") == 0 && p == turn) {
                // Apply Move (Simplified logic duplicated from prev)
                // In real app, share logic or make shared obj.
                board[r][c] = p;
                int opponent = (p == BLACK) ? WHITE : BLACK;
                
                int dr[] = {-1, -1, -1, 0, 0, 1, 1, 1};
                int dc[] = {-1, 0, 1, -1, 1, -1, 0, 1};
                
                for (int i = 0; i < 8; i++) {
                    int nr = r + dr[i];
                    int nc = c + dc[i];
                    int r_temp = nr, c_temp = nc;
                    int count = 0;
                    while(r_temp >= 0 && r_temp < SIZE && c_temp >= 0 && c_temp < SIZE && board[r_temp][c_temp] == opponent) {
                        r_temp += dr[i]; c_temp += dc[i]; count++;
                    }
                    if (count > 0 && r_temp >= 0 && r_temp < SIZE && c_temp >= 0 && c_temp < SIZE && board[r_temp][c_temp] == p) {
                        int rr = r + dr[i]; int cc = c + dc[i];
                        while(board[rr][cc] == opponent) {
                            board[rr][cc] = p; rr += dr[i]; cc += dc[i];
                        }
                    }
                }
                
                turn = opponent;
                update_game_state(room_id, board, turn, status, players);
                cwist_sstring_assign(res->body, "{\"status\":\"ok\"}");
            } else {
                 res->status_code = CWIST_HTTP_FORBIDDEN;
            }
            cJSON_Delete(json);
        } else {
            res->status_code = CWIST_HTTP_BAD_REQUEST;
        }
    }
    else {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        cwist_sstring_assign(res->body, "Not Found");
    }

send:
    cwist_https_send_response(conn, res);
    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
}

int main() {
    init_db();

    cwist_https_context *ctx = NULL;
    cwist_error_t err = cwist_https_init_context(&ctx, "example/othello-web/server.crt", "example/othello-web/server.key");
    
    if (err.errtype == CWIST_ERR_JSON) {
        printf("Init Failed: %s\n", cJSON_GetStringValue(cJSON_GetObjectItem(err.error.err_json, "openssl_error")));
        return 1;
    }

    struct sockaddr_in addr;
    int server_fd = cwist_make_socket_ipv4(&addr, "0.0.0.0", PORT, 10);
    
    if (server_fd < 0) {
        printf("Socket creation failed: %d\n", server_fd);
        return 1;
    }

    printf("Starting HTTPS Othello Server on port %d...\n", PORT);
    cwist_https_server_loop(server_fd, ctx, handle_client);

    cwist_https_destroy_context(ctx);
    cwist_db_close(db_conn);
    return 0;
}
