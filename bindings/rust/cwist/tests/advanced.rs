//! The advanced/dangerous FFI surface wrapped safely: growable strings,
//! SQLite, the job scheduler, JWT, SipHash and the bump arena.

use cwist::{
    sign, sign_ed25519, sign_es256, verify, verify_ed25519, verify_es256, Arena, Db, Error, Key,
    Scheduler, SString,
};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

#[test]
fn siphash_matches_the_reference_empty_message_vector() {
    let key = Key::from_bytes([0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
        0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f]);
    assert_eq!(key.hash(b""), 0x726fdb47dd0e0e31);
}

#[test]
fn siphash_is_deterministic_and_keys_differ() {
    let key = Key::generate();
    let data = b"the quick brown fox";
    assert_eq!(key.hash(data), key.hash(data));

    let other = Key::generate();
    assert_ne!(key.as_bytes(), other.as_bytes());
}

#[test]
fn sstring_assign_append_and_growth() {
    let mut s = SString::new().expect("sstring");
    s.assign("hello ").expect("assign");
    s.append("world").expect("append");
    assert_eq!(s.as_str(), Some("hello world"));

    // Append past any small-string capacity in a loop and check every byte.
    let chunk = "0123456789abcdef";
    for _ in 0..512 {
        s.append(chunk).expect("append loop");
    }
    let expected_len = "hello world".len() + 512 * chunk.len();
    assert_eq!(s.as_bytes().len(), expected_len);
    let body = &s.as_bytes()["hello world".len()..];
    assert!(body.chunks_exact(chunk.len()).all(|c| c == chunk.as_bytes()));
}

#[test]
fn sstring_accepts_embedded_nul_bytes() {
    let mut s = SString::new().expect("sstring");
    s.assign("a\0b").expect("assign");
    s.append("c\0").expect("append");
    assert_eq!(s.as_bytes(), b"a\0bc\0");
}

fn temp_db_path(name: &str) -> std::path::PathBuf {
    std::env::temp_dir().join(format!("cwist-rust-test-{name}-{}.sqlite",
        std::process::id()))
}

#[test]
fn db_exec_query_serialize_and_roundtrip() {
    let path = temp_db_path("roundtrip");
    let image = {
        let db = Db::open(path.to_str().expect("utf-8 path")).expect("open");
        db.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)").expect("create");
        db.exec("INSERT INTO items (name) VALUES ('widget')").expect("insert");

        let rows = db.query("SELECT id, name FROM items").expect("query");
        assert!(rows.contains("widget"), "rows: {rows}");

        db.serialize().expect("serialize")
    };

    // The file can be reopened after the first handle closed.
    let reopened = Db::open(path.to_str().expect("utf-8 path")).expect("reopen");
    let rows = reopened.query("SELECT name FROM items WHERE name = 'widget'").expect("query");
    assert!(rows.contains("widget"), "rows: {rows}");
    drop(reopened);

    // serialize -> open_memory roundtrip preserves the data.
    let memory = Db::open_memory(&image, false).expect("open_memory");
    let rows = memory.query("SELECT name FROM items").expect("query");
    assert!(rows.contains("widget"), "rows: {rows}");
    drop(memory);

    std::fs::remove_file(&path).ok();
}

#[test]
fn db_rejects_bad_sql_and_interior_nul() {
    let db = Db::open(":memory:").expect("open");
    assert_eq!(db.exec("NOT SQL"), Err(Error::Database));
    assert!(matches!(db.exec("SELECT 1;\0oops"), Err(Error::InteriorNul(_))));
}

#[test]
fn db_open_memory_readonly_refuses_writes() {
    let path = temp_db_path("readonly-src");
    let image = {
        let db = Db::open(path.to_str().expect("utf-8 path")).expect("open");
        db.exec("CREATE TABLE t (v INTEGER)").expect("create");
        db.serialize().expect("serialize")
    };
    std::fs::remove_file(&path).ok();

    let ro = Db::open_memory(&image, true).expect("open_memory readonly");
    assert_eq!(ro.exec("INSERT INTO t VALUES (1)"), Err(Error::Database));
}

