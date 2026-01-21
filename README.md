# CWIST

**A modern, lightweight C web framework for building secure, scalable applications.**

CWIST brings the ergonomics of modern web frameworks (like Express or Flask) to C, without sacrificing performance or control.

## Quick Start

### 1. Installation

```bash
git clone https://github.com/your/cwist.git
cd cwist
make install
```

### 2. Hello World

Create `main.c`:

```c
#include <cwist/app.h>
#include <cwist/sstring.h>

void index_handler(cwist_http_request *req, cwist_http_response *res) {
    cwist_sstring_assign(res->body, "Hello from CWIST!");
}

void json_handler(cwist_http_request *req, cwist_http_response *res) {
    cwist_sstring_assign(res->body, "{\"status\": \"ok\"}");
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
}

int main() {
    cwist_app *app = cwist_app_create();
    
    // Define Routes
    cwist_app_get(app, "/", index_handler);
    cwist_app_get(app, "/api", json_handler);
    
    // Go!
    cwist_app_listen(app, 8080);
    
    cwist_app_destroy(app);
    return 0;
}
```

Compile and run:

```bash
gcc -o server main.c -lcwist -lssl -lcrypto -luriparser -lsqlite3 -lcjson
./server
```

## Key Features

*   **⚡ Modern API**: Intuitive `app_get` / `app_post` routing.
*   **🔒 Secure by Default**: Built-in HTTPS support via OpenSSL.
*   **💾 Database Ready**: Simple SQLite integration out of the box.
*   **🚀 Performance**: Multi-threaded architecture.
*   **🛠 Utilities**:
    *   Safe Dynamic Strings (`SString`)
    *   Query Parameter Parsing
    *   JSON Support

## Documentation

*   [Framework & Routing](docs/api/app.md)
*   [Database Access](docs/api/sql.md)
*   [Core HTTP](docs/api/http.md)

## License

MIT
