# Performance baseline: Reubens-MacBook-Pro-2

- Host: Apple M1 Max (10 cores), Darwin 25.6.0
- Tools: tedge 2.0.2~423+gf0a9e19, mosquitto version 2.1.2
- Tree: 9c1254d267869d02c21e74bf8a74ddaa11d239ef (uncommitted changes)
- Each cell is the median of the last 3 runs, as `Rust / C`.

Scenarios that run for some implementations only:

- `samples-push-jitter`: rust only. The C sample has no timestamp field yet, so the C bench module refuses the source-time keys; native-batched-output adds it.


## opcua-polled

| Rate | Variant | Achieved readings/s | Connector µs/reading | Connector CPU % | Broker CPU % | Mapper CPU % | c8y msgs/s | Rate shortfall % | Missing % | drain_s | te/errors |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 20000 | – | 17647 / 17647 | 12.60 / 24.17 | 22 / 44 | 8 / 9 | 0 / 0 | 0 / 0 | 11.8 / 11.8 | 0.00 / 0.00 | 0.0 / 0.0 | 0 / 0 |
| 100000 | – | 64948 / 100000 | 12.58 / 9.22 | 83 / 92 | 32 / 48 | 0 / 0 | 0 / 0 | 35.1 / 0.0 | 0.00 / 0.00 | 0.1 / 0.1 | 0 / 0 |

## samples-flows

| Rate | Variant | Achieved readings/s | Connector µs/reading | Connector CPU % | Broker CPU % | Mapper CPU % | c8y msgs/s | Rate shortfall % | Missing % | drain_s | te/errors |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1000 | – | 882 / 941 | 10.31 / 14.42 | 1 / 1 | 5 / 5 | 42 / 47 | 900 / 971 | 11.8 / 5.9 | 0.00 / 0.00 | 0.0 / 0.1 | 0 / 0 |
| 2000 | – | 1882 / 1882 | 8.75 / 14.20 | 2 / 3 | 10 / 10 | 83 / 89 | 1800 / 1900 | 5.9 / 5.9 | 0.00 / 0.00 | 0.2 / 0.0 | 0 / 0 |
| 4000 | – | 3529 / 3765 | 6.84 / 14.99 | 2 / 6 | 17 / 17 | 147 / 145 | 3046 / 2839 | 11.8 / 5.9 | 0.00 / 0.00 | 3.8 / 5.6 | 0 / 0 |

## samples-latency

| Rate | Variant | Achieved readings/s | Connector µs/reading | Connector CPU % | Broker CPU % | Mapper CPU % | c8y msgs/s | Rate shortfall % | Missing % | drain_s | te/errors |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 50000 | – | 44118 / 50000 | 7.22 / 14.89 | 32 / 74 | 18 / 24 | 0 / 0 | 0 / 0 | 11.8 / 0.0 | 0.00 / 0.00 | 0.2 / 0.0 | 0 / 0 |
| 50000 | 4 instances | 44824 / 44118 | 7.45 / 22.98 | 33 / 103 | 19 / 20 | 0 / 0 | 0 / 0 | 10.4 / 11.8 | 0.00 / 0.00 | 0.3 / 0.4 | 0 / 0 |

## samples-noflows

| Rate | Variant | Achieved readings/s | Connector µs/reading | Connector CPU % | Broker CPU % | Mapper CPU % | c8y msgs/s | Rate shortfall % | Missing % | drain_s | te/errors |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 10000 | – | 8824 / 9518 | 8.06 / 23.45 | 7 / 22 | 4 / 6 | 0 / 0 | 0 / 0 | 11.8 / 4.8 | 0.00 / 0.00 | 0.0 / 0.0 | 0 / 0 |
| 50000 | – | 47059 / 50000 | 7.41 / 19.37 | 33 / 97 | 19 / 26 | 0 / 0 | 0 / 0 | 5.9 / 0.0 | 0.00 / 0.00 | 0.0 / 0.0 | 0 / 0 |
| 100000 | – | 89235 / 100000 | 7.15 / 9.72 | 64 / 97 | 36 / 53 | 0 / 0 | 0 / 0 | 10.8 / 0.0 | 0.00 / 0.00 | 0.2 / 0.0 | 0 / 0 |

## samples-push

| Rate | Variant | Achieved readings/s | Connector µs/reading | Connector CPU % | Broker CPU % | Mapper CPU % | c8y msgs/s | Rate shortfall % | Missing % | drain_s | te/errors |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 10000 | – | 10002 / 9965 | 18.13 / 14.31 | 18 / 14 | 8 / 6 | 0 / 0 | 0 / 0 | 0.0 / 0.4 | 0.00 / 0.00 | 0.0 / 0.0 | 0 / 0 |
| 50000 | – | 50001 / 50115 | 9.80 / 10.94 | 49 / 54 | 22 / 32 | 0 / 0 | 0 / 0 | 0.0 / 0.0 | 0.00 / 0.00 | 0.0 / 0.0 | 0 / 0 |
| 100000 | – | 99996 / 99996 | 9.61 / 9.78 | 96 / 98 | 42 / 64 | 0 / 0 | 0 / 0 | 0.0 / 0.0 | 0.00 / 0.00 | 0.0 / 0.0 | 0 / 0 |

## samples-push-jitter

| Rate | Variant | Achieved readings/s | Connector µs/reading | Connector CPU % | Broker CPU % | Mapper CPU % | c8y msgs/s | Rate shortfall % | Missing % | drain_s | te/errors |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 10000 | ts_resolution 10ms | 9999 / – | 17.27 / – | 17 / – | 7 / – | 0 / – | 0 / – | 0.0 / – | 0.00 / – | 0.0 / – | 0 / – |
| 10000 | ts_resolution 1ms | 9998 / – | 18.57 / – | 18 / – | 7 / – | 0 / – | 0 / – | 0.0 / – | 0.00 / – | 0.0 / – | 0 / – |
| 100000 | ts_resolution 10ms | 99995 / – | 16.23 / – | 162 / – | 53 / – | 0 / – | 0 / – | 0.0 / – | 0.00 / – | 0.0 / – | 0 / – |
| 100000 | ts_resolution 1ms | 99996 / – | 15.80 / – | 157 / – | 52 / – | 0 / – | 0 / – | 0.0 / – | 0.00 / – | 0.0 / – | 0 / – |
