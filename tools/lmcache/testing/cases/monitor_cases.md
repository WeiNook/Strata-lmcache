# Shared cache - monitor cases (Python)

Generated from `terminal/serve/test_lmcache_status.py` by `tools/lmcache/testing/list_cases.py`.  Run them with
(no pytest needed):

```
cd terminal && python3 -m unittest serve.test_lmcache_status -v
```

13 cases.

| class | case | what it checks |
|---|---|---|
| `ReduceTest` | `test_a_store_that_is_down_is_unavailable` | - |
| `ReduceTest` | `test_the_pool_comes_from_storage_manager` | - |
| `ReduceTest` | `test_the_pool_also_comes_from_l1_usage` | - |
| `ReduceTest` | `test_alone_in_the_pool_the_totals_are_this_models` | - |
| `ReduceTest` | `test_a_shared_pool_reports_no_per_model_share` | - |
| `ReduceTest` | `test_a_model_that_never_registered_says_so` | - |
| `ReduceTest` | `test_a_boolean_is_not_a_measurement` | - |
| `SettingsTest` | `test_the_report_plane_follows_the_data_plane` | - |
| `SettingsTest` | `test_the_http_setting_wins` | - |
| `SettingsTest` | `test_the_namespace_defaults_to_the_engines_own` | - |
| `LiveTest` | `test_a_reachable_store_is_reported_without_blocking_the_caller` | - |
| `LiveTest` | `test_an_unreachable_store_is_unavailable_not_an_exception` | - |
| `LiveTest` | `test_a_store_that_is_switched_off_is_not_polled` | - |

The card reads the store's HTTP report plane only; it is polled off the request path and never raises, which is
what the "unreachable store" and "no store configured" cases pin.
