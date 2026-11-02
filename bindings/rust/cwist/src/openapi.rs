//! OpenAPI 3.1 document generation from an app's live route table.

use crate::{App, Error};
use cwist_sys as sys;
use std::ffi::CString;

/// Metadata for a generated OpenAPI document. Strings are copied by CWIST.
#[derive(Debug, Clone, Copy, Default)]
pub struct OpenApiMeta<'a> {
    /// API title, or CWIST's default when absent.
    pub title: Option<&'a str>,
    /// API version, or CWIST's default when absent.
    pub version: Option<&'a str>,
    /// Optional API description.
    pub description: Option<&'a str>,
    /// Optional server URL.
    pub server_url: Option<&'a str>,
}

impl App {
    /// Serves an OpenAPI 3.1 document at `json_path` (default `/openapi.json`).
    /// Calling this more than once on the same app has no additional effect.
    pub fn enable_openapi(
        &mut self,
        json_path: Option<&str>,
        meta: OpenApiMeta<'_>,
    ) -> Result<(), Error> {
        let path = json_path
            .map(|p| CString::new(p).map_err(|_| Error::InteriorNul("OpenAPI path")))
            .transpose()?;
        let title = meta
            .title
            .map(|s| CString::new(s).map_err(|_| Error::InteriorNul("API title")))
            .transpose()?;
        let version = meta
            .version
            .map(|s| CString::new(s).map_err(|_| Error::InteriorNul("API version")))
            .transpose()?;
        let description = meta
            .description
            .map(|s| CString::new(s).map_err(|_| Error::InteriorNul("API description")))
            .transpose()?;
        let server_url = meta
            .server_url
            .map(|s| CString::new(s).map_err(|_| Error::InteriorNul("API server URL")))
            .transpose()?;
        let raw_meta = sys::cwist_openapi_meta_t {
            title: title.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
            version: version.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
            description: description
                .as_ref()
                .map_or(std::ptr::null(), |s| s.as_ptr()),
            server_url: server_url.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
        };
        // SAFETY: the app and all strings are live for the call; CWIST copies
        // the path and metadata it retains.
        unsafe {
            sys::cwist_app_enable_openapi(
                self.raw.as_ptr(),
                path.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
                &raw_meta,
            )
        };
        Ok(())
    }

    /// Adds or replaces the summary and comma-separated tags of one operation.
    pub fn openapi_annotate(
        &mut self,
        method: &str,
        path: &str,
        summary: Option<&str>,
        tags: Option<&str>,
    ) -> Result<(), Error> {
        let method = CString::new(method).map_err(|_| Error::InteriorNul("OpenAPI method"))?;
        let path = CString::new(path).map_err(|_| Error::InteriorNul("OpenAPI path"))?;
        let summary = summary
            .map(|s| CString::new(s).map_err(|_| Error::InteriorNul("OpenAPI summary")))
            .transpose()?;
        let tags = tags
            .map(|s| CString::new(s).map_err(|_| Error::InteriorNul("OpenAPI tags")))
            .transpose()?;
        // SAFETY: the app and strings are live for the call; CWIST copies the
        // annotation values it retains.
        unsafe {
            sys::cwist_openapi_annotate(
                self.raw.as_ptr(),
                method.as_ptr(),
                path.as_ptr(),
                summary.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
                tags.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
            )
        };
        Ok(())
    }
}
