//! JWT signing and verification wrapping CWIST's HMAC-SHA256 implementation.

use crate::http::c_str;
use crate::support::CBuf;
use crate::Error;
use cwist_sys as sys;
use std::ffi::CString;
use std::ptr::NonNull;

/// Signs a JWT token with HMAC-SHA256, returning the compact
/// `header.payload.signature` form.
///
/// `claims_json` is the JSON object for the payload, e.g. `{"sub":"1"}`.
/// Standard claims `exp` and `iat` are added automatically with an expiry
/// `exp_seconds` from now; pass `exp_seconds = 0` to keep full control and
/// supply `exp` yourself. Both strings must not contain an interior NUL.
pub fn sign(claims_json: &str, secret: &str, exp_seconds: i64) -> Result<String, Error> {
    let claims_json = CString::new(claims_json).map_err(|_| Error::InteriorNul("claims JSON"))?;
    let secret = CString::new(secret).map_err(|_| Error::InteriorNul("JWT secret"))?;
    // SAFETY: both strings outlive the call. c_long is 64 bits on every
    // platform CWIST supports (LP64 Linux and macOS), so exp_seconds passes
    // directly.
    let token = unsafe { sys::cwist_jwt_sign(claims_json.as_ptr(), secret.as_ptr(), exp_seconds) };
    // cwist_jwt_sign's contract is cwist_free; the guard releases the token
    // on every path out of this function.
    let token = CBuf::new(token.cast(), sys::cwist_free).ok_or(Error::JwtSign)?;
    // SAFETY: token is a NUL-terminated string owned by the guard, live
    // until end of scope.
    Ok(unsafe { c_str(token.as_ptr().cast()) }
        .ok_or(Error::JwtSign)?
        .to_owned())
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
/// token is malformed, signed with a different secret, or expired.
pub fn verify(token: &str, secret: &str) -> Result<Claims, Error> {
    let token = CString::new(token).map_err(|_| Error::InteriorNul("JWT token"))?;
    let secret = CString::new(secret).map_err(|_| Error::InteriorNul("JWT secret"))?;
    // SAFETY: both strings outlive the call; NULL means verification failed.
    let raw = unsafe { sys::cwist_jwt_verify(token.as_ptr(), secret.as_ptr()) };
    NonNull::new(raw).map(|raw| Claims { raw }).ok_or(Error::InvalidToken)
}
