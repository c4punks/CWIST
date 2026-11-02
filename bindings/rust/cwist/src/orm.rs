//! Typed SQLite ORM results, including exact 64-bit integers.

use crate::http::{c_str, consume};
use crate::Error;
use cwist_sys as sys;
use std::ffi::{c_int, CString};
use std::ptr::NonNull;
use std::slice;

/// One typed value in an ORM result set.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum OrmValue<'a> {
    /// SQL NULL.
    Null,
    /// An exact SQLite INTEGER.
    Integer(i64),
    /// A SQLite REAL.
    Real(f64),
    /// UTF-8 TEXT borrowed from the result set.
    Text(&'a str),
    /// BLOB bytes borrowed from the result set.
    Blob(&'a [u8]),
}

/// A socket-backed ORM session. The socket and session are closed on drop.
pub struct Orm {
    raw: NonNull<sys::cwist_orm_t>,
}

impl Orm {
    /// Opens a SQLite database through CWIST's ORM socket transport.
    pub fn open(path: &str) -> Result<Orm, Error> {
        let path = CString::new(path).map_err(|_| Error::InteriorNul("database path"))?;
        // SAFETY: path is live for the call. The returned fd is owned below.
        let fd = unsafe { sys::cwist_db_transfer_sqlite_to_socket(path.as_ptr()) };
        if fd < 0 {
            return Err(Error::Database);
        }
        // SAFETY: fd is a valid connected socket owned by this call.
        let raw = unsafe { sys::cwist_orm_open_socket(fd) };
        let Some(raw) = NonNull::new(raw) else {
            // SAFETY: close(2) accepts this still-owned valid descriptor.
            unsafe { close(fd) };
            return Err(Error::Database);
        };
        Ok(Orm { raw })
    }

    /// Executes a non-query SQL statement verbatim.
    ///
    /// Validate statements built from user input before calling this method.
    pub fn exec(&self, sql: &str) -> Result<(), Error> {
        let sql = CString::new(sql).map_err(|_| Error::InteriorNul("SQL statement"))?;
        // SAFETY: the session and SQL string are live for the call.
        if consume(unsafe { sys::cwist_orm_exec(self.raw.as_ptr(), sql.as_ptr()) }) {
            Ok(())
        } else {
            Err(Error::Database)
        }
    }

    /// Executes a SELECT-style query and returns storage-class-typed cells.
    /// SQL is passed verbatim and must not be built from untrusted input.
    pub fn query_typed(&self, sql: &str) -> Result<OrmResult, Error> {
        let sql = CString::new(sql).map_err(|_| Error::InteriorNul("SQL statement"))?;
        let mut raw = std::ptr::null_mut();
        // SAFETY: the session and SQL string are live for the call; result is
        // released by OrmResult::drop on success.
        let err = unsafe { sys::cwist_orm_query_typed(self.raw.as_ptr(), sql.as_ptr(), &mut raw) };
        if !consume(err) {
            return Err(Error::Database);
        }
        NonNull::new(raw)
            .map(|raw| OrmResult { raw })
            .ok_or(Error::Database)
    }
}

impl Drop for Orm {
    fn drop(&mut self) {
        // SAFETY: the session is live and owned by this object.
        unsafe { sys::cwist_orm_close_socket(self.raw.as_ptr()) };
    }
}

/// An owned typed ORM result set. Cells and column names borrow it.
pub struct OrmResult {
    raw: NonNull<sys::cwist_orm_result_t>,
}

impl OrmResult {
    fn raw(&self) -> &sys::cwist_orm_result_t {
        // SAFETY: the result is live and owned by this object.
        unsafe { self.raw.as_ref() }
    }

    /// Number of columns in each row.
    pub fn column_count(&self) -> usize {
        self.raw().column_count
    }

    /// Number of rows in the result.
    pub fn row_count(&self) -> usize {
        self.raw().row_count
    }

    /// A column name by zero-based index.
    pub fn column_name(&self, index: usize) -> Option<&str> {
        if index >= self.column_count() {
            return None;
        }
        // SAFETY: index is in bounds and the names live as long as the result.
        unsafe { c_str(*self.raw().columns.add(index)) }
    }

    /// A column index by name, or `column_count()` when absent.
    pub fn column_index(&self, name: &str) -> usize {
        let Ok(name) = CString::new(name) else {
            return self.column_count();
        };
        // SAFETY: the result and name are live for the call.
        unsafe { sys::cwist_orm_result_column_index(self.raw.as_ptr(), name.as_ptr()) }
    }

    /// Reads one cell. Text and blob values borrow this result set.
    pub fn get(&self, row: usize, column: usize) -> Option<OrmValue<'_>> {
        // SAFETY: the result is live; CWIST checks the coordinates.
        let value = unsafe { sys::cwist_orm_result_cell(self.raw.as_ptr(), row, column) };
        let value = unsafe { value.as_ref() }?;
        match value.type_ {
            sys::CWIST_ORM_TYPE_NULL => Some(OrmValue::Null),
            sys::CWIST_ORM_TYPE_INTEGER => Some(OrmValue::Integer(value.i64_)),
            sys::CWIST_ORM_TYPE_REAL => Some(OrmValue::Real(value.real)),
            sys::CWIST_ORM_TYPE_TEXT => {
                // SAFETY: TEXT is NUL-terminated and owned by the result.
                let text = unsafe { c_str(value.text) }?;
                Some(OrmValue::Text(text))
            }
            sys::CWIST_ORM_TYPE_BLOB => {
                if value.blob_len == 0 {
                    return Some(OrmValue::Blob(&[]));
                }
                // SAFETY: non-empty BLOBs have a valid pointer owned by result.
                let bytes = unsafe { slice::from_raw_parts(value.blob, value.blob_len) };
                Some(OrmValue::Blob(bytes))
            }
            _ => None,
        }
    }
}

impl Drop for OrmResult {
    fn drop(&mut self) {
        // SAFETY: the result is live and owned by this object.
        unsafe { sys::cwist_orm_result_free(self.raw.as_ptr()) };
    }
}

unsafe extern "C" {
    fn close(fd: c_int) -> c_int;
}
