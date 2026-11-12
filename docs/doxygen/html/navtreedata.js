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
      [ "Performance, including where it loses", "index.html#autotoc_md7", [
        [ "Two server modes", "index.html#autotoc_md8", null ],
        [ "Holding a million connections", "index.html#autotoc_md9", null ]
      ] ],
      [ "Why C, when Axum and Gin exist?", "index.html#autotoc_md10", null ],
      [ "Memory safety and security in C", "index.html#autotoc_md11", null ],
      [ "Install", "index.html#autotoc_md12", [
        [ "Homebrew", "index.html#autotoc_md13", null ]
      ] ],
      [ "Platform support", "index.html#autotoc_md14", null ],
      [ "Bare-metal STM32", "index.html#autotoc_md15", null ],
      [ "The cwist CLI and hot reload", "index.html#autotoc_md16", null ],
      [ "Linking", "index.html#autotoc_md17", null ],
      [ "Configuration", "index.html#autotoc_md18", null ],
      [ "Feature notes", "index.html#autotoc_md19", null ],
      [ "Latest benchmark run", "index.html#autotoc_md20", null ],
      [ "Stability and conformance", "index.html#autotoc_md21", null ],
      [ "Documentation", "index.html#autotoc_md22", null ],
      [ "Examples", "index.html#autotoc_md23", null ],
      [ "Community", "index.html#autotoc_md24", null ],
      [ "Third-party licenses", "index.html#autotoc_md25", null ],
      [ "Error Handling", "d1/d7e/md_docs_2api-quickref.html#autotoc_md27", [
        [ "make_error", "d1/d7e/md_docs_2api-quickref.html#autotoc_md28", null ],
        [ "cwist_error_is_ok / cwist_error_is_ok_extern", "d1/d7e/md_docs_2api-quickref.html#autotoc_md29", null ],
        [ "Other exported inline helpers", "d1/d7e/md_docs_2api-quickref.html#autotoc_md30", null ]
      ] ],
      [ "SString (cwist_sstring)", "d1/d7e/md_docs_2api-quickref.html#autotoc_md31", [
        [ "Lifecycle", "d1/d7e/md_docs_2api-quickref.html#autotoc_md32", null ],
        [ "Core helpers", "d1/d7e/md_docs_2api-quickref.html#autotoc_md33", null ],
        [ "Trimming", "d1/d7e/md_docs_2api-quickref.html#autotoc_md34", null ],
        [ "Append / Copy", "d1/d7e/md_docs_2api-quickref.html#autotoc_md35", null ],
        [ "Compare / Query", "d1/d7e/md_docs_2api-quickref.html#autotoc_md36", null ]
      ] ],
      [ "HTTP", "d1/d7e/md_docs_2api-quickref.html#autotoc_md37", [
        [ "Request lifecycle", "d1/d7e/md_docs_2api-quickref.html#autotoc_md38", null ],
        [ "Response lifecycle", "d1/d7e/md_docs_2api-quickref.html#autotoc_md39", null ],
        [ "Header helpers", "d1/d7e/md_docs_2api-quickref.html#autotoc_md40", null ],
        [ "Method helpers", "d1/d7e/md_docs_2api-quickref.html#autotoc_md41", null ],
        [ "Socket helpers", "d1/d7e/md_docs_2api-quickref.html#autotoc_md42", null ]
      ] ],
      [ "Session manager", "d1/d7e/md_docs_2api-quickref.html#autotoc_md43", [
        [ "Arena", "d1/d7e/md_docs_2api-quickref.html#autotoc_md44", null ],
        [ "Shared sessions (intrusive ref count)", "d1/d7e/md_docs_2api-quickref.html#autotoc_md45", null ],
        [ "Manager", "d1/d7e/md_docs_2api-quickref.html#autotoc_md46", null ]
      ] ]
    ] ],
    [ "CWIST API Documentation", "d7/d15/md_docs_2API.html", [
      [ "Modules", "d7/d15/md_docs_2API.html#autotoc_md48", null ],
      [ "See also", "d7/d15/md_docs_2API.html#autotoc_md49", null ]
    ] ],
    [ "Framework & App API", "de/d19/md_docs_2api_2app.html", [
      [ "Routing", "de/d19/md_docs_2api_2app.html#autotoc_md56", [
        [ "cwist_app_create", "de/d19/md_docs_2api_2app.html#autotoc_md51", null ],
        [ "cwist_app_use_https", "de/d19/md_docs_2api_2app.html#autotoc_md52", null ],
        [ "cwist_app_use_https2 / cwist_use_https2", "de/d19/md_docs_2api_2app.html#autotoc_md53", null ],
        [ "cwist_app_use_pqc_layer", "de/d19/md_docs_2api_2app.html#autotoc_md54", null ],
        [ "cwist_app_use_db", "de/d19/md_docs_2api_2app.html#autotoc_md55", null ],
        [ "cwist_app_get / cwist_app_post", "de/d19/md_docs_2api_2app.html#autotoc_md57", null ],
        [ "cwist_app_get_ex / _post_ex / _put_ex / _delete_ex / _patch_ex", "de/d19/md_docs_2api_2app.html#autotoc_md58", null ],
        [ "cwist_app_dispatch_memory", "de/d19/md_docs_2api_2app.html#autotoc_md59", null ],
        [ "cwist_app_ws", "de/d19/md_docs_2api_2app.html#autotoc_md60", null ],
        [ "cwist_endpoint_opt_t", "de/d19/md_docs_2api_2app.html#autotoc_md61", null ]
      ] ],
      [ "Static Assets", "de/d19/md_docs_2api_2app.html#autotoc_md62", [
        [ "cwist_app_static", "de/d19/md_docs_2api_2app.html#autotoc_md63", null ]
      ] ],
      [ "Error Handling", "de/d19/md_docs_2api_2app.html#autotoc_md64", [
        [ "cwist_app_set_error_handler", "de/d19/md_docs_2api_2app.html#autotoc_md65", null ],
        [ "Unused Variables", "de/d19/md_docs_2api_2app.html#autotoc_md66", null ]
      ] ],
      [ "Concurrency & Thread-Safety", "de/d19/md_docs_2api_2app.html#autotoc_md67", null ],
      [ "Memory Ownership Rules", "de/d19/md_docs_2api_2app.html#autotoc_md68", [
        [ "cwist_app_listen", "de/d19/md_docs_2api_2app.html#autotoc_md69", null ]
      ] ],
      [ "In-Memory Test Client", "de/d19/md_docs_2api_2app.html#autotoc_md70", [
        [ "cwist_test_client_create / cwist_test_client_destroy", "de/d19/md_docs_2api_2app.html#autotoc_md71", null ],
        [ "Request helpers", "de/d19/md_docs_2api_2app.html#autotoc_md72", null ],
        [ "Cookie jar", "de/d19/md_docs_2api_2app.html#autotoc_md73", null ],
        [ "Test assertion macros", "de/d19/md_docs_2api_2app.html#autotoc_md74", null ]
      ] ]
    ] ],
    [ "Cookie API", "dc/d80/md_docs_2api_2cookie.html", [
      [ "Reading cookies", "dc/d80/md_docs_2api_2cookie.html#autotoc_md76", [
        [ "cwist_cookie_parse", "dc/d80/md_docs_2api_2cookie.html#autotoc_md77", null ],
        [ "cwist_cookie_get", "dc/d80/md_docs_2api_2cookie.html#autotoc_md78", null ]
      ] ],
      [ "Writing cookies", "dc/d80/md_docs_2api_2cookie.html#autotoc_md79", [
        [ "cwist_cookie_options", "dc/d80/md_docs_2api_2cookie.html#autotoc_md80", null ],
        [ "cwist_cookie_set", "dc/d80/md_docs_2api_2cookie.html#autotoc_md81", null ],
        [ "cwist_cookie_delete", "dc/d80/md_docs_2api_2cookie.html#autotoc_md82", null ]
      ] ],
      [ "Encoding", "dc/d80/md_docs_2api_2cookie.html#autotoc_md83", [
        [ "cwist_cookie_encode", "dc/d80/md_docs_2api_2cookie.html#autotoc_md84", null ],
        [ "cwist_cookie_decode", "dc/d80/md_docs_2api_2cookie.html#autotoc_md85", null ]
      ] ],
      [ "Example", "dc/d80/md_docs_2api_2cookie.html#autotoc_md86", null ]
    ] ],
    [ "HTML Components, Scoped CSS, Fragments and Assets", "d8/d83/md_docs_2api_2html.html", [
      [ "Components", "d8/d83/md_docs_2api_2html.html#autotoc_md88", [
        [ "cwist_html_component_create / cwist_html_component_destroy", "d8/d83/md_docs_2api_2html.html#autotoc_md89", null ],
        [ "cwist_html_component_instantiate", "d8/d83/md_docs_2api_2html.html#autotoc_md90", null ]
      ] ],
      [ "Scoped CSS", "d8/d83/md_docs_2api_2html.html#autotoc_md91", [
        [ "cwist_css_scope_init / cwist_css_scope_destroy", "d8/d83/md_docs_2api_2html.html#autotoc_md92", null ],
        [ "cwist_css_scope_class", "d8/d83/md_docs_2api_2html.html#autotoc_md93", null ],
        [ "cwist_css_scope_add_rule", "d8/d83/md_docs_2api_2html.html#autotoc_md94", null ],
        [ "cwist_css_scope_generate_stylesheet", "d8/d83/md_docs_2api_2html.html#autotoc_md95", null ]
      ] ],
      [ "HTML over the wire", "d8/d83/md_docs_2api_2html.html#autotoc_md96", [
        [ "cwist_http_request_wants_fragment / cwist_http_request_fragment_target", "d8/d83/md_docs_2api_2html.html#autotoc_md97", null ],
        [ "cwist_http_response_set_view", "d8/d83/md_docs_2api_2html.html#autotoc_md98", null ],
        [ "cwist_http_response_set_html", "d8/d83/md_docs_2api_2html.html#autotoc_md99", null ],
        [ "cwist_http_response_add_oob", "d8/d83/md_docs_2api_2html.html#autotoc_md100", null ],
        [ "cwist_http_response_html_redirect", "d8/d83/md_docs_2api_2html.html#autotoc_md101", null ],
        [ "Handler example", "d8/d83/md_docs_2api_2html.html#autotoc_md102", null ]
      ] ],
      [ "Asset pipeline", "d8/d83/md_docs_2api_2html.html#autotoc_md103", [
        [ "cwist_css_minify / cwist_css_bundle", "d8/d83/md_docs_2api_2html.html#autotoc_md104", null ],
        [ "cwist_app_asset_add / cwist_app_asset_add_file / cwist_app_asset_url", "d8/d83/md_docs_2api_2html.html#autotoc_md105", null ],
        [ "Putting it together", "d8/d83/md_docs_2api_2html.html#autotoc_md106", null ]
      ] ],
      [ "Rendering the same views in WASM", "d8/d83/md_docs_2api_2html.html#autotoc_md107", null ],
      [ "Example", "d8/d83/md_docs_2api_2html.html#autotoc_md108", null ]
    ] ],
    [ "HTTP Core API", "d2/d9e/md_docs_2api_2http.html", [
      [ "Constants", "d2/d9e/md_docs_2api_2http.html#autotoc_md110", [
        [ "cwist_http_request_create", "d2/d9e/md_docs_2api_2http.html#autotoc_md111", null ],
        [ "cwist_http_parse_request", "d2/d9e/md_docs_2api_2http.html#autotoc_md112", null ],
        [ "cwist_http_receive_request", "d2/d9e/md_docs_2api_2http.html#autotoc_md113", null ],
        [ "cwist_http_response_create", "d2/d9e/md_docs_2api_2http.html#autotoc_md114", null ],
        [ "cwist_http_response_send_file", "d2/d9e/md_docs_2api_2http.html#autotoc_md115", null ],
        [ "cwist_http_header_add", "d2/d9e/md_docs_2api_2http.html#autotoc_md116", null ],
        [ "cwist_http_response_header_add", "d2/d9e/md_docs_2api_2http.html#autotoc_md117", null ],
        [ "cwist_http_header_get", "d2/d9e/md_docs_2api_2http.html#autotoc_md118", null ]
      ] ],
      [ "Server Core", "d2/d9e/md_docs_2api_2http.html#autotoc_md119", [
        [ "Keep-alive handling", "d2/d9e/md_docs_2api_2http.html#autotoc_md120", null ],
        [ "Regression testing", "d2/d9e/md_docs_2api_2http.html#autotoc_md121", null ],
        [ "cwist_http_server_loop", "d2/d9e/md_docs_2api_2http.html#autotoc_md122", null ],
        [ "cwist_accept_socket", "d2/d9e/md_docs_2api_2http.html#autotoc_md123", null ],
        [ "cwist_http_continuation_shed_count", "d2/d9e/md_docs_2api_2http.html#autotoc_md124", null ]
      ] ],
      [ "Security Headers", "d2/d9e/md_docs_2api_2http.html#autotoc_md125", [
        [ "cwist_http_response_add_security_headers", "d2/d9e/md_docs_2api_2http.html#autotoc_md126", null ],
        [ "cwist_http_response_add_hsts", "d2/d9e/md_docs_2api_2http.html#autotoc_md127", null ]
      ] ]
    ] ],
    [ "HTTPS API", "d4/d45/md_docs_2api_2https.html", null ],
    [ "JSON Builder API", "d8/deb/md_docs_2api_2json.html", [
      [ "Functions", "d8/deb/md_docs_2api_2json.html#autotoc_md136", [
        [ "cwist_json_builder_create", "d8/deb/md_docs_2api_2json.html#autotoc_md137", null ],
        [ "cwist_json_begin_object / cwist_json_end_object", "d8/deb/md_docs_2api_2json.html#autotoc_md138", null ],
        [ "cwist_json_add_string", "d8/deb/md_docs_2api_2json.html#autotoc_md139", null ],
        [ "cwist_json_add_int", "d8/deb/md_docs_2api_2json.html#autotoc_md140", null ],
        [ "cwist_json_add_bool", "d8/deb/md_docs_2api_2json.html#autotoc_md141", null ],
        [ "cwist_json_add_null", "d8/deb/md_docs_2api_2json.html#autotoc_md142", null ],
        [ "cwist_json_begin_array / cwist_json_end_array", "d8/deb/md_docs_2api_2json.html#autotoc_md143", null ],
        [ "cwist_json_get_raw", "d8/deb/md_docs_2api_2json.html#autotoc_md144", null ],
        [ "cwist_json_builder_destroy", "d8/deb/md_docs_2api_2json.html#autotoc_md145", null ]
      ] ],
      [ "Example", "d8/deb/md_docs_2api_2json.html#autotoc_md146", null ]
    ] ],
    [ "Middleware API", "da/dbc/md_docs_2api_2middleware.html", [
      [ "Functions", "da/dbc/md_docs_2api_2middleware.html#autotoc_md148", [
        [ "cwist_app_use", "da/dbc/md_docs_2api_2middleware.html#autotoc_md149", null ]
      ] ],
      [ "Built-in Middlewares", "da/dbc/md_docs_2api_2middleware.html#autotoc_md150", [
        [ "Request ID Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md151", null ],
        [ "Access Log Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md152", [
          [ "Async log output", "da/dbc/md_docs_2api_2middleware.html#autotoc_md153", null ]
        ] ],
        [ "Rate Limiter Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md154", null ],
        [ "Redis-backed shared rate limiter", "da/dbc/md_docs_2api_2middleware.html#autotoc_md155", null ],
        [ "CORS Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md156", null ],
        [ "Prometheus Metrics Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md157", null ],
        [ "JWT Auth Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md158", null ],
        [ "Compression Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md159", null ]
      ] ],
      [ "Creating Custom Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md160", null ]
    ] ],
    [ "Query & URI API", "dc/d49/md_docs_2api_2query.html", null ],
    [ "Database API", "d5/d11/md_docs_2api_2sql.html", [
      [ "Schema Migrations", "d5/d11/md_docs_2api_2sql.html#autotoc_md170", [
        [ "cwist_db_open", "d5/d11/md_docs_2api_2sql.html#autotoc_md165", null ],
        [ "cwist_db_open_memory", "d5/d11/md_docs_2api_2sql.html#autotoc_md166", null ],
        [ "cwist_db_serialize", "d5/d11/md_docs_2api_2sql.html#autotoc_md167", null ],
        [ "cwist_db_exec", "d5/d11/md_docs_2api_2sql.html#autotoc_md168", null ],
        [ "cwist_db_query", "d5/d11/md_docs_2api_2sql.html#autotoc_md169", null ],
        [ "Return codes", "d5/d11/md_docs_2api_2sql.html#autotoc_md171", null ],
        [ "cwist_migration_t", "d5/d11/md_docs_2api_2sql.html#autotoc_md172", null ],
        [ "cwist_migrate_up", "d5/d11/md_docs_2api_2sql.html#autotoc_md173", null ],
        [ "cwist_migrate_down", "d5/d11/md_docs_2api_2sql.html#autotoc_md174", null ],
        [ "cwist_migrate_version", "d5/d11/md_docs_2api_2sql.html#autotoc_md175", null ]
      ] ]
    ] ],
    [ "Server-Sent Events API", "d0/d89/md_docs_2api_2sse.html", [
      [ "Types", "d0/d89/md_docs_2api_2sse.html#autotoc_md177", [
        [ "cwist_sse_event_t", "d0/d89/md_docs_2api_2sse.html#autotoc_md178", null ],
        [ "cwist_sse_stream_t", "d0/d89/md_docs_2api_2sse.html#autotoc_md179", null ]
      ] ],
      [ "Response mode", "d0/d89/md_docs_2api_2sse.html#autotoc_md180", [
        [ "cwist_sse_response_init", "d0/d89/md_docs_2api_2sse.html#autotoc_md181", null ],
        [ "cwist_sse_response_event", "d0/d89/md_docs_2api_2sse.html#autotoc_md182", null ],
        [ "cwist_sse_response_write", "d0/d89/md_docs_2api_2sse.html#autotoc_md183", null ],
        [ "cwist_sse_response_comment", "d0/d89/md_docs_2api_2sse.html#autotoc_md184", null ]
      ] ],
      [ "Stream mode", "d0/d89/md_docs_2api_2sse.html#autotoc_md185", [
        [ "cwist_sse_stream_open", "d0/d89/md_docs_2api_2sse.html#autotoc_md186", null ],
        [ "cwist_sse_stream_send", "d0/d89/md_docs_2api_2sse.html#autotoc_md187", null ],
        [ "cwist_sse_stream_write", "d0/d89/md_docs_2api_2sse.html#autotoc_md188", null ],
        [ "cwist_sse_stream_comment", "d0/d89/md_docs_2api_2sse.html#autotoc_md189", null ],
        [ "cwist_sse_stream_close", "d0/d89/md_docs_2api_2sse.html#autotoc_md190", null ]
      ] ],
      [ "Convenience macros", "d0/d89/md_docs_2api_2sse.html#autotoc_md191", null ],
      [ "Example", "d0/d89/md_docs_2api_2sse.html#autotoc_md192", null ]
    ] ],
    [ "SString API", "db/d14/md_docs_2api_2sstring.html", null ],
    [ "WASI targets", "d6/d26/md_docs_2api_2wasi.html", [
      [ "Flavour detection", "d6/d26/md_docs_2api_2wasi.html#autotoc_md203", null ],
      [ "WASI 0.2 socket server", "d6/d26/md_docs_2api_2wasi.html#autotoc_md204", null ],
      [ "WASI preview1 (retired)", "d6/d26/md_docs_2api_2wasi.html#autotoc_md205", [
        [ "What the preview1 build did differently", "d6/d26/md_docs_2api_2wasi.html#autotoc_md206", null ],
        [ "Verification (historical)", "d6/d26/md_docs_2api_2wasi.html#autotoc_md207", null ]
      ] ]
    ] ],
    [ "CWIST on the WASM component model", "d8/d77/md_docs_2api_2wasm-component.html", [
      [ "Why", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md209", null ],
      [ "The boundary", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md210", null ],
      [ "Toolchain status (measured 2026-09-21)", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md211", null ],
      [ "Evaluated: preview3-shim (2026-09-23)", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md212", null ],
      [ "Landed: native async for streaming/SSE (2026-09-23)", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md213", null ],
      [ "Resolved: socket request path under wasip3 (audited 2026-09-22)", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md214", null ],
      [ "Stages", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md215", null ],
      [ "Browser runtime gate (WASI 0.2)", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md216", null ],
      [ "Measured while building stage 2", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md217", null ],
      [ "Non-goals", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md218", null ]
    ] ],
    [ "CWIST on WASM (Legacy Emscripten)", "d5/d0e/md_docs_2api_2wasm.html", [
      [ "Building", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md220", null ],
      [ "What is in the WASM build", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md221", null ],
      [ "Request dispatch from JS", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md222", null ],
      [ "Zero-copy views: cwist/wasm/typedarray.h", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md223", null ],
      [ "Databases under WASM", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md224", null ],
      [ "Bundle size impact of cwist_db", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md225", null ],
      [ "Sessions and cookies", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md226", null ],
      [ "Streaming through the boundary", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md227", null ],
      [ "Runnable example: Service Worker app", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md228", null ],
      [ "Not covered (yet)", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md229", null ],
      [ "Streaming producer (handlers generate the body chunk-by-chunk)", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md230", null ]
    ] ],
    [ "WebSocket API", "db/dc5/md_docs_2api_2websocket.html", [
      [ "Functions", "db/dc5/md_docs_2api_2websocket.html#autotoc_md232", [
        [ "cwist_websocket_upgrade", "db/dc5/md_docs_2api_2websocket.html#autotoc_md233", null ],
        [ "cwist_websocket_receive", "db/dc5/md_docs_2api_2websocket.html#autotoc_md234", null ],
        [ "cwist_websocket_send", "db/dc5/md_docs_2api_2websocket.html#autotoc_md235", null ]
      ] ],
      [ "Opcode Types", "db/dc5/md_docs_2api_2websocket.html#autotoc_md236", null ],
      [ "Integration with cwist_app", "db/dc5/md_docs_2api_2websocket.html#autotoc_md237", null ],
      [ "Async (C1M) WebSocket API", "db/dc5/md_docs_2api_2websocket.html#autotoc_md238", [
        [ "cwist_app_ws_async", "db/dc5/md_docs_2api_2websocket.html#autotoc_md239", null ],
        [ "cwist_ws_on_message_t", "db/dc5/md_docs_2api_2websocket.html#autotoc_md240", null ],
        [ "cwist_websocket_async_send", "db/dc5/md_docs_2api_2websocket.html#autotoc_md241", null ],
        [ "cwist_websocket_async_close", "db/dc5/md_docs_2api_2websocket.html#autotoc_md242", null ]
      ] ],
      [ "GraphQL subscriptions (graphql-ws)", "db/dc5/md_docs_2api_2websocket.html#autotoc_md243", null ]
    ] ],
    [ "Deferred HTTP ownership under full GC", "df/dc5/md_docs_2async-gc-ownership.html", [
      [ "Handoff sequence", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md245", null ],
      [ "Ownership inventory", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md246", [
        [ "Borrowed and external state", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md247", null ]
      ] ],
      [ "HTTP/2 queue ownership", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md248", null ],
      [ "Verification and limits", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md249", null ],
      [ "Scheduler and IO queue ownership", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md250", null ]
    ] ],
    [ "Compile-time build options", "d5/d9e/md_docs_2build-options.html", null ],
    [ "C1M file limits", "db/d4d/md_docs_2c1m-file-limits.html", [
      [ "Cause", "db/d4d/md_docs_2c1m-file-limits.html#autotoc_md253", null ],
      [ "Change", "db/d4d/md_docs_2c1m-file-limits.html#autotoc_md254", null ],
      [ "Tuning", "db/d4d/md_docs_2c1m-file-limits.html#autotoc_md255", null ],
      [ "Checks", "db/d4d/md_docs_2c1m-file-limits.html#autotoc_md256", null ]
    ] ],
    [ "Classic pool burst starvation (#25 follow-up)", "d9/d56/md_docs_2classic-pool-starvation.html", [
      [ "Cause and change", "d9/d56/md_docs_2classic-pool-starvation.html#autotoc_md258", null ],
      [ "Deterministic regression", "d9/d56/md_docs_2classic-pool-starvation.html#autotoc_md259", null ],
      [ "Validation status", "d9/d56/md_docs_2classic-pool-starvation.html#autotoc_md260", null ],
      [ "HTTP measurements and limits", "d9/d56/md_docs_2classic-pool-starvation.html#autotoc_md261", null ]
    ] ],
    [ "Cooperative queuing in the C1M reactor's batch drain (issue #25)", "d1/ddd/md_docs_2cooperative-queuing.html", [
      [ "Background", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md263", null ],
      [ "The gap", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md264", null ],
      [ "The fix", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md265", null ],
      [ "Measured effect", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md266", null ],
      [ "What this does not fix", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md267", null ],
      [ "Tuning", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md268", null ]
    ] ],
    [ "ADR-0001: Restore HTTP response caching through an explicit public FIXED contract", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html", [
      [ "Context", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md270", null ],
      [ "Decision", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md271", [
        [ "1. One parsed eligibility boundary", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md272", null ],
        [ "2. Cache representations, not wire responses", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md273", null ],
        [ "3. Isolate ownership, provenance and capacity", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md274", null ],
        [ "4. Make the compatibility change explicit", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md275", null ]
      ] ],
      [ "Alternatives considered", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md276", null ],
      [ "Consequences", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md277", null ],
      [ "Implementation and release gates", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md278", null ],
      [ "Evidence and source anchors", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md279", null ]
    ] ],
    [ "ADR-0002: TLS certificate hot reload keeps the session-ticket key (STEK) unchanged", "d1/dd3/md_docs_2decisions_20002-tls-hot-reload-stek.html", [
      [ "Context", "d1/dd3/md_docs_2decisions_20002-tls-hot-reload-stek.html#autotoc_md281", null ],
      [ "Decision", "d1/dd3/md_docs_2decisions_20002-tls-hot-reload-stek.html#autotoc_md282", null ],
      [ "Consequences", "d1/dd3/md_docs_2decisions_20002-tls-hot-reload-stek.html#autotoc_md283", null ]
    ] ],
    [ "Prebuilt release assets", "d1/da3/md_docs_2deployment_2release-assets.html", [
      [ "Release branch policy", "d1/da3/md_docs_2deployment_2release-assets.html#autotoc_md286", null ],
      [ "Platform coverage in release notes", "d1/da3/md_docs_2deployment_2release-assets.html#autotoc_md287", null ],
      [ "Using a fat bundle", "d1/da3/md_docs_2deployment_2release-assets.html#autotoc_md288", null ],
      [ "Using the amalgamation", "d1/da3/md_docs_2deployment_2release-assets.html#autotoc_md289", null ],
      [ "Re-running", "d1/da3/md_docs_2deployment_2release-assets.html#autotoc_md290", null ]
    ] ],
    [ "Deploy the WASI 0.2 example with Wasmtime", "db/de3/md_docs_2deployment_2wasmtime-appliance.html", [
      [ "Scope and safety", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md292", null ],
      [ "1. Build a known revision", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md293", null ],
      [ "2. Separate the artifact from state", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md294", null ],
      [ "3. Start and verify", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md295", null ],
      [ "4. Restart, back up, and restore", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md296", null ],
      [ "Before adapting this for production", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md297", null ]
    ] ],
    [ "Zero-downtime restart via socket handoff", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html", [
      [ "Requirements", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html#autotoc_md299", null ],
      [ "Handoff sequence", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html#autotoc_md300", null ],
      [ "systemd integration", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html#autotoc_md301", null ],
      [ "UDP and HTTP/3", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html#autotoc_md302", null ],
      [ "Verifying a deploy", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html#autotoc_md303", null ]
    ] ],
    [ "Durable queue required gate", "d3/dd5/md_docs_2durable-queue-gate.html", [
      [ "Local runs", "d3/dd5/md_docs_2durable-queue-gate.html#autotoc_md305", null ],
      [ "Isolated integration gate", "d3/dd5/md_docs_2durable-queue-gate.html#autotoc_md306", null ],
      [ "CI", "d3/dd5/md_docs_2durable-queue-gate.html#autotoc_md307", null ]
    ] ],
    [ "Bare-metal STM32 with CWIST", "de/d87/md_docs_2embedded_2stm32.html", [
      [ "Supported targets and validation", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md309", null ],
      [ "Build the firmware inputs", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md310", null ],
      [ "Port hooks and runtime contract", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md311", null ],
      [ "HTTP over raw lwIP", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md312", null ],
      [ "Memory sizing", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md313", [
        [ "Request arenas", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md314", null ],
        [ "Connections and responses", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md315", null ],
        [ "lwIP pools", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md316", null ]
      ] ],
      [ "HTTPS", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md317", null ],
      [ "Limitations", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md318", null ]
    ] ],
    [ "FIXED endpoint caching: current behavior", "d3/ddd/md_docs_2fixed-cache-status.html", [
      [ "What the runtime actually does today", "d3/ddd/md_docs_2fixed-cache-status.html#autotoc_md320", null ],
      [ "Evidence", "d3/ddd/md_docs_2fixed-cache-status.html#autotoc_md321", null ],
      [ "Why the learning block is not simply moved", "d3/ddd/md_docs_2fixed-cache-status.html#autotoc_md322", null ],
      [ "Recommended handling", "d3/ddd/md_docs_2fixed-cache-status.html#autotoc_md323", null ]
    ] ],
    [ "Full GC: Automatic Resource Reclamation in CWIST", "d9/dd1/md_docs_2GC.html", [
      [ "Motivation", "d9/dd1/md_docs_2GC.html#autotoc_md325", null ],
      [ "Design", "d9/dd1/md_docs_2GC.html#autotoc_md326", [
        [ "1. cwist_full_gc(bool) — the global toggle", "d9/dd1/md_docs_2GC.html#autotoc_md327", null ],
        [ "2. Reclamation engine: libttak epoch GC", "d9/dd1/md_docs_2GC.html#autotoc_md328", null ],
        [ "3. Thread/process exit sweeps", "d9/dd1/md_docs_2GC.html#autotoc_md329", null ],
        [ "4. Pseudo-RAII for handle-like locals", "d9/dd1/md_docs_2GC.html#autotoc_md330", null ],
        [ "5. Transparent malloc interception", "d9/dd1/md_docs_2GC.html#autotoc_md331", null ],
        [ "6. cwist_alloc internals", "d9/dd1/md_docs_2GC.html#autotoc_md332", null ],
        [ "7. Explicit flushes and reclamation", "d9/dd1/md_docs_2GC.html#autotoc_md333", null ]
      ] ],
      [ "Semantics summary", "d9/dd1/md_docs_2GC.html#autotoc_md334", null ],
      [ "Non-goals and notes", "d9/dd1/md_docs_2GC.html#autotoc_md335", null ],
      [ "Known performance caveat", "d9/dd1/md_docs_2GC.html#autotoc_md336", null ]
    ] ],
    [ "HTTP/1.1 pipeline fairness and framing", "d2/dc2/md_docs_2http-reactor-fairness.html", [
      [ "Scope", "d2/dc2/md_docs_2http-reactor-fairness.html#autotoc_md338", null ],
      [ "Behavioral regression tests", "d2/dc2/md_docs_2http-reactor-fairness.html#autotoc_md339", null ],
      [ "Performance claim boundary", "d2/dc2/md_docs_2http-reactor-fairness.html#autotoc_md340", null ],
      [ "Internal API compatibility", "d2/dc2/md_docs_2http-reactor-fairness.html#autotoc_md341", null ]
    ] ],
    [ "HTTP/3 connection migration", "d8/d64/md_docs_2http3-migration.html", [
      [ "Result for issue #407", "d8/d64/md_docs_2http3-migration.html#autotoc_md343", null ],
      [ "Run the test", "d8/d64/md_docs_2http3-migration.html#autotoc_md344", null ],
      [ "Test coverage", "d8/d64/md_docs_2http3-migration.html#autotoc_md345", null ],
      [ "Limits", "d8/d64/md_docs_2http3-migration.html#autotoc_md346", null ]
    ] ],
    [ "CWIST bare-metal STM32", "d4/dc9/md_docs_2ko_2embedded_2stm32.html", [
      [ "지원 대상과 검증 범위", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md348", null ],
      [ "펌웨어 입력 빌드", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md349", null ],
      [ "포트 훅과 런타임 계약", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md350", null ],
      [ "raw lwIP HTTP", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md351", null ],
      [ "메모리 크기", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md352", [
        [ "요청 arena", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md353", null ],
        [ "연결과 응답", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md354", null ],
        [ "lwIP pool", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md355", null ]
      ] ],
      [ "HTTPS", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md356", null ],
      [ "제한사항", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md357", null ]
    ] ],
    [ "라틴방진 참고 문헌", "da/d05/md_docs_2ko_2references_2latin__square__references.html", [
      [ "1. 정의", "da/d05/md_docs_2ko_2references_2latin__square__references.html#autotoc_md359", null ],
      [ "2. 코드에 쓰인 구성법", "da/d05/md_docs_2ko_2references_2latin__square__references.html#autotoc_md360", [
        [ "2.1 유한체 (소수 거듭제곱)", "da/d05/md_docs_2ko_2references_2latin__square__references.html#autotoc_md361", null ],
        [ "2.2 홀수 소수 거듭제곱 p^e, e >= 2", "da/d05/md_docs_2ko_2references_2latin__square__references.html#autotoc_md362", null ],
        [ "2.3 합성수 차수: MacNeish 곱", "da/d05/md_docs_2ko_2references_2latin__square__references.html#autotoc_md363", null ],
        [ "2.4 코드가 만들지 않는 것", "da/d05/md_docs_2ko_2references_2latin__square__references.html#autotoc_md364", null ]
      ] ],
      [ "3. 역사적 표: Gusuryak 9차 쌍", "da/d05/md_docs_2ko_2references_2latin__square__references.html#autotoc_md365", null ],
      [ "4. CWIST에서의 사용: 테스트에서 런타임 knob 쌍 덮기", "da/d05/md_docs_2ko_2references_2latin__square__references.html#autotoc_md366", null ]
    ] ],
    [ "런타임 설정", "d5/d08/md_docs_2ko_2runtime-config.html", [
      [ "상태", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md368", null ],
      [ "프로세스와 서버 모드", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md369", null ],
      [ "Classic 스레드 풀", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md370", null ],
      [ "HTTP/1.1", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md371", null ],
      [ "HTTPS", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md372", null ],
      [ "HTTP/2", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md373", null ],
      [ "HTTP/3", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md374", null ],
      [ "WebSocket", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md375", null ],
      [ "Reactor", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md376", null ],
      [ "진단", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md377", null ]
    ] ],
    [ "Release Codename Sequencing Specification", "d6/de8/md_docs_2ko_2versioning.html", [
      [ "1. 기본 원칙", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md379", null ],
      [ "2. 2자리 시퀀스 (기본)", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md380", null ],
      [ "3. 3자리 확장 시퀀스", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md381", null ],
      [ "4. 표기", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md382", null ],
      [ "5. API 안정성 게이트 (v4 LTS부터)", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md383", null ],
      [ "6. 메이저 버전의 첫 상승", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md384", null ]
    ] ],
    [ "HTTPS C100k gate", "d0/d49/md_docs_2performance_2https-c100k.html", [
      [ "What runs", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md386", null ],
      [ "Loopback port space", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md387", null ],
      [ "Results (2026-10-17, dev)", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md388", null ],
      [ "Results (2026-10-18, dev): C1M over TLS", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md389", null ],
      [ "Results (2026-10-11, dev)", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md390", [
        [ "Where the memory goes", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md391", null ]
      ] ],
      [ "Scaling past the gate on a single host", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md392", null ],
      [ "Running it", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md393", null ]
    ] ],
    [ "HTTPS pool task queue: static ring vs growable ring", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html", [
      [ "What changed", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md395", null ],
      [ "Setup", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md396", null ],
      [ "1. RSS of https_pool_init() alone", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md397", null ],
      [ "2. 4 workers, steady and churn load (3 alternating rounds)", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md398", null ],
      [ "3. Forcing the ring to grow: 1 worker, 2 pool threads, 6000 simultaneous connections", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md399", null ],
      [ "4. Order and loss under growth: tests/test_https_pool_queue.c", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md400", null ],
      [ "Not done / open", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md401", null ]
    ] ],
    [ "Issue #297 syscall-reduction audit (local, 12-core Ryzen 5600X)", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html", [
      [ "Baseline syscall profile", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md403", null ],
      [ "Item results", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md404", [
        [ "1.1 io_uring multishot accept — NEGATIVE (reverted)", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md405", null ],
        [ "1.2 Larger SQE batches / fewer io_uring_enter — ALREADY IMPLEMENTED / BLOCKED", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md406", null ],
        [ "1.3 Per-accept setsockopt removal — NEGATIVE (reverted)", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md407", null ],
        [ "1.4 Time caching — ALREADY IMPLEMENTED", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md408", null ]
      ] ],
      [ "What the profile says to do instead (ranked)", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md409", null ],
      [ "Method notes", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md410", null ]
    ] ],
    [ "Pluggable TLS backend (CWIST_TLS)", "d9/dda/md_docs_2performance_2tls-backends.html", [
      [ "The seam", "d9/dda/md_docs_2performance_2tls-backends.html#autotoc_md412", null ],
      [ "Forced-off features under CWIST_TLS=mbedtls", "d9/dda/md_docs_2performance_2tls-backends.html#autotoc_md413", null ],
      [ "Degraded features under CWIST_TLS=mbedtls", "d9/dda/md_docs_2performance_2tls-backends.html#autotoc_md414", null ],
      [ "Embedded bundle", "d9/dda/md_docs_2performance_2tls-backends.html#autotoc_md415", null ]
    ] ],
    [ "Opt-in wrk raw/corrected latency histograms", "d1/dfd/md_docs_2performance_2wrk-dual-histogram.html", [
      [ "Build in a disposable wrk checkout", "d1/dfd/md_docs_2performance_2wrk-dual-histogram.html#autotoc_md417", null ],
      [ "Output and interpretation", "d1/dfd/md_docs_2performance_2wrk-dual-histogram.html#autotoc_md418", null ],
      [ "Verification", "d1/dfd/md_docs_2performance_2wrk-dual-histogram.html#autotoc_md419", null ]
    ] ],
    [ "io_uring posted-work wake-up regression (#25)", "dd/d56/md_docs_2reactor-wakeup-regression.html", [
      [ "Scope and cause", "dd/d56/md_docs_2reactor-wakeup-regression.html#autotoc_md421", null ],
      [ "Regression test", "dd/d56/md_docs_2reactor-wakeup-regression.html#autotoc_md422", null ],
      [ "Component measurement (2026-09-12)", "dd/d56/md_docs_2reactor-wakeup-regression.html#autotoc_md423", null ]
    ] ],
    [ "CWIST Library Reference", "da/dca/md_docs_2REFERENCE.html", [
      [ "Modules", "da/dca/md_docs_2REFERENCE.html#autotoc_md432", [
        [ "1. SString (Smart String)", "da/dca/md_docs_2REFERENCE.html#autotoc_md433", null ],
        [ "2. HTTP Server", "da/dca/md_docs_2REFERENCE.html#autotoc_md434", null ],
        [ "3. HTTPS Support", "da/dca/md_docs_2REFERENCE.html#autotoc_md435", null ],
        [ "4. Query Parsing", "da/dca/md_docs_2REFERENCE.html#autotoc_md436", null ],
        [ "5. Database (SQL)", "da/dca/md_docs_2REFERENCE.html#autotoc_md437", null ],
        [ "6. SipHash", "da/dca/md_docs_2REFERENCE.html#autotoc_md438", null ],
        [ "7. Error Handling", "da/dca/md_docs_2REFERENCE.html#autotoc_md439", null ],
        [ "8. Big Dumb Reply (BDR)", "da/dca/md_docs_2REFERENCE.html#autotoc_md440", null ]
      ] ],
      [ "LibTTAK Memory Features", "da/dca/md_docs_2REFERENCE.html#autotoc_md441", null ],
      [ "Example: Othello Web", "da/dca/md_docs_2REFERENCE.html#autotoc_md442", null ],
      [ "Dependencies", "da/dca/md_docs_2REFERENCE.html#autotoc_md443", null ],
      [ "Build", "da/dca/md_docs_2REFERENCE.html#autotoc_md444", null ]
    ] ],
    [ "Latin Square References", "df/d22/md_docs_2references_2latin__square__references.html", [
      [ "1. Definitions", "df/d22/md_docs_2references_2latin__square__references.html#autotoc_md446", null ],
      [ "2. Constructions used in the code", "df/d22/md_docs_2references_2latin__square__references.html#autotoc_md447", [
        [ "2.1 Finite fields (prime powers)", "df/d22/md_docs_2references_2latin__square__references.html#autotoc_md448", null ],
        [ "2.2 Odd prime powers p^e, e >= 2", "df/d22/md_docs_2references_2latin__square__references.html#autotoc_md449", null ],
        [ "2.3 Composite orders: MacNeish's product", "df/d22/md_docs_2references_2latin__square__references.html#autotoc_md450", null ],
        [ "2.4 What the code does not construct", "df/d22/md_docs_2references_2latin__square__references.html#autotoc_md451", null ]
      ] ],
      [ "3. Historical table: the Gusuryak order-9 pair", "df/d22/md_docs_2references_2latin__square__references.html#autotoc_md452", null ],
      [ "4. Use in CWIST: covering runtime knob pairs in tests", "df/d22/md_docs_2references_2latin__square__references.html#autotoc_md453", null ]
    ] ],
    [ "Mux Algorithm References", "d6/d9f/md_docs_2references_2mux__algorithm__references.html", [
      [ "1. Lo Shu Magic Square (Naak-seo / 洛書)", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md455", null ],
      [ "2. Orthogonal Latin-Square Permutation", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md456", null ],
      [ "3. Coefficient Matrix & Linear System Solver", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md457", null ],
      [ "4. Iterative Magnitude Refinement", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md458", null ],
      [ "5. Single-Step Linear Extrapolation", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md459", null ],
      [ "Naming Rationale", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md460", null ]
    ] ],
    [ "v3.5.0 release roadmap", "d7/df9/md_docs_2roadmap-v3_85.html", [
      [ "Scope", "d7/df9/md_docs_2roadmap-v3_85.html#autotoc_md462", null ],
      [ "Release gates", "d7/df9/md_docs_2roadmap-v3_85.html#autotoc_md463", null ],
      [ "Evidence and limits", "d7/df9/md_docs_2roadmap-v3_85.html#autotoc_md464", null ]
    ] ],
    [ "Runtime configuration", "d9/dfa/md_docs_2runtime-config.html", [
      [ "Status", "d9/dfa/md_docs_2runtime-config.html#autotoc_md466", null ],
      [ "Processes and server mode", "d9/dfa/md_docs_2runtime-config.html#autotoc_md467", null ],
      [ "Classic thread pool", "d9/dfa/md_docs_2runtime-config.html#autotoc_md468", null ],
      [ "HTTP/1.1", "d9/dfa/md_docs_2runtime-config.html#autotoc_md469", null ],
      [ "HTTPS", "d9/dfa/md_docs_2runtime-config.html#autotoc_md470", null ],
      [ "HTTP/2", "d9/dfa/md_docs_2runtime-config.html#autotoc_md471", null ],
      [ "HTTP/3", "d9/dfa/md_docs_2runtime-config.html#autotoc_md472", null ],
      [ "WebSocket", "d9/dfa/md_docs_2runtime-config.html#autotoc_md473", null ],
      [ "Reactor", "d9/dfa/md_docs_2runtime-config.html#autotoc_md474", null ],
      [ "Diagnostics", "d9/dfa/md_docs_2runtime-config.html#autotoc_md475", null ]
    ] ],
    [ "Soak testing", "db/d05/md_docs_2soak-testing.html", [
      [ "Environment", "db/d05/md_docs_2soak-testing.html#autotoc_md477", null ],
      [ "Bench server and workload mix", "db/d05/md_docs_2soak-testing.html#autotoc_md478", null ],
      [ "Measurement points", "db/d05/md_docs_2soak-testing.html#autotoc_md479", null ],
      [ "PASS/FAIL criteria", "db/d05/md_docs_2soak-testing.html#autotoc_md480", null ],
      [ "Interpreting a failure", "db/d05/md_docs_2soak-testing.html#autotoc_md481", null ]
    ] ],
    [ "CWIST Web Development Tutorial (A to Z)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html", [
      [ "Table of Contents", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md483", null ],
      [ "1. Hello World: Starting Your First Server", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md484", null ],
      [ "2. Routing and Handlers (Mux Router)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md485", null ],
      [ "3. JSON Parsing and Zod Schema Validation", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md486", null ],
      [ "4. Middleware (CORS, Logging)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md487", null ],
      [ "5. Databases and Migrations (SQLite)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md488", null ],
      [ "6. Authentication (JWT and DB Encryption)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md489", null ],
      [ "7. PQC TLS (Post-Quantum Hybrid Key Exchange)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md490", [
        [ "Security Policy Summary", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md491", null ]
      ] ],
      [ "8. WebSocket Integration (Real-Time Bidirectional Communication)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md492", null ],
      [ "9. Template Engine and Static File Serving", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md493", null ],
      [ "10. Dynamic CSS Composer (WASM and SSR)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md494", [
        [ "Approach A: 100% Server-Side Rendering (SSR)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md495", null ],
        [ "Approach B: Client-Side Composition with React + WebAssembly (WASM)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md496", null ],
        [ "Wrapping Up", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md497", null ]
      ] ]
    ] ],
    [ "CWIST 웹 개발 튜토리얼 (A to Z)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html", [
      [ "목차", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md499", null ],
      [ "1. Hello World: 첫 서버 띄우기", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md500", null ],
      [ "2. 라우팅과 핸들러 (Mux Router)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md501", null ],
      [ "3. JSON 파싱과 Zod 스키마 검증", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md502", null ],
      [ "4. 미들웨어 (CORS, Logging)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md503", null ],
      [ "5. 데이터베이스와 마이그레이션 (SQLite)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md504", null ],
      [ "6. 인증 (JWT 및 DB 암호화)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md505", null ],
      [ "7. PQC TLS (양자내성 하이브리드 키 교환)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md506", [
        [ "보안 정책 요약", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md507", null ]
      ] ],
      [ "8. 웹소켓 연동 (실시간 양방향 통신)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md508", null ],
      [ "9. 템플릿 엔진과 정적 파일 제공", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md509", null ],
      [ "10. 동적 CSS 합성기 (WASM 및 SSR)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md510", [
        [ "방법 A: 100% Server-Side Rendering (SSR)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md511", null ],
        [ "방법 B: React + WebAssembly (WASM) 클라이언트 사이드 합성", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md512", null ],
        [ "마치며", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md513", null ]
      ] ]
    ] ],
    [ "Build a CRUD blog with CWIST", "d4/db0/md_docs_2tutorials_2blog-crud.html", [
      [ "1. Create the application", "d4/db0/md_docs_2tutorials_2blog-crud.html#autotoc_md515", null ],
      [ "2. Define the routes", "d4/db0/md_docs_2tutorials_2blog-crud.html#autotoc_md516", null ],
      [ "3. Validate JSON and return HTTP semantics", "d4/db0/md_docs_2tutorials_2blog-crud.html#autotoc_md517", null ],
      [ "4. Test the API", "d4/db0/md_docs_2tutorials_2blog-crud.html#autotoc_md518", null ]
    ] ],
    [ "Integrate NATS with CWIST", "da/d48/md_docs_2tutorials_2nats-integration.html", [
      [ "1. Separate request and worker responsibilities", "da/d48/md_docs_2tutorials_2nats-integration.html#autotoc_md520", null ],
      [ "2. Publish from a route", "da/d48/md_docs_2tutorials_2nats-integration.html#autotoc_md521", null ],
      [ "3. Reliability checklist", "da/d48/md_docs_2tutorials_2nats-integration.html#autotoc_md522", null ]
    ] ],
    [ "Document an API with OpenAPI and Swagger UI", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html", [
      [ "1. Annotate the routes", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html#autotoc_md524", [
        [ "Annotation tags", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html#autotoc_md525", null ]
      ] ],
      [ "2. Generate openapi.json", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html#autotoc_md526", null ],
      [ "3. Serve the UI", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html#autotoc_md527", null ]
    ] ],
    [ "Build a WebTransport server with CWIST", "d2/dcb/md_docs_2tutorials_2webtransport-server.html", [
      [ "Build", "d2/dcb/md_docs_2tutorials_2webtransport-server.html#autotoc_md529", null ],
      [ "1. Configure HTTP/3 and a session handler", "d2/dcb/md_docs_2tutorials_2webtransport-server.html#autotoc_md530", null ],
      [ "2. Design the session protocol", "d2/dcb/md_docs_2tutorials_2webtransport-server.html#autotoc_md531", [
        [ "Raw datagrams next to WebTransport", "d2/dcb/md_docs_2tutorials_2webtransport-server.html#autotoc_md532", null ]
      ] ],
      [ "3. Operate safely", "d2/dcb/md_docs_2tutorials_2webtransport-server.html#autotoc_md533", null ]
    ] ],
    [ "Release Codename Sequencing Specification", "d0/db4/md_docs_2versioning.html", [
      [ "1. Principles", "d0/db4/md_docs_2versioning.html#autotoc_md535", null ],
      [ "2. Two-letter sequence (default)", "d0/db4/md_docs_2versioning.html#autotoc_md536", null ],
      [ "3. Three-letter extension", "d0/db4/md_docs_2versioning.html#autotoc_md537", null ],
      [ "4. Notation", "d0/db4/md_docs_2versioning.html#autotoc_md538", null ],
      [ "5. API stability gate (from the v4 LTS)", "d0/db4/md_docs_2versioning.html#autotoc_md539", null ],
      [ "6. First ascent of a major version", "d0/db4/md_docs_2versioning.html#autotoc_md540", null ]
    ] ],
    [ "Web Server Benchmark Methodology", "d6/d42/md_docs_2webserver-benchmark.html", [
      [ "Measurement contract v2", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md542", null ],
      [ "Admission and latency semantics", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md543", null ],
      [ "Resource measurements", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md544", [
        [ "Interpreting the context-switch column", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md545", null ]
      ] ],
      [ "Runtime configuration", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md546", null ],
      [ "Provenance, presentation and legacy results", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md547", null ]
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
"d0/d89/md_docs_2api_2sse.html#autotoc_md179",
"d1/d54/md_docs_2deployment_2zero-downtime-restart.html",
"d2/d8b/seq_8c.html#ac4556d6b030fb6c6604ee4e1a8073c89",
"d2/dfe/alloc_8h.html#aeba69949fe17751318e6b5fc84a74096",
"d3/db9/middleware_8h_source.html",
"d4/dca/cwist__redis_8c.html#a3024ccd4a9af5109d24e6c57565d74a1",
"d5/d7a/grpc__channel_8c.html#aa71b8fbaf45b3c74f7a0b634a5da007d",
"d6/d73/async__internal_8h.html#a4212369d5a88e570c7c231df1f26e36c",
"d7/d5d/lwip_8c.html#a8adcbe4d8e6bf7ba1e9e482c6e78337e",
"d8/d16/gc_8c.html#a2406b97d2e5c876a65e828c15e656347",
"d9/d90/structcwist__bdr__t.html#a174ae4d49e4f7e0a59555bed660f81b4",
"d9/dab/net_2http_2http_8h.html#af573428737f46c288e77e35bc78fdd3c",
"da/d46/session_8h.html#ab08aea4f39df5d52d745973cf2aca6ab",
"db/d2e/backend__mbedtls_8c.html#a2fd0e1fef1ff2575cccaa4ce7fa69f0a",
"db/dba/ice_8c.html#aeb9a4b965f7448524d87fe13086ea534",
"dc/da0/proxy_8c.html#a3ff423be2182a6feaeb6a38e32458045",
"dd/d43/http_8c.html#a4212369d5a88e570c7c231df1f26e36c",
"dd/d79/sstring_8c.html#ac0811bcf8f4f42d716341a8ef30ae790",
"de/d67/session_8c_source.html",
"df/d57/latin__square_8h.html#affba43871e18da9a641166fb34072589",
"df/dad/http3__client_8h.html#a051ca5d9ea16c24867801ccde546cd42",
"functions_q.html"
];

var SYNCONMSG = 'click to disable panel synchronisation';
var SYNCOFFMSG = 'click to enable panel synchronisation';