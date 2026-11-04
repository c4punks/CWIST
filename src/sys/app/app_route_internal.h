#ifndef CWIST_APP_ROUTE_INTERNAL_H
#define CWIST_APP_ROUTE_INTERNAL_H

#include <stdbool.h>
#include <cwist/sys/app/app.h>

bool cwist_app_post_checked(cwist_app *app, const char *path, cwist_handler_func handler);

#endif
