/*
 @licstart  The following is the entire license notice for the JavaScript code in this file.

 The MIT License (MIT)

 Copyright (C) 1997-2020 by Dimitri van Heesch

 Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 and associated documentation files (the "Software"), to deal in the Software without restriction,
 including without limitation the rights to use, copy, modify, merge, publish, distribute,
 sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all copies or
 substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 @licend  The above is the entire license notice for the JavaScript code in this file
*/
var NAVTREE =
[
  [ "cwist", "index.html", [
    [ "CWIST API Reference", "d1/d7e/md_docs_2api-quickref.html", [
      [ "Supported platforms", "index.html#autotoc_md3", null ],
      [ "Quick start", "index.html#autotoc_md4", null ],
      [ "Same routes and handlers across four targets", "index.html#autotoc_md5", null ],
      [ "What CWIST includes", "index.html#autotoc_md6", null ],
      [ "Performance: throughput, latency, and tails", "index.html#autotoc_md7", [
        [ "Low-concurrency latency floor", "index.html#autotoc_md8", null ],
        [ "External the-benchmarker / zrk results for v4.3.1", "index.html#autotoc_md9", null ],
        [ "Two server modes", "index.html#autotoc_md10", null ],
        [ "Holding a million connections", "index.html#autotoc_md11", null ]
      ] ],
      [ "Why C, when Axum and Gin exist?", "index.html#autotoc_md12", null ],
      [ "Memory safety and security in C", "index.html#autotoc_md13", null ],
      [ "Install", "index.html#autotoc_md14", [
        [ "Homebrew", "index.html#autotoc_md15", null ],
        [ "Nix", "index.html#autotoc_md16", null ]
      ] ],
      [ "Platform support", "index.html#autotoc_md17", null ],
      [ "Bare-metal STM32", "index.html#autotoc_md18", null ],
      [ "The cwist CLI and hot reload", "index.html#autotoc_md19", null ],
      [ "Linking", "index.html#autotoc_md20", null ],
      [ "Configuration", "index.html#autotoc_md21", null ],
      [ "Feature notes", "index.html#autotoc_md22", null ],
      [ "Latest benchmark run", "index.html#autotoc_md23", null ],
      [ "Stability and conformance", "index.html#autotoc_md24", null ],
      [ "Documentation", "index.html#autotoc_md25", null ],
      [ "Examples", "index.html#autotoc_md26", null ],
      [ "Community", "index.html#autotoc_md27", null ],
      [ "Third-party licenses", "index.html#autotoc_md28", null ],
      [ "Error Handling", "d1/d7e/md_docs_2api-quickref.html#autotoc_md30", [
        [ "make_error", "d1/d7e/md_docs_2api-quickref.html#autotoc_md31", null ],
        [ "cwist_error_is_ok / cwist_error_is_ok_extern", "d1/d7e/md_docs_2api-quickref.html#autotoc_md32", null ],
        [ "Other exported inline helpers", "d1/d7e/md_docs_2api-quickref.html#autotoc_md33", null ]
      ] ],
      [ "SString (cwist_sstring)", "d1/d7e/md_docs_2api-quickref.html#autotoc_md34", [
        [ "Lifecycle", "d1/d7e/md_docs_2api-quickref.html#autotoc_md35", null ],
        [ "Core helpers", "d1/d7e/md_docs_2api-quickref.html#autotoc_md36", null ],
        [ "Trimming", "d1/d7e/md_docs_2api-quickref.html#autotoc_md37", null ],
        [ "Append / Copy", "d1/d7e/md_docs_2api-quickref.html#autotoc_md38", null ],
        [ "Compare / Query", "d1/d7e/md_docs_2api-quickref.html#autotoc_md39", null ]
      ] ],
      [ "HTTP", "d1/d7e/md_docs_2api-quickref.html#autotoc_md40", [
        [ "Request lifecycle", "d1/d7e/md_docs_2api-quickref.html#autotoc_md41", null ],
        [ "Response lifecycle", "d1/d7e/md_docs_2api-quickref.html#autotoc_md42", null ],
        [ "Header helpers", "d1/d7e/md_docs_2api-quickref.html#autotoc_md43", null ],
        [ "Method helpers", "d1/d7e/md_docs_2api-quickref.html#autotoc_md44", null ],
        [ "Socket helpers", "d1/d7e/md_docs_2api-quickref.html#autotoc_md45", null ]
      ] ],
      [ "Session manager", "d1/d7e/md_docs_2api-quickref.html#autotoc_md46", [
        [ "Arena", "d1/d7e/md_docs_2api-quickref.html#autotoc_md47", null ],
        [ "Shared sessions (intrusive ref count)", "d1/d7e/md_docs_2api-quickref.html#autotoc_md48", null ],
        [ "Manager", "d1/d7e/md_docs_2api-quickref.html#autotoc_md49", null ]
      ] ]
    ] ],
    [ "CWIST API Documentation", "d7/d15/md_docs_2API.html", [
      [ "Modules", "d7/d15/md_docs_2API.html#autotoc_md51", null ],
      [ "See also", "d7/d15/md_docs_2API.html#autotoc_md52", null ]
    ] ],
    [ "Framework & App API", "de/d19/md_docs_2api_2app.html", [
      [ "Routing", "de/d19/md_docs_2api_2app.html#autotoc_md59", [
        [ "cwist_app_create", "de/d19/md_docs_2api_2app.html#autotoc_md54", null ],
        [ "cwist_app_use_https", "de/d19/md_docs_2api_2app.html#autotoc_md55", null ],
        [ "cwist_app_use_https2 / cwist_use_https2", "de/d19/md_docs_2api_2app.html#autotoc_md56", null ],
        [ "cwist_app_use_pqc_layer", "de/d19/md_docs_2api_2app.html#autotoc_md57", null ],
        [ "cwist_app_use_db", "de/d19/md_docs_2api_2app.html#autotoc_md58", null ],
        [ "cwist_app_get / cwist_app_post", "de/d19/md_docs_2api_2app.html#autotoc_md60", null ],
        [ "cwist_app_get_ex / _post_ex / _put_ex / _delete_ex / _patch_ex", "de/d19/md_docs_2api_2app.html#autotoc_md61", null ],
        [ "cwist_app_dispatch_memory", "de/d19/md_docs_2api_2app.html#autotoc_md62", null ],
        [ "cwist_app_ws", "de/d19/md_docs_2api_2app.html#autotoc_md63", null ],
        [ "cwist_endpoint_opt_t", "de/d19/md_docs_2api_2app.html#autotoc_md64", null ]
      ] ],
      [ "Static Assets", "de/d19/md_docs_2api_2app.html#autotoc_md65", [
        [ "cwist_app_static", "de/d19/md_docs_2api_2app.html#autotoc_md66", null ]
      ] ],
      [ "Error Handling", "de/d19/md_docs_2api_2app.html#autotoc_md67", [
        [ "cwist_app_set_error_handler", "de/d19/md_docs_2api_2app.html#autotoc_md68", null ],
        [ "Unused Variables", "de/d19/md_docs_2api_2app.html#autotoc_md69", null ]
      ] ],
      [ "Concurrency & Thread-Safety", "de/d19/md_docs_2api_2app.html#autotoc_md70", null ],
      [ "Memory Ownership Rules", "de/d19/md_docs_2api_2app.html#autotoc_md71", [
        [ "cwist_app_listen", "de/d19/md_docs_2api_2app.html#autotoc_md72", null ]
      ] ],
      [ "In-Memory Test Client", "de/d19/md_docs_2api_2app.html#autotoc_md73", [
        [ "cwist_test_client_create / cwist_test_client_destroy", "de/d19/md_docs_2api_2app.html#autotoc_md74", null ],
        [ "Request helpers", "de/d19/md_docs_2api_2app.html#autotoc_md75", null ],
        [ "Cookie jar", "de/d19/md_docs_2api_2app.html#autotoc_md76", null ],
        [ "Test assertion macros", "de/d19/md_docs_2api_2app.html#autotoc_md77", null ]
      ] ]
    ] ],
    [ "Cookie API", "dc/d80/md_docs_2api_2cookie.html", [
      [ "Reading cookies", "dc/d80/md_docs_2api_2cookie.html#autotoc_md79", [
        [ "cwist_cookie_parse", "dc/d80/md_docs_2api_2cookie.html#autotoc_md80", null ],
        [ "cwist_cookie_get", "dc/d80/md_docs_2api_2cookie.html#autotoc_md81", null ]
      ] ],
      [ "Writing cookies", "dc/d80/md_docs_2api_2cookie.html#autotoc_md82", [
        [ "cwist_cookie_options", "dc/d80/md_docs_2api_2cookie.html#autotoc_md83", null ],
        [ "cwist_cookie_set", "dc/d80/md_docs_2api_2cookie.html#autotoc_md84", null ],
        [ "cwist_cookie_delete", "dc/d80/md_docs_2api_2cookie.html#autotoc_md85", null ],
        [ "cwist_cookie_delete_ex", "dc/d80/md_docs_2api_2cookie.html#autotoc_md86", null ]
      ] ],
      [ "Encoding", "dc/d80/md_docs_2api_2cookie.html#autotoc_md87", [
        [ "cwist_cookie_encode", "dc/d80/md_docs_2api_2cookie.html#autotoc_md88", null ],
        [ "cwist_cookie_decode", "dc/d80/md_docs_2api_2cookie.html#autotoc_md89", null ]
      ] ],
      [ "Example", "dc/d80/md_docs_2api_2cookie.html#autotoc_md90", null ]
    ] ],
    [ "HTML Components, Scoped CSS, Fragments and Assets", "d8/d83/md_docs_2api_2html.html", [
      [ "Components", "d8/d83/md_docs_2api_2html.html#autotoc_md92", [
        [ "cwist_html_component_create / cwist_html_component_destroy", "d8/d83/md_docs_2api_2html.html#autotoc_md93", null ],
        [ "cwist_html_component_instantiate", "d8/d83/md_docs_2api_2html.html#autotoc_md94", null ]
      ] ],
      [ "Scoped CSS", "d8/d83/md_docs_2api_2html.html#autotoc_md95", [
        [ "cwist_css_scope_init / cwist_css_scope_destroy", "d8/d83/md_docs_2api_2html.html#autotoc_md96", null ],
        [ "cwist_css_scope_class", "d8/d83/md_docs_2api_2html.html#autotoc_md97", null ],
        [ "cwist_css_scope_add_rule", "d8/d83/md_docs_2api_2html.html#autotoc_md98", null ],
        [ "cwist_css_scope_generate_stylesheet", "d8/d83/md_docs_2api_2html.html#autotoc_md99", null ]
      ] ],
      [ "HTML over the wire", "d8/d83/md_docs_2api_2html.html#autotoc_md100", [
        [ "cwist_http_request_wants_fragment / cwist_http_request_fragment_target", "d8/d83/md_docs_2api_2html.html#autotoc_md101", null ],
        [ "cwist_http_response_set_view", "d8/d83/md_docs_2api_2html.html#autotoc_md102", null ],
        [ "cwist_http_response_set_html", "d8/d83/md_docs_2api_2html.html#autotoc_md103", null ],
        [ "cwist_http_response_add_oob", "d8/d83/md_docs_2api_2html.html#autotoc_md104", null ],
        [ "cwist_http_response_html_redirect", "d8/d83/md_docs_2api_2html.html#autotoc_md105", null ],
        [ "Handler example", "d8/d83/md_docs_2api_2html.html#autotoc_md106", null ]
      ] ],
      [ "Asset pipeline", "d8/d83/md_docs_2api_2html.html#autotoc_md107", [
        [ "cwist_css_minify / cwist_css_bundle", "d8/d83/md_docs_2api_2html.html#autotoc_md108", null ],
        [ "cwist_app_asset_add / cwist_app_asset_add_file / cwist_app_asset_url", "d8/d83/md_docs_2api_2html.html#autotoc_md109", null ],
        [ "Putting it together", "d8/d83/md_docs_2api_2html.html#autotoc_md110", null ]
      ] ],
      [ "Rendering the same views in WASM", "d8/d83/md_docs_2api_2html.html#autotoc_md111", null ],
      [ "Example", "d8/d83/md_docs_2api_2html.html#autotoc_md112", null ]
    ] ],
    [ "HTTP Core API", "d2/d9e/md_docs_2api_2http.html", [
      [ "Constants", "d2/d9e/md_docs_2api_2http.html#autotoc_md114", [
        [ "cwist_http_request_create", "d2/d9e/md_docs_2api_2http.html#autotoc_md115", null ],
        [ "cwist_http_parse_request", "d2/d9e/md_docs_2api_2http.html#autotoc_md116", null ],
        [ "cwist_http_receive_request", "d2/d9e/md_docs_2api_2http.html#autotoc_md117", null ],
        [ "cwist_http_response_create", "d2/d9e/md_docs_2api_2http.html#autotoc_md118", null ],
        [ "cwist_http_response_send_file", "d2/d9e/md_docs_2api_2http.html#autotoc_md119", null ],
        [ "cwist_http_header_add", "d2/d9e/md_docs_2api_2http.html#autotoc_md120", null ],
        [ "cwist_http_response_header_add", "d2/d9e/md_docs_2api_2http.html#autotoc_md121", null ],
        [ "cwist_http_header_get", "d2/d9e/md_docs_2api_2http.html#autotoc_md122", null ]
      ] ],
      [ "Server Core", "d2/d9e/md_docs_2api_2http.html#autotoc_md123", [
        [ "Keep-alive handling", "d2/d9e/md_docs_2api_2http.html#autotoc_md124", null ],
        [ "Regression testing", "d2/d9e/md_docs_2api_2http.html#autotoc_md125", null ],
        [ "cwist_http_server_loop", "d2/d9e/md_docs_2api_2http.html#autotoc_md126", null ],
        [ "cwist_accept_socket", "d2/d9e/md_docs_2api_2http.html#autotoc_md127", null ],
        [ "cwist_http_continuation_shed_count", "d2/d9e/md_docs_2api_2http.html#autotoc_md128", null ]
      ] ],
      [ "Security Headers", "d2/d9e/md_docs_2api_2http.html#autotoc_md129", [
        [ "cwist_http_response_add_security_headers", "d2/d9e/md_docs_2api_2http.html#autotoc_md130", null ],
        [ "cwist_http_response_add_hsts", "d2/d9e/md_docs_2api_2http.html#autotoc_md131", null ]
      ] ]
    ] ],
    [ "HTTPS API", "d4/d45/md_docs_2api_2https.html", null ],
    [ "JSON Builder API", "d8/deb/md_docs_2api_2json.html", [
      [ "Functions", "d8/deb/md_docs_2api_2json.html#autotoc_md140", [
        [ "cwist_json_builder_create", "d8/deb/md_docs_2api_2json.html#autotoc_md141", null ],
        [ "cwist_json_begin_object / cwist_json_end_object", "d8/deb/md_docs_2api_2json.html#autotoc_md142", null ],
        [ "cwist_json_begin_object_key", "d8/deb/md_docs_2api_2json.html#autotoc_md143", null ],
        [ "cwist_json_add_string", "d8/deb/md_docs_2api_2json.html#autotoc_md144", null ],
        [ "cwist_json_add_int", "d8/deb/md_docs_2api_2json.html#autotoc_md145", null ],
        [ "cwist_json_add_bool", "d8/deb/md_docs_2api_2json.html#autotoc_md146", null ],
        [ "cwist_json_add_null", "d8/deb/md_docs_2api_2json.html#autotoc_md147", null ],
        [ "cwist_json_begin_array / cwist_json_end_array", "d8/deb/md_docs_2api_2json.html#autotoc_md148", null ],
        [ "cwist_json_get_raw", "d8/deb/md_docs_2api_2json.html#autotoc_md149", null ],
        [ "cwist_json_builder_destroy", "d8/deb/md_docs_2api_2json.html#autotoc_md150", null ]
      ] ],
      [ "Example", "d8/deb/md_docs_2api_2json.html#autotoc_md151", null ]
    ] ],
    [ "Live message runtime (experimental)", "db/dbb/md_docs_2api_2livemsg.html", [
      [ "Minimal server", "db/dbb/md_docs_2api_2livemsg.html#autotoc_md153", null ],
      [ "Configuration", "db/dbb/md_docs_2api_2livemsg.html#autotoc_md154", null ],
      [ "Message and result ownership", "db/dbb/md_docs_2api_2livemsg.html#autotoc_md155", null ],
      [ "Replies, push, and backpressure", "db/dbb/md_docs_2api_2livemsg.html#autotoc_md156", null ],
      [ "External signaling and embedding", "db/dbb/md_docs_2api_2livemsg.html#autotoc_md157", null ],
      [ "Validation", "db/dbb/md_docs_2api_2livemsg.html#autotoc_md158", null ]
    ] ],
    [ "Middleware API", "da/dbc/md_docs_2api_2middleware.html", [
      [ "Functions", "da/dbc/md_docs_2api_2middleware.html#autotoc_md160", [
        [ "cwist_app_use", "da/dbc/md_docs_2api_2middleware.html#autotoc_md161", null ]
      ] ],
      [ "Built-in Middlewares", "da/dbc/md_docs_2api_2middleware.html#autotoc_md162", [
        [ "Request ID Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md163", null ],
        [ "Access Log Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md164", [
          [ "Async log output", "da/dbc/md_docs_2api_2middleware.html#autotoc_md165", null ]
        ] ],
        [ "Rate Limiter Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md166", null ],
        [ "Redis-backed shared rate limiter", "da/dbc/md_docs_2api_2middleware.html#autotoc_md167", null ],
        [ "CORS Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md168", null ],
        [ "Prometheus Metrics Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md169", null ],
        [ "JWT Auth Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md170", null ],
        [ "Compression Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md171", null ]
      ] ],
      [ "Creating Custom Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md172", null ]
    ] ],
    [ "Query & URI API", "dc/d49/md_docs_2api_2query.html", null ],
    [ "Database API", "d5/d11/md_docs_2api_2sql.html", [
      [ "Schema Migrations", "d5/d11/md_docs_2api_2sql.html#autotoc_md182", [
        [ "cwist_db_open", "d5/d11/md_docs_2api_2sql.html#autotoc_md177", null ],
        [ "cwist_db_open_memory", "d5/d11/md_docs_2api_2sql.html#autotoc_md178", null ],
        [ "cwist_db_serialize", "d5/d11/md_docs_2api_2sql.html#autotoc_md179", null ],
        [ "cwist_db_exec", "d5/d11/md_docs_2api_2sql.html#autotoc_md180", null ],
        [ "cwist_db_query", "d5/d11/md_docs_2api_2sql.html#autotoc_md181", null ],
        [ "Return codes", "d5/d11/md_docs_2api_2sql.html#autotoc_md183", null ],
        [ "cwist_migration_t", "d5/d11/md_docs_2api_2sql.html#autotoc_md184", null ],
        [ "cwist_migrate_up", "d5/d11/md_docs_2api_2sql.html#autotoc_md185", null ],
        [ "cwist_migrate_down", "d5/d11/md_docs_2api_2sql.html#autotoc_md186", null ],
        [ "cwist_migrate_version", "d5/d11/md_docs_2api_2sql.html#autotoc_md187", null ]
      ] ]
    ] ],
    [ "Server-Sent Events API", "d0/d89/md_docs_2api_2sse.html", [
      [ "Types", "d0/d89/md_docs_2api_2sse.html#autotoc_md189", [
        [ "cwist_sse_event_t", "d0/d89/md_docs_2api_2sse.html#autotoc_md190", null ],
        [ "cwist_sse_stream_t", "d0/d89/md_docs_2api_2sse.html#autotoc_md191", null ]
      ] ],
      [ "Response mode", "d0/d89/md_docs_2api_2sse.html#autotoc_md192", [
        [ "cwist_sse_response_init", "d0/d89/md_docs_2api_2sse.html#autotoc_md193", null ],
        [ "cwist_sse_response_event", "d0/d89/md_docs_2api_2sse.html#autotoc_md194", null ],
        [ "cwist_sse_response_write", "d0/d89/md_docs_2api_2sse.html#autotoc_md195", null ],
        [ "cwist_sse_response_comment", "d0/d89/md_docs_2api_2sse.html#autotoc_md196", null ]
      ] ],
      [ "Stream mode", "d0/d89/md_docs_2api_2sse.html#autotoc_md197", [
        [ "cwist_sse_stream_open", "d0/d89/md_docs_2api_2sse.html#autotoc_md198", null ],
        [ "cwist_sse_stream_send", "d0/d89/md_docs_2api_2sse.html#autotoc_md199", null ],
        [ "cwist_sse_stream_write", "d0/d89/md_docs_2api_2sse.html#autotoc_md200", null ],
        [ "cwist_sse_stream_comment", "d0/d89/md_docs_2api_2sse.html#autotoc_md201", null ],
        [ "cwist_sse_stream_close", "d0/d89/md_docs_2api_2sse.html#autotoc_md202", null ]
      ] ],
      [ "Convenience macros", "d0/d89/md_docs_2api_2sse.html#autotoc_md203", null ],
      [ "Example", "d0/d89/md_docs_2api_2sse.html#autotoc_md204", null ]
    ] ],
    [ "SString API", "db/d14/md_docs_2api_2sstring.html", null ],
    [ "WASI targets", "d6/d26/md_docs_2api_2wasi.html", [
      [ "Flavour detection", "d6/d26/md_docs_2api_2wasi.html#autotoc_md215", null ],
      [ "WASI 0.2 socket server", "d6/d26/md_docs_2api_2wasi.html#autotoc_md216", null ],
      [ "WASI preview1 (retired)", "d6/d26/md_docs_2api_2wasi.html#autotoc_md217", [
        [ "What the preview1 build did differently", "d6/d26/md_docs_2api_2wasi.html#autotoc_md218", null ],
        [ "Verification (historical)", "d6/d26/md_docs_2api_2wasi.html#autotoc_md219", null ]
      ] ]
    ] ],
    [ "CWIST on the WASM component model", "d8/d77/md_docs_2api_2wasm-component.html", [
      [ "Why", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md221", null ],
      [ "The boundary", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md222", null ],
      [ "Toolchain status (measured 2026-09-21)", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md223", null ],
      [ "Evaluated: preview3-shim (2026-09-23)", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md224", null ],
      [ "Landed: native async for streaming/SSE (2026-09-23)", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md225", null ],
      [ "Resolved: socket request path under wasip3 (audited 2026-09-22)", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md226", null ],
      [ "Stages", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md227", null ],
      [ "Browser runtime gate (WASI 0.2)", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md228", null ],
      [ "Measured while building stage 2", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md229", null ],
      [ "Non-goals", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md230", null ]
    ] ],
    [ "CWIST on WASM (Legacy Emscripten)", "d5/d0e/md_docs_2api_2wasm.html", [
      [ "Building", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md232", null ],
      [ "What is in the WASM build", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md233", null ],
      [ "Request dispatch from JS", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md234", null ],
      [ "Zero-copy views: cwist/wasm/typedarray.h", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md235", null ],
      [ "Databases under WASM", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md236", null ],
      [ "Bundle size impact of cwist_db", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md237", null ],
      [ "Sessions and cookies", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md238", null ],
      [ "Streaming through the boundary", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md239", null ],
      [ "Runnable example: Service Worker app", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md240", null ],
      [ "Not covered (yet)", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md241", null ],
      [ "Streaming producer (handlers generate the body chunk-by-chunk)", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md242", null ]
    ] ],
    [ "WebSocket API", "db/dc5/md_docs_2api_2websocket.html", [
      [ "Functions", "db/dc5/md_docs_2api_2websocket.html#autotoc_md244", [
        [ "cwist_websocket_upgrade", "db/dc5/md_docs_2api_2websocket.html#autotoc_md245", null ],
        [ "cwist_websocket_upgrade_ex", "db/dc5/md_docs_2api_2websocket.html#autotoc_md246", null ],
        [ "cwist_websocket_receive", "db/dc5/md_docs_2api_2websocket.html#autotoc_md247", null ],
        [ "cwist_websocket_send", "db/dc5/md_docs_2api_2websocket.html#autotoc_md248", null ]
      ] ],
      [ "Opcode Types", "db/dc5/md_docs_2api_2websocket.html#autotoc_md249", null ],
      [ "Integration with cwist_app", "db/dc5/md_docs_2api_2websocket.html#autotoc_md250", null ],
      [ "Async (C1M) WebSocket API", "db/dc5/md_docs_2api_2websocket.html#autotoc_md251", [
        [ "cwist_app_ws_async", "db/dc5/md_docs_2api_2websocket.html#autotoc_md252", null ],
        [ "cwist_ws_on_message_t", "db/dc5/md_docs_2api_2websocket.html#autotoc_md253", null ],
        [ "cwist_websocket_async_send", "db/dc5/md_docs_2api_2websocket.html#autotoc_md254", null ],
        [ "cwist_websocket_async_close", "db/dc5/md_docs_2api_2websocket.html#autotoc_md255", null ]
      ] ],
      [ "GraphQL subscriptions (graphql-ws)", "db/dc5/md_docs_2api_2websocket.html#autotoc_md256", null ]
    ] ],
    [ "Deferred HTTP ownership under full GC", "df/dc5/md_docs_2async-gc-ownership.html", [
      [ "Handoff sequence", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md258", null ],
      [ "Ownership inventory", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md259", [
        [ "Borrowed and external state", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md260", null ]
      ] ],
      [ "HTTP/2 queue ownership", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md261", null ],
      [ "Verification and limits", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md262", null ],
      [ "Scheduler and IO queue ownership", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md263", null ]
    ] ],
    [ "Compile-time build options", "d5/d9e/md_docs_2build-options.html", null ],
    [ "C1M file limits", "db/d4d/md_docs_2c1m-file-limits.html", [
      [ "Cause", "db/d4d/md_docs_2c1m-file-limits.html#autotoc_md266", null ],
      [ "Change", "db/d4d/md_docs_2c1m-file-limits.html#autotoc_md267", null ],
      [ "Tuning", "db/d4d/md_docs_2c1m-file-limits.html#autotoc_md268", null ],
      [ "Checks", "db/d4d/md_docs_2c1m-file-limits.html#autotoc_md269", null ]
    ] ],
    [ "Classic pool burst starvation (#25 follow-up)", "d9/d56/md_docs_2classic-pool-starvation.html", [
      [ "Cause and change", "d9/d56/md_docs_2classic-pool-starvation.html#autotoc_md271", null ],
      [ "Deterministic regression", "d9/d56/md_docs_2classic-pool-starvation.html#autotoc_md272", null ],
      [ "Validation status", "d9/d56/md_docs_2classic-pool-starvation.html#autotoc_md273", null ],
      [ "HTTP measurements and limits", "d9/d56/md_docs_2classic-pool-starvation.html#autotoc_md274", null ]
    ] ],
    [ "Cooperative queuing in the C1M reactor's batch drain (issue #25)", "d1/ddd/md_docs_2cooperative-queuing.html", [
      [ "Background", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md276", null ],
      [ "The gap", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md277", null ],
      [ "The fix", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md278", null ],
      [ "Measured effect", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md279", null ],
      [ "What this does not fix", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md280", null ],
      [ "Tuning", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md281", null ]
    ] ],
    [ "ADR-0001: Restore HTTP response caching through an explicit public FIXED contract", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html", [
      [ "Context", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md283", null ],
      [ "Decision", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md284", [
        [ "1. One parsed eligibility boundary", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md285", null ],
        [ "2. Cache representations, not wire responses", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md286", null ],
        [ "3. Isolate ownership, provenance and capacity", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md287", null ],
        [ "4. Make the compatibility change explicit", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md288", null ]
      ] ],
      [ "Alternatives considered", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md289", null ],
      [ "Consequences", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md290", null ],
      [ "Implementation and release gates", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md291", null ],
      [ "Evidence and source anchors", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md292", null ]
    ] ],
    [ "ADR-0002: TLS certificate hot reload keeps the session-ticket key (STEK) unchanged", "d1/dd3/md_docs_2decisions_20002-tls-hot-reload-stek.html", [
      [ "Context", "d1/dd3/md_docs_2decisions_20002-tls-hot-reload-stek.html#autotoc_md294", null ],
      [ "Decision", "d1/dd3/md_docs_2decisions_20002-tls-hot-reload-stek.html#autotoc_md295", null ],
      [ "Consequences", "d1/dd3/md_docs_2decisions_20002-tls-hot-reload-stek.html#autotoc_md296", null ]
    ] ],
    [ "Prebuilt release assets", "d1/da3/md_docs_2deployment_2release-assets.html", [
      [ "Release branch policy", "d1/da3/md_docs_2deployment_2release-assets.html#autotoc_md299", null ],
      [ "Platform coverage in release notes", "d1/da3/md_docs_2deployment_2release-assets.html#autotoc_md300", null ],
      [ "Using a fat bundle", "d1/da3/md_docs_2deployment_2release-assets.html#autotoc_md301", null ],
      [ "Using the amalgamation", "d1/da3/md_docs_2deployment_2release-assets.html#autotoc_md302", null ],
      [ "Re-running", "d1/da3/md_docs_2deployment_2release-assets.html#autotoc_md303", null ]
    ] ],
    [ "Deploy the WASI 0.2 example with Wasmtime", "db/de3/md_docs_2deployment_2wasmtime-appliance.html", [
      [ "Scope and safety", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md305", null ],
      [ "1. Build a known revision", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md306", null ],
      [ "2. Separate the artifact from state", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md307", null ],
      [ "3. Start and verify", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md308", null ],
      [ "4. Restart, back up, and restore", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md309", null ],
      [ "Before adapting this for production", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md310", null ]
    ] ],
    [ "Zero-downtime restart via socket handoff", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html", [
      [ "Requirements", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html#autotoc_md312", null ],
      [ "Handoff sequence", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html#autotoc_md313", null ],
      [ "systemd integration", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html#autotoc_md314", null ],
      [ "UDP and HTTP/3", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html#autotoc_md315", null ],
      [ "Verifying a deploy", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html#autotoc_md316", null ]
    ] ],
    [ "Durable queue required gate", "d3/dd5/md_docs_2durable-queue-gate.html", [
      [ "Local runs", "d3/dd5/md_docs_2durable-queue-gate.html#autotoc_md318", null ],
      [ "Isolated integration gate", "d3/dd5/md_docs_2durable-queue-gate.html#autotoc_md319", null ],
      [ "CI", "d3/dd5/md_docs_2durable-queue-gate.html#autotoc_md320", null ]
    ] ],
    [ "Bare-metal STM32 with CWIST", "de/d87/md_docs_2embedded_2stm32.html", [
      [ "Supported targets and validation", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md322", null ],
      [ "Build the firmware inputs", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md323", null ],
      [ "Port hooks and runtime contract", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md324", null ],
      [ "HTTP over raw lwIP", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md325", null ],
      [ "Memory sizing", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md326", [
        [ "Request arenas", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md327", null ],
        [ "Connections and responses", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md328", null ],
        [ "lwIP pools", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md329", null ]
      ] ],
      [ "HTTPS", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md330", null ],
      [ "Limitations", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md331", null ]
    ] ],
    [ "FIXED endpoint caching: current behavior", "d3/ddd/md_docs_2fixed-cache-status.html", [
      [ "What the runtime actually does today", "d3/ddd/md_docs_2fixed-cache-status.html#autotoc_md333", null ],
      [ "Evidence", "d3/ddd/md_docs_2fixed-cache-status.html#autotoc_md334", null ],
      [ "Why the learning block is not simply moved", "d3/ddd/md_docs_2fixed-cache-status.html#autotoc_md335", null ],
      [ "Recommended handling", "d3/ddd/md_docs_2fixed-cache-status.html#autotoc_md336", null ]
    ] ],
    [ "Full GC: Automatic Resource Reclamation in CWIST", "d9/dd1/md_docs_2GC.html", [
      [ "Motivation", "d9/dd1/md_docs_2GC.html#autotoc_md338", null ],
      [ "Design", "d9/dd1/md_docs_2GC.html#autotoc_md339", [
        [ "1. cwist_full_gc(bool) — the global toggle", "d9/dd1/md_docs_2GC.html#autotoc_md340", null ],
        [ "2. Reclamation engine: libttak epoch GC", "d9/dd1/md_docs_2GC.html#autotoc_md341", null ],
        [ "3. Thread/process exit sweeps", "d9/dd1/md_docs_2GC.html#autotoc_md342", null ],
        [ "4. Pseudo-RAII for handle-like locals", "d9/dd1/md_docs_2GC.html#autotoc_md343", null ],
        [ "5. Transparent malloc interception", "d9/dd1/md_docs_2GC.html#autotoc_md344", null ],
        [ "6. cwist_alloc internals", "d9/dd1/md_docs_2GC.html#autotoc_md345", null ],
        [ "7. Explicit flushes and reclamation", "d9/dd1/md_docs_2GC.html#autotoc_md346", null ]
      ] ],
      [ "Semantics summary", "d9/dd1/md_docs_2GC.html#autotoc_md347", null ],
      [ "Non-goals and notes", "d9/dd1/md_docs_2GC.html#autotoc_md348", null ],
      [ "Known performance caveat", "d9/dd1/md_docs_2GC.html#autotoc_md349", null ]
    ] ],
    [ "HTTP/1.1 pipeline fairness and framing", "d2/dc2/md_docs_2http-reactor-fairness.html", [
      [ "Scope", "d2/dc2/md_docs_2http-reactor-fairness.html#autotoc_md351", null ],
      [ "Behavioral regression tests", "d2/dc2/md_docs_2http-reactor-fairness.html#autotoc_md352", null ],
      [ "Performance claim boundary", "d2/dc2/md_docs_2http-reactor-fairness.html#autotoc_md353", null ],
      [ "Internal API compatibility", "d2/dc2/md_docs_2http-reactor-fairness.html#autotoc_md354", null ]
    ] ],
    [ "HTTP/3 connection migration", "d8/d64/md_docs_2http3-migration.html", [
      [ "Result for issue #407", "d8/d64/md_docs_2http3-migration.html#autotoc_md356", null ],
      [ "Run the test", "d8/d64/md_docs_2http3-migration.html#autotoc_md357", null ],
      [ "Test coverage", "d8/d64/md_docs_2http3-migration.html#autotoc_md358", null ],
      [ "Limits", "d8/d64/md_docs_2http3-migration.html#autotoc_md359", null ]
    ] ],
    [ "CWIST API 레퍼런스", "d3/d38/md_docs_2ko_2api-quickref.html", [
      [ "에러 처리", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md361", [
        [ "make_error", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md362", null ],
        [ "cwist_error_is_ok / cwist_error_is_ok_extern", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md363", null ],
        [ "그 밖에 내보낸 inline 도우미", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md364", null ]
      ] ],
      [ "SString (cwist_sstring)", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md365", [
        [ "수명 주기", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md366", null ],
        [ "핵심 도우미", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md367", null ],
        [ "공백 제거", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md368", null ],
        [ "덧붙이기 / 복사", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md369", null ],
        [ "비교 / 조회", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md370", null ]
      ] ],
      [ "HTTP", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md371", [
        [ "요청 수명 주기", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md372", null ],
        [ "응답 수명 주기", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md373", null ],
        [ "헤더 도우미", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md374", null ],
        [ "메서드 도우미", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md375", null ],
        [ "소켓 도우미", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md376", null ]
      ] ],
      [ "세션 매니저", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md377", [
        [ "아레나", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md378", null ],
        [ "공유 세션 (침습적 참조 카운트)", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md379", null ],
        [ "매니저", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md380", null ]
      ] ]
    ] ],
    [ "쿠키 API", "db/d04/md_docs_2ko_2api_2cookie.html", [
      [ "쿠키 읽기", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md382", [
        [ "cwist_cookie_parse", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md383", null ],
        [ "cwist_cookie_get", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md384", null ]
      ] ],
      [ "쿠키 쓰기", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md385", [
        [ "cwist_cookie_options", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md386", null ],
        [ "cwist_cookie_set", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md387", null ],
        [ "cwist_cookie_delete", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md388", null ],
        [ "cwist_cookie_delete_ex", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md389", null ]
      ] ],
      [ "인코딩", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md390", [
        [ "cwist_cookie_encode", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md391", null ],
        [ "cwist_cookie_decode", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md392", null ]
      ] ],
      [ "예제", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md393", null ]
    ] ],
    [ "라이브 메시지 런타임(실험적)", "d2/daa/md_docs_2ko_2api_2livemsg.html", [
      [ "최소 서버", "d2/daa/md_docs_2ko_2api_2livemsg.html#autotoc_md395", null ],
      [ "설정", "d2/daa/md_docs_2ko_2api_2livemsg.html#autotoc_md396", null ],
      [ "메시지와 결과 소유권", "d2/daa/md_docs_2ko_2api_2livemsg.html#autotoc_md397", null ],
      [ "응답, 푸시, 백프레셔", "d2/daa/md_docs_2ko_2api_2livemsg.html#autotoc_md398", null ],
      [ "외부 시그널링과 임베딩", "d2/daa/md_docs_2ko_2api_2livemsg.html#autotoc_md399", null ],
      [ "검증", "d2/daa/md_docs_2ko_2api_2livemsg.html#autotoc_md400", null ]
    ] ],
    [ "WebSocket API", "d7/dec/md_docs_2ko_2api_2websocket.html", [
      [ "함수", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md402", [
        [ "cwist_websocket_upgrade", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md403", null ],
        [ "cwist_websocket_upgrade_ex", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md404", null ],
        [ "cwist_websocket_receive", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md405", null ],
        [ "cwist_websocket_send", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md406", null ]
      ] ],
      [ "Opcode 종류", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md407", null ],
      [ "cwist_app과의 연동", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md408", null ],
      [ "비동기 (C1M) WebSocket API", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md409", [
        [ "cwist_app_ws_async", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md410", null ],
        [ "cwist_ws_on_message_t", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md411", null ],
        [ "cwist_websocket_async_send", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md412", null ],
        [ "cwist_websocket_async_close", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md413", null ]
      ] ],
      [ "GraphQL 구독 (graphql-ws)", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md414", null ]
    ] ],
    [ "CWIST bare-metal STM32", "d4/dc9/md_docs_2ko_2embedded_2stm32.html", [
      [ "지원 대상과 검증 범위", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md416", null ],
      [ "펌웨어 입력 빌드", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md417", null ],
      [ "포트 훅과 런타임 계약", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md418", null ],
      [ "raw lwIP HTTP", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md419", null ],
      [ "메모리 크기", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md420", [
        [ "요청 arena", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md421", null ],
        [ "연결과 응답", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md422", null ],
        [ "lwIP pool", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md423", null ]
      ] ],
      [ "HTTPS", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md424", null ],
      [ "제한사항", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md425", null ]
    ] ],
    [ "CWIST 라이브러리 레퍼런스", "d3/d19/md_docs_2ko_2REFERENCE.html", [
      [ "모듈", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md427", [
        [ "1. SString (스마트 문자열)", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md428", null ],
        [ "2. HTTP 서버", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md429", null ],
        [ "3. HTTPS 지원", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md430", null ],
        [ "4. 쿼리 파싱", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md431", null ],
        [ "5. 데이터베이스 (SQL)", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md432", null ],
        [ "6. SipHash", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md433", null ],
        [ "7. 에러 처리", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md434", null ],
        [ "8. Big Dumb Reply (BDR)", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md435", null ]
      ] ],
      [ "LibTTAK 메모리 기능", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md436", null ],
      [ "예제: Othello Web", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md437", null ],
      [ "의존성", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md438", null ],
      [ "빌드", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md439", null ]
    ] ],
    [ "라틴방진 참고 문헌", "da/d05/md_docs_2ko_2references_2latin__square__references.html", [
      [ "1. 정의", "da/d05/md_docs_2ko_2references_2latin__square__references.html#autotoc_md441", null ],
      [ "2. 코드에 쓰인 구성법", "da/d05/md_docs_2ko_2references_2latin__square__references.html#autotoc_md442", [
        [ "2.1 유한체 (소수 거듭제곱)", "da/d05/md_docs_2ko_2references_2latin__square__references.html#autotoc_md443", null ],
        [ "2.2 홀수 소수 거듭제곱 p^e, e >= 2", "da/d05/md_docs_2ko_2references_2latin__square__references.html#autotoc_md444", null ],
        [ "2.3 합성수 차수: MacNeish 곱", "da/d05/md_docs_2ko_2references_2latin__square__references.html#autotoc_md445", null ],
        [ "2.4 코드가 만들지 않는 것", "da/d05/md_docs_2ko_2references_2latin__square__references.html#autotoc_md446", null ]
      ] ],
      [ "3. 역사적 표: Gusuryak 9차 쌍", "da/d05/md_docs_2ko_2references_2latin__square__references.html#autotoc_md447", null ],
      [ "4. CWIST에서의 사용: 테스트에서 런타임 knob 쌍 덮기", "da/d05/md_docs_2ko_2references_2latin__square__references.html#autotoc_md448", null ]
    ] ],
    [ "런타임 설정", "d5/d08/md_docs_2ko_2runtime-config.html", [
      [ "상태", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md450", null ],
      [ "프로세스와 서버 모드", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md451", null ],
      [ "Classic 스레드 풀", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md452", null ],
      [ "HTTP/1.1", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md453", null ],
      [ "HTTPS", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md454", null ],
      [ "HTTP/2", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md455", null ],
      [ "HTTP/3", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md456", null ],
      [ "WebSocket", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md457", null ],
      [ "Reactor", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md458", null ],
      [ "Redis", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md459", null ],
      [ "진단", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md460", null ]
    ] ],
    [ "CWIST로 WebTransport 서버 만들기", "df/d10/md_docs_2ko_2tutorials_2webtransport-server.html", [
      [ "빌드", "df/d10/md_docs_2ko_2tutorials_2webtransport-server.html#autotoc_md462", null ],
      [ "1. HTTP/3과 세션 핸들러 설정하기", "df/d10/md_docs_2ko_2tutorials_2webtransport-server.html#autotoc_md463", null ],
      [ "2. 세션 프로토콜 설계하기", "df/d10/md_docs_2ko_2tutorials_2webtransport-server.html#autotoc_md464", [
        [ "WebTransport와 raw 데이터그램 함께 쓰기", "df/d10/md_docs_2ko_2tutorials_2webtransport-server.html#autotoc_md465", null ]
      ] ],
      [ "3. 안전하게 운영하기", "df/d10/md_docs_2ko_2tutorials_2webtransport-server.html#autotoc_md466", null ]
    ] ],
    [ "Release Codename Sequencing Specification", "d6/de8/md_docs_2ko_2versioning.html", [
      [ "1. 기본 원칙", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md468", null ],
      [ "2. 2자리 시퀀스 (기본)", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md469", null ],
      [ "3. 3자리 확장 시퀀스", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md470", null ],
      [ "4. 표기", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md471", null ],
      [ "5. API 안정성 게이트 (v4 LTS부터)", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md472", null ],
      [ "6. 메이저 버전의 첫 상승", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md473", null ]
    ] ],
    [ "HTTPS C100k gate", "d0/d49/md_docs_2performance_2https-c100k.html", [
      [ "What runs", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md475", null ],
      [ "Loopback port space", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md476", null ],
      [ "Results (2026-10-17, dev)", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md477", null ],
      [ "Results (2026-10-18, dev): C1M over TLS", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md478", null ],
      [ "Results (2026-10-11, dev)", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md479", [
        [ "Where the memory goes", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md480", null ]
      ] ],
      [ "Scaling past the gate on a single host", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md481", null ],
      [ "Running it", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md482", null ]
    ] ],
    [ "HTTPS pool task queue: static ring vs growable ring", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html", [
      [ "What changed", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md484", null ],
      [ "Setup", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md485", null ],
      [ "1. RSS of https_pool_init() alone", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md486", null ],
      [ "2. 4 workers, steady and churn load (3 alternating rounds)", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md487", null ],
      [ "3. Forcing the ring to grow: 1 worker, 2 pool threads, 6000 simultaneous connections", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md488", null ],
      [ "4. Order and loss under growth: tests/test_https_pool_queue.c", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md489", null ],
      [ "Not done / open", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md490", null ]
    ] ],
    [ "Issue #297 syscall-reduction audit (local, 12-core Ryzen 5600X)", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html", [
      [ "Baseline syscall profile", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md492", null ],
      [ "Item results", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md493", [
        [ "1.1 io_uring multishot accept — NEGATIVE (reverted)", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md494", null ],
        [ "1.2 Larger SQE batches / fewer io_uring_enter — ALREADY IMPLEMENTED / BLOCKED", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md495", null ],
        [ "1.3 Per-accept setsockopt removal — NEGATIVE (reverted)", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md496", null ],
        [ "1.4 Time caching — ALREADY IMPLEMENTED", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md497", null ]
      ] ],
      [ "What the profile says to do instead (ranked)", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md498", null ],
      [ "Method notes", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md499", null ]
    ] ],
    [ "Pluggable TLS backend (CWIST_TLS)", "d9/dda/md_docs_2performance_2tls-backends.html", [
      [ "The seam", "d9/dda/md_docs_2performance_2tls-backends.html#autotoc_md501", null ],
      [ "Forced-off features under CWIST_TLS=mbedtls", "d9/dda/md_docs_2performance_2tls-backends.html#autotoc_md502", null ],
      [ "Degraded features under CWIST_TLS=mbedtls", "d9/dda/md_docs_2performance_2tls-backends.html#autotoc_md503", null ],
      [ "Embedded bundle", "d9/dda/md_docs_2performance_2tls-backends.html#autotoc_md504", null ]
    ] ],
    [ "Opt-in wrk raw/corrected latency histograms", "d1/dfd/md_docs_2performance_2wrk-dual-histogram.html", [
      [ "Build in a disposable wrk checkout", "d1/dfd/md_docs_2performance_2wrk-dual-histogram.html#autotoc_md506", null ],
      [ "Output and interpretation", "d1/dfd/md_docs_2performance_2wrk-dual-histogram.html#autotoc_md507", null ],
      [ "Verification", "d1/dfd/md_docs_2performance_2wrk-dual-histogram.html#autotoc_md508", null ]
    ] ],
    [ "io_uring posted-work wake-up regression (#25)", "dd/d56/md_docs_2reactor-wakeup-regression.html", [
      [ "Scope and cause", "dd/d56/md_docs_2reactor-wakeup-regression.html#autotoc_md510", null ],
      [ "Regression test", "dd/d56/md_docs_2reactor-wakeup-regression.html#autotoc_md511", null ],
      [ "Component measurement (2026-09-12)", "dd/d56/md_docs_2reactor-wakeup-regression.html#autotoc_md512", null ]
    ] ],
    [ "CWIST Library Reference", "da/dca/md_docs_2REFERENCE.html", [
      [ "Modules", "da/dca/md_docs_2REFERENCE.html#autotoc_md521", [
        [ "1. SString (Smart String)", "da/dca/md_docs_2REFERENCE.html#autotoc_md522", null ],
        [ "2. HTTP Server", "da/dca/md_docs_2REFERENCE.html#autotoc_md523", null ],
        [ "3. HTTPS Support", "da/dca/md_docs_2REFERENCE.html#autotoc_md524", null ],
        [ "4. Query Parsing", "da/dca/md_docs_2REFERENCE.html#autotoc_md525", null ],
        [ "5. Database (SQL)", "da/dca/md_docs_2REFERENCE.html#autotoc_md526", null ],
        [ "6. SipHash", "da/dca/md_docs_2REFERENCE.html#autotoc_md527", null ],
        [ "7. Error Handling", "da/dca/md_docs_2REFERENCE.html#autotoc_md528", null ],
        [ "8. Big Dumb Reply (BDR)", "da/dca/md_docs_2REFERENCE.html#autotoc_md529", null ]
      ] ],
      [ "LibTTAK Memory Features", "da/dca/md_docs_2REFERENCE.html#autotoc_md530", null ],
      [ "Example: Othello Web", "da/dca/md_docs_2REFERENCE.html#autotoc_md531", null ],
      [ "Dependencies", "da/dca/md_docs_2REFERENCE.html#autotoc_md532", null ],
      [ "Build", "da/dca/md_docs_2REFERENCE.html#autotoc_md533", null ]
    ] ],
    [ "Latin Square References", "df/d22/md_docs_2references_2latin__square__references.html", [
      [ "1. Definitions", "df/d22/md_docs_2references_2latin__square__references.html#autotoc_md535", null ],
      [ "2. Constructions used in the code", "df/d22/md_docs_2references_2latin__square__references.html#autotoc_md536", [
        [ "2.1 Finite fields (prime powers)", "df/d22/md_docs_2references_2latin__square__references.html#autotoc_md537", null ],
        [ "2.2 Odd prime powers p^e, e >= 2", "df/d22/md_docs_2references_2latin__square__references.html#autotoc_md538", null ],
        [ "2.3 Composite orders: MacNeish's product", "df/d22/md_docs_2references_2latin__square__references.html#autotoc_md539", null ],
        [ "2.4 What the code does not construct", "df/d22/md_docs_2references_2latin__square__references.html#autotoc_md540", null ]
      ] ],
      [ "3. Historical table: the Gusuryak order-9 pair", "df/d22/md_docs_2references_2latin__square__references.html#autotoc_md541", null ],
      [ "4. Use in CWIST: covering runtime knob pairs in tests", "df/d22/md_docs_2references_2latin__square__references.html#autotoc_md542", null ]
    ] ],
    [ "Mux Algorithm References", "d6/d9f/md_docs_2references_2mux__algorithm__references.html", [
      [ "1. Lo Shu Magic Square (Naak-seo / 洛書)", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md544", null ],
      [ "2. Orthogonal Latin-Square Permutation", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md545", null ],
      [ "3. Coefficient Matrix & Linear System Solver", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md546", null ],
      [ "4. Iterative Magnitude Refinement", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md547", null ],
      [ "5. Single-Step Linear Extrapolation", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md548", null ],
      [ "Naming Rationale", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md549", null ]
    ] ],
    [ "v3.5.0 release roadmap", "d7/df9/md_docs_2roadmap-v3_85.html", [
      [ "Scope", "d7/df9/md_docs_2roadmap-v3_85.html#autotoc_md551", null ],
      [ "Release gates", "d7/df9/md_docs_2roadmap-v3_85.html#autotoc_md552", null ],
      [ "Evidence and limits", "d7/df9/md_docs_2roadmap-v3_85.html#autotoc_md553", null ]
    ] ],
    [ "Runtime configuration", "d9/dfa/md_docs_2runtime-config.html", [
      [ "Status", "d9/dfa/md_docs_2runtime-config.html#autotoc_md555", null ],
      [ "Processes and server mode", "d9/dfa/md_docs_2runtime-config.html#autotoc_md556", null ],
      [ "Classic thread pool", "d9/dfa/md_docs_2runtime-config.html#autotoc_md557", null ],
      [ "HTTP/1.1", "d9/dfa/md_docs_2runtime-config.html#autotoc_md558", null ],
      [ "HTTPS", "d9/dfa/md_docs_2runtime-config.html#autotoc_md559", null ],
      [ "HTTP/2", "d9/dfa/md_docs_2runtime-config.html#autotoc_md560", null ],
      [ "HTTP/3", "d9/dfa/md_docs_2runtime-config.html#autotoc_md561", null ],
      [ "WebSocket", "d9/dfa/md_docs_2runtime-config.html#autotoc_md562", null ],
      [ "Reactor", "d9/dfa/md_docs_2runtime-config.html#autotoc_md563", null ],
      [ "Redis", "d9/dfa/md_docs_2runtime-config.html#autotoc_md564", null ],
      [ "Diagnostics", "d9/dfa/md_docs_2runtime-config.html#autotoc_md565", null ]
    ] ],
    [ "Soak testing", "db/d05/md_docs_2soak-testing.html", [
      [ "Environment", "db/d05/md_docs_2soak-testing.html#autotoc_md567", null ],
      [ "Bench server and workload mix", "db/d05/md_docs_2soak-testing.html#autotoc_md568", null ],
      [ "Measurement points", "db/d05/md_docs_2soak-testing.html#autotoc_md569", null ],
      [ "PASS/FAIL criteria", "db/d05/md_docs_2soak-testing.html#autotoc_md570", null ],
      [ "Interpreting a failure", "db/d05/md_docs_2soak-testing.html#autotoc_md571", null ]
    ] ],
    [ "CWIST Web Development Tutorial (A to Z)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html", [
      [ "Table of Contents", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md573", null ],
      [ "1. Hello World: Starting Your First Server", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md574", null ],
      [ "2. Routing and Handlers (Mux Router)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md575", null ],
      [ "3. JSON Parsing and Zod Schema Validation", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md576", null ],
      [ "4. Middleware (CORS, Logging)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md577", null ],
      [ "5. Databases and Migrations (SQLite)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md578", null ],
      [ "6. Authentication (JWT and DB Encryption)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md579", null ],
      [ "7. PQC TLS (Post-Quantum Hybrid Key Exchange)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md580", [
        [ "Security Policy Summary", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md581", null ]
      ] ],
      [ "8. WebSocket Integration (Real-Time Bidirectional Communication)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md582", null ],
      [ "9. Template Engine and Static File Serving", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md583", null ],
      [ "10. Dynamic CSS Composer (WASM and SSR)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md584", [
        [ "Approach A: 100% Server-Side Rendering (SSR)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md585", null ],
        [ "Approach B: Client-Side Composition with React + WebAssembly (WASM)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md586", null ],
        [ "Wrapping Up", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md587", null ]
      ] ]
    ] ],
    [ "CWIST 웹 개발 튜토리얼 (A to Z)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html", [
      [ "목차", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md589", null ],
      [ "1. Hello World: 첫 서버 띄우기", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md590", null ],
      [ "2. 라우팅과 핸들러 (Mux Router)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md591", null ],
      [ "3. JSON 파싱과 Zod 스키마 검증", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md592", null ],
      [ "4. 미들웨어 (CORS, Logging)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md593", null ],
      [ "5. 데이터베이스와 마이그레이션 (SQLite)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md594", null ],
      [ "6. 인증 (JWT 및 DB 암호화)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md595", null ],
      [ "7. PQC TLS (양자내성 하이브리드 키 교환)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md596", [
        [ "보안 정책 요약", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md597", null ]
      ] ],
      [ "8. 웹소켓 연동 (실시간 양방향 통신)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md598", null ],
      [ "9. 템플릿 엔진과 정적 파일 제공", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md599", null ],
      [ "10. 동적 CSS 합성기 (WASM 및 SSR)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md600", [
        [ "방법 A: 100% Server-Side Rendering (SSR)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md601", null ],
        [ "방법 B: React + WebAssembly (WASM) 클라이언트 사이드 합성", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md602", null ],
        [ "마치며", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md603", null ]
      ] ]
    ] ],
    [ "Build a CRUD blog with CWIST", "d4/db0/md_docs_2tutorials_2blog-crud.html", [
      [ "1. Create the application", "d4/db0/md_docs_2tutorials_2blog-crud.html#autotoc_md605", null ],
      [ "2. Define the routes", "d4/db0/md_docs_2tutorials_2blog-crud.html#autotoc_md606", null ],
      [ "3. Validate JSON and return HTTP semantics", "d4/db0/md_docs_2tutorials_2blog-crud.html#autotoc_md607", null ],
      [ "4. Test the API", "d4/db0/md_docs_2tutorials_2blog-crud.html#autotoc_md608", null ]
    ] ],
    [ "Integrate NATS with CWIST", "da/d48/md_docs_2tutorials_2nats-integration.html", [
      [ "1. Separate request and worker responsibilities", "da/d48/md_docs_2tutorials_2nats-integration.html#autotoc_md610", null ],
      [ "2. Publish from a route", "da/d48/md_docs_2tutorials_2nats-integration.html#autotoc_md611", null ],
      [ "3. Reliability checklist", "da/d48/md_docs_2tutorials_2nats-integration.html#autotoc_md612", null ]
    ] ],
    [ "Document an API with OpenAPI and Swagger UI", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html", [
      [ "1. Annotate the routes", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html#autotoc_md614", [
        [ "Annotation tags", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html#autotoc_md615", null ]
      ] ],
      [ "2. Generate openapi.json", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html#autotoc_md616", null ],
      [ "3. Serve the UI", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html#autotoc_md617", null ]
    ] ],
    [ "Build a WebTransport server with CWIST", "d2/dcb/md_docs_2tutorials_2webtransport-server.html", [
      [ "Build", "d2/dcb/md_docs_2tutorials_2webtransport-server.html#autotoc_md619", null ],
      [ "1. Configure HTTP/3 and a session handler", "d2/dcb/md_docs_2tutorials_2webtransport-server.html#autotoc_md620", null ],
      [ "2. Design the session protocol", "d2/dcb/md_docs_2tutorials_2webtransport-server.html#autotoc_md621", [
        [ "Raw datagrams next to WebTransport", "d2/dcb/md_docs_2tutorials_2webtransport-server.html#autotoc_md622", null ]
      ] ],
      [ "3. Operate safely", "d2/dcb/md_docs_2tutorials_2webtransport-server.html#autotoc_md623", null ]
    ] ],
    [ "Release Codename Sequencing Specification", "d0/db4/md_docs_2versioning.html", [
      [ "1. Principles", "d0/db4/md_docs_2versioning.html#autotoc_md625", null ],
      [ "2. Two-letter sequence (default)", "d0/db4/md_docs_2versioning.html#autotoc_md626", null ],
      [ "3. Three-letter extension", "d0/db4/md_docs_2versioning.html#autotoc_md627", null ],
      [ "4. Notation", "d0/db4/md_docs_2versioning.html#autotoc_md628", null ],
      [ "5. API stability gate (from the v4 LTS)", "d0/db4/md_docs_2versioning.html#autotoc_md629", null ],
      [ "6. First ascent of a major version", "d0/db4/md_docs_2versioning.html#autotoc_md630", null ]
    ] ],
    [ "Web Server Benchmark Methodology", "d6/d42/md_docs_2webserver-benchmark.html", [
      [ "Measurement contract v2", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md632", null ],
      [ "Admission and latency semantics", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md633", null ],
      [ "Resource measurements", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md634", [
        [ "Interpreting the context-switch column", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md635", null ]
      ] ],
      [ "Runtime configuration", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md636", null ],
      [ "Provenance, presentation and legacy results", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md637", null ]
    ] ],
    [ "Deprecated List", "da/d58/deprecated.html", null ],
    [ "Classes", "annotated.html", [
      [ "Class List", "annotated.html", "annotated_dup" ],
      [ "Class Index", "classes.html", null ],
      [ "Class Members", "functions.html", [
        [ "All", "functions.html", "functions_dup" ],
        [ "Functions", "functions_func.html", null ],
        [ "Variables", "functions_vars.html", "functions_vars" ]
      ] ]
    ] ],
    [ "Files", "files.html", [
      [ "File List", "files.html", "files_dup" ],
      [ "File Members", "globals.html", [
        [ "All", "globals.html", "globals_dup" ],
        [ "Functions", "globals_func.html", "globals_func" ],
        [ "Variables", "globals_vars.html", null ],
        [ "Typedefs", "globals_type.html", "globals_type" ],
        [ "Enumerations", "globals_enum.html", null ],
        [ "Enumerator", "globals_eval.html", "globals_eval" ],
        [ "Macros", "globals_defs.html", "globals_defs" ]
      ] ]
    ] ]
  ] ]
];

var NAVTREEINDEX =
[
"annotated.html",
"d0/d82/structcwist__test__client__cookie.html#acd0fa778aac9138cb4a17936a152bd37",
"d1/d43/builder_8c_source.html",
"d1/dfd/md_docs_2performance_2wrk-dual-histogram.html#autotoc_md507",
"d2/dc2/net_2graphql_2graphql_8h.html#a693bf250b425aac6b6bd211e30a443b9",
"d3/d73/err_2cwist__err_8h.html#ad0d5cab227e2aac13618fbc09c6dadf7",
"d4/d54/grpc__client_8c.html#a56e84851b712aa03ed380e75d24be700",
"d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md455",
"d5/dcf/orm__socket_8c.html#ac337901f5606f0cf6f8e9867ef3fc1c4",
"d6/d9b/orm_8h.html#a8a140e98cb138fd4575440dc18836a9aa6d56bfe9db7fa0c89c67a924946a4ffd",
"d7/d67/net_2http_2https_8h.html#a9a6bef0d3254918fd290cb787150d6d1",
"d8/d16/gc_8c.html#ab6b5184c0bbd3cc7003dbcfdd2f76f98",
"d9/d90/structcwist__bdr__t.html#a6c224571b874ac111326d99deb0ca2c9",
"d9/dab/net_2http_2http_8h.html#afe92033adc5b6922f589f9c5125b2666",
"da/d46/session_8h.html#aa2370fb1ec237c20caa71fcc0b0c7dd0",
"da/df2/livemsg_8c.html#af3f9ceeebe1486dc6a5b7f2e32f00fb4",
"db/d8a/structcwist__http__async__conn.html#ad03866eae1282297ac969cb62a4b5bcf",
"dc/d88/structcwist__webrtc__ctx.html#a2299184085c45b6696e96211409be360",
"dd/d2e/kqueue_8c.html#a63e26ec989f4bd26fb81e81fd85c5fbe",
"dd/d56/http_8h.html#abb4d7c3cd5624faea1646f36b835ec69",
"de/d25/structcwist__bind__rule.html",
"df/d10/md_docs_2ko_2tutorials_2webtransport-server.html#autotoc_md464",
"df/d6d/ssl_8h.html#adddb1e3621bcda5944944166aa4b04bb",
"df/dff/log__async_8c.html#a13c44db93f8c16244d93695832086aa4",
"index.html#autotoc_md18"
];

var SYNCONMSG = 'click to disable panel synchronisation';
var SYNCOFFMSG = 'click to enable panel synchronisation';