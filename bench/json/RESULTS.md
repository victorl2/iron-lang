# JSON benchmark baseline

Measured 2026-10-12 on Linux x86-64, a 13th Gen Intel Core i7-13700K,
with clang 23.1.2. Every process was pinned to CPU 2. Each cell is input
bytes divided by the median of 25 samples after 5 warmups, in decimal GB/s.
`±` is the median absolute deviation within a run; `runs` is half the range
of the three run medians relative to their median.

```sh
bench/json/run.sh build-release/ironc --native --repeat 3 --cpu 2 --out results.txt
```

The default build uses the release target baseline. The native build adds
`-march=native` to every implementation. Iron `-O2` is the normal release
build; the extra `-O3` column measures whether matching the reference
libraries' optimization level changes the baseline.

## Default target

### Parse to a tree

| input | Iron (-O2) | Iron (-O3) | simdjson DOM | yyjson | RapidJSON | Iron / best |
|---|---:|---:|---:|---:|---:|---:|
| twitter.json | 0.295 ±4% (runs ±18%) | 0.318 ±4% (runs ±5%) | 3.083 ±6% (runs ±3%) | 1.050 ±6% (runs ±0%) | 0.337 ±5% (runs ±4%) | 0.10x |
| citm_catalog.json | 0.292 ±4% (runs ±9%) | 0.309 ±4% (runs ±6%) | 3.346 ±2% (runs ±7%) | 1.049 ±2% (runs ±4%) | 0.683 ±3% (runs ±6%) | 0.09x |
| canada.json | 0.179 ±3% (runs ±3%) | 0.163 ±3% (runs ±2%) | 0.933 ±3% (runs ±1%) | 0.818 ±3% (runs ±2%) | 0.200 ±2% (runs ±7%) | 0.19x |
| gsoc-2018.json | 0.547 ±3% (runs ±18%) | 0.529 ±4% (runs ±3%) | 4.111 ±3% (runs ±2%) | 1.035 ±2% (runs ±1%) | 0.312 ±2% (runs ±3%) | 0.13x |
| mesh.json | 0.275 ±3% (runs ±2%) | 0.256 ±3% (runs ±1%) | 0.823 ±4% (runs ±1%) | 0.785 ±3% (runs ±3%) | 0.259 ±3% (runs ±4%) | 0.33x |
| numbers.json | 0.308 ±3% (runs ±2%) | 0.260 ±2% (runs ±0%) | 1.032 ±3% (runs ±3%) | 0.972 ±2% (runs ±4%) | 0.265 ±5% (runs ±2%) | 0.30x |

### Validation only

| input | Iron (-O2) | Iron (-O3) | simdjson DOM | yyjson | RapidJSON | Iron / best |
|---|---:|---:|---:|---:|---:|---:|
| twitter.json | 0.586 ±4% (runs ±3%) | 0.602 ±4% (runs ±2%) | 3.215 ±4% (runs ±7%) | 1.031 ±3% (runs ±2%) | 0.395 ±3% (runs ±3%) | 0.19x |
| citm_catalog.json | 1.236 ±5% (runs ±8%) | 1.247 ±4% (runs ±7%) | 3.279 ±4% (runs ±6%) | 1.039 ±3% (runs ±3%) | 0.779 ±4% (runs ±6%) | 0.38x |
| canada.json | 0.601 ±2% (runs ±1%) | 0.434 ±2% (runs ±4%) | 0.891 ±5% (runs ±4%) | 0.801 ±2% (runs ±2%) | 0.220 ±3% (runs ±1%) | 0.67x |
| gsoc-2018.json | 0.733 ±2% (runs ±2%) | 0.724 ±2% (runs ±1%) | 4.268 ±3% (runs ±2%) | 1.063 ±3% (runs ±3%) | 0.354 ±2% (runs ±2%) | 0.17x |
| mesh.json | 0.594 ±5% (runs ±3%) | 0.468 ±6% (runs ±5%) | 0.841 ±4% (runs ±1%) | 0.778 ±3% (runs ±1%) | 0.337 ±4% (runs ±4%) | 0.71x |
| numbers.json | 0.622 ±3% (runs ±8%) | 0.457 ±6% (runs ±0%) | 1.031 ±4% (runs ±4%) | 0.925 ±3% (runs ±4%) | 0.313 ±7% (runs ±4%) | 0.60x |

### Serialize a parsed tree