#[test]
fn scheduler_spawns_jobs_and_counts_pending() {
    let scheduler = Scheduler::new(2, 16).expect("scheduler");
    let counter = Arc::new(AtomicUsize::new(0));

    for _ in 0..8 {
        let counter = Arc::clone(&counter);
        assert!(scheduler.spawn(move || {
            counter.fetch_add(1, Ordering::SeqCst);
        }));
    }

    let deadline = Instant::now() + Duration::from_secs(10);
    while counter.load(Ordering::SeqCst) < 8 && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(5));
    }
    assert_eq!(counter.load(Ordering::SeqCst), 8);
}

#[test]
fn scheduler_delayed_job_fires_only_after_the_delay() {
    let scheduler = Scheduler::new(1, 8).expect("scheduler");
    let fired = Arc::new(AtomicUsize::new(0));
    let fired_clone = Arc::clone(&fired);

    let delay = Duration::from_millis(300);
    assert!(scheduler.schedule(delay, move || {
        fired_clone.fetch_add(1, Ordering::SeqCst);
    }));
    assert_eq!(scheduler.pending_count(), 1);

    // Not fired immediately after submission.
    std::thread::sleep(Duration::from_millis(80));
    assert_eq!(fired.load(Ordering::SeqCst), 0, "job fired before its delay");

    let deadline = Instant::now() + Duration::from_secs(10);
    while fired.load(Ordering::SeqCst) < 1 && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(5));
    }
    assert_eq!(fired.load(Ordering::SeqCst), 1);

    // The delayed job left the pending queue once it fired (eventually).
    let deadline = Instant::now() + Duration::from_secs(5);
    while scheduler.pending_count() != 0 && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(5));
    }
    assert_eq!(scheduler.pending_count(), 0);
}

#[test]
fn scheduler_drop_with_pending_jobs_does_not_crash() {
    let scheduler = Scheduler::new(1, 8).expect("scheduler");
    for _ in 0..4 {
        // Never fires: CWIST frees each pending node without running it, so
        // every closure here leaks by design (memory leak, no crash/UB).
        assert!(scheduler.schedule(Duration::from_secs(3600), || {}));
    }
    assert_eq!(scheduler.pending_count(), 4);
    drop(scheduler);
}

