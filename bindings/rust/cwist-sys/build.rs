//! Finds an installed libcwist through pkg-config (`cwist.pc`), links it
//! statically, and generates the raw bindings with bindgen from the same
//! headers and include paths.

use std::env;
use std::path::PathBuf;

/// Functions bound in this crate: the surface the safe wrapper planned for
/// v3.8 needs (app lifecycle, routing with a user context, in-memory
/// dispatch, request/response strings and headers, error values).
const FUNCTIONS: &[&str] = &[
    "cwist_app_(create|destroy|listen|use|dispatch_memory)",
    "cwist_app_(get|post|put|delete|patch)(_ex)?",
    "cwist_http_header_(add|get|remove)",
    "cwist_http_method_to_string",
    "cwist_sstring_(create|destroy|assign|assign_len|append|append_len)",
    "cwist_query_map_get",
    "cwist_alloc",
    "cwist_free",
    "make_error",
    "cwist_error_dispose",
    // static inline helpers are invisible to bindgen; these are their exported
    // out-of-line forms (scripts/ci/check_inline_exports.py).
    "cwist_error_is_ok_extern",
    "cwist_endpoint_has_extern",
];

/// Handle types used only through pointers. Keeping them opaque keeps their
/// (large, internal) layouts out of the binding, so changes to them cannot
/// break it.
const OPAQUE: &[&str] = &["cwist_app", "cwist_query_map", "cwist_db", "cJSON", "sqlite3"];

fn main() {
    println!("cargo:rerun-if-changed=wrapper.h");
    println!("cargo:rerun-if-env-changed=PKG_CONFIG_PATH");

    let cwist = pkg_config::Config::new()
        .statik(true)
        .probe("cwist")
        .unwrap_or_else(|err| {
            panic!(
                "cwist-sys: libcwist was not found through pkg-config ({err}).\n\
                 Build and install CWIST first, for example:\n    \
                 make && make install PREFIX=$HOME/.local\n\
                 then set PKG_CONFIG_PATH=$HOME/.local/lib/pkgconfig"
            )
        });

    let mut builder = bindgen::Builder::default()
        .header("wrapper.h")
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        // C enums become integer constants, never Rust enums: a value the
        // C side adds later must not be undefined behaviour in Rust.
        .default_enum_style(bindgen::EnumVariation::Consts)
        .prepend_enum_name(false)
        .allowlist_var("CWIST_.*")
        .allowlist_type("cwist_(http_request|http_response|http_header_node|sstring|error_t)")
        .allowlist_type("cwist_handler(_ex)?_func|cwist_handler_ctx_destroy_func")
        .allowlist_type("cwist_middleware_func");
    for f in FUNCTIONS {
        builder = builder.allowlist_function(f);
    }
    for t in OPAQUE {
        // opaque_type() only changes how a type is emitted; it must also be
        // allowlisted, or fields that point to it have nothing to refer to.
        builder = builder.allowlist_type(t).opaque_type(t);
    }
    for dir in &cwist.include_paths {
        builder = builder.clang_arg(format!("-I{}", dir.display()));
    }
    for (name, value) in &cwist.defines {
        builder = builder.clang_arg(match value {
            Some(v) => format!("-D{name}={v}"),
            None => format!("-D{name}"),
        });
    }

    let bindings = builder
        .generate()
        .expect("cwist-sys: bindgen could not generate bindings from wrapper.h");
    let out = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR is set by cargo"));
    bindings
        .write_to_file(out.join("bindings.rs"))
        .expect("cwist-sys: could not write bindings.rs");
}