| input | Iron (-O2) | Iron (-O3) | simdjson DOM | yyjson | RapidJSON | Iron / best |
|---|---:|---:|---:|---:|---:|---:|
| twitter.json | 0.443 ±3% (runs ±3%) | 0.437 ±4% (runs ±3%) | 0.995 ±4% (runs ±1%) | 4.200 ±6% (runs ±5%) | 0.983 ±6% (runs ±3%) | 0.11x |
| citm_catalog.json | 0.508 ±2% (runs ±2%) | 0.497 ±2% (runs ±3%) | 2.972 ±4% (runs ±7%) | 6.560 ±4% (runs ±7%) | 1.991 ±3% (runs ±6%) | 0.08x |
| canada.json | 0.174 ±3% (runs ±1%) | 0.175 ±2% (runs ±3%) | 0.387 ±2% (runs ±5%) | 0.897 ±7% (runs ±11%) | 0.289 ±2% (runs ±1%) | 0.19x |
| gsoc-2018.json | 0.889 ±3% (runs ±2%) | 0.875 ±3% (runs ±2%) | 0.620 ±3% (runs ±5%) | 2.844 ±5% (runs ±1%) | 0.914 ±5% (runs ±4%) | 0.31x |
| mesh.json | 0.195 ±2% (runs ±2%) | 0.194 ±2% (runs ±1%) | 0.393 ±2% (runs ±1%) | 1.107 ±3% (runs ±2%) | 0.341 ±4% (runs ±1%) | 0.18x |
| numbers.json | 0.219 ±2% (runs ±3%) | 0.223 ±2% (runs ±1%) | 0.437 ±2% (runs ±3%) | 0.869 ±1% (runs ±2%) | 0.269 ±3% (runs ±3%) | 0.26x |

### Parse and visit every value

| input | Iron (-O2) | Iron (-O3) | simdjson DOM | simdjson On-Demand | yyjson | RapidJSON | Iron / best |
|---|---:|---:|---:|---:|---:|---:|---:|
| twitter.json | 0.271 ±4% (runs ±7%) | 0.278 ±2% (runs ±3%) | 2.391 ±3% (runs ±2%) | 1.613 ±3% (runs ±2%) | 0.944 ±3% (runs ±3%) | 0.327 ±5% (runs ±5%) | 0.12x |
| citm_catalog.json | 0.248 ±4% (runs ±5%) | 0.241 ±5% (runs ±4%) | 2.467 ±6% (runs ±4%) | 1.851 ±2% (runs ±2%) | 0.947 ±3% (runs ±4%) | 0.621 ±4% (runs ±3%) | 0.10x |
| canada.json | 0.136 ±4% (runs ±2%) | 0.127 ±3% (runs ±3%) | 0.755 ±2% (runs ±1%) | 0.543 ±3% (runs ±2%) | 0.696 ±4% (runs ±1%) | 0.197 ±4% (runs ±7%) | 0.18x |
| gsoc-2018.json | 0.485 ±5% (runs ±3%) | 0.488 ±5% (runs ±3%) | 3.476 ±4% (runs ±3%) | 3.211 ±3% (runs ±1%) | 1.032 ±4% (runs ±7%) | 0.311 ±4% (runs ±1%) | 0.14x |
| mesh.json | 0.151 ±3% (runs ±3%) | 0.142 ±2% (runs ±2%) | 0.649 ±4% (runs ±1%) | 0.453 ±2% (runs ±1%) | 0.678 ±2% (runs ±5%) | 0.247 ±3% (runs ±2%) | 0.22x |
| numbers.json | 0.226 ±2% (runs ±2%) | 0.200 ±3% (runs ±3%) | 0.799 ±3% (runs ±3%) | 0.579 ±2% (runs ±5%) | 0.810 ±1% (runs ±4%) | 0.250 ±3% (runs ±1%) | 0.28x |

### Many small documents

| mode | Iron (-O2) | Iron (-O3) | simdjson DOM | simdjson On-Demand | yyjson | RapidJSON | Iron / best |
|---|---:|---:|---:|---:|---:|---:|---:|
| parse each | 0.142 ±5% (runs ±4%) | 0.143 ±4% (runs ±3%) | 0.989 ±2% (runs ±1%) | - | 0.857 ±1% (runs ±3%) | 0.189 ±5% (runs ±2%) | 0.14x |
| validate each | 0.325 ±6% (runs ±3%) | 0.327 ±4% (runs ±1%) | 0.956 ±5% (runs ±2%) | - | 0.857 ±2% (runs ±6%) | 0.269 ±6% (runs ±5%) | 0.34x |
| parse and read 3 fields | 0.127 ±5% (runs ±3%) | 0.126 ±7% (runs ±5%) | 0.906 ±2% (runs ±5%) | 2.611 ±3% (runs ±5%) | 0.839 ±2% (runs ±8%) | 0.178 ±8% (runs ±4%) | 0.05x |
| serialize each | 0.147 ±5% (runs ±2%) | 0.142 ±5% (runs ±2%) | - | - | 1.065 ±2% (runs ±1%) | 0.265 ±4% (runs ±3%) | 0.14x |

