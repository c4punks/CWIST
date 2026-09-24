# CWIST Web Development Tutorial (A to Z)

> 한국어: [korean/cwist_tutorial.md](korean/cwist_tutorial.md)

Welcome, especially if you already know Spring Boot or ReactJS! CWIST is a high-performance, ultra-lightweight web framework written in C. This tutorial helps you quickly grasp CWIST's philosophy and structure so you can put it to work on real web applications right away.

> **Philosophy**: "Explicit, lightweight, and safe." CWIST keeps magic to a minimum and uses 100% of C's performance while still offering a modern web development experience (routing, JSON, DB, JWT, WebSocket).

---

## Table of Contents
1. [Hello World: Starting Your First Server](#1-hello-world-starting-your-first-server)
2. [Routing and Handlers (Mux Router)](#2-routing-and-handlers-mux-router)
3. [JSON Parsing and Zod Schema Validation](#3-json-parsing-and-zod-schema-validation)
4. [Middleware (CORS, Logging)](#4-middleware-cors-logging)
5. [Databases and Migrations (SQLite)](#5-databases-and-migrations-sqlite)
6. [Authentication (JWT and DB Encryption)](#6-authentication-jwt-and-db-encryption)
7. [PQC TLS (Post-Quantum Hybrid Key Exchange)](#7-pqc-tls-post-quantum-hybrid-key-exchange)
8. [WebSocket Integration (Real-Time Bidirectional Communication)](#8-websocket-integration-real-time-bidirectional-communication)
9. [Template Engine and Static File Serving](#9-template-engine-and-static-file-serving)
10. [Dynamic CSS Composer (WASM and SSR)](#10-dynamic-css-composer-wasm-and-ssr)

---

## 1. Hello World: Starting Your First Server

The heart of CWIST is the `cwist_app` object. Much like Spring's `ApplicationContext`, it manages the application's lifecycle.

```c
#include <cwist/sys/app/app.h>
#include <cwist/core/sstring/sstring.h>

// Request handler (Spring's @GetMapping)
void hello_handler(cwist_http_request *req, cwist_http_response *res) {
    cwist_sstring_assign(res->body, "Hello, CWIST World!");
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
}

int main() {
    // 1. Create the app
    cwist_app *app = cwist_app_create();
    
    // 2. Configure the router
    cwist_app_get(app, "/hello", hello_handler);
    
    // 3. Run the server on port 8080 (blocking)
    cwist_app_listen(app, 8080);
    
    cwist_app_destroy(app);
    return 0;
}
```

---

## 2. Routing and Handlers (Mux Router)

This section shows how to handle dynamic paths (path parameters) and query strings (query parameters). CWIST offers an intuitive pattern very similar to React Router or Express.

```c
#include <cwist/app.h>
#include <stdio.h>

// 예: /users/:id?role=admin
void user_profile_handler(cwist_http_request *req, cwist_http_response *res) {
    // Read the path parameter (:id)
    const char *user_id = cwist_query_map_get(req->path_params, "id");
    
    // Read the query parameter (?role=admin)
    const char *role = cwist_query_map_get(req->query_params, "role");

    char buf[256];
    snprintf(buf, sizeof(buf), "User ID: %s, Role: %s", user_id, role ? role : "user");
    
    cwist_sstring_assign(res->body, buf);
}

int main() {
    cwist_app *app = cwist_app_create();
    
    // Register a dynamic route
    cwist_app_get(app, "/users/:id", user_profile_handler);
    // ...
}
```

---

## 3. JSON Parsing and Zod Schema Validation

A powerful runtime schema validator inspired by `zod` from the TypeScript world. It safely parses the JSON sent by the client and validates its types.

```c
#include <cwist/app.h>
#include <cwist/core/utils/zod.h>
#include <cjson/cJSON.h>

// 1. 스키마 정의 (name은 필수 문자열, age는 필수 정수)
static const cwist_schema_field_t user_fields[] = {
    {"name", {NULL}, CWIST_FIELD_STRING, true},
    {"age", {NULL}, CWIST_FIELD_INT, true},
};
static const cwist_schema_t user_schema = {user_fields, 2};

void create_user_handler(cwist_http_request *req, cwist_http_response *res) {
    // 2. 검증 (Body -> JSON)
    cJSON *parsed_json = NULL;
    const char *raw = (req->body && req->body->data) ? req->body->data : "";
    cwist_zod_result_t z_res = cwist_zod_parse(raw, &user_schema, &parsed_json);

    if (!z_res.valid || !parsed_json) {
        res->status_code = CWIST_HTTP_BAD_REQUEST;
        cwist_sstring_assign(res->body,
                             z_res.error_count > 0 ? z_res.errors[0].message : "invalid JSON");
        return;
    }

    res->status_code = CWIST_HTTP_CREATED;

    // 3. JSON 응답 생성 (cJSON Builder 패턴)
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "status", "User created");
    cJSON_AddStringToObject(reply, "name", cJSON_GetObjectItem(parsed_json, "name")->valuestring);

    char *json_str = cJSON_PrintUnformatted(reply);
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_sstring_assign(res->body, json_str);

    cJSON_free(json_str);
    cJSON_Delete(reply);
    cJSON_Delete(parsed_json);
}
```

---

## 4. Middleware (CORS, Logging)

You can easily build a pipeline that every request passes through (Spring's Interceptor, Express's Middleware).

```c
#include <cwist/app.h>

// CORS 처리를 위한 미들웨어: next를 호출하면 다음 단계(다음 미들웨어 또는 핸들러)로 진행
void cors_middleware(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) {
    cwist_http_header_add(&res->headers, "Access-Control-Allow-Origin", "*");
    cwist_http_header_add(&res->headers, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");

    // OPTIONS 요청 시 바로 응답 (next를 호출하지 않으면 체인 중단)
    if (req->method == CWIST_HTTP_OPTIONS) {
        res->status_code = CWIST_HTTP_NO_CONTENT;
        return;
    }

    if (next) next(req, res);
}

int main() {
    cwist_app *app = cwist_app_create();

    // 미들웨어 등록 (전역 적용)
    cwist_app_use(app, cors_middleware);
    // ...
}
```

---

## 5. Databases and Migrations (SQLite)

CWIST fully supports embedded SQLite and provides a connection pool and migration tooling tied to the app lifecycle.

```c
#include <cwist/app.h>
#include <cwist/core/db/sql.h>
#include <cwist/core/db/migrate.h>

void get_users_handler(cwist_http_request *req, cwist_http_response *res) {
    // req->db는 cwist_app_use_db()로 앱에 연결한 DB 인스턴스
    cwist_db *db = req->db;

    sqlite3_stmt *stmt = NULL;
    if (!db || sqlite3_prepare_v2(db->conn, "SELECT id, name FROM users", -1, &stmt, NULL) !=
                   SQLITE_OK) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        return;
    }

    cJSON *users_array = cJSON_CreateArray();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        cJSON *user = cJSON_CreateObject();
        cJSON_AddNumberToObject(user, "id", sqlite3_column_int(stmt, 0));
        cJSON_AddStringToObject(user, "name", (const char *)sqlite3_column_text(stmt, 1));
        cJSON_AddItemToArray(users_array, user);
    }
    sqlite3_finalize(stmt);

    char *json_str = cJSON_PrintUnformatted(users_array);
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_sstring_assign(res->body, json_str);
    cJSON_free(json_str);
    cJSON_Delete(users_array);
}

// 스키마 마이그레이션 (version 순서대로 한 번씩만 적용)
static const cwist_migration_t migrations[] = {
    {.version = 1,
     .name = "create_users",
     .up_sql = "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);",
     .down_sql = "DROP TABLE users;"},
    {.version = 2,
     .name = "insert_seed",
     .up_sql = "INSERT INTO users (name) VALUES ('Alice'), ('Bob');",
     .down_sql = "DELETE FROM users;"},
};

int main() {
    cwist_app *app = cwist_app_create();

    // DB 연결 ("file.db" 또는 ":memory:")
    cwist_error_t err = cwist_app_use_db(app, "app_data.db");
    if (!cwist_error_is_ok(&err)) {
        cwist_error_dispose(&err);
        cwist_app_destroy(app);
        return 1;
    }

    // 스키마 마이그레이션 자동 적용
    if (cwist_migrate_up(cwist_app_get_db(app)->conn, migrations, 2) != CWIST_MIGRATE_OK) {
        cwist_app_destroy(app);
        return 1;
    }

    cwist_app_get(app, "/users", get_users_handler);
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
```

---

## 6. Authentication (JWT and DB Encryption)

Stateless JWT authentication, a must for the modern web, is supported through built-in functions.

```c
#include <cwist/app.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/security/jwt/jwt.h>
#include <string.h>

#define SECRET_KEY "my_super_secret"

// Issue a JWT on successful login
void login_handler(cwist_http_request *req, cwist_http_response *res) {
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "user_id", "12345");
    cJSON_AddStringToObject(payload, "role", "admin");
    char *payload_json = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);

    // 3600초(1시간) 유효기간의 토큰 생성 (exp 클레임 자동 추가)
    char *token = cwist_jwt_sign(payload_json, SECRET_KEY, 3600);
    cJSON_free(payload_json);

    if (!token) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        return;
    }
    cwist_sstring_assign(res->body, token);
    cwist_free(token);
}

// API 요청 시 JWT 검증 미들웨어
void auth_middleware(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) {
    const char *auth_header = cwist_http_header_get(req->headers, "Authorization");

    if (auth_header && strncmp(auth_header, "Bearer ", 7) == 0) {
        // 서명과 exp 검증에 성공하면 클레임 객체, 실패하면 NULL
        cwist_jwt_claims *claims = cwist_jwt_verify(auth_header + 7, SECRET_KEY);
        if (claims) {
            // 토큰 유효함, 통과! (예: cwist_jwt_claims_get(claims, "role"))
            cwist_jwt_claims_destroy(claims);
            if (next) next(req, res);
            return;
        }
    }

    // 실패 시 401 응답 (next를 호출하지 않으면 체인 중단)
    res->status_code = CWIST_HTTP_UNAUTHORIZED;
    cwist_sstring_assign(res->body, "{\"error\": \"Unauthorized\"}");
}
```

---

## 7. PQC TLS (Post-Quantum Hybrid Key Exchange)

CWIST can enable post-quantum hybrid TLS with a single line of code. It is a hybrid KEM that combines classic X25519 ECDH with the NIST-standard ML-KEM-768 (Kyber family), so the key exchange stays secure even against quantum computers.

```c
#include <cwist/sys/app/app.h>

int main() {
    cwist_app *app = cwist_app_create();

    // Enable HTTPS
    cwist_app_use_https(app, "server.crt", "server.key");

    // Enable the PQC hybrid layer: one line is all it takes
    cwist_app_use_pqc_layer(app, true);

    // Every TLS 1.3 connection now uses the X25519MLKEM768:X25519:P-256 groups.
    // TLS 1.2 and below are disabled automatically.
    cwist_app_listen(app, 8443);
    cwist_app_destroy(app);
    return 0;
}
```

### Security Policy Summary

| Item | Setting |
|------|------|
| Key Exchange Group | `X25519MLKEM768:X25519:P-256` |
| Minimum TLS version | 1.3 |
| Legacy TLS | Disabled (1.0, 1.1, 1.2 removed) |
| Downgrade protection | Enabled |

> **Note**: This setting applies only to key exchange at the **transport layer**. Switching certificate signatures to PQC as well would require a separate feature such as `cwist_app_use_pqc_cert()`, which is still considered a step too far for the current ecosystem.

---

## 8. WebSocket Integration (Real-Time Bidirectional Communication)

CWIST는 동일한 포트에서 HTTP 통신을 WebSocket으로 쉽게 업그레이드 할 수 있습니다. `cwist_app_ws()`로 경로를 등록하면 업그레이드 핸드셰이크는 프레임워크가 처리하고, 핸들러는 연결된 `cwist_websocket`만 다룹니다.

```c
#include <cwist/app.h>
#include <cwist/net/websocket/websocket.h>
#include <stdio.h>

// 연결 하나당 한 번 호출됩니다. 업그레이드(101 응답)는 프레임워크가 처리하며,
// 이 함수가 반환하면 연결이 정리됩니다.
void chat_handler(cwist_websocket *ws) {
    cwist_ws_frame *frame;
    // 연결이 닫히거나 오류가 나면 NULL (PING에 대한 PONG 응답은 자동)
    while ((frame = cwist_websocket_receive(ws)) != NULL) {
        if (frame->opcode == CWIST_WS_FRAME_CLOSE) {
            cwist_websocket_frame_destroy(frame);
            break;
        }
        if (frame->opcode == CWIST_WS_FRAME_TEXT) {
            printf("Received: %.*s\n", (int)frame->payload_len, (const char *)frame->payload);
            // 에코 응답 (클라이언트로 다시 전송)
            cwist_websocket_send(ws, CWIST_WS_FRAME_TEXT, frame->payload, frame->payload_len);
        }
        cwist_websocket_frame_destroy(frame);
    }
    printf("Client disconnected.\n");
}

int main() {
    cwist_app *app = cwist_app_create();
    cwist_app_ws(app, "/chat", chat_handler);
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
```

---

## 9. Template Engine and Static File Serving

Useful when building an HTML-based SSR (Server-Side Rendering) project or serving a React build output (static files).

```c
#include <cwist/app.h>
#include <cwist/core/template/template.h>
#include <cjson/cJSON.h>

void render_home_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;

    // Inject data (e.g. substitute the {{ title }} variable)
    cJSON *context = cJSON_CreateObject();
    cJSON_AddStringToObject(context, "title", "CWIST Homepage");
    cJSON_AddStringToObject(context, "user", "Developer");

    // Read the template file + render it
    cwist_sstring *output = cwist_template_render_file("views/index.html", context);
    cJSON_Delete(context);

    // Respond after rendering
    if (output) {
        cwist_sstring_assign(res->body, output->data);
        cwist_sstring_destroy(output);
    } else {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "Failed to render template");
    }
    cwist_http_header_add(&res->headers, "Content-Type", "text/html");
}

int main() {
    cwist_app *app = cwist_app_create();

    cwist_app_get(app, "/", render_home_handler);

    // Mount a static directory (for serving React/Vue build output)
    // Requests to the "/public" URL are served files from the "./public" folder
    cwist_app_static(app, "/public", "./public");

    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
```

---

## 10. Dynamic CSS Composer (WASM and SSR)

Going beyond a plain backend, CWIST builds on C's strong numeric computation to ship a **CSS Composer that synthesizes a design system at runtime**. It mathematically derives Hover/Active states from a color's lightness and computes roundness and spacing dynamically.

Here are two typical ways to use it.

### Approach A: 100% Server-Side Rendering (SSR)
The server generates theme CSS dynamically and injects it at render time. This is very useful for offering per-user custom themes.

```c
#include <cwist/app.h>
#include <cwist/core/html/css_composer.h>

void theme_css_handler(cwist_http_request *req, cwist_http_response *res) {
    cwist_css_config cfg;
    cwist_css_config_init(&cfg);

    // Parse the hex code from the query parameter and set it as the primary color
    const char *color = cwist_query_map_get(req->query_params, "color");
    if (color) {
        cfg.primary_color = cwist_color_hex_to_rgb(color);
    }
    
    // Set whether dark mode is on
    const char *dark = cwist_query_map_get(req->query_params, "dark");
    cfg.is_dark_mode = (dark && strcmp(dark, "1") == 0);

    // Run the math needed for CSS composition (HSL conversion, etc.) and return the stylesheet string
    cwist_sstring *css_output = cwist_css_generate_stylesheet(&cfg);

    cwist_sstring_assign(res->body, css_output->data);
    cwist_http_header_add(&res->headers, "Content-Type", "text/css");
    
    cwist_sstring_destroy(css_output);
}

int main() {
    cwist_app *app = cwist_app_create();
    
    // Reachable via <link rel="stylesheet" href="/theme.css?color=ff5733&dark=1">
    cwist_app_get(app, "/theme.css", theme_css_handler);
    
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
```

### Approach B: Client-Side Composition with React + WebAssembly (WASM)
CWIST's `css_composer.c` is written independently of the framework, so you can build it to `.wasm` with Emscripten, import it into a React app, and run the computation instantly on the client.

1. **WASM build (Emscripten)**
```bash
emcc src/core/html/css_composer.c -Iinclude \
    -s EXPORTED_FUNCTIONS="['_cwist_color_hex_to_rgb', '_cwist_css_generate_stylesheet', '_malloc', '_free']" \
    -o public/css_composer.js
```

2. **Using it from React (dynamic design system)**
```javascript
import React, { useEffect, useState } from 'react';

function DynamicThemeApp() {
  const [themeColor, setThemeColor] = useState("#3B82F6");

  useEffect(() => {
    // 1. Load the WASM module
    window.Module().then((module) => {
      // 2. Convert the input hex code into C's RGB struct
      const hexPtr = module.allocateUTF8(themeColor);
      const rgb = module._cwist_color_hex_to_rgb(hexPtr);
      module._free(hexPtr);

      // (Hypothetical example) Lay out C's config struct in memory, then compose the CSS
      // In practice it is easier to write a JS <-> C bridge function (wrapper) and call that.
      const cssStringPtr = module._cwist_css_generate_stylesheet(/* config_ptr */);
      const cssString = module.UTF8ToString(cssStringPtr);

      // 3. Inject it into the browser DOM immediately
      document.getElementById('dynamic-theme').innerText = cssString;
    });
  }, [themeColor]);

  return (
    <div className="bg-body text-main">
      <style id="dynamic-theme"></style>
      <input type="color" value={themeColor} onChange={e => setThemeColor(e.target.value)} />
      <button className="bg-primary radius-md">CWIST Themed Button</button>
    </div>
  );
}
```

---

### 마치며
이 튜토리얼을 통해 C 기반 환경임에도 불구하고 얼마나 친숙하고 선언적으로 웹 개발을 할 수 있는지 확인하셨길 바랍니다. `cwist_app` 구조체가 전체 생명주기를, `cwist_app_get()` 같은 등록 함수가 라우팅을 담당한다는 점만 기억하면 기존 모던 프레임워크와 동일한 아키텍처로 개발을 진행할 수 있습니다.
