/* Unit-only seam: the cache never dereferences the DB pointer. All HTTP,
 * header, string, request and response layouts remain the real public types. */
#define __CWIST_SQL_H__
typedef struct cwist_db cwist_db;
