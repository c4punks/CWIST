//! SipHash-2-4 hashing with a random or caller-supplied 128-bit key.

use cwist_sys as sys;

/// The 16-byte secret key SipHash mixes into every hash.
#[derive(Clone)]
pub struct Key([u8; 16]);

impl Key {
    /// Generates a random key with CWIST's CSPRNG. Distinct calls produce
    /// distinct keys with overwhelming probability.
    pub fn generate() -> Key {
        let mut key = [0u8; 16];
        // SAFETY: key is 16 bytes, matching the uint8_t key[16] parameter.
        unsafe { sys::cwist_generate_hash_seed(key.as_mut_ptr()) };
        Key(key)
    }

    /// Wraps an existing 16-byte key.
    pub fn from_bytes(key: [u8; 16]) -> Key {
        Key(key)
    }

    /// The raw key bytes.
    pub fn as_bytes(&self) -> &[u8; 16] {
        &self.0
    }

    /// Computes the 64-bit SipHash-2-4 of `data` with this key.
    pub fn hash(&self, data: &[u8]) -> u64 {
        // SAFETY: data is read only for the call; the key lives for the call.
        unsafe { sys::siphash24(data.as_ptr().cast(), data.len(), self.0.as_ptr()) }
    }
}