#[test]
fn jwt_sign_verify_roundtrip_and_tamper() {
    let token = sign(r#"{"sub":"user-1","role":"admin"}"#, "s3cret", 3600).expect("sign");
    assert_eq!(token.matches('.').count(), 2);

    let claims = verify(&token, "s3cret").expect("verify");
    assert_eq!(claims.get("sub"), Some("user-1"));
    assert_eq!(claims.get("role"), Some("admin"));
    assert_eq!(claims.get("missing"), None);
    drop(claims);

    // A different secret must not verify.
    assert!(matches!(verify(&token, "wrong"), Err(Error::InvalidToken)));

    // Tampering with the payload breaks the signature.
    let mut tampered = token.clone();
    let dot = tampered.find('.').expect("payload separator");
    tampered.replace_range(dot + 1..dot + 2, "X");
    assert!(matches!(verify(&tampered, "s3cret"), Err(Error::InvalidToken)));

    // Garbage is rejected too.
    assert!(matches!(verify("not-a-token", "s3cret"), Err(Error::InvalidToken)));
}

#[test]
fn arena_zeroed_memory_owns_and_generations() {
    let mut arena = Arena::new(256).expect("arena");

    // Within a generation: zeroed slice, owns() reports it.
    let buf = arena.alloc_bytes(32).expect("alloc");
    assert!(buf.iter().all(|&b| b == 0));
    buf[0] = 0xab;
    let ptr = buf.as_ptr();
    assert!(arena.owns(ptr));
    {
        let _second = arena.alloc_bytes(32).expect("alloc second");
        // Both chunks live in (possibly spilled) generations owned by arena.
    }

    // An unrelated pointer is not owned.
    let foreign = 0x1usize as *const u8;
    assert!(!arena.owns(foreign));

    // An allocation larger than the generation is refused (the C contract is
    // NULL on exhaustion, with heap fallback left to the caller).
    assert!(arena.alloc_bytes(4096).is_none());

    // Typed write/read through alloc_uninit.
    let ints: std::ptr::NonNull<u64> = unsafe { arena.alloc_uninit(4) }.expect("alloc_uninit");
    unsafe {
        for i in 0..4 {
            std::ptr::write(ints.as_ptr().add(i), i as u64 * 7);
        }
        for i in 0..4 {
            assert_eq!(std::ptr::read(ints.as_ptr().add(i)), i as u64 * 7);
        }
    }

    // A roomier arena serves larger allocations, still zeroed.
    let mut roomy = Arena::new(8192).expect("arena 8192");
    let big = roomy.alloc_bytes(4096).expect("alloc big");
    assert!(big.iter().all(|&b| b == 0));
    big[4095] = 1;
}

// P-256 and Ed25519 test keys (throwaway, generated for these tests).
const EC256_PRIV: &str = "-----BEGIN EC PRIVATE KEY-----\n\
MHcCAQEEIDC7BCqt+gWMBQEEX7581vxye5WCTZNUlp2OdYdHwbYSoAoGCCqGSM49\n\
AwEHoUQDQgAEQlZeTKbWplC+kWKJzbfp82Ig7o1N0Pdin5CkqfUEO+prdr/XnLjp\n\
vFFp2mMpAudMNv4Uw8BmpCLrUSeUF1jZKQ==\n\
-----END EC PRIVATE KEY-----\n";
const EC256_PUB: &str = "-----BEGIN PUBLIC KEY-----\n\
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEQlZeTKbWplC+kWKJzbfp82Ig7o1N\n\
0Pdin5CkqfUEO+prdr/XnLjpvFFp2mMpAudMNv4Uw8BmpCLrUSeUF1jZKQ==\n\
-----END PUBLIC KEY-----\n";
const ED25519_PRIV: &str = "-----BEGIN PRIVATE KEY-----\n\
MC4CAQAwBQYDK2VwBCIEIOg0tgwevnUolReLx+S+Kq+tiity9wj7HtCnqnU45TLD\n\
-----END PRIVATE KEY-----\n";
const ED25519_PUB: &str = "-----BEGIN PUBLIC KEY-----\n\
MCowBQYDK2VwAyEAm1eSBuXZV0PVcazp/ZYocHlQfQJVEDOmhcRNj3X1sdI=\n\
-----END PUBLIC KEY-----\n";

#[test]
fn jwt_es256_roundtrip_kid_and_confusion() {
    let token = sign_es256(r#"{"sub":"user-1"}"#, EC256_PRIV, Some("key-1"), 3600).expect("sign");
    assert!(token.starts_with("eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCIsImtpZCI6ImtleS0xIn0"));

    let claims = verify_es256(&token, EC256_PUB).expect("verify");
    assert_eq!(claims.get("sub"), Some("user-1"));
    drop(claims);

    // The private key verifies too (its public half is used).
    assert!(verify_es256(&token, EC256_PRIV).is_ok());

    // Wrong key, wrong algorithm and HMAC verification all reject it.
    assert!(matches!(verify_es256(&token, ED25519_PUB), Err(Error::InvalidToken)));
    assert!(matches!(verify_ed25519(&token, ED25519_PUB), Err(Error::InvalidToken)));
    assert!(matches!(verify(&token, "secret"), Err(Error::InvalidToken)));

    // HS256 tokens are rejected by the asymmetric verifiers (alg confusion).
    let hs = sign(r#"{"sub":"1"}"#, "secret", 3600).expect("hs256 sign");
    assert!(matches!(verify_es256(&hs, EC256_PUB), Err(Error::InvalidToken)));
    assert!(matches!(verify_ed25519(&hs, ED25519_PUB), Err(Error::InvalidToken)));
}

#[test]
fn jwt_ed25519_roundtrip_and_key_type_mismatch() {
    let token = sign_ed25519(r#"{"sub":"alice"}"#, ED25519_PRIV, None, 3600).expect("sign");
    assert!(token.starts_with("eyJhbGciOiJFZERTQSIsInR5cCI6IkpXVCJ9"));

    let claims = verify_ed25519(&token, ED25519_PUB).expect("verify");
    assert_eq!(claims.get("sub"), Some("alice"));
    drop(claims);

    // Keys of the wrong type cannot sign for the other algorithm.
    assert!(sign_es256(r#"{"sub":"1"}"#, ED25519_PRIV, None, 0).is_err());
    assert!(sign_ed25519(r#"{"sub":"1"}"#, EC256_PRIV, None, 0).is_err());
    assert!(sign_es256(r#"{"sub":"1"}"#, "not a pem", None, 0).is_err());
}
