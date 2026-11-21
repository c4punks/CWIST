/** @file waf.h @brief Deterministic request firewall and output sanitization. */
#ifndef __CWIST_WAF_H__
#define __CWIST_WAF_H__

#include <stdbool.h>
#include <stddef.h>
#include <cwist/sys/app/app.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Signature-based request screening middleware.
 *
 * Screens the decoded path and query, header names, URL-encoded form bodies,
 * text bodies (text/x, JSON, XML, GraphQL, JavaScript, +json/+xml, or no
 * Content-Type), and multipart part names, file names, and text fields.
 * Opaque binary bodies (file contents, images, octet-stream, protobuf) are
 * passed to the handler unscanned and are bounded by the body size limit
 * instead. A rejected request gets 400.
 */
cwist_middleware_func cwist_mw_waf_lite(void);
bool cwist_waf_is_safe(const char *input, size_t length);
char *cwist_sanitize_html(const char *input);

#ifdef __cplusplus
}
#endif
#endif