## Native target

### Parse to a tree

| input | Iron (-O2) | Iron (-O3) | simdjson DOM | yyjson | RapidJSON | Iron / best |
|---|---:|---:|---:|---:|---:|---:|
| twitter.json | 0.309 ±3% (runs ±4%) | 0.314 ±4% (runs ±3%) | 3.065 ±6% (runs ±4%) | 1.058 ±5% (runs ±4%) | 0.337 ±4% (runs ±3%) | 0.10x |
| citm_catalog.json | 0.292 ±6% (runs ±3%) | 0.305 ±3% (runs ±2%) | 3.380 ±4% (runs ±4%) | 1.067 ±4% (runs ±2%) | 0.698 ±3% (runs ±3%) | 0.09x |
| canada.json | 0.178 ±4% (runs ±1%) | 0.166 ±4% (runs ±2%) | 0.900 ±4% (runs ±2%) | 0.903 ±2% (runs ±4%) | 0.196 ±3% (runs ±1%) | 0.20x |
| gsoc-2018.json | 0.534 ±4% (runs ±2%) | 0.535 ±3% (runs ±3%) | 4.157 ±2% (runs ±3%) | 1.056 ±3% (runs ±6%) | 0.323 ±3% (runs ±7%) | 0.13x |
| mesh.json | 0.260 ±3% (runs ±2%) | 0.254 ±4% (runs ±1%) | 0.848 ±6% (runs ±4%) | 0.873 ±5% (runs ±1%) | 0.256 ±4% (runs ±3%) | 0.30x |
| numbers.json | 0.310 ±4% (runs ±0%) | 0.269 ±4% (runs ±1%) | 1.077 ±4% (runs ±3%) | 1.111 ±4% (runs ±4%) | 0.266 ±5% (runs ±1%) | 0.28x |

### Validation only

| input | Iron (-O2) | Iron (-O3) | simdjson DOM | yyjson | RapidJSON | Iron / best |
|---|---:|---:|---:|---:|---:|---:|
| twitter.json | 0.593 ±4% (runs ±4%) | 0.588 ±3% (runs ±3%) | 3.224 ±7% (runs ±2%) | 1.108 ±4% (runs ±9%) | 0.405 ±4% (runs ±2%) | 0.18x |
| citm_catalog.json | 1.247 ±3% (runs ±1%) | 1.264 ±3% (runs ±1%) | 3.182 ±2% (runs ±2%) | 1.038 ±2% (runs ±3%) | 0.774 ±3% (runs ±1%) | 0.40x |
| canada.json | 0.596 ±3% (runs ±4%) | 0.466 ±2% (runs ±4%) | 0.896 ±2% (runs ±4%) | 0.899 ±2% (runs ±7%) | 0.218 ±2% (runs ±6%) | 0.66x |
| gsoc-2018.json | 0.723 ±4% (runs ±5%) | 0.722 ±3% (runs ±3%) | 4.128 ±3% (runs ±3%) | 1.255 ±4% (runs ±13%) | 0.356 ±3% (runs ±2%) | 0.18x |
| mesh.json | 0.518 ±5% (runs ±6%) | 0.490 ±4% (runs ±5%) | 0.807 ±4% (runs ±4%) | 0.882 ±4% (runs ±4%) | 0.344 ±4% (runs ±1%) | 0.59x |
| numbers.json | 0.692 ±5% (runs ±7%) | 0.489 ±5% (runs ±3%) | 1.036 ±2% (runs ±3%) | 1.077 ±3% (runs ±5%) | 0.312 ±4% (runs ±7%) | 0.64x |

### Serialize a parsed tree

