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
      [ "Measured while building stage 2", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md212", null ],
      [ "Non-goals", "d8/d77/md_docs_2api_2wasm-component.html#autotoc_md213", null ]
    ] ],
    [ "CWIST on WASM (Emscripten)", "d5/d0e/md_docs_2api_2wasm.html", [
      [ "Building", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md215", null ],
      [ "What is in the WASM build", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md216", null ],
      [ "Request dispatch from JS", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md217", null ],
      [ "Zero-copy views: <tt>cwist/wasm/typedarray.h</tt>", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md218", null ],
      [ "Databases under WASM", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md219", null ],
      [ "Bundle size impact of <tt>cwist_db</tt>", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md220", null ],
      [ "Sessions and cookies", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md221", null ],
      [ "Streaming through the boundary", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md222", null ],
      [ "Runnable example: Service Worker app", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md223", null ],
      [ "Not covered (yet)", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md224", null ],
      [ "Streaming producer (handlers generate the body chunk-by-chunk)", "d5/d0e/md_docs_2api_2wasm.html#autotoc_md225", null ]
    ] ],
    [ "WebSocket API", "db/dc5/md_docs_2api_2websocket.html", [
      [ "Functions", "db/dc5/md_docs_2api_2websocket.html#autotoc_md227", [
        [ "<tt>cwist_websocket_upgrade</tt>", "db/dc5/md_docs_2api_2websocket.html#autotoc_md228", null ],
        [ "<tt>cwist_websocket_receive</tt>", "db/dc5/md_docs_2api_2websocket.html#autotoc_md229", null ],
        [ "<tt>cwist_websocket_send</tt>", "db/dc5/md_docs_2api_2websocket.html#autotoc_md230", null ]
      ] ],
      [ "Opcode Types", "db/dc5/md_docs_2api_2websocket.html#autotoc_md231", null ],
      [ "Integration with <tt>cwist_app</tt>", "db/dc5/md_docs_2api_2websocket.html#autotoc_md232", null ],
      [ "Async (C1M) WebSocket API", "db/dc5/md_docs_2api_2websocket.html#autotoc_md233", [
        [ "<tt>cwist_app_ws_async</tt>", "db/dc5/md_docs_2api_2websocket.html#autotoc_md234", null ],
        [ "<tt>cwist_ws_on_message_t</tt>", "db/dc5/md_docs_2api_2websocket.html#autotoc_md235", null ],
        [ "<tt>cwist_websocket_async_send</tt>", "db/dc5/md_docs_2api_2websocket.html#autotoc_md236", null ],
        [ "<tt>cwist_websocket_async_close</tt>", "db/dc5/md_docs_2api_2websocket.html#autotoc_md237", null ]
      ] ],
      [ "GraphQL subscriptions (graphql-ws)", "db/dc5/md_docs_2api_2websocket.html#autotoc_md238", null ]
    ] ],
    [ "Deferred HTTP ownership under full GC", "df/dc5/md_docs_2async-gc-ownership.html", [
      [ "Handoff sequence", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md240", null ],
      [ "Ownership inventory", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md241", [
        [ "Borrowed and external state", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md242", null ]
      ] ],
      [ "HTTP/2 queue ownership", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md243", null ],
      [ "Verification and limits", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md244", null ],
      [ "Scheduler and IO queue ownership", "df/dc5/md_docs_2async-gc-ownership.html#autotoc_md245", null ]
    ] ],
    [ "C1M file limits", "db/d4d/md_docs_2c1m-file-limits.html", [
      [ "Cause", "db/d4d/md_docs_2c1m-file-limits.html#autotoc_md247", null ],
      [ "Change", "db/d4d/md_docs_2c1m-file-limits.html#autotoc_md248", null ],
      [ "Tuning", "db/d4d/md_docs_2c1m-file-limits.html#autotoc_md249", null ],
      [ "Checks", "db/d4d/md_docs_2c1m-file-limits.html#autotoc_md250", null ]
    ] ],
    [ "Classic pool burst starvation (#25 follow-up)", "d9/d56/md_docs_2classic-pool-starvation.html", [
      [ "Cause and change", "d9/d56/md_docs_2classic-pool-starvation.html#autotoc_md252", null ],
      [ "Deterministic regression", "d9/d56/md_docs_2classic-pool-starvation.html#autotoc_md253", null ],
      [ "Validation status", "d9/d56/md_docs_2classic-pool-starvation.html#autotoc_md254", null ],
      [ "HTTP measurements and limits", "d9/d56/md_docs_2classic-pool-starvation.html#autotoc_md255", null ]
    ] ],
    [ "Cooperative queuing in the C1M reactor's batch drain (issue #25)", "d1/ddd/md_docs_2cooperative-queuing.html", [
      [ "Background", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md257", null ],
      [ "The gap", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md258", null ],
      [ "The fix", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md259", null ],
      [ "Measured effect", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md260", null ],
      [ "What this does not fix", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md261", null ],
      [ "Tuning", "d1/ddd/md_docs_2cooperative-queuing.html#autotoc_md262", null ]
    ] ],
    [ "ADR-0001: Restore HTTP response caching through an explicit public FIXED contract", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html", [
      [ "Context", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md264", null ],
      [ "Decision", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md265", [
        [ "1. One parsed eligibility boundary", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md266", null ],
        [ "2. Cache representations, not wire responses", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md267", null ],
        [ "3. Isolate ownership, provenance and capacity", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md268", null ],
        [ "4. Make the compatibility change explicit", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md269", null ]
      ] ],
      [ "Alternatives considered", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md270", null ],
      [ "Consequences", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md271", null ],
      [ "Implementation and release gates", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md272", null ],
      [ "Evidence and source anchors", "de/d51/md_docs_2decisions_20001-safe-public-fixed-response-cache.html#autotoc_md273", null ]
    ] ],
    [ "ADR-0002: TLS certificate hot reload keeps the session-ticket key (STEK) unchanged", "d1/dd3/md_docs_2decisions_20002-tls-hot-reload-stek.html", [
      [ "Context", "d1/dd3/md_docs_2decisions_20002-tls-hot-reload-stek.html#autotoc_md275", null ],
      [ "Decision", "d1/dd3/md_docs_2decisions_20002-tls-hot-reload-stek.html#autotoc_md276", null ],
      [ "Consequences", "d1/dd3/md_docs_2decisions_20002-tls-hot-reload-stek.html#autotoc_md277", null ]
    ] ],
    [ "Deploy the WASI 0.2 example with Wasmtime", "db/de3/md_docs_2deployment_2wasmtime-appliance.html", [
      [ "Scope and safety", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md280", null ],
      [ "1. Build a known revision", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md281", null ],
      [ "2. Separate the artifact from state", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md282", null ],
      [ "3. Start and verify", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md283", null ],
      [ "4. Restart, back up, and restore", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md284", null ],
      [ "Before adapting this for production", "db/de3/md_docs_2deployment_2wasmtime-appliance.html#autotoc_md285", null ]
    ] ],
    [ "Zero-downtime restart via socket handoff", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html", [
      [ "Requirements", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html#autotoc_md287", null ],
      [ "Handoff sequence", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html#autotoc_md288", null ],
      [ "systemd integration", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html#autotoc_md289", null ],
      [ "UDP and HTTP/3", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html#autotoc_md290", null ],
      [ "Verifying a deploy", "d1/d54/md_docs_2deployment_2zero-downtime-restart.html#autotoc_md291", null ]
    ] ],
    [ "FIXED endpoint caching: current behavior", "d3/ddd/md_docs_2fixed-cache-status.html", [
      [ "What the runtime actually does today", "d3/ddd/md_docs_2fixed-cache-status.html#autotoc_md293", null ],
      [ "Evidence", "d3/ddd/md_docs_2fixed-cache-status.html#autotoc_md294", null ],
      [ "Why the learning block is not simply moved", "d3/ddd/md_docs_2fixed-cache-status.html#autotoc_md295", null ],
      [ "Recommended handling", "d3/ddd/md_docs_2fixed-cache-status.html#autotoc_md296", null ]
    ] ],
    [ "Full GC: Automatic Resource Reclamation in CWIST", "d9/dd1/md_docs_2GC.html", [
      [ "Motivation", "d9/dd1/md_docs_2GC.html#autotoc_md298", null ],
      [ "Design", "d9/dd1/md_docs_2GC.html#autotoc_md299", [
        [ "1. <tt>cwist_full_gc(bool)</tt> — the global toggle", "d9/dd1/md_docs_2GC.html#autotoc_md300", null ],
        [ "2. Reclamation engine: libttak epoch GC", "d9/dd1/md_docs_2GC.html#autotoc_md301", null ],
        [ "3. Thread/process exit sweeps", "d9/dd1/md_docs_2GC.html#autotoc_md302", null ],
        [ "4. Pseudo-RAII for handle-like locals", "d9/dd1/md_docs_2GC.html#autotoc_md303", null ],
        [ "5. Transparent <tt>malloc</tt> interception", "d9/dd1/md_docs_2GC.html#autotoc_md304", null ],
        [ "6. <tt>cwist_alloc</tt> internals", "d9/dd1/md_docs_2GC.html#autotoc_md305", null ],
        [ "7. Explicit flushes and reclamation", "d9/dd1/md_docs_2GC.html#autotoc_md306", null ]
      ] ],
      [ "Semantics summary", "d9/dd1/md_docs_2GC.html#autotoc_md307", null ],
      [ "Non-goals and notes", "d9/dd1/md_docs_2GC.html#autotoc_md308", null ],
      [ "Known performance caveat", "d9/dd1/md_docs_2GC.html#autotoc_md309", null ]
    ] ],
    [ "HTTP/1.1 pipeline fairness and framing", "d2/dc2/md_docs_2http-reactor-fairness.html", [
      [ "Scope", "d2/dc2/md_docs_2http-reactor-fairness.html#autotoc_md311", null ],
      [ "Behavioral regression tests", "d2/dc2/md_docs_2http-reactor-fairness.html#autotoc_md312", null ],
      [ "Performance claim boundary", "d2/dc2/md_docs_2http-reactor-fairness.html#autotoc_md313", null ],
      [ "Internal API compatibility", "d2/dc2/md_docs_2http-reactor-fairness.html#autotoc_md314", null ]
    ] ],
    [ "런타임 설정", "d5/d08/md_docs_2ko_2runtime-config.html", [
      [ "상태", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md316", null ],
      [ "프로세스와 서버 모드", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md317", null ],
      [ "Classic 스레드 풀", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md318", null ],
      [ "HTTP/1.1", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md319", null ],
      [ "HTTPS", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md320", null ],
      [ "HTTP/2", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md321", null ],
      [ "HTTP/3", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md322", null ],
      [ "WebSocket", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md323", null ],
      [ "Reactor", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md324", null ],
      [ "진단", "d5/d08/md_docs_2ko_2runtime-config.html#autotoc_md325", null ]
    ] ],
    [ "Release Codename Sequencing Specification", "d6/de8/md_docs_2ko_2versioning.html", [
      [ "1. 기본 원칙", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md328", null ],
      [ "2. 2자리 시퀀스 (기본)", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md330", null ],
      [ "3. 3자리 확장 시퀀스", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md332", null ],
      [ "4. 표기", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md334", null ],
      [ "5. API 안정성 게이트 (v4 LTS부터)", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md336", null ],
      [ "6. 메이저 버전의 첫 상승", "d6/de8/md_docs_2ko_2versioning.html#autotoc_md338", null ]
    ] ],
    [ "HTTPS C100k gate", "d0/d49/md_docs_2performance_2https-c100k.html", [
      [ "What runs", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md340", null ],
      [ "Loopback port space", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md341", null ],
      [ "Results (2026-10-17, dev)", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md342", null ],
      [ "Results (2026-10-18, dev): C1M over TLS", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md343", null ],
      [ "Results (2026-10-11, dev)", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md344", [
        [ "Where the memory goes", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md345", null ]
      ] ],
      [ "Scaling past the gate on a single host", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md346", null ],
      [ "Running it", "d0/d49/md_docs_2performance_2https-c100k.html#autotoc_md347", null ]
    ] ],
    [ "HTTPS pool task queue: static ring vs growable ring", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html", [
      [ "What changed", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md349", null ],
      [ "Setup", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md350", null ],
      [ "1. RSS of <tt>https_pool_init()</tt> alone", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md351", null ],
      [ "2. 4 workers, steady and churn load (3 alternating rounds)", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md352", null ],
      [ "3. Forcing the ring to grow: 1 worker, 2 pool threads, 6000 simultaneous connections", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md353", null ],
      [ "4. Order and loss under growth: <tt>tests/test_https_pool_queue.c</tt>", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md354", null ],
      [ "Not done / open", "df/d28/md_docs_2performance_2https-pool-dynamic-queue.html#autotoc_md355", null ]
    ] ],
    [ "Issue #297 syscall-reduction audit (local, 12-core Ryzen 5600X)", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html", [
      [ "Baseline syscall profile", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md357", null ],
      [ "Item results", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md358", [
        [ "1.1 io_uring multishot accept — NEGATIVE (reverted)", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md359", null ],
        [ "1.2 Larger SQE batches / fewer io_uring_enter — ALREADY IMPLEMENTED / BLOCKED", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md360", null ],
        [ "1.3 Per-accept setsockopt removal — NEGATIVE (reverted)", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md361", null ],
        [ "1.4 Time caching — ALREADY IMPLEMENTED", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md362", null ]
      ] ],
      [ "What the profile says to do instead (ranked)", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md363", null ],
      [ "Method notes", "d0/d34/md_docs_2performance_2issue-297-syscall-audit.html#autotoc_md364", null ]
    ] ],
    [ "Opt-in wrk raw/corrected latency histograms", "d1/dfd/md_docs_2performance_2wrk-dual-histogram.html", [
      [ "Build in a disposable wrk checkout", "d1/dfd/md_docs_2performance_2wrk-dual-histogram.html#autotoc_md366", null ],
      [ "Output and interpretation", "d1/dfd/md_docs_2performance_2wrk-dual-histogram.html#autotoc_md367", null ],
      [ "Verification", "d1/dfd/md_docs_2performance_2wrk-dual-histogram.html#autotoc_md368", null ]
    ] ],
    [ "io_uring posted-work wake-up regression (#25)", "dd/d56/md_docs_2reactor-wakeup-regression.html", [
      [ "Scope and cause", "dd/d56/md_docs_2reactor-wakeup-regression.html#autotoc_md370", null ],
      [ "Regression test", "dd/d56/md_docs_2reactor-wakeup-regression.html#autotoc_md371", null ],
      [ "Component measurement (2026-09-12)", "dd/d56/md_docs_2reactor-wakeup-regression.html#autotoc_md372", null ]
    ] ],
    [ "CWIST Library Reference", "da/dca/md_docs_2REFERENCE.html", [
      [ "Modules", "da/dca/md_docs_2REFERENCE.html#autotoc_md381", [
        [ "1. SString (Smart String)", "da/dca/md_docs_2REFERENCE.html#autotoc_md382", null ],
        [ "2. HTTP Server", "da/dca/md_docs_2REFERENCE.html#autotoc_md383", null ],
        [ "3. HTTPS Support", "da/dca/md_docs_2REFERENCE.html#autotoc_md384", null ],
        [ "4. Query Parsing", "da/dca/md_docs_2REFERENCE.html#autotoc_md385", null ],
        [ "5. Database (SQL)", "da/dca/md_docs_2REFERENCE.html#autotoc_md386", null ],
        [ "6. SipHash", "da/dca/md_docs_2REFERENCE.html#autotoc_md387", null ],
        [ "7. Error Handling", "da/dca/md_docs_2REFERENCE.html#autotoc_md388", null ],
        [ "8. Big Dumb Reply (BDR)", "da/dca/md_docs_2REFERENCE.html#autotoc_md389", null ]
      ] ],
      [ "LibTTAK Memory Features", "da/dca/md_docs_2REFERENCE.html#autotoc_md390", null ],
      [ "Example: Othello Web", "da/dca/md_docs_2REFERENCE.html#autotoc_md391", null ],
      [ "Dependencies", "da/dca/md_docs_2REFERENCE.html#autotoc_md392", null ],
      [ "Build", "da/dca/md_docs_2REFERENCE.html#autotoc_md393", null ]
    ] ],
    [ "Mux Algorithm References", "d6/d9f/md_docs_2references_2mux__algorithm__references.html", [
      [ "1. Lo Shu Magic Square (Naak-seo / 洛書)", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md396", null ],
      [ "2. Orthogonal Latin-Square Permutation", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md398", null ],
      [ "3. Coefficient Matrix & Linear System Solver", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md400", null ],
      [ "4. Iterative Magnitude Refinement", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md402", null ],
      [ "5. Single-Step Linear Extrapolation", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md404", null ],
      [ "Naming Rationale", "d6/d9f/md_docs_2references_2mux__algorithm__references.html#autotoc_md406", null ]
    ] ],
    [ "v3.5.0 release roadmap", "d7/df9/md_docs_2roadmap-v3_85.html", [
      [ "Scope", "d7/df9/md_docs_2roadmap-v3_85.html#autotoc_md408", null ],
      [ "Release gates", "d7/df9/md_docs_2roadmap-v3_85.html#autotoc_md409", null ],
      [ "Evidence and limits", "d7/df9/md_docs_2roadmap-v3_85.html#autotoc_md410", null ]
    ] ],
    [ "Runtime configuration", "d9/dfa/md_docs_2runtime-config.html", [
      [ "Status", "d9/dfa/md_docs_2runtime-config.html#autotoc_md412", null ],
      [ "Processes and server mode", "d9/dfa/md_docs_2runtime-config.html#autotoc_md413", null ],
      [ "Classic thread pool", "d9/dfa/md_docs_2runtime-config.html#autotoc_md414", null ],
      [ "HTTP/1.1", "d9/dfa/md_docs_2runtime-config.html#autotoc_md415", null ],
      [ "HTTPS", "d9/dfa/md_docs_2runtime-config.html#autotoc_md416", null ],
      [ "HTTP/2", "d9/dfa/md_docs_2runtime-config.html#autotoc_md417", null ],
      [ "HTTP/3", "d9/dfa/md_docs_2runtime-config.html#autotoc_md418", null ],
      [ "WebSocket", "d9/dfa/md_docs_2runtime-config.html#autotoc_md419", null ],
      [ "Reactor", "d9/dfa/md_docs_2runtime-config.html#autotoc_md420", null ],
      [ "Diagnostics", "d9/dfa/md_docs_2runtime-config.html#autotoc_md421", null ]
    ] ],
    [ "Soak testing", "db/d05/md_docs_2soak-testing.html", [
      [ "Environment", "db/d05/md_docs_2soak-testing.html#autotoc_md423", null ],
      [ "Bench server and workload mix", "db/d05/md_docs_2soak-testing.html#autotoc_md424", null ],
      [ "Measurement points", "db/d05/md_docs_2soak-testing.html#autotoc_md425", null ],
      [ "PASS/FAIL criteria", "db/d05/md_docs_2soak-testing.html#autotoc_md426", null ],
      [ "Interpreting a failure", "db/d05/md_docs_2soak-testing.html#autotoc_md427", null ]
    ] ],
    [ "CWIST Web Development Tutorial (A to Z)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html", [
      [ "</blockquote>", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md429", null ],
      [ "Table of Contents", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md430", null ],
      [ "1. Hello World: Starting Your First Server", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md432", null ],
      [ "2. Routing and Handlers (Mux Router)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md434", null ],
      [ "3. JSON Parsing and Zod Schema Validation", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md436", null ],
      [ "4. Middleware (CORS, Logging)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md438", null ],
      [ "5. Databases and Migrations (SQLite)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md440", null ],
      [ "6. Authentication (JWT and DB Encryption)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md442", null ],
      [ "7. PQC TLS (Post-Quantum Hybrid Key Exchange)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md444", [
        [ "Security Policy Summary", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md445", null ]
      ] ],
      [ "</blockquote>", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md446", null ],
      [ "8. WebSocket Integration (Real-Time Bidirectional Communication)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md447", null ],
      [ "9. Template Engine and Static File Serving", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md449", null ],
      [ "10. Dynamic CSS Composer (WASM and SSR)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md451", [
        [ "Approach A: 100% Server-Side Rendering (SSR)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md452", null ],
        [ "Approach B: Client-Side Composition with React + WebAssembly (WASM)", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md453", null ],
        [ "Wrapping Up", "dd/d4c/md_docs_2tutorial_2cwist__tutorial.html#autotoc_md455", null ]
      ] ]
    ] ],
    [ "CWIST 웹 개발 튜토리얼 (A to Z)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html", [
      [ "</blockquote>", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md457", null ],
      [ "목차", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md458", null ],
      [ "1. Hello World: 첫 서버 띄우기", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md460", null ],
      [ "2. 라우팅과 핸들러 (Mux Router)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md462", null ],
      [ "3. JSON 파싱과 Zod 스키마 검증", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md464", null ],
      [ "4. 미들웨어 (CORS, Logging)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md466", null ],
      [ "5. 데이터베이스와 마이그레이션 (SQLite)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md468", null ],
      [ "6. 인증 (JWT 및 DB 암호화)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md470", null ],
      [ "7. PQC TLS (양자내성 하이브리드 키 교환)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md472", [
        [ "보안 정책 요약", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md473", null ]
      ] ],
      [ "</blockquote>", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md474", null ],
      [ "8. 웹소켓 연동 (실시간 양방향 통신)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md475", null ],
      [ "9. 템플릿 엔진과 정적 파일 제공", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md477", null ],
      [ "10. 동적 CSS 합성기 (WASM 및 SSR)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md479", [
        [ "방법 A: 100% Server-Side Rendering (SSR)", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md480", null ],
        [ "방법 B: React + WebAssembly (WASM) 클라이언트 사이드 합성", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md481", null ],
        [ "마치며", "df/df1/md_docs_2tutorial_2korean_2cwist__tutorial.html#autotoc_md483", null ]
      ] ]
    ] ],
    [ "Build a CRUD blog with CWIST", "d4/db0/md_docs_2tutorials_2blog-crud.html", [
      [ "1. Create the application", "d4/db0/md_docs_2tutorials_2blog-crud.html#autotoc_md485", null ],
      [ "2. Define the routes", "d4/db0/md_docs_2tutorials_2blog-crud.html#autotoc_md486", null ],
      [ "3. Validate JSON and return HTTP semantics", "d4/db0/md_docs_2tutorials_2blog-crud.html#autotoc_md487", null ],
      [ "4. Test the API", "d4/db0/md_docs_2tutorials_2blog-crud.html#autotoc_md488", null ]
    ] ],
    [ "Integrate NATS with CWIST", "da/d48/md_docs_2tutorials_2nats-integration.html", [
      [ "1. Separate request and worker responsibilities", "da/d48/md_docs_2tutorials_2nats-integration.html#autotoc_md490", null ],
      [ "2. Publish from a route", "da/d48/md_docs_2tutorials_2nats-integration.html#autotoc_md491", null ],
      [ "3. Reliability checklist", "da/d48/md_docs_2tutorials_2nats-integration.html#autotoc_md492", null ]
    ] ],
    [ "Document an API with OpenAPI and Swagger UI", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html", [
      [ "1. Annotate the routes", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html#autotoc_md494", [
        [ "Annotation tags", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html#autotoc_md495", null ]
      ] ],
      [ "2. Generate <tt>openapi.json</tt>", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html#autotoc_md496", null ],
      [ "3. Serve the UI", "d6/d8e/md_docs_2tutorials_2openapi-swagger.html#autotoc_md497", null ]
    ] ],
    [ "Build a WebTransport server with CWIST", "d2/dcb/md_docs_2tutorials_2webtransport-server.html", [
      [ "Build", "d2/dcb/md_docs_2tutorials_2webtransport-server.html#autotoc_md499", null ],
      [ "1. Configure HTTP/3 and a session handler", "d2/dcb/md_docs_2tutorials_2webtransport-server.html#autotoc_md500", null ],
      [ "2. Design the session protocol", "d2/dcb/md_docs_2tutorials_2webtransport-server.html#autotoc_md501", null ],
      [ "3. Operate safely", "d2/dcb/md_docs_2tutorials_2webtransport-server.html#autotoc_md502", null ]
    ] ],
    [ "Release Codename Sequencing Specification", "d0/db4/md_docs_2versioning.html", [
      [ "1. Principles", "d0/db4/md_docs_2versioning.html#autotoc_md505", null ],
      [ "2. Two-letter sequence (default)", "d0/db4/md_docs_2versioning.html#autotoc_md507", null ],
      [ "3. Three-letter extension", "d0/db4/md_docs_2versioning.html#autotoc_md509", null ],
      [ "4. Notation", "d0/db4/md_docs_2versioning.html#autotoc_md511", null ],
      [ "5. API stability gate (from the v4 LTS)", "d0/db4/md_docs_2versioning.html#autotoc_md513", null ],
      [ "6. First ascent of a major version", "d0/db4/md_docs_2versioning.html#autotoc_md515", null ]
    ] ],
    [ "Web Server Benchmark Methodology", "d6/d42/md_docs_2webserver-benchmark.html", [
      [ "Measurement contract v2", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md517", null ],
      [ "Admission and latency semantics", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md518", null ],
      [ "Resource measurements", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md519", [
        [ "Interpreting the context-switch column", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md520", null ]
      ] ],
      [ "Runtime configuration", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md521", null ],
      [ "Provenance, presentation and legacy results", "d6/d42/md_docs_2webserver-benchmark.html#autotoc_md522", null ]
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
"d1/d7e/md_docs_2api-quickref.html#autotoc_md28",
"d2/db0/http2_8c.html#a8eb11d103067a9bd77487bd3507d595c",
"d3/d65/tls__chain_8c.html#a9aa965c5db46729f7083407fa1245412",
"d4/d57/websocket__async_8c.html#aea47dc15a41ca2b941e768194f61be9d",
"d5/d78/builder_8h.html#ad573bafc31829b9b907369170937563d",
"d6/d9b/orm_8h.html#a1ce173b162b55a29c2fde9f59aa743d4",
"d7/d67/net_2http_2https_8h.html#acf6a58660a7dc7d7a6f0f3da3ee24300",
"d8/d80/jwt_8c.html#a449066f8812ceed8be3af264d626b123",
"d9/dab/net_2http_2http_8h.html#a4416bbcd7cb058bcb1a34a5b1bd18fd4",
"d9/dd1/md_docs_2GC.html#autotoc_md302",
"da/dc2/bind_8h.html#a915cde2d874761e285316cab0344d4b2",
"db/db7/pool_8h.html#aa1e09018ceaf9e09828563d70e26a5fa",
"dc/d91/structcwist__mux__route.html#a7cd82ac83249cfe5d35321172f95af9b",
"dd/d43/http_8c.html#a634dbffe29953aca5d6beab2d448f0f9",
"dd/d7f/query_8c.html#a93292394e0096daa1a997758f0346a93",
"de/daf/http3_8c.html#a0e633ec429baf49ca1ab37356642b844",
"df/d7f/app_8c.html#aad2e21f9e5d7a675696bcd15456e299c",
"functions_vars.html"
];

var SYNCONMSG = 'click to disable panel synchronisation';
var SYNCOFFMSG = 'click to enable panel synchronisation';