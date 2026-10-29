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
    [ "CWIST API 레퍼런스", "d3/d38/md_docs_2ko_2api-quickref.html", [
      [ "에러 처리", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md343", [
        [ "make_error", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md344", null ],
        [ "cwist_error_is_ok / cwist_error_is_ok_extern", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md345", null ],
        [ "그 밖에 내보낸 inline 도우미", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md346", null ]
      ] ],
      [ "SString (cwist_sstring)", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md347", [
        [ "수명 주기", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md348", null ],
        [ "핵심 도우미", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md349", null ],
        [ "공백 제거", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md350", null ],
        [ "덧붙이기 / 복사", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md351", null ],
        [ "비교 / 조회", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md352", null ]
      ] ],
      [ "HTTP", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md353", [
        [ "요청 수명 주기", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md354", null ],
        [ "응답 수명 주기", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md355", null ],
        [ "헤더 도우미", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md356", null ],
        [ "메서드 도우미", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md357", null ],
        [ "소켓 도우미", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md358", null ]
      ] ],
      [ "세션 매니저", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md359", [
        [ "아레나", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md360", null ],
        [ "공유 세션 (침습적 참조 카운트)", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md361", null ],
        [ "매니저", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md362", null ]
      ] ]
    ] ],
    [ "CWIST API 문서", "dc/d79/md_docs_2ko_2API.html", [
      [ "모듈", "dc/d79/md_docs_2ko_2API.html#autotoc_md364", null ],
      [ "함께 보기", "dc/d79/md_docs_2ko_2API.html#autotoc_md365", null ]
    ] ],
    [ "프레임워크와 앱 API", "da/dc3/md_docs_2ko_2api_2app.html", [
      [ "라우팅", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md372", [
        [ "cwist_app_create", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md367", null ],
        [ "cwist_app_use_https", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md368", null ],
        [ "cwist_app_use_https2 / cwist_use_https2", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md369", null ],
        [ "cwist_app_use_pqc_layer", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md370", null ],
        [ "cwist_app_use_db", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md371", null ],
        [ "cwist_app_get / cwist_app_post", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md373", null ],
        [ "cwist_app_get_ex / _post_ex / _put_ex / _delete_ex / _patch_ex", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md374", null ],
        [ "cwist_app_dispatch_memory", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md375", null ],
        [ "cwist_app_ws", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md376", null ],
        [ "cwist_endpoint_opt_t", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md377", null ]
      ] ],
      [ "정적 자산", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md378", [
        [ "cwist_app_static", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md379", null ]
      ] ],
      [ "에러 처리", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md380", [
        [ "cwist_app_set_error_handler", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md381", null ],
        [ "사용하지 않는 변수", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md382", null ]
      ] ],
      [ "동시성과 스레드 안전성", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md383", null ],
      [ "메모리 소유권 규칙", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md384", [
        [ "cwist_app_listen", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md385", null ]
      ] ],
      [ "인메모리 테스트 클라이언트", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md386", [
        [ "cwist_test_client_create / cwist_test_client_destroy", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md387", null ],
        [ "요청 도우미", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md388", null ],
        [ "쿠키 저장소", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md389", null ],
        [ "테스트 단언 매크로", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md390", null ]
      ] ]
    ] ],
    [ "쿠키 API", "db/d04/md_docs_2ko_2api_2cookie.html", [
      [ "쿠키 읽기", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md392", [
        [ "cwist_cookie_parse", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md393", null ],
        [ "cwist_cookie_get", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md394", null ]
      ] ],
      [ "쿠키 쓰기", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md395", [
        [ "cwist_cookie_options", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md396", null ],
        [ "cwist_cookie_set", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md397", null ],
        [ "cwist_cookie_delete", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md398", null ]
      ] ],
      [ "인코딩", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md399", [
        [ "cwist_cookie_encode", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md400", null ],
        [ "cwist_cookie_decode", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md401", null ]
      ] ],
      [ "예제", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md402", null ]
    ] ],
    [ "HTML 컴포넌트, 범위 CSS, 프래그먼트와 자산", "d1/df9/md_docs_2ko_2api_2html.html", [
      [ "컴포넌트", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md404", [
        [ "cwist_html_component_create / cwist_html_component_destroy", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md405", null ],
        [ "cwist_html_component_instantiate", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md406", null ]
      ] ],
      [ "범위 CSS", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md407", [
        [ "cwist_css_scope_init / cwist_css_scope_destroy", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md408", null ],
        [ "cwist_css_scope_class", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md409", null ],
        [ "cwist_css_scope_add_rule", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md410", null ],
        [ "cwist_css_scope_generate_stylesheet", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md411", null ]
      ] ],
      [ "HTML over the wire", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md412", [
        [ "cwist_http_request_wants_fragment / cwist_http_request_fragment_target", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md413", null ],
        [ "cwist_http_response_set_view", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md414", null ],
        [ "cwist_http_response_set_html", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md415", null ],
        [ "cwist_http_response_add_oob", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md416", null ],
        [ "cwist_http_response_html_redirect", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md417", null ],
        [ "핸들러 예제", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md418", null ]
      ] ],
      [ "자산 파이프라인", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md419", [
        [ "cwist_css_minify / cwist_css_bundle", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md420", null ],
        [ "cwist_app_asset_add / cwist_app_asset_add_file / cwist_app_asset_url", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md421", null ],
        [ "모두 합치기", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md422", null ]
      ] ],
      [ "WASM에서 같은 뷰 렌더링하기", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md423", null ],
      [ "예제", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md424", null ]
    ] ],
    [ "HTTP 코어 API", "db/ddd/md_docs_2ko_2api_2http.html", [
      [ "상수", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md426", [
        [ "cwist_http_request_create", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md427", null ],
        [ "cwist_http_parse_request", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md428", null ],
        [ "cwist_http_receive_request", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md429", null ],
        [ "cwist_http_response_create", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md430", null ],
        [ "cwist_http_response_send_file", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md431", null ],
        [ "cwist_http_header_add", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md432", null ],
        [ "cwist_http_response_header_add", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md433", null ],
        [ "cwist_http_header_get", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md434", null ]
      ] ],
      [ "서버 코어", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md435", [
        [ "Keep-alive 처리", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md436", null ],
        [ "회귀 테스트", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md437", null ],
        [ "cwist_http_server_loop", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md438", null ],
        [ "cwist_accept_socket", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md439", null ],
        [ "cwist_http_continuation_shed_count", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md440", null ]
      ] ],
      [ "보안 헤더", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md441", [
        [ "cwist_http_response_add_security_headers", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md442", null ],
        [ "cwist_http_response_add_hsts", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md443", null ]
      ] ]
    ] ],
    [ "HTTPS API", "d2/d2f/md_docs_2ko_2api_2https.html", null ],
    [ "JSON 빌더 API", "d8/d3f/md_docs_2ko_2api_2json.html", [
      [ "함수", "d8/d3f/md_docs_2ko_2api_2json.html#autotoc_md452", [
        [ "cwist_json_builder_create", "d8/d3f/md_docs_2ko_2api_2json.html#autotoc_md453", null ],
        [ "cwist_json_begin_object / cwist_json_end_object", "d8/d3f/md_docs_2ko_2api_2json.html#autotoc_md454", null ],
        [ "cwist_json_add_string", "d8/d3f/md_docs_2ko_2api_2json.html#autotoc_md455", null ],
        [ "cwist_json_add_int", "d8/d3f/md_docs_2ko_2api_2json.html#autotoc_md456", null ],
        [ "cwist_json_add_bool", "d8/d3f/md_docs_2ko_2api_2json.html#autotoc_md457", null ],
        [ "cwist_json_add_null", "d8/d3f/md_docs_2ko_2api_2json.html#autotoc_md458", null ],
        [ "cwist_json_begin_array / cwist_json_end_array", "d8/d3f/md_docs_2ko_2api_2json.html#autotoc_md459", null ],
        [ "cwist_json_get_raw", "d8/d3f/md_docs_2ko_2api_2json.html#autotoc_md460", null ],
        [ "cwist_json_builder_destroy", "d8/d3f/md_docs_2ko_2api_2json.html#autotoc_md461", null ]
      ] ],
      [ "예제", "d8/d3f/md_docs_2ko_2api_2json.html#autotoc_md462", null ]
    ] ],
    [ "미들웨어 API", "d5/d3e/md_docs_2ko_2api_2middleware.html", [
      [ "함수", "d5/d3e/md_docs_2ko_2api_2middleware.html#autotoc_md464", [
        [ "cwist_app_use", "d5/d3e/md_docs_2ko_2api_2middleware.html#autotoc_md465", null ]
      ] ],
      [ "내장 미들웨어", "d5/d3e/md_docs_2ko_2api_2middleware.html#autotoc_md466", [
        [ "Request ID 미들웨어", "d5/d3e/md_docs_2ko_2api_2middleware.html#autotoc_md467", null ],
        [ "액세스 로그 미들웨어", "d5/d3e/md_docs_2ko_2api_2middleware.html#autotoc_md468", [
          [ "비동기 로그 출력", "d5/d3e/md_docs_2ko_2api_2middleware.html#autotoc_md469", null ]
        ] ],
        [ "속도 제한 미들웨어", "d5/d3e/md_docs_2ko_2api_2middleware.html#autotoc_md470", null ],
        [ "Redis 기반 공유 rate limiter", "d5/d3e/md_docs_2ko_2api_2middleware.html#autotoc_md471", null ],
        [ "CORS 미들웨어", "d5/d3e/md_docs_2ko_2api_2middleware.html#autotoc_md472", null ],
        [ "Prometheus 메트릭 미들웨어", "d5/d3e/md_docs_2ko_2api_2middleware.html#autotoc_md473", null ],
        [ "JWT 인증 미들웨어", "d5/d3e/md_docs_2ko_2api_2middleware.html#autotoc_md474", null ],
        [ "압축 미들웨어", "d5/d3e/md_docs_2ko_2api_2middleware.html#autotoc_md475", null ]
      ] ],
      [ "사용자 정의 미들웨어 만들기", "d5/d3e/md_docs_2ko_2api_2middleware.html#autotoc_md476", null ]
    ] ],
    [ "쿼리와 URI API", "dd/da8/md_docs_2ko_2api_2query.html", null ],
    [ "데이터베이스 API", "d5/d75/md_docs_2ko_2api_2sql.html", [
      [ "스키마 마이그레이션", "d5/d75/md_docs_2ko_2api_2sql.html#autotoc_md486", [
        [ "cwist_db_open", "d5/d75/md_docs_2ko_2api_2sql.html#autotoc_md481", null ],
        [ "cwist_db_open_memory", "d5/d75/md_docs_2ko_2api_2sql.html#autotoc_md482", null ],
        [ "cwist_db_serialize", "d5/d75/md_docs_2ko_2api_2sql.html#autotoc_md483", null ],
        [ "cwist_db_exec", "d5/d75/md_docs_2ko_2api_2sql.html#autotoc_md484", null ],
        [ "cwist_db_query", "d5/d75/md_docs_2ko_2api_2sql.html#autotoc_md485", null ],
        [ "반환 코드", "d5/d75/md_docs_2ko_2api_2sql.html#autotoc_md487", null ],
        [ "cwist_migration_t", "d5/d75/md_docs_2ko_2api_2sql.html#autotoc_md488", null ],
        [ "cwist_migrate_up", "d5/d75/md_docs_2ko_2api_2sql.html#autotoc_md489", null ],
        [ "cwist_migrate_down", "d5/d75/md_docs_2ko_2api_2sql.html#autotoc_md490", null ],
        [ "cwist_migrate_version", "d5/d75/md_docs_2ko_2api_2sql.html#autotoc_md491", null ]
      ] ]
    ] ],
    [ "Server-Sent Events API", "d3/df7/md_docs_2ko_2api_2sse.html", [
      [ "타입", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md493", [
        [ "cwist_sse_event_t", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md494", null ],
        [ "cwist_sse_stream_t", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md495", null ]
      ] ],
      [ "응답 모드", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md496", [
        [ "cwist_sse_response_init", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md497", null ],
        [ "cwist_sse_response_event", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md498", null ],
        [ "cwist_sse_response_write", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md499", null ],
        [ "cwist_sse_response_comment", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md500", null ]
      ] ],
      [ "스트림 모드", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md501", [
        [ "cwist_sse_stream_open", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md502", null ],
        [ "cwist_sse_stream_send", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md503", null ],
        [ "cwist_sse_stream_write", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md504", null ],
        [ "cwist_sse_stream_comment", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md505", null ],
        [ "cwist_sse_stream_close", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md506", null ]
      ] ],
      [ "편의 매크로", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md507", null ],
      [ "예제", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md508", null ]
    ] ],
    [ "SString API", "d6/d66/md_docs_2ko_2api_2sstring.html", null ],
    [ "WASI 대상", "d0/d3c/md_docs_2ko_2api_2wasi.html", [
      [ "종류 감지", "d0/d3c/md_docs_2ko_2api_2wasi.html#autotoc_md519", null ],
      [ "WASI 0.2 소켓 서버", "d0/d3c/md_docs_2ko_2api_2wasi.html#autotoc_md520", null ],
      [ "WASI preview1 (폐기됨)", "d0/d3c/md_docs_2ko_2api_2wasi.html#autotoc_md521", [
        [ "preview1 빌드가 다르게 했던 것", "d0/d3c/md_docs_2ko_2api_2wasi.html#autotoc_md522", null ],
        [ "검증 (기록)", "d0/d3c/md_docs_2ko_2api_2wasi.html#autotoc_md523", null ]
      ] ]
    ] ],
    [ "WASM 컴포넌트 모델 위의 CWIST", "dd/d41/md_docs_2ko_2api_2wasm-component.html", [
      [ "이유", "dd/d41/md_docs_2ko_2api_2wasm-component.html#autotoc_md525", null ],
      [ "경계", "dd/d41/md_docs_2ko_2api_2wasm-component.html#autotoc_md526", null ],
      [ "도구 체인 현황 (2026-09-21 측정)", "dd/d41/md_docs_2ko_2api_2wasm-component.html#autotoc_md527", null ],
      [ "평가 완료: preview3-shim (2026-09-23)", "dd/d41/md_docs_2ko_2api_2wasm-component.html#autotoc_md528", null ],
      [ "반영 완료: 스트리밍/SSE를 위한 네이티브 비동기 (2026-09-23)", "dd/d41/md_docs_2ko_2api_2wasm-component.html#autotoc_md529", null ],
      [ "해결됨: wasip3에서의 소켓 요청 경로 (2026-09-22 감사)", "dd/d41/md_docs_2ko_2api_2wasm-component.html#autotoc_md530", null ],
      [ "단계", "dd/d41/md_docs_2ko_2api_2wasm-component.html#autotoc_md531", null ],
      [ "브라우저 런타임 관문 (WASI 0.2)", "dd/d41/md_docs_2ko_2api_2wasm-component.html#autotoc_md532", null ],
      [ "2단계를 만들며 측정한 것", "dd/d41/md_docs_2ko_2api_2wasm-component.html#autotoc_md533", null ],
      [ "목표가 아닌 것", "dd/d41/md_docs_2ko_2api_2wasm-component.html#autotoc_md534", null ]
    ] ],
    [ "WASM 위의 CWIST (Emscripten)", "d2/d56/md_docs_2ko_2api_2wasm.html", [
      [ "빌드하기", "d2/d56/md_docs_2ko_2api_2wasm.html#autotoc_md536", null ],
      [ "WASM 빌드에 들어 있는 것", "d2/d56/md_docs_2ko_2api_2wasm.html#autotoc_md537", null ],
      [ "JS에서 요청 디스패치하기", "d2/d56/md_docs_2ko_2api_2wasm.html#autotoc_md538", null ],
      [ "제로 카피 뷰: cwist/wasm/typedarray.h", "d2/d56/md_docs_2ko_2api_2wasm.html#autotoc_md539", null ],
      [ "WASM에서의 데이터베이스", "d2/d56/md_docs_2ko_2api_2wasm.html#autotoc_md540", null ],
      [ "cwist_db가 번들 크기에 주는 영향", "d2/d56/md_docs_2ko_2api_2wasm.html#autotoc_md541", null ],
      [ "세션과 쿠키", "d2/d56/md_docs_2ko_2api_2wasm.html#autotoc_md542", null ],
      [ "경계를 통과하는 스트리밍", "d2/d56/md_docs_2ko_2api_2wasm.html#autotoc_md543", null ],
      [ "실행 가능한 예제: Service Worker 앱", "d2/d56/md_docs_2ko_2api_2wasm.html#autotoc_md544", null ],
      [ "(아직) 다루지 않는 것", "d2/d56/md_docs_2ko_2api_2wasm.html#autotoc_md545", null ],
      [ "스트리밍 생산자 (핸들러가 본문을 청크 단위로 생성)", "d2/d56/md_docs_2ko_2api_2wasm.html#autotoc_md546", null ]
    ] ],
    [ "WebSocket API", "d7/dec/md_docs_2ko_2api_2websocket.html", [
      [ "함수", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md548", [
        [ "cwist_websocket_upgrade", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md549", null ],
        [ "cwist_websocket_receive", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md550", null ],
        [ "cwist_websocket_send", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md551", null ]
      ] ],
      [ "Opcode 종류", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md552", null ],
      [ "cwist_app과의 연동", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md553", null ],
      [ "비동기 (C1M) WebSocket API", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md554", [
        [ "cwist_app_ws_async", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md555", null ],
        [ "cwist_ws_on_message_t", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md556", null ],
        [ "cwist_websocket_async_send", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md557", null ],
        [ "cwist_websocket_async_close", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md558", null ]
      ] ],
      [ "GraphQL 구독 (graphql-ws)", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md559", null ]
    ] ],
    [ "full GC에서의 연기된 HTTP 소유권", "d3/dcc/md_docs_2ko_2async-gc-ownership.html", [
      [ "인계 순서", "d3/dcc/md_docs_2ko_2async-gc-ownership.html#autotoc_md561", null ],
      [ "소유권 목록", "d3/dcc/md_docs_2ko_2async-gc-ownership.html#autotoc_md562", [
        [ "빌린 상태와 외부 상태", "d3/dcc/md_docs_2ko_2async-gc-ownership.html#autotoc_md563", null ]
      ] ],
      [ "HTTP/2 큐 소유권", "d3/dcc/md_docs_2ko_2async-gc-ownership.html#autotoc_md564", null ],
      [ "검증과 한계", "d3/dcc/md_docs_2ko_2async-gc-ownership.html#autotoc_md565", null ],
      [ "스케줄러와 IO 큐 소유권", "d3/dcc/md_docs_2ko_2async-gc-ownership.html#autotoc_md566", null ]
    ] ],
    [ "C1M 파일 한도", "db/dc4/md_docs_2ko_2c1m-file-limits.html", [
      [ "원인", "db/dc4/md_docs_2ko_2c1m-file-limits.html#autotoc_md568", null ],
      [ "변경", "db/dc4/md_docs_2ko_2c1m-file-limits.html#autotoc_md569", null ],
      [ "조정", "db/dc4/md_docs_2ko_2c1m-file-limits.html#autotoc_md570", null ],
      [ "검증", "db/dc4/md_docs_2ko_2c1m-file-limits.html#autotoc_md571", null ]
    ] ],
    [ "Classic 풀의 버스트 기아 (#25 후속)", "da/d07/md_docs_2ko_2classic-pool-starvation.html", [
      [ "원인과 변경", "da/d07/md_docs_2ko_2classic-pool-starvation.html#autotoc_md573", null ],
      [ "결정론적 회귀 테스트", "da/d07/md_docs_2ko_2classic-pool-starvation.html#autotoc_md574", null ],
      [ "검증 상태", "da/d07/md_docs_2ko_2classic-pool-starvation.html#autotoc_md575", null ],
      [ "HTTP 측정과 한계", "da/d07/md_docs_2ko_2classic-pool-starvation.html#autotoc_md576", null ]
    ] ],
    [ "C1M 리액터 묶음 비우기에서의 협력적 큐잉 (이슈 #25)", "da/d90/md_docs_2ko_2cooperative-queuing.html", [
      [ "배경", "da/d90/md_docs_2ko_2cooperative-queuing.html#autotoc_md578", null ],
      [ "빈틈", "da/d90/md_docs_2ko_2cooperative-queuing.html#autotoc_md579", null ],
      [ "수정", "da/d90/md_docs_2ko_2cooperative-queuing.html#autotoc_md580", null ],
      [ "측정된 효과", "da/d90/md_docs_2ko_2cooperative-queuing.html#autotoc_md581", null ],
      [ "이 수정이 해결하지 않는 것", "da/d90/md_docs_2ko_2cooperative-queuing.html#autotoc_md582", null ],
      [ "조정", "da/d90/md_docs_2ko_2cooperative-queuing.html#autotoc_md583", null ]
    ] ],
    [ "ADR-0001: 명시적인 공개 FIXED 계약으로 HTTP 응답 캐싱 복원하기", "d6/dda/md_docs_2ko_2decisions_20001-safe-public-fixed-response-cache.html", [
      [ "배경", "d6/dda/md_docs_2ko_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md585", null ],
      [ "결정", "d6/dda/md_docs_2ko_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md586", [
        [ "1. 하나의 파싱된 자격 경계", "d6/dda/md_docs_2ko_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md587", null ],
        [ "2. 와이어 응답이 아니라 표현을 캐시하기", "d6/dda/md_docs_2ko_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md588", null ],
        [ "3. 소유권, 출처, 용량 격리하기", "d6/dda/md_docs_2ko_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md589", null ],
        [ "4. 호환성 변경을 명시하기", "d6/dda/md_docs_2ko_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md590", null ]
      ] ],
      [ "검토한 대안", "d6/dda/md_docs_2ko_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md591", null ],
      [ "결과", "d6/dda/md_docs_2ko_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md592", null ],
      [ "구현과 릴리스 관문", "d6/dda/md_docs_2ko_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md593", null ],
      [ "근거와 소스 기준점", "d6/dda/md_docs_2ko_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md594", null ]
    ] ],
    [ "릴리스 사전 빌드 에셋", "da/dcd/md_docs_2ko_2deployment_2release-assets.html", [
      [ "fat 번들 사용하기", "da/dcd/md_docs_2ko_2deployment_2release-assets.html#autotoc_md596", null ],
      [ "amalgamation 사용하기", "da/dcd/md_docs_2ko_2deployment_2release-assets.html#autotoc_md597", null ],
      [ "다시 실행하기", "da/dcd/md_docs_2ko_2deployment_2release-assets.html#autotoc_md598", null ]
    ] ],
    [ "Wasmtime으로 WASI 0.2 예제 배포하기", "d1/dc3/md_docs_2ko_2deployment_2wasmtime-appliance.html", [
      [ "범위와 안전", "d1/dc3/md_docs_2ko_2deployment_2wasmtime-appliance.html#autotoc_md600", null ],
      [ "1. 알려진 리비전 빌드하기", "d1/dc3/md_docs_2ko_2deployment_2wasmtime-appliance.html#autotoc_md601", null ],
      [ "2. 산출물과 상태 분리하기", "d1/dc3/md_docs_2ko_2deployment_2wasmtime-appliance.html#autotoc_md602", null ],
      [ "3. 시작하고 검증하기", "d1/dc3/md_docs_2ko_2deployment_2wasmtime-appliance.html#autotoc_md603", null ],
      [ "4. 재시작, 백업, 복원", "d1/dc3/md_docs_2ko_2deployment_2wasmtime-appliance.html#autotoc_md604", null ],
      [ "프로덕션에 맞게 고치기 전에", "d1/dc3/md_docs_2ko_2deployment_2wasmtime-appliance.html#autotoc_md605", null ]
    ] ],
    [ "소켓 핸드오프로 무중단 재시작하기", "d1/d45/md_docs_2ko_2deployment_2zero-downtime-restart.html", [
      [ "요구 사항", "d1/d45/md_docs_2ko_2deployment_2zero-downtime-restart.html#autotoc_md607", null ],
      [ "핸드오프 순서", "d1/d45/md_docs_2ko_2deployment_2zero-downtime-restart.html#autotoc_md608", null ],
      [ "systemd 통합", "d1/d45/md_docs_2ko_2deployment_2zero-downtime-restart.html#autotoc_md609", null ],
      [ "UDP와 HTTP/3", "d1/d45/md_docs_2ko_2deployment_2zero-downtime-restart.html#autotoc_md610", null ],
      [ "배포 검증", "d1/d45/md_docs_2ko_2deployment_2zero-downtime-restart.html#autotoc_md611", null ]
    ] ],
    [ "영속 큐 필수 게이트", "dd/dd4/md_docs_2ko_2durable-queue-gate.html", [
      [ "로컬 실행", "dd/dd4/md_docs_2ko_2durable-queue-gate.html#autotoc_md613", null ],
      [ "격리된 통합 게이트", "dd/dd4/md_docs_2ko_2durable-queue-gate.html#autotoc_md614", null ],
      [ "CI", "dd/dd4/md_docs_2ko_2durable-queue-gate.html#autotoc_md615", null ]
    ] ],
    [ "CWIST bare-metal STM32", "d4/dc9/md_docs_2ko_2embedded_2stm32.html", [
      [ "지원 대상과 검증 범위", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md617", null ],
      [ "펌웨어 입력 빌드", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md618", null ],
      [ "포트 훅과 런타임 계약", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md619", null ],
      [ "raw lwIP HTTP", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md620", null ],
      [ "메모리 크기", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md621", [
        [ "요청 arena", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md622", null ],
        [ "연결과 응답", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md623", null ],
        [ "lwIP pool", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md624", null ]
      ] ],
      [ "HTTPS", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md625", null ],
      [ "제한사항", "d4/dc9/md_docs_2ko_2embedded_2stm32.html#autotoc_md626", null ]
    ] ],
    [ "FIXED 엔드포인트 캐싱: 현재 동작", "d7/dab/md_docs_2ko_2fixed-cache-status.html", [
      [ "지금 런타임이 실제로 하는 일", "d7/dab/md_docs_2ko_2fixed-cache-status.html#autotoc_md628", null ],
      [ "근거", "d7/dab/md_docs_2ko_2fixed-cache-status.html#autotoc_md629", null ],
      [ "학습 블록을 그냥 옮기지 않는 이유", "d7/dab/md_docs_2ko_2fixed-cache-status.html#autotoc_md630", null ],
      [ "계약이 복원될 때까지의 권장 처리", "d7/dab/md_docs_2ko_2fixed-cache-status.html#autotoc_md631", null ]
    ] ],
    [ "Full GC: CWIST의 자동 자원 회수", "d1/dde/md_docs_2ko_2GC.html", [
      [ "동기", "d1/dde/md_docs_2ko_2GC.html#autotoc_md633", null ],
      [ "설계", "d1/dde/md_docs_2ko_2GC.html#autotoc_md634", [
        [ "1. cwist_full_gc(bool) - 전역 스위치", "d1/dde/md_docs_2ko_2GC.html#autotoc_md635", null ],
        [ "2. 회수 엔진: libttak 에포크 GC", "d1/dde/md_docs_2ko_2GC.html#autotoc_md636", null ],
        [ "3. 스레드/프로세스 종료 정리", "d1/dde/md_docs_2ko_2GC.html#autotoc_md637", null ],
        [ "4. 핸들 같은 지역 변수를 위한 의사 RAII", "d1/dde/md_docs_2ko_2GC.html#autotoc_md638", null ],
        [ "5. 투명한 malloc 가로채기", "d1/dde/md_docs_2ko_2GC.html#autotoc_md639", null ],
        [ "6. cwist_alloc 내부", "d1/dde/md_docs_2ko_2GC.html#autotoc_md640", null ],
        [ "7. 명시적 flush와 회수", "d1/dde/md_docs_2ko_2GC.html#autotoc_md641", null ]
      ] ],
      [ "의미 요약", "d1/dde/md_docs_2ko_2GC.html#autotoc_md642", null ],
      [ "목표가 아닌 것과 참고 사항", "d1/dde/md_docs_2ko_2GC.html#autotoc_md643", null ],
      [ "알려진 성능 주의 사항", "d1/dde/md_docs_2ko_2GC.html#autotoc_md644", null ]
    ] ],
    [ "HTTP/1.1 파이프라인 공정성과 프레이밍", "d4/dd7/md_docs_2ko_2http-reactor-fairness.html", [
      [ "범위", "d4/dd7/md_docs_2ko_2http-reactor-fairness.html#autotoc_md646", null ],
      [ "동작 회귀 테스트", "d4/dd7/md_docs_2ko_2http-reactor-fairness.html#autotoc_md647", null ],
      [ "성능 주장의 경계", "d4/dd7/md_docs_2ko_2http-reactor-fairness.html#autotoc_md648", null ],
      [ "내부 API 호환성", "d4/dd7/md_docs_2ko_2http-reactor-fairness.html#autotoc_md649", null ]
    ] ],
    [ "선택적 wrk 원시/보정 지연 히스토그램", "d2/de4/md_docs_2ko_2performance_2wrk-dual-histogram.html", [
      [ "버려도 되는 wrk 체크아웃에서 빌드하기", "d2/de4/md_docs_2ko_2performance_2wrk-dual-histogram.html#autotoc_md651", null ],
      [ "출력과 해석", "d2/de4/md_docs_2ko_2performance_2wrk-dual-histogram.html#autotoc_md652", null ],
      [ "검증", "d2/de4/md_docs_2ko_2performance_2wrk-dual-histogram.html#autotoc_md653", null ]
    ] ],
    [ "io_uring 게시 작업 깨우기 회귀 (#25)", "dc/dd4/md_docs_2ko_2reactor-wakeup-regression.html", [
      [ "범위와 원인", "dc/dd4/md_docs_2ko_2reactor-wakeup-regression.html#autotoc_md655", null ],
      [ "회귀 테스트", "dc/dd4/md_docs_2ko_2reactor-wakeup-regression.html#autotoc_md656", null ],
      [ "구성 요소 측정 (2026-09-12)", "dc/dd4/md_docs_2ko_2reactor-wakeup-regression.html#autotoc_md657", null ]
    ] ],
    [ "CWIST 라이브러리 레퍼런스", "d3/d19/md_docs_2ko_2REFERENCE.html", [
      [ "모듈", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md667", [
        [ "1. SString (스마트 문자열)", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md668", null ],
        [ "2. HTTP 서버", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md669", null ],
        [ "3. HTTPS 지원", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md670", null ],
        [ "4. 쿼리 파싱", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md671", null ],
        [ "5. 데이터베이스 (SQL)", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md672", null ],
        [ "6. SipHash", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md673", null ],
        [ "7. 에러 처리", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md674", null ],
        [ "8. Big Dumb Reply (BDR)", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md675", null ]
      ] ],
      [ "LibTTAK 메모리 기능", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md676", null ],
      [ "예제: Othello Web", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md677", null ],
      [ "의존성", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md678", null ],
      [ "빌드", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md679", null ]
    ] ],
    [ "Mux 알고리즘 참고 자료", "d4/d7c/md_docs_2ko_2references_2mux__algorithm__references.html", [
      [ "1. 낙서 마방진 (洛書)", "d4/d7c/md_docs_2ko_2references_2mux__algorithm__references.html#autotoc_md681", null ],
      [ "2. 직교 라틴 방진 순열", "d4/d7c/md_docs_2ko_2references_2mux__algorithm__references.html#autotoc_md682", null ],
      [ "3. 계수 행렬과 선형 연립방정식 풀이", "d4/d7c/md_docs_2ko_2references_2mux__algorithm__references.html#autotoc_md683", null ],
      [ "4. 반복적인 크기 정제", "d4/d7c/md_docs_2ko_2references_2mux__algorithm__references.html#autotoc_md684", null ],
      [ "5. 단일 단계 선형 외삽", "d4/d7c/md_docs_2ko_2references_2mux__algorithm__references.html#autotoc_md685", null ],
      [ "이름을 바꾼 이유", "d4/d7c/md_docs_2ko_2references_2mux__algorithm__references.html#autotoc_md686", null ]
    ] ],
    [ "런타임 설정", "d5/d08/md_docs_2ko_2runtime-config.html", [
      [ "상태", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md688", null ],
      [ "프로세스와 서버 모드", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md689", null ],
      [ "Classic 스레드 풀", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md690", null ],
      [ "HTTP/1.1", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md691", null ],
      [ "HTTPS", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md692", null ],
      [ "HTTP/2", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md693", null ],
      [ "HTTP/3", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md694", null ],
      [ "WebSocket", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md695", null ],
      [ "Reactor", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md696", null ],
      [ "진단", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md697", null ]
    ] ],
    [ "CWIST로 CRUD 블로그 만들기", "d6/d25/md_docs_2ko_2tutorials_2blog-crud.html", [
      [ "1. 애플리케이션 만들기", "d6/d25/md_docs_2ko_2tutorials_2blog-crud.html#autotoc_md699", null ],
      [ "2. 라우트 정의하기", "d6/d25/md_docs_2ko_2tutorials_2blog-crud.html#autotoc_md700", null ],
      [ "3. JSON을 검증하고 HTTP 의미에 맞게 응답하기", "d6/d25/md_docs_2ko_2tutorials_2blog-crud.html#autotoc_md701", null ],
      [ "4. API 테스트하기", "d6/d25/md_docs_2ko_2tutorials_2blog-crud.html#autotoc_md702", null ]
    ] ],
    [ "CWIST와 NATS 연동하기", "dd/d38/md_docs_2ko_2tutorials_2nats-integration.html", [
      [ "1. 요청과 워커의 책임 나누기", "dd/d38/md_docs_2ko_2tutorials_2nats-integration.html#autotoc_md704", null ],
      [ "2. 라우트에서 게시하기", "dd/d38/md_docs_2ko_2tutorials_2nats-integration.html#autotoc_md705", null ],
      [ "3. 신뢰성 점검 목록", "dd/d38/md_docs_2ko_2tutorials_2nats-integration.html#autotoc_md706", null ]
    ] ],
    [ "OpenAPI와 Swagger UI로 API 문서화하기", "dc/dbf/md_docs_2ko_2tutorials_2openapi-swagger.html", [
      [ "1. 라우트에 주석 달기", "dc/dbf/md_docs_2ko_2tutorials_2openapi-swagger.html#autotoc_md708", [
        [ "주석 태그", "dc/dbf/md_docs_2ko_2tutorials_2openapi-swagger.html#autotoc_md709", null ]
      ] ],
      [ "2. openapi.json 생성하기", "dc/dbf/md_docs_2ko_2tutorials_2openapi-swagger.html#autotoc_md710", null ],
      [ "3. UI 제공하기", "dc/dbf/md_docs_2ko_2tutorials_2openapi-swagger.html#autotoc_md711", null ]
    ] ],
    [ "CWIST로 WebTransport 서버 만들기", "df/d10/md_docs_2ko_2tutorials_2webtransport-server.html", [
      [ "빌드", "df/d10/md_docs_2ko_2tutorials_2webtransport-server.html#autotoc_md713", null ],
      [ "1. HTTP/3과 세션 핸들러 설정하기", "df/d10/md_docs_2ko_2tutorials_2webtransport-server.html#autotoc_md714", null ],
      [ "2. 세션 프로토콜 설계하기", "df/d10/md_docs_2ko_2tutorials_2webtransport-server.html#autotoc_md715", null ],
      [ "3. 안전하게 운영하기", "df/d10/md_docs_2ko_2tutorials_2webtransport-server.html#autotoc_md716", null ]
    ] ],
    [ "Release Codename Sequencing Specification", "d6/de8/md_docs_2ko_2versioning.html", [
      [ "1. 기본 원칙", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md718", null ],
      [ "2. 2자리 시퀀스 (기본)", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md719", null ],
      [ "3. 3자리 확장 시퀀스", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md720", null ],
      [ "4. 표기", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md721", null ],
      [ "5. API 안정성 게이트 (v4 LTS부터)", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md722", null ],
      [ "6. 메이저 버전의 첫 상승", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md723", null ]
    ] ],
    [ "웹 서버 벤치마크 측정 방법", "d8/d7e/md_docs_2ko_2webserver-benchmark.html", [
      [ "부하 프로필", "d8/d7e/md_docs_2ko_2webserver-benchmark.html#autotoc_md725", [
        [ "컨텍스트 스위치 열 해석하기", "d8/d7e/md_docs_2ko_2webserver-benchmark.html#autotoc_md726", null ],
        [ "튜닝된 저지연 실행", "d8/d7e/md_docs_2ko_2webserver-benchmark.html#autotoc_md727", null ]
      ] ],
      [ "CWIST", "d8/d7e/md_docs_2ko_2webserver-benchmark.html#autotoc_md728", null ],
      [ "Axum", "d8/d7e/md_docs_2ko_2webserver-benchmark.html#autotoc_md729", null ],
      [ "Gin (Go)", "d8/d7e/md_docs_2ko_2webserver-benchmark.html#autotoc_md730", null ],
      [ "Spring Boot (JVM 공정성 설정)", "d8/d7e/md_docs_2ko_2webserver-benchmark.html#autotoc_md731", null ],
      [ "기록하는 메타데이터", "d8/d7e/md_docs_2ko_2webserver-benchmark.html#autotoc_md732", null ],
      [ "지연 분포 차트", "d8/d7e/md_docs_2ko_2webserver-benchmark.html#autotoc_md733", null ],
      [ "알려진 한계", "d8/d7e/md_docs_2ko_2webserver-benchmark.html#autotoc_md734", null ]
    ] ],
    [ "HTTPS C100k gate", "d0/d49/md_docs_2performance_2https-c100k.html", [
      [ "What runs", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md736", null ],
      [ "Loopback port space", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md737", null ],
      [ "Results (2026-10-17, dev)", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md738", null ],
      [ "Results (2026-10-18, dev): C1M over TLS", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md739", null ],
      [ "Results (2026-10-11, dev)", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md740", [
        [ "Where the memory goes", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md741", null ]
      ] ],
      [ "Scaling past the gate on a single host", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md742", null ],
      [ "Running it", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md743", null ]
    ] ],
    [ "HTTPS pool task queue: static ring vs growable ring", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html", [
      [ "What changed", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md745", null ],
      [ "Setup", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md746", null ],
      [ "1. RSS of https_pool_init() alone", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md747", null ],
      [ "2. 4 workers, steady and churn load (3 alternating rounds)", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md748", null ],
      [ "3. Forcing the ring to grow: 1 worker, 2 pool threads, 6000 simultaneous connections", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md749", null ],
      [ "4. Order and loss under growth: tests/test_https_pool_queue.c", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md750", null ],
      [ "Not done / open", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md751", null ]
    ] ],
    [ "Issue #297 syscall-reduction audit (local, 12-core Ryzen 5600X)", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html", [
      [ "Baseline syscall profile", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md753", null ],
      [ "Item results", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md754", [
        [ "1.1 io_uring multishot accept — NEGATIVE (reverted)", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md755", null ],
        [ "1.2 Larger SQE batches / fewer io_uring_enter — ALREADY IMPLEMENTED / BLOCKED", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md756", null ],
        [ "1.3 Per-accept setsockopt removal — NEGATIVE (reverted)", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md757", null ],
        [ "1.4 Time caching — ALREADY IMPLEMENTED", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md758", null ]
      ] ],
      [ "What the profile says to do instead (ranked)", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md759", null ],
      [ "Method notes", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md760", null ]
    ] ],
    [ "Pluggable TLS backend (CWIST_TLS)", "d9/dda/md_docs_2performance_2tls-backends.html", [
      [ "The seam", "d9/dda/md_docs_2performance_2tls-backends.html#autotoc_md762", null ],
      [ "Forced-off features under CWIST_TLS=mbedtls", "d9/dda/md_docs_2performance_2tls-backends.html#autotoc_md763", null ],
      [ "Degraded features under CWIST_TLS=mbedtls", "d9/dda/md_docs_2performance_2tls-backends.html#autotoc_md764", null ],
      [ "Embedded bundle", "d9/dda/md_docs_2performance_2tls-backends.html#autotoc_md765", null ]
    ] ],
    [ "Opt-in wrk raw/corrected latency histograms", "d1/dfd/md_docs_2performance_2wrk-dual-histogram.html", [
      [ "Build in a disposable wrk checkout", "d1/dfd/md_docs_2performance_2wrk-dual-histogram.html#autotoc_md767", null ],
      [ "Output and interpretation", "d1/dfd/md_docs_2performance_2wrk-dual-histogram.html#autotoc_md768", null ],
      [ "Verification", "d1/dfd/md_docs_2performance_2wrk-dual-histogram.html#autotoc_md769", null ]
    ] ],
    [ "io_uring posted-work wake-up regression (#25)", "dd/d56/md_docs_2reactor-wakeup-regression.html", [
      [ "Scope and cause", "dd/d56/md_docs_2reactor-wakeup-regression.html#autotoc_md771", null ],
      [ "Regression test", "dd/d56/md_docs_2reactor-wakeup-regression.html#autotoc_md772", null ],
      [ "Component measurement (2026-09-12)", "dd/d56/md_docs_2reactor-wakeup-regression.html#autotoc_md773", null ]
    ] ],
    [ "CWIST Library Reference", "da/dca/md_docs_2REFERENCE.html", [
      [ "Modules", "da/dca/md_docs_2REFERENCE.html#autotoc_md782", [
        [ "1. SString (Smart String)", "da/dca/md_docs_2REFERENCE.html#autotoc_md783", null ],
        [ "2. HTTP Server", "da/dca/md_docs_2REFERENCE.html#autotoc_md784", null ],
        [ "3. HTTPS Support", "da/dca/md_docs_2REFERENCE.html#autotoc_md785", null ],
        [ "4. Query Parsing", "da/dca/md_docs_2REFERENCE.html#autotoc_md786", null ],
        [ "5. Database (SQL)", "da/dca/md_docs_2REFERENCE.html#autotoc_md787", null ],
        [ "6. SipHash", "da/dca/md_docs_2REFERENCE.html#autotoc_md788", null ],
        [ "7. Error Handling", "da/dca/md_docs_2REFERENCE.html#autotoc_md789", null ],
        [ "8. Big Dumb Reply (BDR)", "da/dca/md_docs_2REFERENCE.html#autotoc_md790", null ]
      ] ],
      [ "LibTTAK Memory Features", "da/dca/md_docs_2REFERENCE.html#autotoc_md791", null ],
      [ "Example: Othello Web", "da/dca/md_docs_2REFERENCE.html#autotoc_md792", null ],
      [ "Dependencies", "da/dca/md_docs_2REFERENCE.html#autotoc_md793", null ],
      [ "Build", "da/dca/md_docs_2REFERENCE.html#autotoc_md794", null ]
    ] ],
    [ "Mux Algorithm References", "d6/d9f/md_docs_2references_2mux__algorithm__references.html", [
      [ "1. Lo Shu Magic Square (Naak-seo / 洛書)", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md796", null ],
      [ "2. Orthogonal Latin-Square Permutation", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md797", null ],
      [ "3. Coefficient Matrix & Linear System Solver", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md798", null ],
      [ "4. Iterative Magnitude Refinement", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md799", null ],
      [ "5. Single-Step Linear Extrapolation", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md800", null ],
      [ "Naming Rationale", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md801", null ]
    ] ],
    [ "Runtime configuration", "d9/dfa/md_docs_2runtime-config.html", [
      [ "Status", "d9/dfa/md_docs_2runtime-config.html#autotoc_md803", null ],
      [ "Processes and server mode", "d9/dfa/md_docs_2runtime-config.html#autotoc_md804", null ],
      [ "Classic thread pool", "d9/dfa/md_docs_2runtime-config.html#autotoc_md805", null ],
      [ "HTTP/1.1", "d9/dfa/md_docs_2runtime-config.html#autotoc_md806", null ],
      [ "HTTPS", "d9/dfa/md_docs_2runtime-config.html#autotoc_md807", null ],
      [ "HTTP/2", "d9/dfa/md_docs_2runtime-config.html#autotoc_md808", null ],
      [ "HTTP/3", "d9/dfa/md_docs_2runtime-config.html#autotoc_md809", null ],
      [ "WebSocket", "d9/dfa/md_docs_2runtime-config.html#autotoc_md810", null ],
      [ "Reactor", "d9/dfa/md_docs_2runtime-config.html#autotoc_md811", null ],
      [ "Diagnostics", "d9/dfa/md_docs_2runtime-config.html#autotoc_md812", null ]
    ] ],
    [ "Soak testing", "db/d05/md_docs_2soak-testing.html", [
      [ "Environment", "db/d05/md_docs_2soak-testing.html#autotoc_md814", null ],
      [ "Bench server and workload mix", "db/d05/md_docs_2soak-testing.html#autotoc_md815", null ],
      [ "Measurement points", "db/d05/md_docs_2soak-testing.html#autotoc_md816", null ],
      [ "PASS/FAIL criteria", "db/d05/md_docs_2soak-testing.html#autotoc_md817", null ],
      [ "Interpreting a failure", "db/d05/md_docs_2soak-testing.html#autotoc_md818", null ]
    ] ],
    [ "CWIST Web Development Tutorial (A to Z)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html", [
      [ "Table of Contents", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md820", null ],
      [ "1. Hello World: Starting Your First Server", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md821", null ],
      [ "2. Routing and Handlers (Mux Router)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md822", null ],
      [ "3. JSON Parsing and Zod Schema Validation", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md823", null ],
      [ "4. Middleware (CORS, Logging)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md824", null ],
      [ "5. Databases and Migrations (SQLite)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md825", null ],
      [ "6. Authentication (JWT and DB Encryption)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md826", null ],
      [ "7. PQC TLS (Post-Quantum Hybrid Key Exchange)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md827", [
        [ "Security Policy Summary", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md828", null ]
      ] ],
      [ "8. WebSocket Integration (Real-Time Bidirectional Communication)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md829", null ],
      [ "9. Template Engine and Static File Serving", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md830", null ],
      [ "10. Dynamic CSS Composer (WASM and SSR)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md831", [
        [ "Approach A: 100% Server-Side Rendering (SSR)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md832", null ],
        [ "Approach B: Client-Side Composition with React + WebAssembly (WASM)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md833", null ],
        [ "Wrapping Up", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md834", null ]
      ] ]
    ] ],
    [ "CWIST 웹 개발 튜토리얼 (A to Z)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html", [
      [ "목차", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md836", null ],
      [ "1. Hello World: 첫 서버 띄우기", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md837", null ],
      [ "2. 라우팅과 핸들러 (Mux Router)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md838", null ],
      [ "3. JSON 파싱과 Zod 스키마 검증", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md839", null ],
      [ "4. 미들웨어 (CORS, Logging)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md840", null ],
      [ "5. 데이터베이스와 마이그레이션 (SQLite)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md841", null ],
      [ "6. 인증 (JWT 및 DB 암호화)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md842", null ],
      [ "7. PQC TLS (양자내성 하이브리드 키 교환)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md843", [
        [ "보안 정책 요약", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md844", null ]
      ] ],
      [ "8. 웹소켓 연동 (실시간 양방향 통신)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md845", null ],
      [ "9. 템플릿 엔진과 정적 파일 제공", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md846", null ],
      [ "10. 동적 CSS 합성기 (WASM 및 SSR)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md847", [
        [ "방법 A: 100% Server-Side Rendering (SSR)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md848", null ],
        [ "방법 B: React + WebAssembly (WASM) 클라이언트 사이드 합성", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md849", null ],
        [ "마치며", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md850", null ]
      ] ]
    ] ],
    [ "Build a CRUD blog with CWIST", "d4/db0/md_docs_2tutorials_2blog-crud.html", [
      [ "1. Create the application", "d4/db0/md_docs_2tutorials_2blog-crud.html#autotoc_md852", null ],
      [ "2. Define the routes", "d4/db0/md_docs_2tutorials_2blog-crud.html#autotoc_md853", null ],
      [ "3. Validate JSON and return HTTP semantics", "d4/db0/md_docs_2tutorials_2blog-crud.html#autotoc_md854", null ],
      [ "4. Test the API", "d4/db0/md_docs_2tutorials_2blog-crud.html#autotoc_md855", null ]
    ] ],
    [ "Integrate NATS with CWIST", "da/d48/md_docs_2tutorials_2nats-integration.html", [
      [ "1. Separate request and worker responsibilities", "da/d48/md_docs_2tutorials_2nats-integration.html#autotoc_md857", null ],
      [ "2. Publish from a route", "da/d48/md_docs_2tutorials_2nats-integration.html#autotoc_md858", null ],
      [ "3. Reliability checklist", "da/d48/md_docs_2tutorials_2nats-integration.html#autotoc_md859", null ]
    ] ],
    [ "Document an API with OpenAPI and Swagger UI", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html", [
      [ "1. Annotate the routes", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html#autotoc_md861", [
        [ "Annotation tags", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html#autotoc_md862", null ]
      ] ],
      [ "2. Generate openapi.json", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html#autotoc_md863", null ],
      [ "3. Serve the UI", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html#autotoc_md864", null ]
    ] ],
    [ "Build a WebTransport server with CWIST", "d2/dcb/md_docs_2tutorials_2webtransport-server.html", [
      [ "Build", "d2/dcb/md_docs_2tutorials_2webtransport-server.html#autotoc_md866", null ],
      [ "1. Configure HTTP/3 and a session handler", "d2/dcb/md_docs_2tutorials_2webtransport-server.html#autotoc_md867", null ],
      [ "2. Design the session protocol", "d2/dcb/md_docs_2tutorials_2webtransport-server.html#autotoc_md868", null ],
      [ "3. Operate safely", "d2/dcb/md_docs_2tutorials_2webtransport-server.html#autotoc_md869", null ]
    ] ],
    [ "Release Codename Sequencing Specification", "d0/db4/md_docs_2versioning.html", [
      [ "1. Principles", "d0/db4/md_docs_2versioning.html#autotoc_md871", null ],
      [ "2. Two-letter sequence (default)", "d0/db4/md_docs_2versioning.html#autotoc_md872", null ],
      [ "3. Three-letter extension", "d0/db4/md_docs_2versioning.html#autotoc_md873", null ],
      [ "4. Notation", "d0/db4/md_docs_2versioning.html#autotoc_md874", null ],
      [ "5. API stability gate (from the v4 LTS)", "d0/db4/md_docs_2versioning.html#autotoc_md875", null ],
      [ "6. First ascent of a major version", "d0/db4/md_docs_2versioning.html#autotoc_md876", null ]
    ] ],
    [ "Web Server Benchmark Methodology", "d6/d42/md_docs_2webserver-benchmark.html", [
      [ "Load profile", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md878", [
        [ "Interpreting the context-switch column", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md879", null ],
        [ "Tuned low-latency run", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md880", null ]
      ] ],
      [ "CWIST", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md881", null ],
      [ "Actix", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md882", null ],
      [ "Axum", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md883", null ],
      [ "Gin (Go)", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md884", null ],
      [ "Spring Boot (JVM fairness configuration)", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md885", null ],
      [ "Recorded metadata", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md886", null ],
      [ "Latency distribution chart", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md887", null ],
      [ "Known limitations", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md888", null ]
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
"d0/d89/md_docs_2api_2sse.html#autotoc_md177",
"d1/d53/structcwist__redis__reply.html#af01049073ffb3fe8e199ca8386c77249",
"d2/d56/md_docs_2ko_2api_2wasm.html",
"d2/dd9/structcwist__orm__value.html#af34cb2357fb8fd9ae8381ad4891bb9b5",
"d3/da3/wasi_8h_source.html",
"d4/db3/session__manager_8c.html#a8cefd1119397912329c9d845b41e425a",
"d5/d67/evp_8h.html#a2589687b0e8c05928f20af83655f2fd6",
"d6/d3c/seq_8h.html#ab396be91e3b59ec366de554e453032eb",
"d6/de8/md_docs_2ko_2versioning.html#autotoc_md721",
"d7/de9/http2__flow__control_8h.html#a947d19fa7e2aca3e99de73b0f95e7df6",
"d9/d16/http2__flow__control_8c.html#a0350eabe77ee3c2766becdaad70f313f",
"d9/dab/net_2http_2http_8h.html#a99b5ad0e71431205a397a336983f4f4daceff721f8db75496e24dda2bae3a1da3",
"da/d0a/css__composer_8h.html#af1d6b42d55ec612d65e9672a16732066",
"da/dc3/md_docs_2ko_2api_2app.html#autotoc_md385",
"db/d84/async_8c.html#ad639757d36621b24b35b618df8675e5b",
"dc/d80/md_docs_2api_2cookie.html#autotoc_md82",
"dd/d2d/siphash_8c.html",
"dd/d56/http_8h.html#aa7eeb74d97e9a5416ffc3cd53366683da1b62dbba767ff5fa8502f2a4237c37e5",
"de/d25/structcwist__bind__rule.html#a0862fcae43401adbf8b857f5eac6c8c7",
"df/d34/metrics_8h.html#a4b994f8e8094a926156bcc218559ffe5a8db70e6b63b8b2d364d0933e2e62684e",
"df/d7f/app_8c.html#a95fd64f2e957b0ba3b98187d5b320e84",
"dir_fd94c5c8b4925c340f10cd382b3978ca.html"
];

var SYNCONMSG = 'click to disable panel synchronisation';
var SYNCOFFMSG = 'click to enable panel synchronisation';