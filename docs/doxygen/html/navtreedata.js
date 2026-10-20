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
        [ "<tt>make_error</tt>", "d1/d7e/md_docs_2api-quickref.html#autotoc_md26", null ],
        [ "<tt>cwist_error_is_ok</tt> / <tt>cwist_error_is_ok_extern</tt>", "d1/d7e/md_docs_2api-quickref.html#autotoc_md27", null ],
        [ "Other exported inline helpers", "d1/d7e/md_docs_2api-quickref.html#autotoc_md28", null ]
      ] ],
      [ "SString (<tt>cwist_sstring</tt>)", "d1/d7e/md_docs_2api-quickref.html#autotoc_md29", [
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
        [ "<tt>cwist_app_create</tt>", "de/d19/md_docs_2api_2app.html#autotoc_md49", null ],
        [ "<tt>cwist_app_use_https</tt>", "de/d19/md_docs_2api_2app.html#autotoc_md50", null ],
        [ "<tt>cwist_app_use_https2</tt> / <tt>cwist_use_https2</tt>", "de/d19/md_docs_2api_2app.html#autotoc_md51", null ],
        [ "<tt>cwist_app_use_pqc_layer</tt>", "de/d19/md_docs_2api_2app.html#autotoc_md52", null ],
        [ "<tt>cwist_app_use_db</tt>", "de/d19/md_docs_2api_2app.html#autotoc_md53", null ],
        [ "<tt>cwist_app_get</tt> / <tt>cwist_app_post</tt>", "de/d19/md_docs_2api_2app.html#autotoc_md55", null ],
        [ "<tt>cwist_app_get_ex</tt> / <tt>_post_ex</tt> / <tt>_put_ex</tt> / <tt>_delete_ex</tt> / <tt>_patch_ex</tt>", "de/d19/md_docs_2api_2app.html#autotoc_md56", null ],
        [ "<tt>cwist_app_dispatch_memory</tt>", "de/d19/md_docs_2api_2app.html#autotoc_md57", null ],
        [ "<tt>cwist_app_ws</tt>", "de/d19/md_docs_2api_2app.html#autotoc_md58", null ],
        [ "<tt>cwist_endpoint_opt_t</tt>", "de/d19/md_docs_2api_2app.html#autotoc_md59", null ]
      ] ],
      [ "Static Assets", "de/d19/md_docs_2api_2app.html#autotoc_md60", [
        [ "<tt>cwist_app_static</tt>", "de/d19/md_docs_2api_2app.html#autotoc_md61", null ]
      ] ],
      [ "Error Handling", "de/d19/md_docs_2api_2app.html#autotoc_md62", [
        [ "<tt>cwist_app_set_error_handler</tt>", "de/d19/md_docs_2api_2app.html#autotoc_md63", null ],
        [ "Unused Variables", "de/d19/md_docs_2api_2app.html#autotoc_md64", null ]
      ] ],
      [ "Concurrency & Thread-Safety", "de/d19/md_docs_2api_2app.html#autotoc_md65", null ],
      [ "Memory Ownership Rules", "de/d19/md_docs_2api_2app.html#autotoc_md66", [
        [ "<tt>cwist_app_listen</tt>", "de/d19/md_docs_2api_2app.html#autotoc_md67", null ]
      ] ],
      [ "In-Memory Test Client", "de/d19/md_docs_2api_2app.html#autotoc_md68", [
        [ "<tt>cwist_test_client_create</tt> / <tt>cwist_test_client_destroy</tt>", "de/d19/md_docs_2api_2app.html#autotoc_md69", null ],
        [ "Request helpers", "de/d19/md_docs_2api_2app.html#autotoc_md70", null ],
        [ "Cookie jar", "de/d19/md_docs_2api_2app.html#autotoc_md71", null ],
        [ "Test assertion macros", "de/d19/md_docs_2api_2app.html#autotoc_md72", null ]
      ] ]
    ] ],
    [ "Cookie API", "dc/d80/md_docs_2api_2cookie.html", [
      [ "Reading cookies", "dc/d80/md_docs_2api_2cookie.html#autotoc_md74", [
        [ "<tt>cwist_cookie_parse</tt>", "dc/d80/md_docs_2api_2cookie.html#autotoc_md75", null ],
        [ "<tt>cwist_cookie_get</tt>", "dc/d80/md_docs_2api_2cookie.html#autotoc_md76", null ]
      ] ],
      [ "Writing cookies", "dc/d80/md_docs_2api_2cookie.html#autotoc_md77", [
        [ "<tt>cwist_cookie_options</tt>", "dc/d80/md_docs_2api_2cookie.html#autotoc_md78", null ],
        [ "<tt>cwist_cookie_set</tt>", "dc/d80/md_docs_2api_2cookie.html#autotoc_md79", null ],
        [ "<tt>cwist_cookie_delete</tt>", "dc/d80/md_docs_2api_2cookie.html#autotoc_md80", null ]
      ] ],
      [ "Encoding", "dc/d80/md_docs_2api_2cookie.html#autotoc_md81", [
        [ "<tt>cwist_cookie_encode</tt>", "dc/d80/md_docs_2api_2cookie.html#autotoc_md82", null ],
        [ "<tt>cwist_cookie_decode</tt>", "dc/d80/md_docs_2api_2cookie.html#autotoc_md83", null ]
      ] ],
      [ "Example", "dc/d80/md_docs_2api_2cookie.html#autotoc_md84", null ]
    ] ],
    [ "HTML Components, Scoped CSS, Fragments and Assets", "d8/d83/md_docs_2api_2html.html", [
      [ "Components", "d8/d83/md_docs_2api_2html.html#autotoc_md86", [
        [ "<tt>cwist_html_component_create</tt> / <tt>cwist_html_component_destroy</tt>", "d8/d83/md_docs_2api_2html.html#autotoc_md87", null ],
        [ "<tt>cwist_html_component_instantiate</tt>", "d8/d83/md_docs_2api_2html.html#autotoc_md88", null ]
      ] ],
      [ "Scoped CSS", "d8/d83/md_docs_2api_2html.html#autotoc_md89", [
        [ "<tt>cwist_css_scope_init</tt> / <tt>cwist_css_scope_destroy</tt>", "d8/d83/md_docs_2api_2html.html#autotoc_md90", null ],
        [ "<tt>cwist_css_scope_class</tt>", "d8/d83/md_docs_2api_2html.html#autotoc_md91", null ],
        [ "<tt>cwist_css_scope_add_rule</tt>", "d8/d83/md_docs_2api_2html.html#autotoc_md92", null ],
        [ "<tt>cwist_css_scope_generate_stylesheet</tt>", "d8/d83/md_docs_2api_2html.html#autotoc_md93", null ]
      ] ],
      [ "HTML over the wire", "d8/d83/md_docs_2api_2html.html#autotoc_md94", [
        [ "<tt>cwist_http_request_wants_fragment</tt> / <tt>cwist_http_request_fragment_target</tt>", "d8/d83/md_docs_2api_2html.html#autotoc_md95", null ],
        [ "<tt>cwist_http_response_set_view</tt>", "d8/d83/md_docs_2api_2html.html#autotoc_md96", null ],
        [ "<tt>cwist_http_response_set_html</tt>", "d8/d83/md_docs_2api_2html.html#autotoc_md97", null ],
        [ "<tt>cwist_http_response_add_oob</tt>", "d8/d83/md_docs_2api_2html.html#autotoc_md98", null ],
        [ "<tt>cwist_http_response_html_redirect</tt>", "d8/d83/md_docs_2api_2html.html#autotoc_md99", null ],
        [ "Handler example", "d8/d83/md_docs_2api_2html.html#autotoc_md100", null ]
      ] ],
      [ "Asset pipeline", "d8/d83/md_docs_2api_2html.html#autotoc_md101", [
        [ "<tt>cwist_css_minify</tt> / <tt>cwist_css_bundle</tt>", "d8/d83/md_docs_2api_2html.html#autotoc_md102", null ],
        [ "<tt>cwist_app_asset_add</tt> / <tt>cwist_app_asset_add_file</tt> / <tt>cwist_app_asset_url</tt>", "d8/d83/md_docs_2api_2html.html#autotoc_md103", null ],
        [ "Putting it together", "d8/d83/md_docs_2api_2html.html#autotoc_md104", null ]
      ] ],
      [ "Rendering the same views in WASM", "d8/d83/md_docs_2api_2html.html#autotoc_md105", null ],
      [ "Example", "d8/d83/md_docs_2api_2html.html#autotoc_md106", null ]
    ] ],
    [ "HTTP Core API", "d2/d9e/md_docs_2api_2http.html", [
      [ "Constants", "d2/d9e/md_docs_2api_2http.html#autotoc_md108", [
        [ "<tt>cwist_http_request_create</tt>", "d2/d9e/md_docs_2api_2http.html#autotoc_md109", null ],
        [ "<tt>cwist_http_parse_request</tt>", "d2/d9e/md_docs_2api_2http.html#autotoc_md110", null ],
        [ "<tt>cwist_http_receive_request</tt>", "d2/d9e/md_docs_2api_2http.html#autotoc_md111", null ],
        [ "<tt>cwist_http_response_create</tt>", "d2/d9e/md_docs_2api_2http.html#autotoc_md112", null ],
        [ "<tt>cwist_http_response_send_file</tt>", "d2/d9e/md_docs_2api_2http.html#autotoc_md113", null ],
        [ "<tt>cwist_http_header_add</tt>", "d2/d9e/md_docs_2api_2http.html#autotoc_md114", null ],
        [ "<tt>cwist_http_header_get</tt>", "d2/d9e/md_docs_2api_2http.html#autotoc_md115", null ]
      ] ],
      [ "Server Core", "d2/d9e/md_docs_2api_2http.html#autotoc_md116", [
        [ "Keep-alive handling", "d2/d9e/md_docs_2api_2http.html#autotoc_md117", null ],
        [ "Regression testing", "d2/d9e/md_docs_2api_2http.html#autotoc_md118", null ],
        [ "<tt>cwist_http_server_loop</tt>", "d2/d9e/md_docs_2api_2http.html#autotoc_md119", null ],
        [ "<tt>cwist_accept_socket</tt>", "d2/d9e/md_docs_2api_2http.html#autotoc_md120", null ],
        [ "<tt>cwist_http_continuation_shed_count</tt>", "d2/d9e/md_docs_2api_2http.html#autotoc_md121", null ]
      ] ],
      [ "Security Headers", "d2/d9e/md_docs_2api_2http.html#autotoc_md122", [
        [ "<tt>cwist_http_response_add_security_headers</tt>", "d2/d9e/md_docs_2api_2http.html#autotoc_md123", null ],
        [ "<tt>cwist_http_response_add_hsts</tt>", "d2/d9e/md_docs_2api_2http.html#autotoc_md124", null ]
      ] ]
    ] ],
    [ "HTTPS API", "d4/d45/md_docs_2api_2https.html", null ],
    [ "JSON Builder API", "d8/deb/md_docs_2api_2json.html", [
      [ "Functions", "d8/deb/md_docs_2api_2json.html#autotoc_md133", [
        [ "<tt>cwist_json_builder_create</tt>", "d8/deb/md_docs_2api_2json.html#autotoc_md134", null ],
        [ "<tt>cwist_json_begin_object</tt> / <tt>cwist_json_end_object</tt>", "d8/deb/md_docs_2api_2json.html#autotoc_md135", null ],
        [ "<tt>cwist_json_add_string</tt>", "d8/deb/md_docs_2api_2json.html#autotoc_md136", null ],
        [ "<tt>cwist_json_add_int</tt>", "d8/deb/md_docs_2api_2json.html#autotoc_md137", null ],
        [ "<tt>cwist_json_add_bool</tt>", "d8/deb/md_docs_2api_2json.html#autotoc_md138", null ],
        [ "<tt>cwist_json_add_null</tt>", "d8/deb/md_docs_2api_2json.html#autotoc_md139", null ],
        [ "<tt>cwist_json_begin_array</tt> / <tt>cwist_json_end_array</tt>", "d8/deb/md_docs_2api_2json.html#autotoc_md140", null ],
        [ "<tt>cwist_json_get_raw</tt>", "d8/deb/md_docs_2api_2json.html#autotoc_md141", null ],
        [ "<tt>cwist_json_builder_destroy</tt>", "d8/deb/md_docs_2api_2json.html#autotoc_md142", null ]
      ] ],
      [ "Example", "d8/deb/md_docs_2api_2json.html#autotoc_md143", null ]
    ] ],
    [ "Middleware API", "da/dbc/md_docs_2api_2middleware.html", [
      [ "Functions", "da/dbc/md_docs_2api_2middleware.html#autotoc_md145", [
        [ "<tt>cwist_app_use</tt>", "da/dbc/md_docs_2api_2middleware.html#autotoc_md146", null ]
      ] ],
      [ "Built-in Middlewares", "da/dbc/md_docs_2api_2middleware.html#autotoc_md147", [
        [ "Request ID Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md148", null ],
        [ "Access Log Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md149", [
          [ "Async log output", "da/dbc/md_docs_2api_2middleware.html#autotoc_md150", null ]
        ] ],
        [ "Rate Limiter Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md151", null ],
        [ "CORS Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md152", null ],
        [ "Prometheus Metrics Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md153", null ],
        [ "JWT Auth Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md154", null ],
        [ "Compression Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md155", null ]
      ] ],
      [ "Creating Custom Middleware", "da/dbc/md_docs_2api_2middleware.html#autotoc_md156", null ]
    ] ],
    [ "Query & URI API", "dc/d49/md_docs_2api_2query.html", null ],
    [ "Database API", "d5/d11/md_docs_2api_2sql.html", [
      [ "Schema Migrations", "d5/d11/md_docs_2api_2sql.html#autotoc_md166", [
        [ "<tt>cwist_db_open</tt>", "d5/d11/md_docs_2api_2sql.html#autotoc_md161", null ],
        [ "<tt>cwist_db_open_memory</tt>", "d5/d11/md_docs_2api_2sql.html#autotoc_md162", null ],
        [ "<tt>cwist_db_serialize</tt>", "d5/d11/md_docs_2api_2sql.html#autotoc_md163", null ],
        [ "<tt>cwist_db_exec</tt>", "d5/d11/md_docs_2api_2sql.html#autotoc_md164", null ],
        [ "<tt>cwist_db_query</tt>", "d5/d11/md_docs_2api_2sql.html#autotoc_md165", null ],
        [ "Return codes", "d5/d11/md_docs_2api_2sql.html#autotoc_md167", null ],
        [ "<tt>cwist_migration_t</tt>", "d5/d11/md_docs_2api_2sql.html#autotoc_md168", null ],
        [ "<tt>cwist_migrate_up</tt>", "d5/d11/md_docs_2api_2sql.html#autotoc_md169", null ],
        [ "<tt>cwist_migrate_down</tt>", "d5/d11/md_docs_2api_2sql.html#autotoc_md170", null ],
        [ "<tt>cwist_migrate_version</tt>", "d5/d11/md_docs_2api_2sql.html#autotoc_md171", null ]
      ] ]
    ] ],
    [ "Server-Sent Events API", "d0/d89/md_docs_2api_2sse.html", [
      [ "Types", "d0/d89/md_docs_2api_2sse.html#autotoc_md173", [
        [ "<tt>cwist_sse_event_t</tt>", "d0/d89/md_docs_2api_2sse.html#autotoc_md174", null ],
        [ "<tt>cwist_sse_stream_t</tt>", "d0/d89/md_docs_2api_2sse.html#autotoc_md175", null ]
      ] ],
      [ "Response mode", "d0/d89/md_docs_2api_2sse.html#autotoc_md176", [
        [ "<tt>cwist_sse_response_init</tt>", "d0/d89/md_docs_2api_2sse.html#autotoc_md177", null ],
        [ "<tt>cwist_sse_response_event</tt>", "d0/d89/md_docs_2api_2sse.html#autotoc_md178", null ],
        [ "<tt>cwist_sse_response_write</tt>", "d0/d89/md_docs_2api_2sse.html#autotoc_md179", null ],
        [ "<tt>cwist_sse_response_comment</tt>", "d0/d89/md_docs_2api_2sse.html#autotoc_md180", null ]
      ] ],
      [ "Stream mode", "d0/d89/md_docs_2api_2sse.html#autotoc_md181", [
        [ "<tt>cwist_sse_stream_open</tt>", "d0/d89/md_docs_2api_2sse.html#autotoc_md182", null ],
        [ "<tt>cwist_sse_stream_send</tt>", "d0/d89/md_docs_2api_2sse.html#autotoc_md183", null ],
        [ "<tt>cwist_sse_stream_write</tt>", "d0/d89/md_docs_2api_2sse.html#autotoc_md184", null ],
        [ "<tt>cwist_sse_stream_comment</tt>", "d0/d89/md_docs_2api_2sse.html#autotoc_md185", null ],
        [ "<tt>cwist_sse_stream_close</tt>", "d0/d89/md_docs_2api_2sse.html#autotoc_md186", null ]
      ] ],
      [ "Convenience macros", "d0/d89/md_docs_2api_2sse.html#autotoc_md187", null ],
      [ "Example", "d0/d89/md_docs_2api_2sse.html#autotoc_md188", null ]
    ] ],
    [ "SString API", "db/d14/md_docs_2api_2sstring.html", null ],
    [ "WASI targets", "d6/d26/md_docs_2api_2wasi.html", [
      [ "Flavour detection", "d6/d26/md_docs_2api_2wasi.html#autotoc_md199", null ],
      [ "WASI 0.2 socket server", "d6/d26/md_docs_2api_2wasi.html#autotoc_md200", null ],
      [ "WASI preview1 (retired)", "d6/d26/md_docs_2api_2wasi.html#autotoc_md201", [
        [ "What the preview1 build did differently", "d6/d26/md_docs_2api_2wasi.html#autotoc_md202", null ],
        [ "Verification (historical)", "d6/d26/md_docs_2api_2wasi.html#autotoc_md203", null ]
      ] ]
    ] ],
    [ "CWIST on the WASM component model", "d8/d77/md_docs_2api_2wasm-component.html", [
      [ "Why", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md205", null ],
      [ "The boundary", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md206", null ],
      [ "Toolchain status (measured 2026-09-21)", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md207", null ],
      [ "Evaluated: preview3-shim (2026-09-23)", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md208", null ],
      [ "Landed: native async for streaming/SSE (2026-09-23)", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md209", null ],
      [ "Resolved: socket request path under wasip3 (audited 2026-09-22)", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md210", null ],
      [ "Stages", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md211", null ],
      [ "Browser runtime gate (WASI 0.2)", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md212", null ],
      [ "Measured while building stage 2", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md213", null ],
      [ "Non-goals", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md214", null ]
    ] ],
    [ "CWIST on WASM (Emscripten)", "d5/d0e/md_docs_2api_2wasm.html", [
      [ "Building", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md216", null ],
      [ "What is in the WASM build", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md217", null ],
      [ "Request dispatch from JS", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md218", null ],
      [ "Zero-copy views: <tt>cwist/wasm/typedarray.h</tt>", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md219", null ],
      [ "Databases under WASM", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md220", null ],
      [ "Bundle size impact of <tt>cwist_db</tt>", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md221", null ],
      [ "Sessions and cookies", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md222", null ],
      [ "Streaming through the boundary", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md223", null ],
      [ "Runnable example: Service Worker app", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md224", null ],
      [ "Not covered (yet)", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md225", null ],
      [ "Streaming producer (handlers generate the body chunk-by-chunk)", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md226", null ]
    ] ],
    [ "WebSocket API", "db/dc5/md_docs_2api_2websocket.html", [
      [ "Functions", "db/dc5/md_docs_2api_2websocket.html#autotoc_md228", [
        [ "<tt>cwist_websocket_upgrade</tt>", "db/dc5/md_docs_2api_2websocket.html#autotoc_md229", null ],
        [ "<tt>cwist_websocket_receive</tt>", "db/dc5/md_docs_2api_2websocket.html#autotoc_md230", null ],
        [ "<tt>cwist_websocket_send</tt>", "db/dc5/md_docs_2api_2websocket.html#autotoc_md231", null ]
      ] ],
      [ "Opcode Types", "db/dc5/md_docs_2api_2websocket.html#autotoc_md232", null ],
      [ "Integration with <tt>cwist_app</tt>", "db/dc5/md_docs_2api_2websocket.html#autotoc_md233", null ],
      [ "Async (C1M) WebSocket API", "db/dc5/md_docs_2api_2websocket.html#autotoc_md234", [
        [ "<tt>cwist_app_ws_async</tt>", "db/dc5/md_docs_2api_2websocket.html#autotoc_md235", null ],
        [ "<tt>cwist_ws_on_message_t</tt>", "db/dc5/md_docs_2api_2websocket.html#autotoc_md236", null ],
        [ "<tt>cwist_websocket_async_send</tt>", "db/dc5/md_docs_2api_2websocket.html#autotoc_md237", null ],
        [ "<tt>cwist_websocket_async_close</tt>", "db/dc5/md_docs_2api_2websocket.html#autotoc_md238", null ]
      ] ],
      [ "GraphQL subscriptions (graphql-ws)", "db/dc5/md_docs_2api_2websocket.html#autotoc_md239", null ]
    ] ],
    [ "Deferred HTTP ownership under full GC", "df/dc5/md_docs_2async-gc-ownership.html", [
      [ "Handoff sequence", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md241", null ],
      [ "Ownership inventory", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md242", [
        [ "Borrowed and external state", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md243", null ]
      ] ],
      [ "HTTP/2 queue ownership", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md244", null ],
      [ "Verification and limits", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md245", null ],
      [ "Scheduler and IO queue ownership", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md246", null ]
    ] ],
    [ "C1M file limits", "db/d4d/md_docs_2c1m-file-limits.html", [
      [ "Cause", "db/d4d/md_docs_2c1m-file-limits.html#autotoc_md248", null ],
      [ "Change", "db/d4d/md_docs_2c1m-file-limits.html#autotoc_md249", null ],
      [ "Tuning", "db/d4d/md_docs_2c1m-file-limits.html#autotoc_md250", null ],
      [ "Checks", "db/d4d/md_docs_2c1m-file-limits.html#autotoc_md251", null ]
    ] ],
    [ "Classic pool burst starvation (#25 follow-up)", "d9/d56/md_docs_2classic-pool-starvation.html", [
      [ "Cause and change", "d9/d56/md_docs_2classic-pool-starvation.html#autotoc_md253", null ],
      [ "Deterministic regression", "d9/d56/md_docs_2classic-pool-starvation.html#autotoc_md254", null ],
      [ "Validation status", "d9/d56/md_docs_2classic-pool-starvation.html#autotoc_md255", null ],
      [ "HTTP measurements and limits", "d9/d56/md_docs_2classic-pool-starvation.html#autotoc_md256", null ]
    ] ],
    [ "Cooperative queuing in the C1M reactor's batch drain (issue #25)", "d1/ddd/md_docs_2cooperative-queuing.html", [
      [ "Background", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md258", null ],
      [ "The gap", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md259", null ],
      [ "The fix", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md260", null ],
      [ "Measured effect", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md261", null ],
      [ "What this does not fix", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md262", null ],
      [ "Tuning", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md263", null ]
    ] ],
    [ "ADR-0001: Restore HTTP response caching through an explicit public FIXED contract", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html", [
      [ "Context", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md265", null ],
      [ "Decision", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md266", [
        [ "1. One parsed eligibility boundary", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md267", null ],
        [ "2. Cache representations, not wire responses", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md268", null ],
        [ "3. Isolate ownership, provenance and capacity", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md269", null ],
        [ "4. Make the compatibility change explicit", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md270", null ]
      ] ],
      [ "Alternatives considered", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md271", null ],
      [ "Consequences", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md272", null ],
      [ "Implementation and release gates", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md273", null ],
      [ "Evidence and source anchors", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md274", null ]
    ] ],
    [ "ADR-0002: TLS certificate hot reload keeps the session-ticket key (STEK) unchanged", "d1/dd3/md_docs_2decisions_20002-tls-hot-reload-stek.html", [
      [ "Context", "d1/dd3/md_docs_2decisions_20002-tls-hot-reload-stek.html#autotoc_md276", null ],
      [ "Decision", "d1/dd3/md_docs_2decisions_20002-tls-hot-reload-stek.html#autotoc_md277", null ],
      [ "Consequences", "d1/dd3/md_docs_2decisions_20002-tls-hot-reload-stek.html#autotoc_md278", null ]
    ] ],
    [ "Deploy the WASI 0.2 example with Wasmtime", "db/de3/md_docs_2deployment_2wasmtime-appliance.html", [
      [ "Scope and safety", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md281", null ],
      [ "1. Build a known revision", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md282", null ],
      [ "2. Separate the artifact from state", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md283", null ],
      [ "3. Start and verify", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md284", null ],
      [ "4. Restart, back up, and restore", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md285", null ],
      [ "Before adapting this for production", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md286", null ]
    ] ],
    [ "Zero-downtime restart via socket handoff", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html", [
      [ "Requirements", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html#autotoc_md288", null ],
      [ "Handoff sequence", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html#autotoc_md289", null ],
      [ "systemd integration", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html#autotoc_md290", null ],
      [ "UDP and HTTP/3", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html#autotoc_md291", null ],
      [ "Verifying a deploy", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html#autotoc_md292", null ]
    ] ],
    [ "Durable queue required gate", "d3/dd5/md_docs_2durable-queue-gate.html", [
      [ "Local runs", "d3/dd5/md_docs_2durable-queue-gate.html#autotoc_md294", null ],
      [ "Isolated integration gate", "d3/dd5/md_docs_2durable-queue-gate.html#autotoc_md295", null ],
      [ "CI", "d3/dd5/md_docs_2durable-queue-gate.html#autotoc_md296", null ]
    ] ],
    [ "FIXED endpoint caching: current behavior", "d3/ddd/md_docs_2fixed-cache-status.html", [
      [ "What the runtime actually does today", "d3/ddd/md_docs_2fixed-cache-status.html#autotoc_md298", null ],
      [ "Evidence", "d3/ddd/md_docs_2fixed-cache-status.html#autotoc_md299", null ],
      [ "Why the learning block is not simply moved", "d3/ddd/md_docs_2fixed-cache-status.html#autotoc_md300", null ],
      [ "Recommended handling", "d3/ddd/md_docs_2fixed-cache-status.html#autotoc_md301", null ]
    ] ],
    [ "Full GC: Automatic Resource Reclamation in CWIST", "d9/dd1/md_docs_2GC.html", [
      [ "Motivation", "d9/dd1/md_docs_2GC.html#autotoc_md303", null ],
      [ "Design", "d9/dd1/md_docs_2GC.html#autotoc_md304", [
        [ "1. <tt>cwist_full_gc(bool)</tt> — the global toggle", "d9/dd1/md_docs_2GC.html#autotoc_md305", null ],
        [ "2. Reclamation engine: libttak epoch GC", "d9/dd1/md_docs_2GC.html#autotoc_md306", null ],
        [ "3. Thread/process exit sweeps", "d9/dd1/md_docs_2GC.html#autotoc_md307", null ],
        [ "4. Pseudo-RAII for handle-like locals", "d9/dd1/md_docs_2GC.html#autotoc_md308", null ],
        [ "5. Transparent <tt>malloc</tt> interception", "d9/dd1/md_docs_2GC.html#autotoc_md309", null ],
        [ "6. <tt>cwist_alloc</tt> internals", "d9/dd1/md_docs_2GC.html#autotoc_md310", null ],
        [ "7. Explicit flushes and reclamation", "d9/dd1/md_docs_2GC.html#autotoc_md311", null ]
      ] ],
      [ "Semantics summary", "d9/dd1/md_docs_2GC.html#autotoc_md312", null ],
      [ "Non-goals and notes", "d9/dd1/md_docs_2GC.html#autotoc_md313", null ],
      [ "Known performance caveat", "d9/dd1/md_docs_2GC.html#autotoc_md314", null ]
    ] ],
    [ "HTTP/1.1 pipeline fairness and framing", "d2/dc2/md_docs_2http-reactor-fairness.html", [
      [ "Scope", "d2/dc2/md_docs_2http-reactor-fairness.html#autotoc_md316", null ],
      [ "Behavioral regression tests", "d2/dc2/md_docs_2http-reactor-fairness.html#autotoc_md317", null ],
      [ "Performance claim boundary", "d2/dc2/md_docs_2http-reactor-fairness.html#autotoc_md318", null ],
      [ "Internal API compatibility", "d2/dc2/md_docs_2http-reactor-fairness.html#autotoc_md319", null ]
    ] ],
    [ "CWIST API 레퍼런스", "d3/d38/md_docs_2ko_2api-quickref.html", [
      [ "에러 처리", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md321", [
        [ "<tt>make_error</tt>", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md322", null ],
        [ "<tt>cwist_error_is_ok</tt> / <tt>cwist_error_is_ok_extern</tt>", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md323", null ],
        [ "그 밖에 내보낸 inline 도우미", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md324", null ]
      ] ],
      [ "SString (<tt>cwist_sstring</tt>)", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md325", [
        [ "수명 주기", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md326", null ],
        [ "핵심 도우미", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md327", null ],
        [ "공백 제거", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md328", null ],
        [ "덧붙이기 / 복사", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md329", null ],
        [ "비교 / 조회", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md330", null ]
      ] ],
      [ "HTTP", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md331", [
        [ "요청 수명 주기", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md332", null ],
        [ "응답 수명 주기", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md333", null ],
        [ "헤더 도우미", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md334", null ],
        [ "메서드 도우미", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md335", null ],
        [ "소켓 도우미", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md336", null ]
      ] ],
      [ "세션 매니저", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md337", [
        [ "아레나", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md338", null ],
        [ "공유 세션 (침습적 참조 카운트)", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md339", null ],
        [ "매니저", "d3/d38/md_docs_2ko_2api-quickref.html#autotoc_md340", null ]
      ] ]
    ] ],
    [ "CWIST API 문서", "dc/d79/md_docs_2ko_2API.html", [
      [ "모듈", "dc/d79/md_docs_2ko_2API.html#autotoc_md342", null ],
      [ "함께 보기", "dc/d79/md_docs_2ko_2API.html#autotoc_md343", null ]
    ] ],
    [ "프레임워크와 앱 API", "da/dc3/md_docs_2ko_2api_2app.html", [
      [ "라우팅", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md350", [
        [ "<tt>cwist_app_create</tt>", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md345", null ],
        [ "<tt>cwist_app_use_https</tt>", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md346", null ],
        [ "<tt>cwist_app_use_https2</tt> / <tt>cwist_use_https2</tt>", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md347", null ],
        [ "<tt>cwist_app_use_pqc_layer</tt>", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md348", null ],
        [ "<tt>cwist_app_use_db</tt>", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md349", null ],
        [ "<tt>cwist_app_get</tt> / <tt>cwist_app_post</tt>", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md351", null ],
        [ "<tt>cwist_app_get_ex</tt> / <tt>_post_ex</tt> / <tt>_put_ex</tt> / <tt>_delete_ex</tt> / <tt>_patch_ex</tt>", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md352", null ],
        [ "<tt>cwist_app_dispatch_memory</tt>", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md353", null ],
        [ "<tt>cwist_app_ws</tt>", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md354", null ],
        [ "<tt>cwist_endpoint_opt_t</tt>", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md355", null ]
      ] ],
      [ "정적 자산", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md356", [
        [ "<tt>cwist_app_static</tt>", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md357", null ]
      ] ],
      [ "에러 처리", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md358", [
        [ "<tt>cwist_app_set_error_handler</tt>", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md359", null ],
        [ "사용하지 않는 변수", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md360", null ]
      ] ],
      [ "동시성과 스레드 안전성", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md361", null ],
      [ "메모리 소유권 규칙", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md362", [
        [ "<tt>cwist_app_listen</tt>", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md363", null ]
      ] ],
      [ "인메모리 테스트 클라이언트", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md364", [
        [ "<tt>cwist_test_client_create</tt> / <tt>cwist_test_client_destroy</tt>", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md365", null ],
        [ "요청 도우미", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md366", null ],
        [ "쿠키 저장소", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md367", null ],
        [ "테스트 단언 매크로", "da/dc3/md_docs_2ko_2api_2app.html#autotoc_md368", null ]
      ] ]
    ] ],
    [ "쿠키 API", "db/d04/md_docs_2ko_2api_2cookie.html", [
      [ "쿠키 읽기", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md370", [
        [ "<tt>cwist_cookie_parse</tt>", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md371", null ],
        [ "<tt>cwist_cookie_get</tt>", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md372", null ]
      ] ],
      [ "쿠키 쓰기", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md373", [
        [ "<tt>cwist_cookie_options</tt>", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md374", null ],
        [ "<tt>cwist_cookie_set</tt>", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md375", null ],
        [ "<tt>cwist_cookie_delete</tt>", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md376", null ]
      ] ],
      [ "인코딩", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md377", [
        [ "<tt>cwist_cookie_encode</tt>", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md378", null ],
        [ "<tt>cwist_cookie_decode</tt>", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md379", null ]
      ] ],
      [ "예제", "db/d04/md_docs_2ko_2api_2cookie.html#autotoc_md380", null ]
    ] ],
    [ "HTML 컴포넌트, 범위 CSS, 프래그먼트와 자산", "d1/df9/md_docs_2ko_2api_2html.html", [
      [ "컴포넌트", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md382", [
        [ "<tt>cwist_html_component_create</tt> / <tt>cwist_html_component_destroy</tt>", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md383", null ],
        [ "<tt>cwist_html_component_instantiate</tt>", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md384", null ]
      ] ],
      [ "범위 CSS", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md385", [
        [ "<tt>cwist_css_scope_init</tt> / <tt>cwist_css_scope_destroy</tt>", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md386", null ],
        [ "<tt>cwist_css_scope_class</tt>", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md387", null ],
        [ "<tt>cwist_css_scope_add_rule</tt>", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md388", null ],
        [ "<tt>cwist_css_scope_generate_stylesheet</tt>", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md389", null ]
      ] ],
      [ "HTML over the wire", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md390", [
        [ "<tt>cwist_http_request_wants_fragment</tt> / <tt>cwist_http_request_fragment_target</tt>", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md391", null ],
        [ "<tt>cwist_http_response_set_view</tt>", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md392", null ],
        [ "<tt>cwist_http_response_set_html</tt>", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md393", null ],
        [ "<tt>cwist_http_response_add_oob</tt>", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md394", null ],
        [ "<tt>cwist_http_response_html_redirect</tt>", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md395", null ],
        [ "핸들러 예제", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md396", null ]
      ] ],
      [ "자산 파이프라인", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md397", [
        [ "<tt>cwist_css_minify</tt> / <tt>cwist_css_bundle</tt>", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md398", null ],
        [ "<tt>cwist_app_asset_add</tt> / <tt>cwist_app_asset_add_file</tt> / <tt>cwist_app_asset_url</tt>", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md399", null ],
        [ "모두 합치기", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md400", null ]
      ] ],
      [ "WASM에서 같은 뷰 렌더링하기", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md401", null ],
      [ "예제", "d1/df9/md_docs_2ko_2api_2html.html#autotoc_md402", null ]
    ] ],
    [ "HTTP 코어 API", "db/ddd/md_docs_2ko_2api_2http.html", [
      [ "상수", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md404", [
        [ "<tt>cwist_http_request_create</tt>", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md405", null ],
        [ "<tt>cwist_http_parse_request</tt>", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md406", null ],
        [ "<tt>cwist_http_receive_request</tt>", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md407", null ],
        [ "<tt>cwist_http_response_create</tt>", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md408", null ],
        [ "<tt>cwist_http_response_send_file</tt>", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md409", null ],
        [ "<tt>cwist_http_header_add</tt>", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md410", null ],
        [ "<tt>cwist_http_header_get</tt>", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md411", null ]
      ] ],
      [ "서버 코어", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md412", [
        [ "Keep-alive 처리", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md413", null ],
        [ "회귀 테스트", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md414", null ],
        [ "<tt>cwist_http_server_loop</tt>", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md415", null ],
        [ "<tt>cwist_accept_socket</tt>", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md416", null ],
        [ "<tt>cwist_http_continuation_shed_count</tt>", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md417", null ]
      ] ],
      [ "보안 헤더", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md418", [
        [ "<tt>cwist_http_response_add_security_headers</tt>", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md419", null ],
        [ "<tt>cwist_http_response_add_hsts</tt>", "db/ddd/md_docs_2ko_2api_2http.html#autotoc_md420", null ]
      ] ]
    ] ],
    [ "HTTPS API", "d2/d2f/md_docs_2ko_2api_2https.html", null ],
    [ "JSON 빌더 API", "d8/d3f/md_docs_2ko_2api_2json.html", [
      [ "함수", "d8/d3f/md_docs_2ko_2api_2json.html#autotoc_md429", [
        [ "<tt>cwist_json_builder_create</tt>", "d8/d3f/md_docs_2ko_2api_2json.html#autotoc_md430", null ],
        [ "<tt>cwist_json_begin_object</tt> / <tt>cwist_json_end_object</tt>", "d8/d3f/md_docs_2ko_2api_2json.html#autotoc_md431", null ],
        [ "<tt>cwist_json_add_string</tt>", "d8/d3f/md_docs_2ko_2api_2json.html#autotoc_md432", null ],
        [ "<tt>cwist_json_add_int</tt>", "d8/d3f/md_docs_2ko_2api_2json.html#autotoc_md433", null ],
        [ "<tt>cwist_json_add_bool</tt>", "d8/d3f/md_docs_2ko_2api_2json.html#autotoc_md434", null ],
        [ "<tt>cwist_json_add_null</tt>", "d8/d3f/md_docs_2ko_2api_2json.html#autotoc_md435", null ],
        [ "<tt>cwist_json_begin_array</tt> / <tt>cwist_json_end_array</tt>", "d8/d3f/md_docs_2ko_2api_2json.html#autotoc_md436", null ],
        [ "<tt>cwist_json_get_raw</tt>", "d8/d3f/md_docs_2ko_2api_2json.html#autotoc_md437", null ],
        [ "<tt>cwist_json_builder_destroy</tt>", "d8/d3f/md_docs_2ko_2api_2json.html#autotoc_md438", null ]
      ] ],
      [ "예제", "d8/d3f/md_docs_2ko_2api_2json.html#autotoc_md439", null ]
    ] ],
    [ "미들웨어 API", "d5/d3e/md_docs_2ko_2api_2middleware.html", [
      [ "함수", "d5/d3e/md_docs_2ko_2api_2middleware.html#autotoc_md441", [
        [ "<tt>cwist_app_use</tt>", "d5/d3e/md_docs_2ko_2api_2middleware.html#autotoc_md442", null ]
      ] ],
      [ "내장 미들웨어", "d5/d3e/md_docs_2ko_2api_2middleware.html#autotoc_md443", [
        [ "Request ID 미들웨어", "d5/d3e/md_docs_2ko_2api_2middleware.html#autotoc_md444", null ],
        [ "액세스 로그 미들웨어", "d5/d3e/md_docs_2ko_2api_2middleware.html#autotoc_md445", [
          [ "비동기 로그 출력", "d5/d3e/md_docs_2ko_2api_2middleware.html#autotoc_md446", null ]
        ] ],
        [ "속도 제한 미들웨어", "d5/d3e/md_docs_2ko_2api_2middleware.html#autotoc_md447", null ],
        [ "CORS 미들웨어", "d5/d3e/md_docs_2ko_2api_2middleware.html#autotoc_md448", null ],
        [ "Prometheus 메트릭 미들웨어", "d5/d3e/md_docs_2ko_2api_2middleware.html#autotoc_md449", null ],
        [ "JWT 인증 미들웨어", "d5/d3e/md_docs_2ko_2api_2middleware.html#autotoc_md450", null ],
        [ "압축 미들웨어", "d5/d3e/md_docs_2ko_2api_2middleware.html#autotoc_md451", null ]
      ] ],
      [ "사용자 정의 미들웨어 만들기", "d5/d3e/md_docs_2ko_2api_2middleware.html#autotoc_md452", null ]
    ] ],
    [ "쿼리와 URI API", "dd/da8/md_docs_2ko_2api_2query.html", null ],
    [ "데이터베이스 API", "d5/d75/md_docs_2ko_2api_2sql.html", [
      [ "스키마 마이그레이션", "d5/d75/md_docs_2ko_2api_2sql.html#autotoc_md462", [
        [ "<tt>cwist_db_open</tt>", "d5/d75/md_docs_2ko_2api_2sql.html#autotoc_md457", null ],
        [ "<tt>cwist_db_open_memory</tt>", "d5/d75/md_docs_2ko_2api_2sql.html#autotoc_md458", null ],
        [ "<tt>cwist_db_serialize</tt>", "d5/d75/md_docs_2ko_2api_2sql.html#autotoc_md459", null ],
        [ "<tt>cwist_db_exec</tt>", "d5/d75/md_docs_2ko_2api_2sql.html#autotoc_md460", null ],
        [ "<tt>cwist_db_query</tt>", "d5/d75/md_docs_2ko_2api_2sql.html#autotoc_md461", null ],
        [ "반환 코드", "d5/d75/md_docs_2ko_2api_2sql.html#autotoc_md463", null ],
        [ "<tt>cwist_migration_t</tt>", "d5/d75/md_docs_2ko_2api_2sql.html#autotoc_md464", null ],
        [ "<tt>cwist_migrate_up</tt>", "d5/d75/md_docs_2ko_2api_2sql.html#autotoc_md465", null ],
        [ "<tt>cwist_migrate_down</tt>", "d5/d75/md_docs_2ko_2api_2sql.html#autotoc_md466", null ],
        [ "<tt>cwist_migrate_version</tt>", "d5/d75/md_docs_2ko_2api_2sql.html#autotoc_md467", null ]
      ] ]
    ] ],
    [ "Server-Sent Events API", "d3/df7/md_docs_2ko_2api_2sse.html", [
      [ "타입", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md469", [
        [ "<tt>cwist_sse_event_t</tt>", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md470", null ],
        [ "<tt>cwist_sse_stream_t</tt>", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md471", null ]
      ] ],
      [ "응답 모드", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md472", [
        [ "<tt>cwist_sse_response_init</tt>", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md473", null ],
        [ "<tt>cwist_sse_response_event</tt>", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md474", null ],
        [ "<tt>cwist_sse_response_write</tt>", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md475", null ],
        [ "<tt>cwist_sse_response_comment</tt>", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md476", null ]
      ] ],
      [ "스트림 모드", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md477", [
        [ "<tt>cwist_sse_stream_open</tt>", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md478", null ],
        [ "<tt>cwist_sse_stream_send</tt>", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md479", null ],
        [ "<tt>cwist_sse_stream_write</tt>", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md480", null ],
        [ "<tt>cwist_sse_stream_comment</tt>", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md481", null ],
        [ "<tt>cwist_sse_stream_close</tt>", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md482", null ]
      ] ],
      [ "편의 매크로", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md483", null ],
      [ "예제", "d3/df7/md_docs_2ko_2api_2sse.html#autotoc_md484", null ]
    ] ],
    [ "SString API", "d6/d66/md_docs_2ko_2api_2sstring.html", null ],
    [ "WASI 대상", "d0/d3c/md_docs_2ko_2api_2wasi.html", [
      [ "종류 감지", "d0/d3c/md_docs_2ko_2api_2wasi.html#autotoc_md495", null ],
      [ "WASI 0.2 소켓 서버", "d0/d3c/md_docs_2ko_2api_2wasi.html#autotoc_md496", null ],
      [ "WASI preview1 (폐기됨)", "d0/d3c/md_docs_2ko_2api_2wasi.html#autotoc_md497", [
        [ "preview1 빌드가 다르게 했던 것", "d0/d3c/md_docs_2ko_2api_2wasi.html#autotoc_md498", null ],
        [ "검증 (기록)", "d0/d3c/md_docs_2ko_2api_2wasi.html#autotoc_md499", null ]
      ] ]
    ] ],
    [ "WASM 컴포넌트 모델 위의 CWIST", "dd/d41/md_docs_2ko_2api_2wasm-component.html", [
      [ "이유", "dd/d41/md_docs_2ko_2api_2wasm-component.html#autotoc_md501", null ],
      [ "경계", "dd/d41/md_docs_2ko_2api_2wasm-component.html#autotoc_md502", null ],
      [ "도구 체인 현황 (2026-09-21 측정)", "dd/d41/md_docs_2ko_2api_2wasm-component.html#autotoc_md503", null ],
      [ "평가 완료: preview3-shim (2026-09-23)", "dd/d41/md_docs_2ko_2api_2wasm-component.html#autotoc_md504", null ],
      [ "반영 완료: 스트리밍/SSE를 위한 네이티브 비동기 (2026-09-23)", "dd/d41/md_docs_2ko_2api_2wasm-component.html#autotoc_md505", null ],
      [ "해결됨: wasip3에서의 소켓 요청 경로 (2026-09-22 감사)", "dd/d41/md_docs_2ko_2api_2wasm-component.html#autotoc_md506", null ],
      [ "단계", "dd/d41/md_docs_2ko_2api_2wasm-component.html#autotoc_md507", null ],
      [ "브라우저 런타임 관문 (WASI 0.2)", "dd/d41/md_docs_2ko_2api_2wasm-component.html#autotoc_md508", null ],
      [ "2단계를 만들며 측정한 것", "dd/d41/md_docs_2ko_2api_2wasm-component.html#autotoc_md509", null ],
      [ "목표가 아닌 것", "dd/d41/md_docs_2ko_2api_2wasm-component.html#autotoc_md510", null ]
    ] ],
    [ "WASM 위의 CWIST (Emscripten)", "d2/d56/md_docs_2ko_2api_2wasm.html", [
      [ "빌드하기", "d2/d56/md_docs_2ko_2api_2wasm.html#autotoc_md512", null ],
      [ "WASM 빌드에 들어 있는 것", "d2/d56/md_docs_2ko_2api_2wasm.html#autotoc_md513", null ],
      [ "JS에서 요청 디스패치하기", "d2/d56/md_docs_2ko_2api_2wasm.html#autotoc_md514", null ],
      [ "제로 카피 뷰: <tt>cwist/wasm/typedarray.h</tt>", "d2/d56/md_docs_2ko_2api_2wasm.html#autotoc_md515", null ],
      [ "WASM에서의 데이터베이스", "d2/d56/md_docs_2ko_2api_2wasm.html#autotoc_md516", null ],
      [ "<tt>cwist_db</tt>가 번들 크기에 주는 영향", "d2/d56/md_docs_2ko_2api_2wasm.html#autotoc_md517", null ],
      [ "세션과 쿠키", "d2/d56/md_docs_2ko_2api_2wasm.html#autotoc_md518", null ],
      [ "경계를 통과하는 스트리밍", "d2/d56/md_docs_2ko_2api_2wasm.html#autotoc_md519", null ],
      [ "실행 가능한 예제: Service Worker 앱", "d2/d56/md_docs_2ko_2api_2wasm.html#autotoc_md520", null ],
      [ "(아직) 다루지 않는 것", "d2/d56/md_docs_2ko_2api_2wasm.html#autotoc_md521", null ],
      [ "스트리밍 생산자 (핸들러가 본문을 청크 단위로 생성)", "d2/d56/md_docs_2ko_2api_2wasm.html#autotoc_md522", null ]
    ] ],
    [ "WebSocket API", "d7/dec/md_docs_2ko_2api_2websocket.html", [
      [ "함수", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md524", [
        [ "<tt>cwist_websocket_upgrade</tt>", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md525", null ],
        [ "<tt>cwist_websocket_receive</tt>", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md526", null ],
        [ "<tt>cwist_websocket_send</tt>", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md527", null ]
      ] ],
      [ "Opcode 종류", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md528", null ],
      [ "<tt>cwist_app</tt>과의 연동", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md529", null ],
      [ "비동기 (C1M) WebSocket API", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md530", [
        [ "<tt>cwist_app_ws_async</tt>", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md531", null ],
        [ "<tt>cwist_ws_on_message_t</tt>", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md532", null ],
        [ "<tt>cwist_websocket_async_send</tt>", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md533", null ],
        [ "<tt>cwist_websocket_async_close</tt>", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md534", null ]
      ] ],
      [ "GraphQL 구독 (graphql-ws)", "d7/dec/md_docs_2ko_2api_2websocket.html#autotoc_md535", null ]
    ] ],
    [ "full GC에서의 연기된 HTTP 소유권", "d3/dcc/md_docs_2ko_2async-gc-ownership.html", [
      [ "인계 순서", "d3/dcc/md_docs_2ko_2async-gc-ownership.html#autotoc_md537", null ],
      [ "소유권 목록", "d3/dcc/md_docs_2ko_2async-gc-ownership.html#autotoc_md538", [
        [ "빌린 상태와 외부 상태", "d3/dcc/md_docs_2ko_2async-gc-ownership.html#autotoc_md539", null ]
      ] ],
      [ "HTTP/2 큐 소유권", "d3/dcc/md_docs_2ko_2async-gc-ownership.html#autotoc_md540", null ],
      [ "검증과 한계", "d3/dcc/md_docs_2ko_2async-gc-ownership.html#autotoc_md541", null ],
      [ "스케줄러와 IO 큐 소유권", "d3/dcc/md_docs_2ko_2async-gc-ownership.html#autotoc_md542", null ]
    ] ],
    [ "C1M 파일 한도", "db/dc4/md_docs_2ko_2c1m-file-limits.html", [
      [ "원인", "db/dc4/md_docs_2ko_2c1m-file-limits.html#autotoc_md544", null ],
      [ "변경", "db/dc4/md_docs_2ko_2c1m-file-limits.html#autotoc_md545", null ],
      [ "조정", "db/dc4/md_docs_2ko_2c1m-file-limits.html#autotoc_md546", null ],
      [ "검증", "db/dc4/md_docs_2ko_2c1m-file-limits.html#autotoc_md547", null ]
    ] ],
    [ "Classic 풀의 버스트 기아 (#25 후속)", "da/d07/md_docs_2ko_2classic-pool-starvation.html", [
      [ "원인과 변경", "da/d07/md_docs_2ko_2classic-pool-starvation.html#autotoc_md549", null ],
      [ "결정론적 회귀 테스트", "da/d07/md_docs_2ko_2classic-pool-starvation.html#autotoc_md550", null ],
      [ "검증 상태", "da/d07/md_docs_2ko_2classic-pool-starvation.html#autotoc_md551", null ],
      [ "HTTP 측정과 한계", "da/d07/md_docs_2ko_2classic-pool-starvation.html#autotoc_md552", null ]
    ] ],
    [ "C1M 리액터 묶음 비우기에서의 협력적 큐잉 (이슈 #25)", "da/d90/md_docs_2ko_2cooperative-queuing.html", [
      [ "배경", "da/d90/md_docs_2ko_2cooperative-queuing.html#autotoc_md554", null ],
      [ "빈틈", "da/d90/md_docs_2ko_2cooperative-queuing.html#autotoc_md555", null ],
      [ "수정", "da/d90/md_docs_2ko_2cooperative-queuing.html#autotoc_md556", null ],
      [ "측정된 효과", "da/d90/md_docs_2ko_2cooperative-queuing.html#autotoc_md557", null ],
      [ "이 수정이 해결하지 않는 것", "da/d90/md_docs_2ko_2cooperative-queuing.html#autotoc_md558", null ],
      [ "조정", "da/d90/md_docs_2ko_2cooperative-queuing.html#autotoc_md559", null ]
    ] ],
    [ "ADR-0001: 명시적인 공개 FIXED 계약으로 HTTP 응답 캐싱 복원하기", "d6/dda/md_docs_2ko_2decisions_20001-safe-public-fixed-response-cache.html", [
      [ "배경", "d6/dda/md_docs_2ko_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md561", null ],
      [ "결정", "d6/dda/md_docs_2ko_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md562", [
        [ "1. 하나의 파싱된 자격 경계", "d6/dda/md_docs_2ko_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md563", null ],
        [ "2. 와이어 응답이 아니라 표현을 캐시하기", "d6/dda/md_docs_2ko_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md564", null ],
        [ "3. 소유권, 출처, 용량 격리하기", "d6/dda/md_docs_2ko_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md565", null ],
        [ "4. 호환성 변경을 명시하기", "d6/dda/md_docs_2ko_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md566", null ]
      ] ],
      [ "검토한 대안", "d6/dda/md_docs_2ko_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md567", null ],
      [ "결과", "d6/dda/md_docs_2ko_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md568", null ],
      [ "구현과 릴리스 관문", "d6/dda/md_docs_2ko_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md569", null ],
      [ "근거와 소스 기준점", "d6/dda/md_docs_2ko_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md570", null ]
    ] ],
    [ "Wasmtime으로 WASI 0.2 예제 배포하기", "d1/dc3/md_docs_2ko_2deployment_2wasmtime-appliance.html", [
      [ "범위와 안전", "d1/dc3/md_docs_2ko_2deployment_2wasmtime-appliance.html#autotoc_md572", null ],
      [ "1. 알려진 리비전 빌드하기", "d1/dc3/md_docs_2ko_2deployment_2wasmtime-appliance.html#autotoc_md573", null ],
      [ "2. 산출물과 상태 분리하기", "d1/dc3/md_docs_2ko_2deployment_2wasmtime-appliance.html#autotoc_md574", null ],
      [ "3. 시작하고 검증하기", "d1/dc3/md_docs_2ko_2deployment_2wasmtime-appliance.html#autotoc_md575", null ],
      [ "4. 재시작, 백업, 복원", "d1/dc3/md_docs_2ko_2deployment_2wasmtime-appliance.html#autotoc_md576", null ],
      [ "프로덕션에 맞게 고치기 전에", "d1/dc3/md_docs_2ko_2deployment_2wasmtime-appliance.html#autotoc_md577", null ]
    ] ],
    [ "소켓 핸드오프로 무중단 재시작하기", "d1/d45/md_docs_2ko_2deployment_2zero-downtime-restart.html", [
      [ "요구 사항", "d1/d45/md_docs_2ko_2deployment_2zero-downtime-restart.html#autotoc_md579", null ],
      [ "핸드오프 순서", "d1/d45/md_docs_2ko_2deployment_2zero-downtime-restart.html#autotoc_md580", null ],
      [ "systemd 통합", "d1/d45/md_docs_2ko_2deployment_2zero-downtime-restart.html#autotoc_md581", null ],
      [ "UDP와 HTTP/3", "d1/d45/md_docs_2ko_2deployment_2zero-downtime-restart.html#autotoc_md582", null ],
      [ "배포 검증", "d1/d45/md_docs_2ko_2deployment_2zero-downtime-restart.html#autotoc_md583", null ]
    ] ],
    [ "영속 큐 필수 게이트", "dd/dd4/md_docs_2ko_2durable-queue-gate.html", [
      [ "로컬 실행", "dd/dd4/md_docs_2ko_2durable-queue-gate.html#autotoc_md585", null ],
      [ "격리된 통합 게이트", "dd/dd4/md_docs_2ko_2durable-queue-gate.html#autotoc_md586", null ],
      [ "CI", "dd/dd4/md_docs_2ko_2durable-queue-gate.html#autotoc_md587", null ]
    ] ],
    [ "FIXED 엔드포인트 캐싱: 현재 동작", "d7/dab/md_docs_2ko_2fixed-cache-status.html", [
      [ "지금 런타임이 실제로 하는 일", "d7/dab/md_docs_2ko_2fixed-cache-status.html#autotoc_md589", null ],
      [ "근거", "d7/dab/md_docs_2ko_2fixed-cache-status.html#autotoc_md590", null ],
      [ "학습 블록을 그냥 옮기지 않는 이유", "d7/dab/md_docs_2ko_2fixed-cache-status.html#autotoc_md591", null ],
      [ "계약이 복원될 때까지의 권장 처리", "d7/dab/md_docs_2ko_2fixed-cache-status.html#autotoc_md592", null ]
    ] ],
    [ "Full GC: CWIST의 자동 자원 회수", "d1/dde/md_docs_2ko_2GC.html", [
      [ "동기", "d1/dde/md_docs_2ko_2GC.html#autotoc_md594", null ],
      [ "설계", "d1/dde/md_docs_2ko_2GC.html#autotoc_md595", [
        [ "1. <tt>cwist_full_gc(bool)</tt> - 전역 스위치", "d1/dde/md_docs_2ko_2GC.html#autotoc_md596", null ],
        [ "2. 회수 엔진: libttak 에포크 GC", "d1/dde/md_docs_2ko_2GC.html#autotoc_md597", null ],
        [ "3. 스레드/프로세스 종료 정리", "d1/dde/md_docs_2ko_2GC.html#autotoc_md598", null ],
        [ "4. 핸들 같은 지역 변수를 위한 의사 RAII", "d1/dde/md_docs_2ko_2GC.html#autotoc_md599", null ],
        [ "5. 투명한 <tt>malloc</tt> 가로채기", "d1/dde/md_docs_2ko_2GC.html#autotoc_md600", null ],
        [ "6. <tt>cwist_alloc</tt> 내부", "d1/dde/md_docs_2ko_2GC.html#autotoc_md601", null ],
        [ "7. 명시적 flush와 회수", "d1/dde/md_docs_2ko_2GC.html#autotoc_md602", null ]
      ] ],
      [ "의미 요약", "d1/dde/md_docs_2ko_2GC.html#autotoc_md603", null ],
      [ "목표가 아닌 것과 참고 사항", "d1/dde/md_docs_2ko_2GC.html#autotoc_md604", null ],
      [ "알려진 성능 주의 사항", "d1/dde/md_docs_2ko_2GC.html#autotoc_md605", null ]
    ] ],
    [ "HTTP/1.1 파이프라인 공정성과 프레이밍", "d4/dd7/md_docs_2ko_2http-reactor-fairness.html", [
      [ "범위", "d4/dd7/md_docs_2ko_2http-reactor-fairness.html#autotoc_md607", null ],
      [ "동작 회귀 테스트", "d4/dd7/md_docs_2ko_2http-reactor-fairness.html#autotoc_md608", null ],
      [ "성능 주장의 경계", "d4/dd7/md_docs_2ko_2http-reactor-fairness.html#autotoc_md609", null ],
      [ "내부 API 호환성", "d4/dd7/md_docs_2ko_2http-reactor-fairness.html#autotoc_md610", null ]
    ] ],
    [ "선택적 wrk 원시/보정 지연 히스토그램", "d2/de4/md_docs_2ko_2performance_2wrk-dual-histogram.html", [
      [ "버려도 되는 wrk 체크아웃에서 빌드하기", "d2/de4/md_docs_2ko_2performance_2wrk-dual-histogram.html#autotoc_md612", null ],
      [ "출력과 해석", "d2/de4/md_docs_2ko_2performance_2wrk-dual-histogram.html#autotoc_md613", null ],
      [ "검증", "d2/de4/md_docs_2ko_2performance_2wrk-dual-histogram.html#autotoc_md614", null ]
    ] ],
    [ "io_uring 게시 작업 깨우기 회귀 (#25)", "dc/dd4/md_docs_2ko_2reactor-wakeup-regression.html", [
      [ "범위와 원인", "dc/dd4/md_docs_2ko_2reactor-wakeup-regression.html#autotoc_md616", null ],
      [ "회귀 테스트", "dc/dd4/md_docs_2ko_2reactor-wakeup-regression.html#autotoc_md617", null ],
      [ "구성 요소 측정 (2026-09-12)", "dc/dd4/md_docs_2ko_2reactor-wakeup-regression.html#autotoc_md618", null ]
    ] ],
    [ "CWIST 라이브러리 레퍼런스", "d3/d19/md_docs_2ko_2REFERENCE.html", [
      [ "모듈", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md628", [
        [ "1. SString (스마트 문자열)", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md629", null ],
        [ "2. HTTP 서버", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md630", null ],
        [ "3. HTTPS 지원", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md631", null ],
        [ "4. 쿼리 파싱", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md632", null ],
        [ "5. 데이터베이스 (SQL)", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md633", null ],
        [ "6. SipHash", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md634", null ],
        [ "7. 에러 처리", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md635", null ],
        [ "8. Big Dumb Reply (BDR)", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md636", null ]
      ] ],
      [ "LibTTAK 메모리 기능", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md637", null ],
      [ "예제: Othello Web", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md638", null ],
      [ "의존성", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md639", null ],
      [ "빌드", "d3/d19/md_docs_2ko_2REFERENCE.html#autotoc_md640", null ]
    ] ],
    [ "Mux 알고리즘 참고 자료", "d4/d7c/md_docs_2ko_2references_2mux__algorithm__references.html", [
      [ "1. 낙서 마방진 (洛書)", "d4/d7c/md_docs_2ko_2references_2mux__algorithm__references.html#autotoc_md643", null ],
      [ "2. 직교 라틴 방진 순열", "d4/d7c/md_docs_2ko_2references_2mux__algorithm__references.html#autotoc_md645", null ],
      [ "3. 계수 행렬과 선형 연립방정식 풀이", "d4/d7c/md_docs_2ko_2references_2mux__algorithm__references.html#autotoc_md647", null ],
      [ "4. 반복적인 크기 정제", "d4/d7c/md_docs_2ko_2references_2mux__algorithm__references.html#autotoc_md649", null ],
      [ "5. 단일 단계 선형 외삽", "d4/d7c/md_docs_2ko_2references_2mux__algorithm__references.html#autotoc_md651", null ],
      [ "이름을 바꾼 이유", "d4/d7c/md_docs_2ko_2references_2mux__algorithm__references.html#autotoc_md653", null ]
    ] ],
    [ "런타임 설정", "d5/d08/md_docs_2ko_2runtime-config.html", [
      [ "상태", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md655", null ],
      [ "프로세스와 서버 모드", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md656", null ],
      [ "Classic 스레드 풀", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md657", null ],
      [ "HTTP/1.1", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md658", null ],
      [ "HTTPS", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md659", null ],
      [ "HTTP/2", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md660", null ],
      [ "HTTP/3", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md661", null ],
      [ "WebSocket", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md662", null ],
      [ "Reactor", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md663", null ],
      [ "진단", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md664", null ]
    ] ],
    [ "CWIST로 CRUD 블로그 만들기", "d6/d25/md_docs_2ko_2tutorials_2blog-crud.html", [
      [ "1. 애플리케이션 만들기", "d6/d25/md_docs_2ko_2tutorials_2blog-crud.html#autotoc_md666", null ],
      [ "2. 라우트 정의하기", "d6/d25/md_docs_2ko_2tutorials_2blog-crud.html#autotoc_md667", null ],
      [ "3. JSON을 검증하고 HTTP 의미에 맞게 응답하기", "d6/d25/md_docs_2ko_2tutorials_2blog-crud.html#autotoc_md668", null ],
      [ "4. API 테스트하기", "d6/d25/md_docs_2ko_2tutorials_2blog-crud.html#autotoc_md669", null ]
    ] ],
    [ "CWIST와 NATS 연동하기", "dd/d38/md_docs_2ko_2tutorials_2nats-integration.html", [
      [ "1. 요청과 워커의 책임 나누기", "dd/d38/md_docs_2ko_2tutorials_2nats-integration.html#autotoc_md671", null ],
      [ "2. 라우트에서 게시하기", "dd/d38/md_docs_2ko_2tutorials_2nats-integration.html#autotoc_md672", null ],
      [ "3. 신뢰성 점검 목록", "dd/d38/md_docs_2ko_2tutorials_2nats-integration.html#autotoc_md673", null ]
    ] ],
    [ "OpenAPI와 Swagger UI로 API 문서화하기", "dc/dbf/md_docs_2ko_2tutorials_2openapi-swagger.html", [
      [ "1. 라우트에 주석 달기", "dc/dbf/md_docs_2ko_2tutorials_2openapi-swagger.html#autotoc_md675", [
        [ "주석 태그", "dc/dbf/md_docs_2ko_2tutorials_2openapi-swagger.html#autotoc_md676", null ]
      ] ],
      [ "2. <tt>openapi.json</tt> 생성하기", "dc/dbf/md_docs_2ko_2tutorials_2openapi-swagger.html#autotoc_md677", null ],
      [ "3. UI 제공하기", "dc/dbf/md_docs_2ko_2tutorials_2openapi-swagger.html#autotoc_md678", null ]
    ] ],
    [ "CWIST로 WebTransport 서버 만들기", "df/d10/md_docs_2ko_2tutorials_2webtransport-server.html", [
      [ "빌드", "df/d10/md_docs_2ko_2tutorials_2webtransport-server.html#autotoc_md680", null ],
      [ "1. HTTP/3과 세션 핸들러 설정하기", "df/d10/md_docs_2ko_2tutorials_2webtransport-server.html#autotoc_md681", null ],
      [ "2. 세션 프로토콜 설계하기", "df/d10/md_docs_2ko_2tutorials_2webtransport-server.html#autotoc_md682", null ],
      [ "3. 안전하게 운영하기", "df/d10/md_docs_2ko_2tutorials_2webtransport-server.html#autotoc_md683", null ]
    ] ],
    [ "Release Codename Sequencing Specification", "d6/de8/md_docs_2ko_2versioning.html", [
      [ "1. 기본 원칙", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md686", null ],
      [ "2. 2자리 시퀀스 (기본)", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md688", null ],
      [ "3. 3자리 확장 시퀀스", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md690", null ],
      [ "4. 표기", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md692", null ],
      [ "5. API 안정성 게이트 (v4 LTS부터)", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md694", null ],
      [ "6. 메이저 버전의 첫 상승", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md696", null ]
    ] ],
    [ "웹 서버 벤치마크 측정 방법", "d8/d7e/md_docs_2ko_2webserver-benchmark.html", [
      [ "부하 프로필", "d8/d7e/md_docs_2ko_2webserver-benchmark.html#autotoc_md698", [
        [ "컨텍스트 스위치 열 해석하기", "d8/d7e/md_docs_2ko_2webserver-benchmark.html#autotoc_md699", null ],
        [ "튜닝된 저지연 실행", "d8/d7e/md_docs_2ko_2webserver-benchmark.html#autotoc_md700", null ]
      ] ],
      [ "CWIST", "d8/d7e/md_docs_2ko_2webserver-benchmark.html#autotoc_md701", null ],
      [ "Axum", "d8/d7e/md_docs_2ko_2webserver-benchmark.html#autotoc_md702", null ],
      [ "Gin (Go)", "d8/d7e/md_docs_2ko_2webserver-benchmark.html#autotoc_md703", null ],
      [ "Spring Boot (JVM 공정성 설정)", "d8/d7e/md_docs_2ko_2webserver-benchmark.html#autotoc_md704", null ],
      [ "기록하는 메타데이터", "d8/d7e/md_docs_2ko_2webserver-benchmark.html#autotoc_md705", null ],
      [ "지연 분포 차트", "d8/d7e/md_docs_2ko_2webserver-benchmark.html#autotoc_md706", null ],
      [ "알려진 한계", "d8/d7e/md_docs_2ko_2webserver-benchmark.html#autotoc_md707", null ]
    ] ],
    [ "HTTPS C100k gate", "d0/d49/md_docs_2performance_2https-c100k.html", [
      [ "What runs", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md709", null ],
      [ "Loopback port space", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md710", null ],
      [ "Results (2026-10-17, dev)", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md711", null ],
      [ "Results (2026-10-18, dev): C1M over TLS", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md712", null ],
      [ "Results (2026-10-11, dev)", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md713", [
        [ "Where the memory goes", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md714", null ]
      ] ],
      [ "Scaling past the gate on a single host", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md715", null ],
      [ "Running it", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md716", null ]
    ] ],
    [ "HTTPS pool task queue: static ring vs growable ring", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html", [
      [ "What changed", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md718", null ],
      [ "Setup", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md719", null ],
      [ "1. RSS of <tt>https_pool_init()</tt> alone", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md720", null ],
      [ "2. 4 workers, steady and churn load (3 alternating rounds)", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md721", null ],
      [ "3. Forcing the ring to grow: 1 worker, 2 pool threads, 6000 simultaneous connections", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md722", null ],
      [ "4. Order and loss under growth: <tt>tests/test_https_pool_queue.c</tt>", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md723", null ],
      [ "Not done / open", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md724", null ]
    ] ],
    [ "Issue #297 syscall-reduction audit (local, 12-core Ryzen 5600X)", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html", [
      [ "Baseline syscall profile", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md726", null ],
      [ "Item results", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md727", [
        [ "1.1 io_uring multishot accept — NEGATIVE (reverted)", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md728", null ],
        [ "1.2 Larger SQE batches / fewer io_uring_enter — ALREADY IMPLEMENTED / BLOCKED", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md729", null ],
        [ "1.3 Per-accept setsockopt removal — NEGATIVE (reverted)", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md730", null ],
        [ "1.4 Time caching — ALREADY IMPLEMENTED", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md731", null ]
      ] ],
      [ "What the profile says to do instead (ranked)", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md732", null ],
      [ "Method notes", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md733", null ]
    ] ],
    [ "Opt-in wrk raw/corrected latency histograms", "d1/dfd/md_docs_2performance_2wrk-dual-histogram.html", [
      [ "Build in a disposable wrk checkout", "d1/dfd/md_docs_2performance_2wrk-dual-histogram.html#autotoc_md735", null ],
      [ "Output and interpretation", "d1/dfd/md_docs_2performance_2wrk-dual-histogram.html#autotoc_md736", null ],
      [ "Verification", "d1/dfd/md_docs_2performance_2wrk-dual-histogram.html#autotoc_md737", null ]
    ] ],
    [ "io_uring posted-work wake-up regression (#25)", "dd/d56/md_docs_2reactor-wakeup-regression.html", [
      [ "Scope and cause", "dd/d56/md_docs_2reactor-wakeup-regression.html#autotoc_md739", null ],
      [ "Regression test", "dd/d56/md_docs_2reactor-wakeup-regression.html#autotoc_md740", null ],
      [ "Component measurement (2026-09-12)", "dd/d56/md_docs_2reactor-wakeup-regression.html#autotoc_md741", null ]
    ] ],
    [ "CWIST Library Reference", "da/dca/md_docs_2REFERENCE.html", [
      [ "Modules", "da/dca/md_docs_2REFERENCE.html#autotoc_md750", [
        [ "1. SString (Smart String)", "da/dca/md_docs_2REFERENCE.html#autotoc_md751", null ],
        [ "2. HTTP Server", "da/dca/md_docs_2REFERENCE.html#autotoc_md752", null ],
        [ "3. HTTPS Support", "da/dca/md_docs_2REFERENCE.html#autotoc_md753", null ],
        [ "4. Query Parsing", "da/dca/md_docs_2REFERENCE.html#autotoc_md754", null ],
        [ "5. Database (SQL)", "da/dca/md_docs_2REFERENCE.html#autotoc_md755", null ],
        [ "6. SipHash", "da/dca/md_docs_2REFERENCE.html#autotoc_md756", null ],
        [ "7. Error Handling", "da/dca/md_docs_2REFERENCE.html#autotoc_md757", null ],
        [ "8. Big Dumb Reply (BDR)", "da/dca/md_docs_2REFERENCE.html#autotoc_md758", null ]
      ] ],
      [ "LibTTAK Memory Features", "da/dca/md_docs_2REFERENCE.html#autotoc_md759", null ],
      [ "Example: Othello Web", "da/dca/md_docs_2REFERENCE.html#autotoc_md760", null ],
      [ "Dependencies", "da/dca/md_docs_2REFERENCE.html#autotoc_md761", null ],
      [ "Build", "da/dca/md_docs_2REFERENCE.html#autotoc_md762", null ]
    ] ],
    [ "Mux Algorithm References", "d6/d9f/md_docs_2references_2mux__algorithm__references.html", [
      [ "1. Lo Shu Magic Square (Naak-seo / 洛書)", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md765", null ],
      [ "2. Orthogonal Latin-Square Permutation", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md767", null ],
      [ "3. Coefficient Matrix & Linear System Solver", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md769", null ],
      [ "4. Iterative Magnitude Refinement", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md771", null ],
      [ "5. Single-Step Linear Extrapolation", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md773", null ],
      [ "Naming Rationale", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md775", null ]
    ] ],
    [ "Runtime configuration", "d9/dfa/md_docs_2runtime-config.html", [
      [ "Status", "d9/dfa/md_docs_2runtime-config.html#autotoc_md777", null ],
      [ "Processes and server mode", "d9/dfa/md_docs_2runtime-config.html#autotoc_md778", null ],
      [ "Classic thread pool", "d9/dfa/md_docs_2runtime-config.html#autotoc_md779", null ],
      [ "HTTP/1.1", "d9/dfa/md_docs_2runtime-config.html#autotoc_md780", null ],
      [ "HTTPS", "d9/dfa/md_docs_2runtime-config.html#autotoc_md781", null ],
      [ "HTTP/2", "d9/dfa/md_docs_2runtime-config.html#autotoc_md782", null ],
      [ "HTTP/3", "d9/dfa/md_docs_2runtime-config.html#autotoc_md783", null ],
      [ "WebSocket", "d9/dfa/md_docs_2runtime-config.html#autotoc_md784", null ],
      [ "Reactor", "d9/dfa/md_docs_2runtime-config.html#autotoc_md785", null ],
      [ "Diagnostics", "d9/dfa/md_docs_2runtime-config.html#autotoc_md786", null ]
    ] ],
    [ "Soak testing", "db/d05/md_docs_2soak-testing.html", [
      [ "Environment", "db/d05/md_docs_2soak-testing.html#autotoc_md788", null ],
      [ "Bench server and workload mix", "db/d05/md_docs_2soak-testing.html#autotoc_md789", null ],
      [ "Measurement points", "db/d05/md_docs_2soak-testing.html#autotoc_md790", null ],
      [ "PASS/FAIL criteria", "db/d05/md_docs_2soak-testing.html#autotoc_md791", null ],
      [ "Interpreting a failure", "db/d05/md_docs_2soak-testing.html#autotoc_md792", null ]
    ] ],
    [ "CWIST Web Development Tutorial (A to Z)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html", [
      [ "</blockquote>", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md794", null ],
      [ "Table of Contents", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md795", null ],
      [ "1. Hello World: Starting Your First Server", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md797", null ],
      [ "2. Routing and Handlers (Mux Router)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md799", null ],
      [ "3. JSON Parsing and Zod Schema Validation", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md801", null ],
      [ "4. Middleware (CORS, Logging)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md803", null ],
      [ "5. Databases and Migrations (SQLite)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md805", null ],
      [ "6. Authentication (JWT and DB Encryption)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md807", null ],
      [ "7. PQC TLS (Post-Quantum Hybrid Key Exchange)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md809", [
        [ "Security Policy Summary", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md810", null ]
      ] ],
      [ "</blockquote>", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md811", null ],
      [ "8. WebSocket Integration (Real-Time Bidirectional Communication)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md812", null ],
      [ "9. Template Engine and Static File Serving", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md814", null ],
      [ "10. Dynamic CSS Composer (WASM and SSR)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md816", [
        [ "Approach A: 100% Server-Side Rendering (SSR)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md817", null ],
        [ "Approach B: Client-Side Composition with React + WebAssembly (WASM)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md818", null ],
        [ "Wrapping Up", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md820", null ]
      ] ]
    ] ],
    [ "CWIST 웹 개발 튜토리얼 (A to Z)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html", [
      [ "</blockquote>", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md822", null ],
      [ "목차", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md823", null ],
      [ "1. Hello World: 첫 서버 띄우기", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md825", null ],
      [ "2. 라우팅과 핸들러 (Mux Router)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md827", null ],
      [ "3. JSON 파싱과 Zod 스키마 검증", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md829", null ],
      [ "4. 미들웨어 (CORS, Logging)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md831", null ],
      [ "5. 데이터베이스와 마이그레이션 (SQLite)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md833", null ],
      [ "6. 인증 (JWT 및 DB 암호화)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md835", null ],
      [ "7. PQC TLS (양자내성 하이브리드 키 교환)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md837", [
        [ "보안 정책 요약", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md838", null ]
      ] ],
      [ "</blockquote>", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md839", null ],
      [ "8. 웹소켓 연동 (실시간 양방향 통신)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md840", null ],
      [ "9. 템플릿 엔진과 정적 파일 제공", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md842", null ],
      [ "10. 동적 CSS 합성기 (WASM 및 SSR)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md844", [
        [ "방법 A: 100% Server-Side Rendering (SSR)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md845", null ],
        [ "방법 B: React + WebAssembly (WASM) 클라이언트 사이드 합성", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md846", null ],
        [ "마치며", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md848", null ]
      ] ]
    ] ],
    [ "Build a CRUD blog with CWIST", "d4/db0/md_docs_2tutorials_2blog-crud.html", [
      [ "1. Create the application", "d4/db0/md_docs_2tutorials_2blog-crud.html#autotoc_md850", null ],
      [ "2. Define the routes", "d4/db0/md_docs_2tutorials_2blog-crud.html#autotoc_md851", null ],
      [ "3. Validate JSON and return HTTP semantics", "d4/db0/md_docs_2tutorials_2blog-crud.html#autotoc_md852", null ],
      [ "4. Test the API", "d4/db0/md_docs_2tutorials_2blog-crud.html#autotoc_md853", null ]
    ] ],
    [ "Integrate NATS with CWIST", "da/d48/md_docs_2tutorials_2nats-integration.html", [
      [ "1. Separate request and worker responsibilities", "da/d48/md_docs_2tutorials_2nats-integration.html#autotoc_md855", null ],
      [ "2. Publish from a route", "da/d48/md_docs_2tutorials_2nats-integration.html#autotoc_md856", null ],
      [ "3. Reliability checklist", "da/d48/md_docs_2tutorials_2nats-integration.html#autotoc_md857", null ]
    ] ],
    [ "Document an API with OpenAPI and Swagger UI", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html", [
      [ "1. Annotate the routes", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html#autotoc_md859", [
        [ "Annotation tags", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html#autotoc_md860", null ]
      ] ],
      [ "2. Generate <tt>openapi.json</tt>", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html#autotoc_md861", null ],
      [ "3. Serve the UI", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html#autotoc_md862", null ]
    ] ],
    [ "Build a WebTransport server with CWIST", "d2/dcb/md_docs_2tutorials_2webtransport-server.html", [
      [ "Build", "d2/dcb/md_docs_2tutorials_2webtransport-server.html#autotoc_md864", null ],
      [ "1. Configure HTTP/3 and a session handler", "d2/dcb/md_docs_2tutorials_2webtransport-server.html#autotoc_md865", null ],
      [ "2. Design the session protocol", "d2/dcb/md_docs_2tutorials_2webtransport-server.html#autotoc_md866", null ],
      [ "3. Operate safely", "d2/dcb/md_docs_2tutorials_2webtransport-server.html#autotoc_md867", null ]
    ] ],
    [ "Release Codename Sequencing Specification", "d0/db4/md_docs_2versioning.html", [
      [ "1. Principles", "d0/db4/md_docs_2versioning.html#autotoc_md870", null ],
      [ "2. Two-letter sequence (default)", "d0/db4/md_docs_2versioning.html#autotoc_md872", null ],
      [ "3. Three-letter extension", "d0/db4/md_docs_2versioning.html#autotoc_md874", null ],
      [ "4. Notation", "d0/db4/md_docs_2versioning.html#autotoc_md876", null ],
      [ "5. API stability gate (from the v4 LTS)", "d0/db4/md_docs_2versioning.html#autotoc_md878", null ],
      [ "6. First ascent of a major version", "d0/db4/md_docs_2versioning.html#autotoc_md880", null ]
    ] ],
    [ "Web Server Benchmark Methodology", "d6/d42/md_docs_2webserver-benchmark.html", [
      [ "Load profile", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md882", [
        [ "Interpreting the context-switch column", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md883", null ],
        [ "Tuned low-latency run", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md884", null ]
      ] ],
      [ "CWIST", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md885", null ],
      [ "Actix", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md886", null ],
      [ "Axum", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md887", null ],
      [ "Gin (Go)", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md888", null ],
      [ "Spring Boot (JVM fairness configuration)", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md889", null ],
      [ "Recorded metadata", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md890", null ],
      [ "Latency distribution chart", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md891", null ],
      [ "Known limitations", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md892", null ]
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
"d1/d7a/net_2http_2query_8h.html#a85c42bf26fba0b85420412688e5781d5",
"d2/d7d/writer__fast_8h.html#a16d29465f9bdc2f8fc3de524bffc6e20ae526a8c934797fb2571868ee9206c873",
"d2/dfe/alloc_8h.html#a42c763d29fa97fa0fb23b0fd91dab294",
"d3/dc7/core_2utils_2json__builder_8h.html#a88a116a0cf5bb638deb16291e02717ce",
"d4/dca/cwist__redis_8c.html#add9ab710ae751669463a298f5084cf37",
"d5/da8/protobuf_8c_source.html",
"d6/dad/websocket__async_8h.html",
"d7/d95/https_8h.html#acbe47c933c7464136b05dbbd42864b80",
"d8/d80/jwt_8c.html#a99a9d7d8249d30e9ac1c349fee2ad480",
"d9/dab/net_2http_2http_8h.html#a5f7073935626544aabe5744073f4f91c",
"d9/dd1/md_docs_2GC.html#autotoc_md311",
"da/dc2/bind_8h.html#a454338c5b7f23725d0bd86c03fcc1c8b",
"db/d8a/structcwist__http__async__conn.html#a5e64d22487c86624d3f8ac5c16f3cd65",
"dc/d85/reactor_8h.html#a48813b43eecb4b6630de99cb0006f8df",
"dd/d38/md_docs_2ko_2tutorials_2nats-integration.html#autotoc_md673",
"dd/d56/http_8h.html#ae7b52eebcb4d266bd4ec47b2649c16eb",
"de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html",
"df/d4c/compress_8h.html#aa1b3515b51073e8b429cfa5f442ae56f",
"df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md848"
];

var SYNCONMSG = 'click to disable panel synchronisation';
var SYNCOFFMSG = 'click to enable panel synchronisation';