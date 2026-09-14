-- wrk tail latency reporting script for CWIST benchmarks
done = function(summary, latency, requests)
    io.write("--------------------------------------------------\n")
    io.write(string.format("Requests/sec:  %12.2f\n", summary.requests / (summary.duration / 1000000)))
    io.write(string.format("Avg Latency:   %12.3f ms\n", (latency.mean / 1000)))
    io.write(string.format("Min Latency:   %12.3f ms\n", (latency.min / 1000)))
    io.write(string.format("Max Latency:   %12.3f ms\n", (latency.max / 1000)))
    io.write("Latency Distribution:\n")
    for _, p in ipairs({ 50, 75, 90, 99, 99.9, 99.99, 99.999 }) do
        io.write(string.format("  %7g%% %12.3fms\n", p, latency:percentile(p) / 1000))
    end
    io.write("--------------------------------------------------\n")
    -- Machine-readable, fail-closed admission data. These are wrk's corrected
    -- latency statistics, not an uncorrected per-request histogram.
    local quantiles = {}
    for _, p in ipairs({ 50, 75, 90, 99, 99.9, 99.99, 99.999 }) do
        table.insert(quantiles, string.format('"%g":%.17g', p, latency:percentile(p)))
    end
    io.write(string.format('CWIST_METRICS {"requests":%d,"duration_us":%d,"mean_us":%.17g,"min_us":%.17g,"max_us":%.17g,"percentiles_us":{%s},"errors":{"connect":%d,"read":%d,"write":%d,"timeout":%d,"status":%d}}\n',
        summary.requests, summary.duration, latency.mean, latency.min, latency.max,
        table.concat(quantiles, ','), summary.errors.connect, summary.errors.read,
        summary.errors.write, summary.errors.timeout, summary.errors.status))
end
