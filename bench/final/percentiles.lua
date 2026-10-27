-- wrk report script: prints the percentiles wrk's --latency leaves out (p95 in particular) and the error counters.
done = function(summary, latency, requests)
  io.write("--percentiles (microseconds)\n")
  for _, p in ipairs({50, 75, 90, 95, 99, 99.9}) do
    io.write(string.format("p%s %d\n", tostring(p), latency:percentile(p)))
  end
  io.write(string.format("--totals\nrequests %d\nduration_us %d\nbytes %d\n", summary.requests, summary.duration, summary.bytes))
  io.write(string.format("errors connect=%d read=%d write=%d status=%d timeout=%d\n",
    summary.errors.connect, summary.errors.read, summary.errors.write, summary.errors.status, summary.errors.timeout))
end
