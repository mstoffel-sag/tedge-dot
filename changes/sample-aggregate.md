### Mean, minimum and maximum per period in measurement output

A connector in measurement output can now publish, for every point and period, the time-weighted
mean, the minimum, the maximum or the last value, instead of one sampled reading. It accounts for
every reading of the period, so a spike or a drift between two published values still shows. See
[doc/measurement-output.md](../doc/measurement-output.md#aggregating-the-mean-minimum-and-maximum-of-each-period).

```toml
[[device]]
name  = "line1"
batch = { aggregate = ["mean", "min", "max"], every = "1s" }   # temp_mean, temp_min, temp_max
```

- `aggregate = "mean"` keeps the point's series name; a list publishes `<series>_<aggregate>`.
- The mean is weighted by how long each value held, so change-only pushed data averages correctly.
- An aggregated point cannot also have a `report` policy; the configuration is refused.
- The OT Connector Contract is now version 0.3.0 (§5.5, `batch.aggregate`).
