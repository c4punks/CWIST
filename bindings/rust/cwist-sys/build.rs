//! Finds an installed libcwist through pkg-config (`cwist.pc`), links it
//! statically, and generates the raw bindings with bindgen from the same
//! headers and include paths.

use std::env;
use std::path::PathBuf;

/// Functions bound in this crate: the surface the safe wrapper planned for
/// v3.8 needs (app lifecycle, routing with a user context, in-memory
/// dispatch, request/response strings and headers, error values).
const FUNCTIONS: &[&str] = &[
    "cwist_app_(create|destroy|listen|listen_ex|use|use_ex|dispatch_memory)",
    // Graceful shutdown of a running server (sys/app/shutdown.h).
    "cwist_shutdown_(request|reset)",
    "cwist_app_(get|post|put|delete|patch)(_ex)?",
    "cwist_http_header_(add|get|remove)",
    "cwist_http_response_(create|destroy)",
    "cwist_http_method_to_string",
    "cwist_sstring_(create|destroy|assign|assign_len|append|append_len)",
    "cwist_query_map_get",
    "cwist_async_(defer|retain|release|set_timeout|respond|respond_with|abort)",
    "cwist_mw_(request_id|access_log|rate_limit_ip|rate_limit_reset|metrics|cors|jwt_auth|compress|jwt_get_claims)",
    // Signs tokens for the JWT middleware (security/jwt/jwt.h).
    "cwist_jwt_sign",
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
const OPAQUE: &[&str] = &["cwist_app", "cwist_query_map", "cwist_db", "cJSON", "sqlite3", "cwist_async"];

/// System libraries `cwist.pc` names as plain `-l` flags. Their own pkg-config
/// files supply the directory when it is not a default one (Homebrew), the
/// same way the CWIST Makefile finds them.
const SYSTEM_DEPS: &[&str] =
    &["libzstd", "libbrotlienc", "libbrotlicommon", "libbrotlidec", "libcurl", "libnghttp2"];

/// Emits the link search paths and libraries from `cwist.pc`. Archives found
/// in the pkg-config link paths (libcwist and its bundled dependencies) are
/// linked statically, everything else as a system library.
///
/// CWIST's own directories are searched first: they hold the bundled
/// BoringSSL `libssl.a`/`libcrypto.a`, and another OpenSSL installed next to
/// the system libraries (Homebrew's, for one) must not be picked instead.
///
/// `cwist.pc` names the C++ runtime `-lstdc++`, which Clang's driver maps to
/// libc++ on Apple platforms. rustc links with `-nodefaultlibs`, so the name
/// reaches the linker unchanged there; use `c++` directly instead.
fn emit_link_flags(cwist: &pkg_config::Library) {
    let apple = env::var("CARGO_CFG_TARGET_VENDOR").as_deref() == Ok("apple");
    let mut search: Vec<PathBuf> = cwist.link_paths.clone();
    for dep in SYSTEM_DEPS {
        // Optional: without a .pc file the library must be on a default path.
        if let Ok(lib) = pkg_config::Config::new().cargo_metadata(false).probe(dep) {
            for dir in lib.link_paths {
                if !search.contains(&dir) {
                    search.push(dir);
                }
            }
        }
    }
    for dir in &search {
        println!("cargo:rustc-link-search=native={}", dir.display());
    }
    for lib in &cwist.libs {
        let lib = if apple && lib == "stdc++" { "c++" } else { lib.as_str() };
        let archive = format!("lib{lib}.a");
        let kind = if cwist.link_paths.iter().any(|dir| dir.join(&archive).is_file()) {
            "static"
        } else {
            "dylib"
        };
        println!("cargo:rustc-link-lib={kind}={lib}");
    }
}

fn main() {
    println!("cargo:rerun-if-changed=wrapper.h");
    println!("cargo:rerun-if-env-changed=PKG_CONFIG_PATH");

    let cwist = pkg_config::Config::new()
        .statik(true)
        // Link lines are emitted below, so the C++ runtime can be adjusted.
        .cargo_metadata(false)
        .probe("cwist")
        .unwrap_or_else(|err| {
            panic!(
                "cwist-sys: libcwist was not found through pkg-config ({err}).\n\
                 Build and install CWIST first, for example:\n    \
                 make && make install PREFIX=$HOME/.local\n\
                 then set PKG_CONFIG_PATH=$HOME/.local/lib/pkgconfig"
            )
        });

    emit_link_flags(&cwist);

    let mut builder = bindgen::Builder::default()
        .header("wrapper.h")
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        // C enums become integer constants, never Rust enums: a value the
        // C side adds later must not be undefined behaviour in Rust.
        .default_enum_style(bindgen::EnumVariation::Consts)
        .prepend_enum_name(false)
        // Some system stdatomic.h headers define _Atomic as a type-qualifier
        // macro that does not accept parenthesised types. Treat _Atomic(T) as
        // _Atomic T so CWIST atomic pointer fields parse on every host.
        .clang_arg("-D_Atomic=_Atomic")
        .allowlist_var("CWIST_.*")
        // Seconds cwist_app_listen*() waits for connections to drain after a
        // shutdown request.
        .allowlist_var("g_cwist_drain_timeout_sec")
        .allowlist_type("cwist_(http_request|http_response|http_header_node|sstring|error_t|http_status_t|jwt_claims)")
        .allowlist_type("cwist_handler(_ex)?_func|cwist_handler_ctx_destroy_func")
        .allowlist_type("cwist_middleware(_func|_func_ex|_ctx_destroy_func)")
        .allowlist_type("cwist_log_format_t");
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
