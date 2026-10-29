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
      [ "Quick start", "index.html#autotoc_md3", null ],
      [ "One app, three places to run it", "index.html#autotoc_md4", null ],
      [ "What CWIST includes", "index.html#autotoc_md5", null ],
      [ "Performance, including where it loses", "index.html#autotoc_md6", [
        [ "Two server modes", "index.html#autotoc_md7", null ],
        [ "Holding a million connections", "index.html#autotoc_md8", null ]
      ] ],
      [ "Why C, when Axum and Gin exist?", "index.html#autotoc_md9", null ],
      [ "Memory safety and security in C", "index.html#autotoc_md10", null ],
      [ "Install", "index.html#autotoc_md11", [
        [ "Homebrew", "index.html#autotoc_md12", null ]
      ] ],
      [ "Platform support", "index.html#autotoc_md13", null ],
      [ "The cwist CLI and hot reload", "index.html#autotoc_md14", null ],
      [ "Linking", "index.html#autotoc_md15", null ],
      [ "Configuration", "index.html#autotoc_md16", null ],
      [ "Feature notes", "index.html#autotoc_md17", null ],
      [ "Latest benchmark run", "index.html#autotoc_md18", null ],
      [ "Stability and conformance", "index.html#autotoc_md19", null ],
      [ "Documentation", "index.html#autotoc_md20", null ],
      [ "Examples", "index.html#autotoc_md21", null ],
      [ "Community", "index.html#autotoc_md22", null ],
      [ "Third-party licenses", "index.html#autotoc_md23", null ],
      [ "Error Handling", "d1/d7e/md_docs_2api-quickref.html#autotoc_md25", [
        [ "make_error", "d1/d7e/md_docs_2api-quickref.html#autotoc_md26", null ],
        [ "cwist_error_is_ok / cwist_error_is_ok_extern", "d1/d7e/md_docs_2api-quickref.html#autotoc_md27", null ],
        [ "Other exported inline helpers", "d1/d7e/md_docs_2api-quickref.html#autotoc_md28", null ]
      ] ],
      [ "SString (cwist_sstring)", "d1/d7e/md_docs_2api-quickref.html#autotoc_md29", [
        [ "Lifecycle", "d1/d7e/md_docs_2api-quickref.html#autotoc_md30", null ],
        [ "Core helpers", "d1/d7e/md_docs_2api-quickref.html#autotoc_md31", null ],
        [ "Trimming", "d1/d7e/md_docs_2api-quickref.html#autotoc_md32", null ],
        [ "Append / Copy", "d1/d7e/md_docs_2api-quickref.html#autotoc_md33", null ],
        [ "Compare / Query", "d1/d7e/md_docs_2api-quickref.html#autotoc_md34", null ]
      ] ],
      [ "HTTP", "d1/d7e/md_docs_2api-quickref.html#autotoc_md35", [
        [ "Request lifecycle", "d1/d7e/md_docs_2api-quickref.html#autotoc_md36", null ],
        [ "Response lifecycle", "d1/d7e/md_docs_2api-quickref.html#autotoc_md37", null ],
        [ "Header helpers", "d1/d7e/md_docs_2api-quickref.html#autotoc_md38", null ],
        [ "Method helpers", "d1/d7e/md_docs_2api-quickref.html#autotoc_md39", null ],
        [ "Socket helpers", "d1/d7e/md_docs_2api-quickref.html#autotoc_md40", null ]
      ] ],
      [ "Session manager", "d1/d7e/md_docs_2api-quickref.html#autotoc_md41", [
        [ "Arena", "d1/d7e/md_docs_2api-quickref.html#autotoc_md42", null ],
        [ "Shared sessions (intrusive ref count)", "d1/d7e/md_docs_2api-quickref.html#autotoc_md43", null ],
        [ "Manager", "d1/d7e/md_docs_2api-quickref.html#autotoc_md44", null ]
      ] ]
    ] ],
    [ "CWIST API Documentation", "d7/d15/md_docs_2API.html", [
      [ "Modules", "d7/d15/md_docs_2API.html#autotoc_md46", null ],
      [ "See also", "d7/d15/md_docs_2API.html#autotoc_md47", null ]
    ] ],
    [ "Framework & App API", "de/d19/md_docs_2api_2app.html", [
      [ "Routing", "de/d19/md_docs_2api_2app.html#autotoc_md54", [
        [ "cwist_app_create", "de/d19/md_docs_2api_2app.html#autotoc_md49", null ],
        [ "cwist_app_use_https", "de/d19/md_docs_2api_2app.html#autotoc_md50", null ],
        [ "cwist_app_use_https2 / cwist_use_https2", "de/d19/md_docs_2api_2app.html#autotoc_md51", null ],
        [ "cwist_app_use_pqc_layer", "de/d19/md_docs_2api_2app.html#autotoc_md52", null ],
        [ "cwist_app_use_db", "de/d19/md_docs_2api_2app.html#autotoc_md53", null ],
        [ "cwist_app_get / cwist_app_post", "de/d19/md_docs_2api_2app.html#autotoc_md55", null ],
        [ "cwist_app_get_ex / _post_ex / _put_ex / _delete_ex / _patch_ex", "de/d19/md_docs_2api_2app.html#autotoc_md56", null ],
        [ "cwist_app_dispatch_memory", "de/d19/md_docs_2api_2app.html#autotoc_md57", null ],
        [ "cwist_app_ws", "de/d19/md_docs_2api_2app.html#autotoc_md58", null ],
        [ "cwist_endpoint_opt_t", "de/d19/md_docs_2api_2app.html#autotoc_md59", null ]
      ] ],
      [ "Static Assets", "de/d19/md_docs_2api_2app.html#autotoc_md60", [
        [ "cwist_app_static", "de/d19/md_docs_2api_2app.html#autotoc_md61", null ]
      ] ],
      [ "Error Handling", "de/d19/md_docs_2api_2app.html#autotoc_md62", [
        [ "cwist_app_set_error_handler", "de/d19/md_docs_2api_2app.html#autotoc_md63", null ],
        [ "Unused Variables", "de/d19/md_docs_2api_2app.html#autotoc_md64", null ]
      ] ],
      [ "Concurrency & Thread-Safety", "de/d19/md_docs_2api_2app.html#autotoc_md65", null ],
      [ "Memory Ownership Rules", "de/d19/md_docs_2api_2app.html#autotoc_md66", [
        [ "cwist_app_listen", "de/d19/md_docs_2api_2app.html#autotoc_md67", null ]
      ] ],
      [ "In-Memory Test Client", "de/d19/md_docs_2api_2app.html#autotoc_md68", [
        [ "cwist_test_client_create / cwist_test_client_destroy", "de/d19/md_docs_2api_2app.html#autotoc_md69", null ],
        [ "Request helpers", "de/d19/md_docs_2api_2app.html#autotoc_md70", null ],
        [ "Cookie jar", "de/d19/md_docs_2api_2app.html#autotoc_md71", null ],
        [ "Test assertion macros", "de/d19/md_docs_2api_2app.html#autotoc_md72", null ]
      ] ]
    ] ],
    [ "Cookie API", "dc/d80/md_docs_2api_2cookie.html", [
      [ "Reading cookies", "dc/d80/md_docs_2api_2cookie.html#autotoc_md74", [
        [ "cwist_cookie_parse", "dc/d80/md_docs_2api_2cookie.html#autotoc_md75", null ],
        [ "cwist_cookie_get", "dc/d80/md_docs_2api_2cookie.html#autotoc_md76", null ]
      ] ],
      [ "Writing cookies", "dc/d80/md_docs_2api_2cookie.html#autotoc_md77", [
        [ "cwist_cookie_options", "dc/d80/md_docs_2api_2cookie.html#autotoc_md78", null ],
        [ "cwist_cookie_set", "dc/d80/md_docs_2api_2cookie.html#autotoc_md79", null ],
        [ "cwist_cookie_delete", "dc/d80/md_docs_2api_2cookie.html#autotoc_md80", null ]
      ] ],
      [ "Encoding", "dc/d80/md_docs_2api_2cookie.html#autotoc_md81", [
        [ "cwist_cookie_encode", "dc/d80/md_docs_2api_2cookie.html#autotoc_md82", null ],
        [ "cwist_cookie_decode", "dc/d80/md_docs_2api_2cookie.html#autotoc_md83", null ]
      ] ],
      [ "Example", "dc/d80/md_docs_2api_2cookie.html#autotoc_md84", null ]
    ] ],
    [ "HTML Components, Scoped CSS, Fragments and Assets", "d8/d83/md_docs_2api_2html.html", [
      [ "Components", "d8/d83/md_docs_2api_2html.html#autotoc_md86", [
        [ "cwist_html_component_create / cwist_html_component_destroy", "d8/d83/md_docs_2api_2html.html#autotoc_md87", null ],
        [ "cwist_html_component_instantiate", "d8/d83/md_docs_2api_2html.html#autotoc_md88", null ]
      ] ],
      [ "Scoped CSS", "d8/d83/md_docs_2api_2html.html#autotoc_md89", [
        [ "cwist_css_scope_init / cwist_css_scope_destroy", "d8/d83/md_docs_2api_2html.html#autotoc_md90", null ],
        [ "cwist_css_scope_class", "d8/d83/md_docs_2api_2html.html#autotoc_md91", null ],
        [ "cwist_css_scope_add_rule", "d8/d83/md_docs_2api_2html.html#autotoc_md92", null ],
        [ "cwist_css_scope_generate_stylesheet", "d8/d83/md_docs_2api_2html.html#autotoc_md93", null ]
      ] ],
      [ "HTML over the wire", "d8/d83/md_docs_2api_2html.html#autotoc_md94", [
        [ "cwist_http_request_wants_fragment / cwist_http_request_fragment_target", "d8/d83/md_docs_2api_2html.html#autotoc_md95", null ],
        [ "cwist_http_response_set_view", "d8/d83/md_docs_2api_2html.html#autotoc_md96", null ],
        [ "cwist_http_response_set_html", "d8/d83/md_docs_2api_2html.html#autotoc_md97", null ],
        [ "cwist_http_response_add_oob", "d8/d83/md_docs_2api_2html.html#autotoc_md98", null ],
        [ "cwist_http_response_html_redirect", "d8/d83/md_docs_2api_2html.html#autotoc_md99", null ],
        [ "Handler example", "d8/d83/md_docs_2api_2html.html#autotoc_md100", null ]
      ] ],
      [ "Asset pipeline", "d8/d83/md_docs_2api_2html.html#autotoc_md101", [
        [ "cwist_css_minify / cwist_css_bundle", "d8/d83/md_docs_2api_2html.html#autotoc_md102", null ],
        [ "cwist_app_asset_add / cwist_app_asset_add_file / cwist_app_asset_url", "d8/d83/md_docs_2api_2html.html#autotoc_md103", null ],
        [ "Putting it together", "d8/d83/md_docs_2api_2html.html#autotoc_md104", null ]
      ] ],
      [ "Rendering the same views in WASM", "d8/d83/md_docs_2api_2html.html#autotoc_md105", null ],
      [ "Example", "d8/d83/md_docs_2api_2html.html#autotoc_md106", null ]
    ] ],
    [ "HTTP Core API", "d2/d9e/md_docs_2api_2http.html", [
      [ "Constants", "d2/d9e/md_docs_2api_2http.html#autotoc_md108", [
        [ "cwist_http_request_create", "d2/d9e/md_docs_2api_2http.html#autotoc_md109", null ],
        [ "cwist_http_parse_request", "d2/d9e/md_docs_2api_2http.html#autotoc_md110", null ],
        [ "cwist_http_receive_request", "d2/d9e/md_docs_2api_2http.html#autotoc_md111", null ],
        [ "cwist_http_response_create", "d2/d9e/md_docs_2api_2http.html#autotoc_md112", null ],
        [ "cwist_http_response_send_file", "d2/d9e/md_docs_2api_2http.html#autotoc_md113", null ],
        [ "cwist_http_header_add", "d2/d9e/md_docs_2api_2http.html#autotoc_md114", null ],
        [ "cwist_http_response_header_add", "d2/d9e/md_docs_2api_2http.html#autotoc_md115", null ],
        [ "cwist_http_header_get", "d2/d9e/md_docs_2api_2http.html#autotoc_md116", null ]
      ] ],
      [ "Server Core", "d2/d9e/md_docs_2api_2http.html#autotoc_md117", [
        [ "Keep-alive handling", "d2/d9e/md_docs_2api_2http.html#autotoc_md118", null ],
        [ "Regression testing", "d2/d9e/md_docs_2api_2http.html#autotoc_md119", null ],
        [ "cwist_http_server_loop", "d2/d9e/md_docs_2api_2http.html#autotoc_md120", null ],
        [ "cwist_accept_socket", "d2/d9e/md_docs_2api_2http.html#autotoc_md121", null ],
        [ "cwist_http_continuation_shed_count", "d2/d9e/md_docs_2api_2http.html#autotoc_md122", null ]
      ] ],
      [ "Security Headers", "d2/d9e/md_docs_2api_2http.html#autotoc_md123", [
        [ "cwist_http_response_add_security_headers", "d2/d9e/md_docs_2api_2http.html#autotoc_md124", null ],
        [ "cwist_http_response_add_hsts", "d2/d9e/md_docs_2api_2http.html#autotoc_md125", null ]
      ] ]
    ] ],
    [ "HTTPS API", "d4/d45/md_docs_2api_2https.html", null ],
    [ "JSON Builder API", "d8/deb/md_docs_2api_2json.html", [
      [ "Functions", "d8/deb/md_docs_2api_2json.html#autotoc_md134", [
        [ "cwist_json_builder_create", "d8/deb/md_docs_2api_2json.html#autotoc_md135", null ],
        [ "cwist_json_begin_object / cwist_json_end_object", "d8/deb/md_docs_2api_2json.html#autotoc_md136", null ],
        [ "cwist_json_add_string", "d8/deb/md_docs_2api_2json.html#autotoc_md137", null ],
        [ "cwist_json_add_int", "d8/deb/md_docs_2api_2json.html#autotoc_md138", null ],
        [ "cwist_json_add_bool", "d8/deb/md_docs_2api_2json.html#autotoc_md139", null ],
        [ "cwist_json_add_null", "d8/deb/md_docs_2api_2json.html#autotoc_md140", null ],
        [ "cwist_json_begin_array / cwist_json_end_array", "d8/deb/md_docs_2api_2json.html#autotoc_md141", null ],
        [ "cwist_json_get_raw", "d8/deb/md_docs_2api_2json.html#autotoc_md142", null ],
        [ "cwist_json_builder_destroy", "d8/deb/md_docs_2api_2json.html#autotoc_md143", null ]
      ] ],
      [ "Example", "d8/deb/md_docs_2api_2json.html#autotoc_md144", null ]
    ] ],
    [ "Middleware API", "da/dbc/md_docs_2api_2middleware.html", [
      [ "Functions", "da/dbc/md_docs_2api_2middleware.html#autotoc_md146", [
        [ "cwist_app_use", "da/dbc/md_docs_2api_2middleware.html#autotoc_md147", null ]
      ] ],
      [ "Built-in Middlewares", "da/dbc/md_docs_2api_2middleware.html#autotoc_md148", [
        [ "Request ID Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md149", null ],
        [ "Access Log Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md150", [
          [ "Async log output", "da/dbc/md_docs_2api_2middleware.html#autotoc_md151", null ]
        ] ],
        [ "Rate Limiter Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md152", null ],
        [ "Redis-backed shared rate limiter", "da/dbc/md_docs_2api_2middleware.html#autotoc_md153", null ],
        [ "CORS Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md154", null ],
        [ "Prometheus Metrics Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md155", null ],
        [ "JWT Auth Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md156", null ],
        [ "Compression Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md157", null ]
      ] ],
      [ "Creating Custom Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md158", null ]
    ] ],
    [ "Query & URI API", "dc/d49/md_docs_2api_2query.html", null ],
    [ "Database API", "d5/d11/md_docs_2api_2sql.html", [
      [ "Schema Migrations", "d5/d11/md_docs_2api_2sql.html#autotoc_md168", [
        [ "cwist_db_open", "d5/d11/md_docs_2api_2sql.html#autotoc_md163", null ],
        [ "cwist_db_open_memory", "d5/d11/md_docs_2api_2sql.html#autotoc_md164", null ],
        [ "cwist_db_serialize", "d5/d11/md_docs_2api_2sql.html#autotoc_md165", null ],
        [ "cwist_db_exec", "d5/d11/md_docs_2api_2sql.html#autotoc_md166", null ],
        [ "cwist_db_query", "d5/d11/md_docs_2api_2sql.html#autotoc_md167", null ],
        [ "Return codes", "d5/d11/md_docs_2api_2sql.html#autotoc_md169", null ],
        [ "cwist_migration_t", "d5/d11/md_docs_2api_2sql.html#autotoc_md170", null ],
        [ "cwist_migrate_up", "d5/d11/md_docs_2api_2sql.html#autotoc_md171", null ],
        [ "cwist_migrate_down", "d5/d11/md_docs_2api_2sql.html#autotoc_md172", null ],
        [ "cwist_migrate_version", "d5/d11/md_docs_2api_2sql.html#autotoc_md173", null ]
      ] ]
    ] ],
    [ "Server-Sent Events API", "d0/d89/md_docs_2api_2sse.html", [
      [ "Types", "d0/d89/md_docs_2api_2sse.html#autotoc_md175", [
        [ "cwist_sse_event_t", "d0/d89/md_docs_2api_2sse.html#autotoc_md176", null ],
        [ "cwist_sse_stream_t", "d0/d89/md_docs_2api_2sse.html#autotoc_md177", null ]
      ] ],
      [ "Response mode", "d0/d89/md_docs_2api_2sse.html#autotoc_md178", [
        [ "cwist_sse_response_init", "d0/d89/md_docs_2api_2sse.html#autotoc_md179", null ],
        [ "cwist_sse_response_event", "d0/d89/md_docs_2api_2sse.html#autotoc_md180", null ],
        [ "cwist_sse_response_write", "d0/d89/md_docs_2api_2sse.html#autotoc_md181", null ],
        [ "cwist_sse_response_comment", "d0/d89/md_docs_2api_2sse.html#autotoc_md182", null ]
      ] ],
      [ "Stream mode", "d0/d89/md_docs_2api_2sse.html#autotoc_md183", [
        [ "cwist_sse_stream_open", "d0/d89/md_docs_2api_2sse.html#autotoc_md184", null ],
        [ "cwist_sse_stream_send", "d0/d89/md_docs_2api_2sse.html#autotoc_md185", null ],
        [ "cwist_sse_stream_write", "d0/d89/md_docs_2api_2sse.html#autotoc_md186", null ],
        [ "cwist_sse_stream_comment", "d0/d89/md_docs_2api_2sse.html#autotoc_md187", null ],
        [ "cwist_sse_stream_close", "d0/d89/md_docs_2api_2sse.html#autotoc_md188", null ]
      ] ],
      [ "Convenience macros", "d0/d89/md_docs_2api_2sse.html#autotoc_md189", null ],
      [ "Example", "d0/d89/md_docs_2api_2sse.html#autotoc_md190", null ]
    ] ],
    [ "SString API", "db/d14/md_docs_2api_2sstring.html", null ],
    [ "WASI targets", "d6/d26/md_docs_2api_2wasi.html", [
      [ "Flavour detection", "d6/d26/md_docs_2api_2wasi.html#autotoc_md201", null ],
      [ "WASI 0.2 socket server", "d6/d26/md_docs_2api_2wasi.html#autotoc_md202", null ],
      [ "WASI preview1 (retired)", "d6/d26/md_docs_2api_2wasi.html#autotoc_md203", [
        [ "What the preview1 build did differently", "d6/d26/md_docs_2api_2wasi.html#autotoc_md204", null ],
        [ "Verification (historical)", "d6/d26/md_docs_2api_2wasi.html#autotoc_md205", null ]
      ] ]
    ] ],
    [ "CWIST on the WASM component model", "d8/d77/md_docs_2api_2wasm-component.html", [
      [ "Why", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md207", null ],
      [ "The boundary", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md208", null ],
      [ "Toolchain status (measured 2026-09-21)", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md209", null ],
      [ "Evaluated: preview3-shim (2026-09-23)", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md210", null ],
      [ "Landed: native async for streaming/SSE (2026-09-23)", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md211", null ],
      [ "Resolved: socket request path under wasip3 (audited 2026-09-22)", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md212", null ],
      [ "Stages", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md213", null ],
      [ "Browser runtime gate (WASI 0.2)", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md214", null ],
      [ "Measured while building stage 2", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md215", null ],
      [ "Non-goals", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md216", null ]
    ] ],
    [ "CWIST on WASM (Legacy Emscripten)", "d5/d0e/md_docs_2api_2wasm.html", [
      [ "Building", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md218", null ],
      [ "What is in the WASM build", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md219", null ],
      [ "Request dispatch from JS", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md220", null ],
      [ "Zero-copy views: cwist/wasm/typedarray.h", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md221", null ],
      [ "Databases under WASM", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md222", null ],
      [ "Bundle size impact of cwist_db", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md223", null ],
      [ "Sessions and cookies", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md224", null ],
      [ "Streaming through the boundary", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md225", null ],
      [ "Runnable example: Service Worker app", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md226", null ],
      [ "Not covered (yet)", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md227", null ],
      [ "Streaming producer (handlers generate the body chunk-by-chunk)", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md228", null ]
    ] ],
    [ "WebSocket API", "db/dc5/md_docs_2api_2websocket.html", [
      [ "Functions", "db/dc5/md_docs_2api_2websocket.html#autotoc_md230", [
        [ "cwist_websocket_upgrade", "db/dc5/md_docs_2api_2websocket.html#autotoc_md231", null ],
        [ "cwist_websocket_receive", "db/dc5/md_docs_2api_2websocket.html#autotoc_md232", null ],
        [ "cwist_websocket_send", "db/dc5/md_docs_2api_2websocket.html#autotoc_md233", null ]
      ] ],
      [ "Opcode Types", "db/dc5/md_docs_2api_2websocket.html#autotoc_md234", null ],
      [ "Integration with cwist_app", "db/dc5/md_docs_2api_2websocket.html#autotoc_md235", null ],
      [ "Async (C1M) WebSocket API", "db/dc5/md_docs_2api_2websocket.html#autotoc_md236", [
        [ "cwist_app_ws_async", "db/dc5/md_docs_2api_2websocket.html#autotoc_md237", null ],
        [ "cwist_ws_on_message_t", "db/dc5/md_docs_2api_2websocket.html#autotoc_md238", null ],
        [ "cwist_websocket_async_send", "db/dc5/md_docs_2api_2websocket.html#autotoc_md239", null ],
        [ "cwist_websocket_async_close", "db/dc5/md_docs_2api_2websocket.html#autotoc_md240", null ]
      ] ],
      [ "GraphQL subscriptions (graphql-ws)", "db/dc5/md_docs_2api_2websocket.html#autotoc_md241", null ]
    ] ],
    [ "Deferred HTTP ownership under full GC", "df/dc5/md_docs_2async-gc-ownership.html", [
      [ "Handoff sequence", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md243", null ],
      [ "Ownership inventory", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md244", [
        [ "Borrowed and external state", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md245", null ]
      ] ],
      [ "HTTP/2 queue ownership", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md246", null ],
      [ "Verification and limits", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md247", null ],
      [ "Scheduler and IO queue ownership", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md248", null ]
    ] ],
    [ "C1M file limits", "db/d4d/md_docs_2c1m-file-limits.html", [
      [ "Cause", "db/d4d/md_docs_2c1m-file-limits.html#autotoc_md250", null ],
      [ "Change", "db/d4d/md_docs_2c1m-file-limits.html#autotoc_md251", null ],
      [ "Tuning", "db/d4d/md_docs_2c1m-file-limits.html#autotoc_md252", null ],
      [ "Checks", "db/d4d/md_docs_2c1m-file-limits.html#autotoc_md253", null ]
    ] ],
    [ "Classic pool burst starvation (#25 follow-up)", "d9/d56/md_docs_2classic-pool-starvation.html", [
      [ "Cause and change", "d9/d56/md_docs_2classic-pool-starvation.html#autotoc_md255", null ],
      [ "Deterministic regression", "d9/d56/md_docs_2classic-pool-starvation.html#autotoc_md256", null ],
      [ "Validation status", "d9/d56/md_docs_2classic-pool-starvation.html#autotoc_md257", null ],
      [ "HTTP measurements and limits", "d9/d56/md_docs_2classic-pool-starvation.html#autotoc_md258", null ]
    ] ],
    [ "Cooperative queuing in the C1M reactor's batch drain (issue #25)", "d1/ddd/md_docs_2cooperative-queuing.html", [
      [ "Background", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md260", null ],
      [ "The gap", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md261", null ],
      [ "The fix", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md262", null ],
      [ "Measured effect", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md263", null ],
      [ "What this does not fix", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md264", null ],
      [ "Tuning", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md265", null ]
    ] ],
    [ "ADR-0001: Restore HTTP response caching through an explicit public FIXED contract", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html", [
      [ "Context", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md267", null ],
      [ "Decision", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md268", [
        [ "1. One parsed eligibility boundary", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md269", null ],
        [ "2. Cache representations, not wire responses", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md270", null ],
        [ "3. Isolate ownership, provenance and capacity", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md271", null ],
        [ "4. Make the compatibility change explicit", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md272", null ]
      ] ],
      [ "Alternatives considered", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md273", null ],
      [ "Consequences", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md274", null ],
      [ "Implementation and release gates", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md275", null ],
      [ "Evidence and source anchors", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md276", null ]
    ] ],
    [ "ADR-0002: TLS certificate hot reload keeps the session-ticket key (STEK) unchanged", "d1/dd3/md_docs_2decisions_20002-tls-hot-reload-stek.html", [
      [ "Context", "d1/dd3/md_docs_2decisions_20002-tls-hot-reload-stek.html#autotoc_md278", null ],
      [ "Decision", "d1/dd3/md_docs_2decisions_20002-tls-hot-reload-stek.html#autotoc_md279", null ],
      [ "Consequences", "d1/dd3/md_docs_2decisions_20002-tls-hot-reload-stek.html#autotoc_md280", null ]
    ] ],
    [ "Prebuilt release assets", "d1/da3/md_docs_2deployment_2release-assets.html", [
      [ "Using a fat bundle", "d1/da3/md_docs_2deployment_2release-assets.html#autotoc_md283", null ],
      [ "Using the amalgamation", "d1/da3/md_docs_2deployment_2release-assets.html#autotoc_md284", null ],
      [ "Re-running", "d1/da3/md_docs_2deployment_2release-assets.html#autotoc_md285", null ]
    ] ],
    [ "Deploy the WASI 0.2 example with Wasmtime", "db/de3/md_docs_2deployment_2wasmtime-appliance.html", [
      [ "Scope and safety", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md287", null ],
      [ "1. Build a known revision", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md288", null ],
      [ "2. Separate the artifact from state", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md289", null ],
      [ "3. Start and verify", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md290", null ],
      [ "4. Restart, back up, and restore", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md291", null ],
      [ "Before adapting this for production", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md292", null ]
    ] ],
    [ "Zero-downtime restart via socket handoff", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html", [
      [ "Requirements", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html#autotoc_md294", null ],
      [ "Handoff sequence", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html#autotoc_md295", null ],
      [ "systemd integration", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html#autotoc_md296", null ],
      [ "UDP and HTTP/3", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html#autotoc_md297", null ],
      [ "Verifying a deploy", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html#autotoc_md298", null ]
    ] ],
    [ "Durable queue required gate", "d3/dd5/md_docs_2durable-queue-gate.html", [
      [ "Local runs", "d3/dd5/md_docs_2durable-queue-gate.html#autotoc_md300", null ],
      [ "Isolated integration gate", "d3/dd5/md_docs_2durable-queue-gate.html#autotoc_md301", null ],
      [ "CI", "d3/dd5/md_docs_2durable-queue-gate.html#autotoc_md302", null ]
    ] ],
    [ "Bare-metal STM32 with CWIST", "de/d87/md_docs_2embedded_2stm32.html", [
      [ "Supported targets and validation", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md304", null ],
      [ "Build the firmware inputs", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md305", null ],
      [ "Port hooks and runtime contract", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md306", null ],
      [ "HTTP over raw lwIP", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md307", null ],
      [ "Memory sizing", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md308", [
        [ "Request arenas", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md309", null ],
        [ "Connections and responses", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md310", null ],
        [ "lwIP pools", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md311", null ]
      ] ],
      [ "HTTPS", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md312", null ],
      [ "Limitations", "de/d87/md_docs_2embedded_2stm32.html#autotoc_md313", null ]
    ] ],
    [ "FIXED endpoint caching: current behavior", "d3/ddd/md_docs_2fixed-cache-status.html", [
      [ "What the runtime actually does today", "d3/ddd/md_docs_2fixed-cache-status.html#autotoc_md315", null ],
      [ "Evidence", "d3/ddd/md_docs_2fixed-cache-status.html#autotoc_md316", null ],
      [ "Why the learning block is not simply moved", "d3/ddd/md_docs_2fixed-cache-status.html#autotoc_md317", null ],
      [ "Recommended handling", "d3/ddd/md_docs_2fixed-cache-status.html#autotoc_md318", null ]
    ] ],
    [ "Full GC: Automatic Resource Reclamation in CWIST", "d9/dd1/md_docs_2GC.html", [
      [ "Motivation", "d9/dd1/md_docs_2GC.html#autotoc_md320", null ],
      [ "Design", "d9/dd1/md_docs_2GC.html#autotoc_md321", [
        [ "1. cwist_full_gc(bool) — the global toggle", "d9/dd1/md_docs_2GC.html#autotoc_md322", null ],
        [ "2. Reclamation engine: libttak epoch GC", "d9/dd1/md_docs_2GC.html#autotoc_md323", null ],
        [ "3. Thread/process exit sweeps", "d9/dd1/md_docs_2GC.html#autotoc_md324", null ],
        [ "4. Pseudo-RAII for handle-like locals", "d9/dd1/md_docs_2GC.html#autotoc_md325", null ],
        [ "5. Transparent malloc interception", "d9/dd1/md_docs_2GC.html#autotoc_md326", null ],
        [ "6. cwist_alloc internals", "d9/dd1/md_docs_2GC.html#autotoc_md327", null ],
        [ "7. Explicit flushes and reclamation", "d9/dd1/md_docs_2GC.html#autotoc_md328", null ]
      ] ],
      [ "Semantics summary", "d9/dd1/md_docs_2GC.html#autotoc_md329", null ],
      [ "Non-goals and notes", "d9/dd1/md_docs_2GC.html#autotoc_md330", null ],
      [ "Known performance caveat", "d9/dd1/md_docs_2GC.html#autotoc_md331", null ]
    ] ],
    [ "HTTP/1.1 pipeline fairness and framing", "d2/dc2/md_docs_2http-reactor-fairness.html", [
      [ "Scope", "d2/dc2/md_docs_2http-reactor-fairness.html#autotoc_md333", null ],
      [ "Behavioral regression tests", "d2/dc2/md_docs_2http-reactor-fairness.html#autotoc_md334", null ],
      [ "Performance claim boundary", "d2/dc2/md_docs_2http-reactor-fairness.html#autotoc_md335", null ],
      [ "Internal API compatibility", "d2/dc2/md_docs_2http-reactor-fairness.html#autotoc_md336", null ]
    ] ],
    [ "HTTP/3 connection migration", "d8/d64/md_docs_2http3-migration.html", [
      [ "Result for issue #407", "d8/d64/md_docs_2http3-migration.html#autotoc_md338", null ],
      [ "Run the test", "d8/d64/md_docs_2http3-migration.html#autotoc_md339", null ],
      [ "Test coverage", "d8/d64/md_docs_2http3-migration.html#autotoc_md340", null ],
      [ "Limits", "d8/d64/md_docs_2http3-migration.html#autotoc_md341", null ]
    ] ],
    [ "런타임 설정", "d5/d08/md_docs_2ko_2runtime-config.html", [
      [ "상태", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md343", null ],
      [ "프로세스와 서버 모드", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md344", null ],
      [ "Classic 스레드 풀", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md345", null ],
      [ "HTTP/1.1", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md346", null ],
      [ "HTTPS", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md347", null ],
      [ "HTTP/2", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md348", null ],
      [ "HTTP/3", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md349", null ],
      [ "WebSocket", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md350", null ],
      [ "Reactor", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md351", null ],
      [ "진단", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md352", null ]
    ] ],
    [ "Release Codename Sequencing Specification", "d6/de8/md_docs_2ko_2versioning.html", [
      [ "1. 기본 원칙", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md354", null ],
      [ "2. 2자리 시퀀스 (기본)", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md355", null ],
      [ "3. 3자리 확장 시퀀스", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md356", null ],
      [ "4. 표기", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md357", null ],
      [ "5. API 안정성 게이트 (v4 LTS부터)", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md358", null ],
      [ "6. 메이저 버전의 첫 상승", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md359", null ]
    ] ],
    [ "HTTPS C100k gate", "d0/d49/md_docs_2performance_2https-c100k.html", [
      [ "What runs", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md361", null ],
      [ "Loopback port space", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md362", null ],
      [ "Results (2026-10-17, dev)", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md363", null ],
      [ "Results (2026-10-18, dev): C1M over TLS", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md364", null ],
      [ "Results (2026-10-11, dev)", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md365", [
        [ "Where the memory goes", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md366", null ]
      ] ],
      [ "Scaling past the gate on a single host", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md367", null ],
      [ "Running it", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md368", null ]
    ] ],
    [ "HTTPS pool task queue: static ring vs growable ring", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html", [
      [ "What changed", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md370", null ],
      [ "Setup", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md371", null ],
      [ "1. RSS of https_pool_init() alone", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md372", null ],
      [ "2. 4 workers, steady and churn load (3 alternating rounds)", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md373", null ],
      [ "3. Forcing the ring to grow: 1 worker, 2 pool threads, 6000 simultaneous connections", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md374", null ],
      [ "4. Order and loss under growth: tests/test_https_pool_queue.c", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md375", null ],
      [ "Not done / open", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md376", null ]
    ] ],
    [ "Issue #297 syscall-reduction audit (local, 12-core Ryzen 5600X)", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html", [
      [ "Baseline syscall profile", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md378", null ],
      [ "Item results", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md379", [
        [ "1.1 io_uring multishot accept — NEGATIVE (reverted)", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md380", null ],
        [ "1.2 Larger SQE batches / fewer io_uring_enter — ALREADY IMPLEMENTED / BLOCKED", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md381", null ],
        [ "1.3 Per-accept setsockopt removal — NEGATIVE (reverted)", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md382", null ],
        [ "1.4 Time caching — ALREADY IMPLEMENTED", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md383", null ]
      ] ],
      [ "What the profile says to do instead (ranked)", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md384", null ],
      [ "Method notes", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md385", null ]
    ] ],
    [ "Pluggable TLS backend (CWIST_TLS)", "d9/dda/md_docs_2performance_2tls-backends.html", [
      [ "The seam", "d9/dda/md_docs_2performance_2tls-backends.html#autotoc_md387", null ],
      [ "Forced-off features under CWIST_TLS=mbedtls", "d9/dda/md_docs_2performance_2tls-backends.html#autotoc_md388", null ],
      [ "Degraded features under CWIST_TLS=mbedtls", "d9/dda/md_docs_2performance_2tls-backends.html#autotoc_md389", null ],
      [ "Embedded bundle", "d9/dda/md_docs_2performance_2tls-backends.html#autotoc_md390", null ]
    ] ],
    [ "Opt-in wrk raw/corrected latency histograms", "d1/dfd/md_docs_2performance_2wrk-dual-histogram.html", [
      [ "Build in a disposable wrk checkout", "d1/dfd/md_docs_2performance_2wrk-dual-histogram.html#autotoc_md392", null ],
      [ "Output and interpretation", "d1/dfd/md_docs_2performance_2wrk-dual-histogram.html#autotoc_md393", null ],
      [ "Verification", "d1/dfd/md_docs_2performance_2wrk-dual-histogram.html#autotoc_md394", null ]
    ] ],
    [ "io_uring posted-work wake-up regression (#25)", "dd/d56/md_docs_2reactor-wakeup-regression.html", [
      [ "Scope and cause", "dd/d56/md_docs_2reactor-wakeup-regression.html#autotoc_md396", null ],
      [ "Regression test", "dd/d56/md_docs_2reactor-wakeup-regression.html#autotoc_md397", null ],
      [ "Component measurement (2026-09-12)", "dd/d56/md_docs_2reactor-wakeup-regression.html#autotoc_md398", null ]
    ] ],
    [ "CWIST Library Reference", "da/dca/md_docs_2REFERENCE.html", [
      [ "Modules", "da/dca/md_docs_2REFERENCE.html#autotoc_md407", [
        [ "1. SString (Smart String)", "da/dca/md_docs_2REFERENCE.html#autotoc_md408", null ],
        [ "2. HTTP Server", "da/dca/md_docs_2REFERENCE.html#autotoc_md409", null ],
        [ "3. HTTPS Support", "da/dca/md_docs_2REFERENCE.html#autotoc_md410", null ],
        [ "4. Query Parsing", "da/dca/md_docs_2REFERENCE.html#autotoc_md411", null ],
        [ "5. Database (SQL)", "da/dca/md_docs_2REFERENCE.html#autotoc_md412", null ],
        [ "6. SipHash", "da/dca/md_docs_2REFERENCE.html#autotoc_md413", null ],
        [ "7. Error Handling", "da/dca/md_docs_2REFERENCE.html#autotoc_md414", null ],
        [ "8. Big Dumb Reply (BDR)", "da/dca/md_docs_2REFERENCE.html#autotoc_md415", null ]
      ] ],
      [ "LibTTAK Memory Features", "da/dca/md_docs_2REFERENCE.html#autotoc_md416", null ],
      [ "Example: Othello Web", "da/dca/md_docs_2REFERENCE.html#autotoc_md417", null ],
      [ "Dependencies", "da/dca/md_docs_2REFERENCE.html#autotoc_md418", null ],
      [ "Build", "da/dca/md_docs_2REFERENCE.html#autotoc_md419", null ]
    ] ],
    [ "Mux Algorithm References", "d6/d9f/md_docs_2references_2mux__algorithm__references.html", [
      [ "1. Lo Shu Magic Square (Naak-seo / 洛書)", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md421", null ],
      [ "2. Orthogonal Latin-Square Permutation", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md422", null ],
      [ "3. Coefficient Matrix & Linear System Solver", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md423", null ],
      [ "4. Iterative Magnitude Refinement", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md424", null ],
      [ "5. Single-Step Linear Extrapolation", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md425", null ],
      [ "Naming Rationale", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md426", null ]
    ] ],
    [ "v3.5.0 release roadmap", "d7/df9/md_docs_2roadmap-v3_85.html", [
      [ "Scope", "d7/df9/md_docs_2roadmap-v3_85.html#autotoc_md428", null ],
      [ "Release gates", "d7/df9/md_docs_2roadmap-v3_85.html#autotoc_md429", null ],
      [ "Evidence and limits", "d7/df9/md_docs_2roadmap-v3_85.html#autotoc_md430", null ]
    ] ],
    [ "Runtime configuration", "d9/dfa/md_docs_2runtime-config.html", [
      [ "Status", "d9/dfa/md_docs_2runtime-config.html#autotoc_md432", null ],
      [ "Processes and server mode", "d9/dfa/md_docs_2runtime-config.html#autotoc_md433", null ],
      [ "Classic thread pool", "d9/dfa/md_docs_2runtime-config.html#autotoc_md434", null ],
      [ "HTTP/1.1", "d9/dfa/md_docs_2runtime-config.html#autotoc_md435", null ],
      [ "HTTPS", "d9/dfa/md_docs_2runtime-config.html#autotoc_md436", null ],
      [ "HTTP/2", "d9/dfa/md_docs_2runtime-config.html#autotoc_md437", null ],
      [ "HTTP/3", "d9/dfa/md_docs_2runtime-config.html#autotoc_md438", null ],
      [ "WebSocket", "d9/dfa/md_docs_2runtime-config.html#autotoc_md439", null ],
      [ "Reactor", "d9/dfa/md_docs_2runtime-config.html#autotoc_md440", null ],
      [ "Diagnostics", "d9/dfa/md_docs_2runtime-config.html#autotoc_md441", null ]
    ] ],
    [ "Soak testing", "db/d05/md_docs_2soak-testing.html", [
      [ "Environment", "db/d05/md_docs_2soak-testing.html#autotoc_md443", null ],
      [ "Bench server and workload mix", "db/d05/md_docs_2soak-testing.html#autotoc_md444", null ],
      [ "Measurement points", "db/d05/md_docs_2soak-testing.html#autotoc_md445", null ],
      [ "PASS/FAIL criteria", "db/d05/md_docs_2soak-testing.html#autotoc_md446", null ],
      [ "Interpreting a failure", "db/d05/md_docs_2soak-testing.html#autotoc_md447", null ]
    ] ],
    [ "CWIST Web Development Tutorial (A to Z)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html", [
      [ "Table of Contents", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md449", null ],
      [ "1. Hello World: Starting Your First Server", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md450", null ],
      [ "2. Routing and Handlers (Mux Router)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md451", null ],
      [ "3. JSON Parsing and Zod Schema Validation", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md452", null ],
      [ "4. Middleware (CORS, Logging)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md453", null ],
      [ "5. Databases and Migrations (SQLite)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md454", null ],
      [ "6. Authentication (JWT and DB Encryption)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md455", null ],
      [ "7. PQC TLS (Post-Quantum Hybrid Key Exchange)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md456", [
        [ "Security Policy Summary", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md457", null ]
      ] ],
      [ "8. WebSocket Integration (Real-Time Bidirectional Communication)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md458", null ],
      [ "9. Template Engine and Static File Serving", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md459", null ],
      [ "10. Dynamic CSS Composer (WASM and SSR)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md460", [
        [ "Approach A: 100% Server-Side Rendering (SSR)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md461", null ],
        [ "Approach B: Client-Side Composition with React + WebAssembly (WASM)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md462", null ],
        [ "Wrapping Up", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md463", null ]
      ] ]
    ] ],
    [ "CWIST 웹 개발 튜토리얼 (A to Z)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html", [
      [ "목차", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md465", null ],
      [ "1. Hello World: 첫 서버 띄우기", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md466", null ],
      [ "2. 라우팅과 핸들러 (Mux Router)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md467", null ],
      [ "3. JSON 파싱과 Zod 스키마 검증", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md468", null ],
      [ "4. 미들웨어 (CORS, Logging)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md469", null ],
      [ "5. 데이터베이스와 마이그레이션 (SQLite)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md470", null ],
      [ "6. 인증 (JWT 및 DB 암호화)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md471", null ],
      [ "7. PQC TLS (양자내성 하이브리드 키 교환)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md472", [
        [ "보안 정책 요약", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md473", null ]
      ] ],
      [ "8. 웹소켓 연동 (실시간 양방향 통신)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md474", null ],
      [ "9. 템플릿 엔진과 정적 파일 제공", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md475", null ],
      [ "10. 동적 CSS 합성기 (WASM 및 SSR)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md476", [
        [ "방법 A: 100% Server-Side Rendering (SSR)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md477", null ],
        [ "방법 B: React + WebAssembly (WASM) 클라이언트 사이드 합성", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md478", null ],
        [ "마치며", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md479", null ]
      ] ]
    ] ],
    [ "Build a CRUD blog with CWIST", "d4/db0/md_docs_2tutorials_2blog-crud.html", [
      [ "1. Create the application", "d4/db0/md_docs_2tutorials_2blog-crud.html#autotoc_md481", null ],
      [ "2. Define the routes", "d4/db0/md_docs_2tutorials_2blog-crud.html#autotoc_md482", null ],
      [ "3. Validate JSON and return HTTP semantics", "d4/db0/md_docs_2tutorials_2blog-crud.html#autotoc_md483", null ],
      [ "4. Test the API", "d4/db0/md_docs_2tutorials_2blog-crud.html#autotoc_md484", null ]
    ] ],
    [ "Integrate NATS with CWIST", "da/d48/md_docs_2tutorials_2nats-integration.html", [
      [ "1. Separate request and worker responsibilities", "da/d48/md_docs_2tutorials_2nats-integration.html#autotoc_md486", null ],
      [ "2. Publish from a route", "da/d48/md_docs_2tutorials_2nats-integration.html#autotoc_md487", null ],
      [ "3. Reliability checklist", "da/d48/md_docs_2tutorials_2nats-integration.html#autotoc_md488", null ]
    ] ],
    [ "Document an API with OpenAPI and Swagger UI", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html", [
      [ "1. Annotate the routes", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html#autotoc_md490", [
        [ "Annotation tags", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html#autotoc_md491", null ]
      ] ],
      [ "2. Generate openapi.json", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html#autotoc_md492", null ],
      [ "3. Serve the UI", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html#autotoc_md493", null ]
    ] ],
    [ "Build a WebTransport server with CWIST", "d2/dcb/md_docs_2tutorials_2webtransport-server.html", [
      [ "Build", "d2/dcb/md_docs_2tutorials_2webtransport-server.html#autotoc_md495", null ],
      [ "1. Configure HTTP/3 and a session handler", "d2/dcb/md_docs_2tutorials_2webtransport-server.html#autotoc_md496", null ],
      [ "2. Design the session protocol", "d2/dcb/md_docs_2tutorials_2webtransport-server.html#autotoc_md497", null ],
      [ "3. Operate safely", "d2/dcb/md_docs_2tutorials_2webtransport-server.html#autotoc_md498", null ]
    ] ],
    [ "Release Codename Sequencing Specification", "d0/db4/md_docs_2versioning.html", [
      [ "1. Principles", "d0/db4/md_docs_2versioning.html#autotoc_md500", null ],
      [ "2. Two-letter sequence (default)", "d0/db4/md_docs_2versioning.html#autotoc_md501", null ],
      [ "3. Three-letter extension", "d0/db4/md_docs_2versioning.html#autotoc_md502", null ],
      [ "4. Notation", "d0/db4/md_docs_2versioning.html#autotoc_md503", null ],
      [ "5. API stability gate (from the v4 LTS)", "d0/db4/md_docs_2versioning.html#autotoc_md504", null ],
      [ "6. First ascent of a major version", "d0/db4/md_docs_2versioning.html#autotoc_md505", null ]
    ] ],
    [ "Web Server Benchmark Methodology", "d6/d42/md_docs_2webserver-benchmark.html", [
      [ "Measurement contract v2", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md507", null ],
      [ "Admission and latency semantics", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md508", null ],
      [ "Resource measurements", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md509", [
        [ "Interpreting the context-switch column", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md510", null ]
      ] ],
      [ "Runtime configuration", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md511", null ],
      [ "Provenance, presentation and legacy results", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md512", null ]
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
"d0/d89/md_docs_2api_2sse.html#autotoc_md183",
"d1/d7a/net_2http_2query_8h.html#a131c400794b813dbab329e32457049c6",
"d2/db0/http2_8c.html#a3ee11404022960b1faf0b91ff21ddf46",
"d3/d4c/grpc__channel_8h.html#a99bc36269ecfc3111d28c5fd891f9706",
"d4/d48/db__sync_8c.html",
"d5/d11/md_docs_2api_2sql.html#autotoc_md169",
"d5/df8/json__builder_8h_source.html",
"d6/dbb/grpc_8h.html#ae6d54ac7b7f38f562c3b13a4b82c59d6",
"d7/dba/structcwist__fix__server__mem.html#a113ff4343f4d9e628b0d2871cc13f7c5",
"d9/d16/http2__flow__control_8c.html",
"d9/dab/net_2http_2http_8h.html#a99b5ad0e71431205a397a336983f4f4dac46b4fd5f0cc8a4ad5580df3d6418303",
"da/d0c/arena_8h.html#ac30d670b1d371d79c081756c226b000d",
"da/dec/core_2macros_8h.html#aef36010bc28946b5b8dd50b79cae8da5",
"db/db7/pool_8h.html#aa1e09018ceaf9e09828563d70e26a5fa",
"dc/d91/structcwist__mux__route.html#a7cd82ac83249cfe5d35321172f95af9b",
"dd/d43/http_8c.html#a546e727db4f5d4420f59f7cc41c0e382",
"dd/d7f/query_8c.html#a2df19e2544e3de48d2340412b2e03f32",
"de/d91/async_8h.html#a06d2294a5d48650ba613d6a608858b14",
"df/d6d/ssl_8h.html#a6d72286382ff2d3d13954e512d23165f",
"df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html",
"index.html#autotoc_md16"
];

var SYNCONMSG = 'click to disable panel synchronisation';
var SYNCOFFMSG = 'click to enable panel synchronisation';