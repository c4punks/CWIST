/** @file query.h
 * @brief Compatibility header: forwards to <cwist/net/http/query.h>.
 *
 * This used to be a second copy of the query map definitions. It shared the
 * __CWIST_QUERY_H__ include guard with the real header but had fallen behind
 * (no arena field), so whichever header a translation unit included first
 * decided the layout of cwist_query_map. It now includes the real header.
 */
#ifndef CWIST_QUERY_COMPAT_H
#define CWIST_QUERY_COMPAT_H

#include <cwist/net/http/query.h>

#endif