| input | Iron (-O2) | Iron (-O3) | simdjson DOM | yyjson | RapidJSON | Iron / best |
|---|---:|---:|---:|---:|---:|---:|
| twitter.json | 0.470 ±3% (runs ±3%) | 0.470 ±4% (runs ±6%) | 1.071 ±4% (runs ±1%) | 3.746 ±3% (runs ±5%) | 0.952 ±2% (runs ±4%) | 0.13x |
| citm_catalog.json | 0.503 ±2% (runs ±1%) | 0.501 ±3% (runs ±3%) | 2.843 ±3% (runs ±2%) | 6.330 ±4% (runs ±2%) | 2.019 ±5% (runs ±5%) | 0.08x |
| canada.json | 0.177 ±2% (runs ±4%) | 0.176 ±2% (runs ±2%) | 0.381 ±2% (runs ±1%) | 0.981 ±3% (runs ±2%) | 0.295 ±2% (runs ±1%) | 0.18x |
| gsoc-2018.json | 0.844 ±2% (runs ±3%) | 0.845 ±2% (runs ±3%) | 0.587 ±5% (runs ±4%) | 2.882 ±6% (runs ±4%) | 0.914 ±3% (runs ±1%) | 0.29x |
| mesh.json | 0.198 ±2% (runs ±7%) | 0.202 ±2% (runs ±1%) | 0.385 ±3% (runs ±2%) | 1.134 ±2% (runs ±8%) | 0.358 ±3% (runs ±1%) | 0.18x |
| numbers.json | 0.226 ±4% (runs ±4%) | 0.222 ±3% (runs ±1%) | 0.430 ±2% (runs ±2%) | 0.927 ±2% (runs ±4%) | 0.275 ±2% (runs ±2%) | 0.24x |

### Parse and visit every value

| input | Iron (-O2) | Iron (-O3) | simdjson DOM | simdjson On-Demand | yyjson | RapidJSON | Iron / best |
|---|---:|---:|---:|---:|---:|---:|---:|
| twitter.json | 0.255 ±4% (runs ±4%) | 0.270 ±3% (runs ±4%) | 2.462 ±2% (runs ±1%) | 1.699 ±2% (runs ±3%) | 1.041 ±3% (runs ±3%) | 0.322 ±3% (runs ±2%) | 0.11x |
| citm_catalog.json | 0.239 ±5% (runs ±5%) | 0.232 ±6% (runs ±5%) | 2.568 ±4% (runs ±5%) | 1.849 ±3% (runs ±4%) | 0.949 ±4% (runs ±6%) | 0.644 ±4% (runs ±2%) | 0.09x |
| canada.json | 0.138 ±6% (runs ±2%) | 0.128 ±4% (runs ±11%) | 0.751 ±4% (runs ±2%) | 0.526 ±1% (runs ±1%) | 0.788 ±2% (runs ±2%) | 0.202 ±4% (runs ±3%) | 0.18x |
| gsoc-2018.json | 0.470 ±4% (runs ±2%) | 0.486 ±3% (runs ±2%) | 3.370 ±5% (runs ±4%) | 3.123 ±4% (runs ±1%) | 1.040 ±3% (runs ±7%) | 0.308 ±3% (runs ±1%) | 0.14x |
| mesh.json | 0.149 ±5% (runs ±2%) | 0.146 ±4% (runs ±8%) | 0.651 ±3% (runs ±5%) | 0.433 ±1% (runs ±1%) | 0.708 ±4% (runs ±0%) | 0.250 ±3% (runs ±4%) | 0.21x |
| numbers.json | 0.232 ±5% (runs ±4%) | 0.215 ±5% (runs ±4%) | 0.866 ±3% (runs ±2%) | 0.556 ±2% (runs ±2%) | 0.966 ±3% (runs ±3%) | 0.256 ±4% (runs ±1%) | 0.24x |

### Many small documents

| mode | Iron (-O2) | Iron (-O3) | simdjson DOM | simdjson On-Demand | yyjson | RapidJSON | Iron / best |
|---|---:|---:|---:|---:|---:|---:|---:|
| parse each | 0.142 ±4% (runs ±1%) | 0.145 ±4% (runs ±1%) | 1.007 ±2% (runs ±0%) | - | 0.917 ±2% (runs ±4%) | 0.192 ±4% (runs ±2%) | 0.14x |
| validate each | 0.319 ±6% (runs ±8%) | 0.293 ±5% (runs ±17%) | 1.003 ±3% (runs ±1%) | - | 0.898 ±5% (runs ±5%) | 0.271 ±7% (runs ±3%) | 0.32x |
| parse and read 3 fields | 0.128 ±8% (runs ±4%) | 0.136 ±4% (runs ±4%) | 0.964 ±2% (runs ±1%) | 2.621 ±3% (runs ±1%) | 0.867 ±2% (runs ±14%) | 0.188 ±4% (runs ±2%) | 0.05x |
| serialize each | 0.143 ±5% (runs ±5%) | 0.141 ±2% (runs ±2%) | - | - | 1.011 ±2% (runs ±6%) | 0.243 ±6% (runs ±4%) | 0.14x |
