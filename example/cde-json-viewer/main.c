/**
 * @file main.c
 * @brief CDE-style JSON viewer using the app-level API with HTML builder
 *        and dynamic CSS composer.
 */

#include <cwist/app.h>
#include <cwist/core/html/builder.h>
#include <cwist/core/html/css_composer.h>
#include <cjson/cJSON.h>

static const char *MOCK_JSON =
    "{\"System\":\"Solaris 2.5.1\",\"Host\":\"sun-sparc-station\","
    "\"User\":\"yjlee\",\"Shell\":\"/bin/csh\","
    "\"Uptime\":\"42 days\",\"Load\":\"0.01, 0.05, 0.00\"}";

static cwist_sstring *form_ui(cJSON *json) {
    cwist_css_config cfg;
    cwist_css_config_init(&cfg);
    cfg.primary_color   = cwist_color_hex_to_rgb("#4b6983");
    cfg.secondary_color = cwist_color_hex_to_rgb("#5d97a6");
    cwist_sstring *css  = cwist_css_generate_stylesheet(&cfg);

    cwist_html_element_t *html = cwist_html_element_create("html");
    cwist_html_element_t *head = cwist_html_element_create("head");
    cwist_html_element_t *style = cwist_html_element_create("style");
    cwist_html_element_set_text(style, css->data);
    cwist_html_element_add_child(head, style);
    cwist_html_element_add_child(html, head);

    cwist_html_element_t *body = cwist_html_element_create("body");
    cwist_html_element_t *win  = cwist_html_element_create("div");
    cwist_html_element_add_class(win, "container");

    cwist_html_element_t *title = cwist_html_element_create("h2");
    cwist_html_element_set_text(title, "System Monitor");
    cwist_html_element_add_child(win, title);

    cwist_html_element_t *table = cwist_html_element_create("table");
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, json) {
        if (cJSON_IsString(item)) {
            cwist_html_element_t *tr = cwist_html_element_create("tr");

            cwist_html_element_t *td_key = cwist_html_element_create("td");
            cwist_html_element_set_text(td_key, item->string);
            cwist_html_element_add_child(tr, td_key);

            cwist_html_element_t *td_val = cwist_html_element_create("td");
            cwist_html_element_set_text(td_val, item->valuestring);
            cwist_html_element_add_child(tr, td_val);

            cwist_html_element_add_child(table, tr);
        }
    }
    cwist_html_element_add_child(win, table);
    cwist_html_element_add_child(body, win);
    cwist_html_element_add_child(html, body);

    cwist_sstring *out = cwist_html_render(html);
    cwist_html_element_destroy(html);
    cwist_sstring_destroy(css);
    return out;
}

void send_response(int client_fd, cwist_http_response *res) {
    smartstring *raw = smartstring_create();
    
    // Status Line
    char status_line[128];
    snprintf(status_line, 127, "%s %d %s\r\n", res->version->data, res->status_code, res->status_text->data);
    smartstring_append(raw, status_line);

    // Headers
    cwist_http_header_node *curr = res->headers;
    while(curr) {
        smartstring_append(raw, curr->key->data);
        smartstring_append(raw, ": ");
        smartstring_append(raw, curr->value->data);
        smartstring_append(raw, "\r\n");
        curr = curr->next;
    }
    smartstring_append(raw, "\r\n"); // End of headers

    // Body
    if(res->body->data) {
        smartstring_append(raw, res->body->data);
    }

    if (raw->data) {
        send(client_fd, raw->data, strlen(raw->data), 0);
    }
    smartstring_destroy(raw);
}

void handle_client(int client_fd) {
    char buffer[BUFFER_SIZE];
    int read_len = read(client_fd, buffer, BUFFER_SIZE - 1);
    if (read_len < 0) {
        close(client_fd);
        return;
    }
    cwist_sstring *html = form_ui(json);
    cJSON_Delete(json);
    cwist_http_header_add(&res->headers, "Content-Type", "text/html");
    cwist_sstring_assign(res->body, html->data);
    cwist_sstring_destroy(html);
}

int main() {
    int port = 8080, backlog = 128;
    const char *addr = "127.0.0.1";
    struct sockaddr_in sockv4;
    
    int server_fd =  cwist_make_socket_ipv4(&sockv4, addr, port, backlog);
    if (server_fd < 0) {
        printf("Failed to create server socket: %d\n", server_fd);
        return 1;
    }

    printf("Server listening on port %d\n", PORT);
    printf("Visit http://%s:%d to see the CDE JSON Viewer\n", addr, port);

    cwist_accept_socket(server_fd, (struct sockaddr*)&sockv4, handle_client);
    return 0;
}
