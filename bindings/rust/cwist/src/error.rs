use std::fmt;

/// Errors reported by the safe API.
#[derive(Debug, Clone, PartialEq, Eq)]
#[non_exhaustive]
pub enum Error {
    /// `cwist_app_create()` failed (out of memory).
    AppCreate,
    /// CWIST rejected a route registration. The handler has already been
    /// released.
    Route {
        /// The path that could not be registered.
        path: String,
    },
    /// CWIST rejected a middleware registration. The middleware has already
    /// been released.
    Middleware,
    /// A string passed to CWIST contains a NUL byte, which C cannot represent.
    InteriorNul(&'static str),
    /// CWIST rejected a response header, for example one containing CR or LF.
    Header,
    /// CWIST could not store a response body (out of memory).
    Body,
    /// In-memory dispatch failed: the request was malformed or the response
    /// could not be serialized.
    Dispatch,
    /// The server could not start, for example because the port is in use.
    Listen {
        /// The port that could not be served.
        port: u16,
    },
    /// Another [`App`](crate::App) is already listening in this process;
    /// CWIST runs one server per process.
    AlreadyListening,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::AppCreate => f.write_str("could not create the CWIST app"),
            Error::Route { path } => write!(f, "could not register route {path:?}"),
            Error::Middleware => f.write_str("could not register the middleware"),
            Error::InteriorNul(what) => write!(f, "{what} contains a NUL byte"),
            Error::Header => f.write_str("CWIST rejected the response header"),
            Error::Body => f.write_str("could not store the response body"),
            Error::Dispatch => f.write_str("the request could not be dispatched"),
            Error::Listen { port } => write!(f, "could not serve on port {port}"),
            Error::AlreadyListening => f.write_str("a CWIST server is already running"),
        }
    }
}

impl std::error::Error for Error {}
