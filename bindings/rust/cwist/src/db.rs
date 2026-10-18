//! Owned wrapper for CWIST's SQLite database handle (`cwist_db`).

use crate::http::{consume, c_str};
use crate::support::CBuf;
use crate::Error;
use cwist_sys as sys;
use std::ffi::{c_void, CString};
use std::ptr::{self, NonNull};
use std::slice;

/// A SQLite database opened through CWIST.
///
/// SQL statements are passed to SQLite verbatim. There is no parameter
/// binding in this API: if any part of a statement comes from a request,
/// build and validate it in Rust first, or you have a SQL injection hole.
///
/// `Db` is neither `Send` nor `Sync`: a single SQLite connection is used from
/// the thread that owns it.
pub struct Db {
    raw: NonNull<sys::cwist_db>,
}

impl Db {
    /// Opens the SQLite file at `path` (`":memory:"` for a private
    /// in-memory database). Returns an error if the path contains an
    /// interior NUL byte or the file cannot be opened.
    pub fn open(path: &str) -> Result<Db, Error> {
        let path = CString::new(path).map_err(|_| Error::InteriorNul("database path"))?;
        let mut raw: *mut sys::cwist_db = ptr::null_mut();
        // SAFETY: raw receives a new handle; the path outlives the call.
        let err = unsafe { sys::cwist_db_open(&mut raw, path.as_ptr()) };
        Self::from_open_result(consume(err), raw)
    }

    /// Opens a database from a serialized SQLite image, e.g. one produced by
    /// [`Db::serialize`]. The image is copied, so `buf` only needs to live
    /// for the call. With `readonly = true` writes fail with
    /// `SQLITE_READONLY`; with `false` the in-memory database may grow.
    pub fn open_memory(buf: &[u8], readonly: bool) -> Result<Db, Error> {
        let mut raw: *mut sys::cwist_db = ptr::null_mut();
        // SAFETY: raw receives a new handle; buf is read only for the call.
        let err = unsafe {
            sys::cwist_db_open_memory(
                &mut raw,
                buf.as_ptr().cast::<c_void>(),
                buf.len(),
                readonly.into(),
            )
        };
        Self::from_open_result(consume(err), raw)
    }

    /// Closes the handle again when CWIST reported success but left a
    /// partial result, then reports the error.
    fn from_open_result(ok: bool, raw: *mut sys::cwist_db) -> Result<Db, Error> {
        match (ok, NonNull::new(raw)) {
            (true, Some(raw)) => Ok(Db { raw }),
            (true, None) | (false, None) => Err(Error::Database),
            (false, Some(raw)) => {
                // SAFETY: raw is a handle cwist_db_open* produced even though
                // it reported failure; close it so it does not leak.
                unsafe { sys::cwist_db_close(raw.as_ptr()) };
                Err(Error::Database)
            }
        }
    }

    /// Executes a statement that returns no rows (`INSERT`, `UPDATE`,
    /// `DELETE`, `CREATE`, ...). The SQL is passed verbatim.
    pub fn exec(&self, sql: &str) -> Result<(), Error> {
        let sql = CString::new(sql).map_err(|_| Error::InteriorNul("SQL statement"))?;
        // SAFETY: the db is live; the SQL outlives the call.
        let err = unsafe { sys::cwist_db_exec(self.raw.as_ptr(), sql.as_ptr()) };
        if consume(err) {
            Ok(())
        } else {
            Err(Error::Database)
        }
    }

    /// Runs a query and returns its rows as a JSON array of objects, e.g.
    /// `[{"id":1,"name":"foo"}]`. The SQL is passed verbatim.
    pub fn query(&self, sql: &str) -> Result<String, Error> {
        let sql = CString::new(sql).map_err(|_| Error::InteriorNul("SQL statement"))?;
        let mut result: *mut sys::cJSON = ptr::null_mut();
        // SAFETY: the db is live; result starts NULL and receives a new tree
        // owned by this function.
        let err = unsafe { sys::cwist_db_query(self.raw.as_ptr(), sql.as_ptr(), &mut result) };
        // The guard deletes the tree on every path out of this function.
        let tree = JsonTree::new(result);
        if !consume(err) {
            return Err(Error::Database);
        }
        let tree = tree.ok_or(Error::Database)?;
        // SAFETY: tree is the live cJSON tree returned above.
        let printed = unsafe { sys::cJSON_PrintUnformatted(tree.as_ptr()) };
        // CWIST installs cJSON hooks (a constructor in core/mem/alloc.c) that
        // route cJSON's allocator through cwist_malloc/cwist_free, so the
        // printed buffer goes to the hook-aware cJSON_free, not libc free.
        let printed = CBuf::new(printed.cast(), sys::cJSON_free).ok_or(Error::Database)?;
        // SAFETY: printed is a NUL-terminated string owned by the cJSON
        // print; it stays live until the guard drops at end of scope.
        Ok(unsafe { c_str(printed.as_ptr().cast()) }
            .ok_or(Error::Database)?
            .to_owned())
    }

    /// Serializes the database image into a buffer suitable for persisting or
    /// handing to [`Db::open_memory`].
    pub fn serialize(&self) -> Result<Vec<u8>, Error> {
        let mut out: *mut c_void = ptr::null_mut();
        let mut out_len = 0;
        // SAFETY: the db is live; out/out_len receive a fresh buffer.
        let err = unsafe { sys::cwist_db_serialize(self.raw.as_ptr(), &mut out, &mut out_len) };
        // The guard frees `out` with cwist_free (its documented deallocator)
        // on every path, including the error ones.
        let buf = CBuf::new(out, sys::cwist_free);
        if !consume(err) {
            return Err(Error::Database);
        }
        let buf = buf.ok_or(Error::Database)?;
        // SAFETY: CWIST returned out_len bytes at buf; the copy happens while
        // the guard keeps the buffer live.
        Ok(unsafe { slice::from_raw_parts(buf.as_ptr().cast::<u8>(), out_len) }.to_vec())
    }
}

/// A cJSON tree returned by a query, deleted on drop.
struct JsonTree(NonNull<sys::cJSON>);

impl JsonTree {
    fn new(ptr: *mut sys::cJSON) -> Option<JsonTree> {
        NonNull::new(ptr).map(JsonTree)
    }

    fn as_ptr(&self) -> *const sys::cJSON {
        self.0.as_ptr()
    }
}

impl Drop for JsonTree {
    fn drop(&mut self) {
        // SAFETY: the tree is live and owned by this guard.
        unsafe { sys::cJSON_Delete(self.0.as_ptr()) };
    }
}

impl Drop for Db {
    fn drop(&mut self) {
        // SAFETY: the db is live and owned by this struct.
        unsafe { sys::cwist_db_close(self.raw.as_ptr()) };
    }
}
