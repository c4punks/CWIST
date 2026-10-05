/**
 * @file healthz.c
 * @brief Health probe registry and JSON endpoint generation.
 */

#include <cwist/sys/health/healthz.h>
#include <cwist/core/utils/json_builder.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/core/mem/alloc.h>
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Internal probe registry
 * ---------------------------------------------------------------------- */

#define CWIST_HEALTHZ_MAX_PROBES 16

typedef struct cwist_healthz_entry {
    const char *name;
    cwist_health_probe_fn fn;
    void *ctx;
    bool active;
} cwist_healthz_entry_t;

static cwist_healthz_entry_t g_entries[CWIST_HEALTHZ_MAX_PROBES];
static int g_entry_count = 0;

/* -------------------------------------------------------------------------
 * Registration
 * ---------------------------------------------------------------------- */

/**
 * @brief Register or update a health probe.
 *
 * If a probe with the same name is already active, its function and context
 * are replaced in place. Otherwise the probe is appended to the registry.
 *
 * @param name Probe name; must be non-NULL and outlive the registration.
 * @param fn Probe function; must be non-NULL.
 * @param ctx Opaque context passed to @p fn on each evaluation.
 * @return true on success, false if @p name or @p fn is NULL or the registry
 *         is full.
 */
bool cwist_healthz_register(const char *name, cwist_health_probe_fn fn, void *ctx) {
    if (!name || !fn) return false;
    if (g_entry_count >= CWIST_HEALTHZ_MAX_PROBES) return false;

    for (int i = 0; i < g_entry_count; ++i) {
        if (g_entries[i].active && strcmp(g_entries[i].name, name) == 0) {
            g_entries[i].fn = fn;
            g_entries[i].ctx = ctx;
            return true;
        }
    }

    g_entries[g_entry_count].name   = name;
    g_entries[g_entry_count].fn     = fn;
    g_entries[g_entry_count].ctx    = ctx;
    g_entries[g_entry_count].active = true;
    g_entry_count++;
    return true;
}

/**
 * @brief Deactivate a registered health probe.
 *
 * Marks every active entry with the given name as inactive; inactive entries
 * are skipped by @ref cwist_healthz_run and their slots may be reused by
 * @ref cwist_healthz_register. The name and function pointers are not cleared.
 *
 * @param name Probe name to remove; NULL is a no-op.
 */
void cwist_healthz_unregister(const char *name) {
    if (!name) return;
    for (int i = 0; i < g_entry_count; ++i) {
        if (g_entries[i].active && strcmp(g_entries[i].name, name) == 0) {
            g_entries[i].active = false;
        }
    }
}

/* -------------------------------------------------------------------------
 * Evaluation
 * ---------------------------------------------------------------------- */

/**
 * @brief Map a health status enum to its JSON string representation.
 *
 * @param s Health status value.
 * @return "ok", "degraded", "fail", or "unknown" for unrecognized values.
 */
static const char *status_str(cwist_health_status_t s) {
    switch (s) {
        case CWIST_HEALTH_OK:       return "ok";
        case CWIST_HEALTH_DEGRADED: return "degraded";
        case CWIST_HEALTH_FAIL:     return "fail";
        default:                    return "unknown";
    }
}

/**
 * @brief Run all active probes and aggregate their results.
 *
 * Probes are evaluated in registration order. The overall status is FAIL if
 * any probe reports FAIL, DEGRADED if at least one reports DEGRADED and none
 * report FAIL, and OK otherwise.
 *
 * @param out_probes Optional array receiving per-probe results; at most
 *        @p max_probes entries are written.
 * @param max_probes Capacity of @p out_probes.
 * @param out_count Optional output receiving the number of active probes
 *        evaluated.
 * @param out_overall Optional output receiving the aggregated status.
 */
void cwist_healthz_run(cwist_health_probe_t *out_probes, size_t max_probes, size_t *out_count,
                       cwist_health_status_t *out_overall) {
    size_t count = 0;
    cwist_health_status_t overall = CWIST_HEALTH_OK;

    for (int i = 0; i < g_entry_count && count < max_probes; ++i) {
        if (!g_entries[i].active) continue;
        cwist_health_probe_t r = g_entries[i].fn(g_entries[i].ctx);
        out_probes[count++] = r;
        if (r.status == CWIST_HEALTH_FAIL) overall = CWIST_HEALTH_FAIL;
        else if (r.status == CWIST_HEALTH_DEGRADED && overall == CWIST_HEALTH_OK)
            overall = CWIST_HEALTH_DEGRADED;
    }

    if (out_count) *out_count = count;
    if (out_overall) *out_overall = overall;
}

/* -------------------------------------------------------------------------
 * HTTP response helper
 * ---------------------------------------------------------------------- */

/**
 * @brief Serve the /healthz HTTP endpoint as a JSON response.
 *
 * Runs all registered probes and writes a JSON body with an overall "status"
 * field and a "probes" array of per-probe name, status, and optional message.
 * The response Content-Type is set to application/json.
 *
 * @param res Response to fill; NULL is a no-op. The body and headers are
 *        (re)assigned on success.
 */
void cwist_app_healthz(cwist_http_response *res) {
    if (!res) return;

    cwist_health_probe_t probes[CWIST_HEALTHZ_MAX_PROBES];
    size_t count = 0;
    cwist_health_status_t overall = CWIST_HEALTH_OK;
    cwist_healthz_run(probes, CWIST_HEALTHZ_MAX_PROBES, &count, &overall);

    cwist_json_builder *jb = cwist_json_builder_create();
    cwist_json_begin_object(jb);
    cwist_json_add_string(jb, "status", status_str(overall));
    cwist_json_begin_array(jb, "probes");
    for (size_t i = 0; i < count; ++i) {
        cwist_json_begin_object(jb);
        cwist_json_add_string(jb, "name", probes[i].name);
        cwist_json_add_string(jb, "status", status_str(probes[i].status));
        if (probes[i].message) {
            cwist_json_add_string(jb, "message", probes[i].message);
        }
        cwist_json_end_object(jb);
    }
    cwist_json_end_array(jb);
    cwist_json_end_object(jb);

    const char *json = cwist_json_get_raw(jb);
    cwist_sstring_assign(res->body, (char *)json);
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");

    switch (overall) {
        case CWIST_HEALTH_OK:       res->status_code = CWIST_HTTP_OK; break;
        case CWIST_HEALTH_DEGRADED: res->status_code = CWIST_HTTP_NOT_IMPLEMENTED; /* 501 reused */ break;
        case CWIST_HEALTH_FAIL:     res->status_code = CWIST_HTTP_SERVICE_UNAVAILABLE; break;
    }
    cwist_json_builder_destroy(jb);
}
