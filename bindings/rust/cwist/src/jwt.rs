//! JWT signing and verification: HMAC-SHA256, ES256 (ECDSA P-256) and
//! Ed25519, wrapping CWIST's implementation.

use crate::http::c_str;
use crate::support::CBuf;
use crate::Error;
use cwist_sys as sys;
use std::ffi::{c_char, c_long, CString};
use std::ptr::{self, NonNull};

fn nul_free(s: &str, what: &'static str) -> Result<CString, Error> {
    CString::new(s).map_err(|_| Error::InteriorNul(what))
}

/// Copies the signed token into an owned String; the C buffer is released
/// with cwist_free by the guard on every path.
fn take_token(raw: *mut c_char) -> Result<String, Error> {
    let token = CBuf::new(raw.cast(), sys::cwist_free).ok_or(Error::JwtSign)?;
    // SAFETY: token is a NUL-terminated string owned by the guard, live
    // until end of scope.
    Ok(unsafe { c_str(token.as_ptr().cast()) }
        .ok_or(Error::JwtSign)?
        .to_owned())
}

type VerifyFn =
    unsafe extern "C" fn(*const c_char, *const c_char) -> *mut sys::cwist_jwt_claims;

fn verify_with(verify_fn: VerifyFn, token: &str, key: &str) -> Result<Claims, Error> {
    let token = nul_free(token, "JWT token")?;
    let key = nul_free(key, "JWT key")?;
    // SAFETY: both strings outlive the call; NULL means verification failed.
    let raw = unsafe { verify_fn(token.as_ptr(), key.as_ptr()) };
    NonNull::new(raw).map(|raw| Claims { raw }).ok_or(Error::InvalidToken)
}

/// Signs a JWT token with HMAC-SHA256, returning the compact
/// `header.payload.signature` form.
///
/// `claims_json` is the JSON object for the payload, e.g. `{"sub":"1"}`.
/// Standard claims `exp` and `iat` are added automatically with an expiry
/// `exp_seconds` from now; pass `exp_seconds = 0` to keep full control and
/// supply `exp` yourself. Both strings must not contain an interior NUL.
pub fn sign(claims_json: &str, secret: &str, exp_seconds: i64) -> Result<String, Error> {
    let claims_json = nul_free(claims_json, "claims JSON")?;
    let secret = nul_free(secret, "JWT secret")?;
    // SAFETY: both strings outlive the call. c_long is 64 bits on every
    // platform CWIST supports (LP64 Linux and macOS), so exp_seconds passes
    // directly.
    let token = unsafe { sys::cwist_jwt_sign(claims_json.as_ptr(), secret.as_ptr(), exp_seconds) };
    take_token(token)
}

/// Claims decoded from a verified JWT token, destroyed on drop.
///
/// Values returned by [`Claims::get`] borrow this object.
pub struct Claims {
    raw: NonNull<sys::cwist_jwt_claims>,
}

impl Claims {
    /// The value of one claim, if present and valid UTF-8.
    pub fn get(&self, key: &str) -> Option<&str> {
        let key = CString::new(key).ok()?;
        // SAFETY: the returned pointer is valid until the claims are
        // destroyed, which cannot happen while &self is borrowed.
        unsafe { c_str(sys::cwist_jwt_claims_get(self.raw.as_ptr(), key.as_ptr())) }
    }
}

impl Drop for Claims {
    fn drop(&mut self) {
        // SAFETY: the claims are live and owned by this struct.
        unsafe { sys::cwist_jwt_claims_destroy(self.raw.as_ptr()) };
    }
}

/// Verifies a JWT token's structure, HMAC-SHA256 signature and `exp` claim,
/// returning its decoded claims. Returns [`Error::InvalidToken`] when the
/// token is malformed, signed with a different secret or algorithm, or
/// expired.
pub fn verify(token: &str, secret: &str) -> Result<Claims, Error> {
    verify_with(sys::cwist_jwt_verify, token, secret)
}

type SignAsymFn = unsafe extern "C" fn(
    *const c_char,
    *const c_char,
    *const c_char,
    c_long,
) -> *mut c_char;

fn sign_asym(
    sign_fn: SignAsymFn,
    claims_json: &str,
    pem_private_key: &str,
    kid: Option<&str>,
    exp_seconds: i64,
) -> Result<String, Error> {
    let claims_json = nul_free(claims_json, "claims JSON")?;
    let key = nul_free(pem_private_key, "PEM private key")?;
    let kid = kid.map(|k| nul_free(k, "key id")).transpose()?;
    let kid_ptr = kid.as_ref().map(|k| k.as_ptr()).unwrap_or(ptr::null());
    // SAFETY: all strings outlive the call; exp_seconds fits c_long on LP64.
    let token = unsafe {
        sign_fn(claims_json.as_ptr(), key.as_ptr(), kid_ptr, exp_seconds)
    };
    take_token(token)
}

/// Signs a JWT token with ES256 (ECDSA over P-256 with SHA-256), returning
/// the compact `header.payload.signature` form.
///
/// `pem_private_key` is a PEM-encoded EC private key on P-256. `kid`, when
/// given, is written into the JWT header. See [`sign`] for the `exp_seconds`
/// contract.
pub fn sign_es256(
    claims_json: &str,
    pem_private_key: &str,
    kid: Option<&str>,
    exp_seconds: i64,
) -> Result<String, Error> {
    sign_asym(sys::cwist_jwt_sign_es256, claims_json, pem_private_key, kid, exp_seconds)
}

/// Verifies an ES256-signed JWT token against a PEM-encoded EC P-256 public
/// (or private) key. The token header must declare exactly `alg: ES256`, so
/// a token signed with any other algorithm — including HS256 — is rejected
/// with [`Error::InvalidToken`].
pub fn verify_es256(token: &str, pem_key: &str) -> Result<Claims, Error> {
    verify_with(sys::cwist_jwt_verify_es256, token, pem_key)
}

/// Signs a JWT token with EdDSA (Ed25519). See [`sign_es256`] for the
/// parameter contract.
pub fn sign_ed25519(
    claims_json: &str,
    pem_private_key: &str,
    kid: Option<&str>,
    exp_seconds: i64,
) -> Result<String, Error> {
    sign_asym(sys::cwist_jwt_sign_ed25519, claims_json, pem_private_key, kid, exp_seconds)
}

/// Verifies an Ed25519-signed JWT token against a PEM-encoded Ed25519 public
/// (or private) key. The token header must declare exactly `alg: EdDSA`.
pub fn verify_ed25519(token: &str, pem_key: &str) -> Result<Claims, Error> {
    verify_with(sys::cwist_jwt_verify_ed25519, token, pem_key)
}
