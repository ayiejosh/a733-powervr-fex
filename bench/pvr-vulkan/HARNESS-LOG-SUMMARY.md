# Consolidated harness-log.jsonl

102 records, grouped by (driver, probe, size). Each row shows the run count and the range
actually observed, because a single number hides the spread.

| driver | probe | size | runs | speed (min-max) | correctness |
|---|---|---|---|---|---|
| driver | cstp | 64 | 2 | - | - |
| driver | cstpf | 64 | 2 | - | - |
| driver | cstpi | 64 | 2 | - | - |
| driver | vkheavy | 2048 | 2 | - | - |
| driver | vkrender | 2048 | 2 | - | - |
| driver | vkrender | 512 | 2 | - | - |
| powervr | cstp | 64 | 2 | 284.900-361.000 M inv/s | - |
| powervr | cstpf | 64 | 2 | 87.000-88.200 M inv/s | - |
| powervr | cstpi | 64 | 2 | 70.700-72.700 M inv/s | - |
| powervr | cstpin | 64 | 1 | 72.000-72.000 M inv/s | - |
| powervr | vkheavy | 2048 | 6 | 255.036-256.111 ms/frame | - |
| powervr | vkrender | 1024 | 2 | 3.979-3.983 ms/frame | 2/2 PASS |
| powervr | vkrender | 2048 | 8 | 13.512-14.116 ms/frame | 7/7 PASS |
| powervr | vkrender | 4096 | 2 | 53.128-53.892 ms/frame | 2/2 PASS |
| powervr | vkrender | 512 | 8 | 1.435-1.743 ms/frame | 7/7 PASS |
| pvrsrvkm | cstp | 64 | 7 | 101.900-413.100 M inv/s | - |
| pvrsrvkm | cstpf | 64 | 6 | 44.800-153.100 M inv/s | - |
| pvrsrvkm | cstpi | 64 | 6 | 70.800-153.400 M inv/s | - |
| pvrsrvkm | cstpi1 | 64 | 1 | 146.500-146.500 M inv/s | - |
| pvrsrvkm | cstpin | 64 | 2 | 145.700-154.200 M inv/s | - |
| pvrsrvkm | vkheavy | 2048 | 7 | 180.065-180.206 ms/frame | - |
| pvrsrvkm | vkrender | 1024 | 2 | 1.707-1.776 ms/frame | 2/2 PASS |
| pvrsrvkm | vkrender | 2048 | 14 | 5.392-7.260 ms/frame | 14/14 PASS |
| pvrsrvkm | vkrender | 256 | 1 | 0.612-0.612 ms/frame | 1/1 PASS |
| pvrsrvkm | vkrender | 4096 | 2 | 23.678-27.796 ms/frame | 2/2 PASS |
| pvrsrvkm | vkrender | 512 | 9 | 0.682-0.836 ms/frame | 9/9 PASS |
