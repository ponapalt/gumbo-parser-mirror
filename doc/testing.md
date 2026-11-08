## Testing

If you want to check against fresh set of WPT's tree construction scenarios,
you need to clone [wpt](https://github.com/web-platform-tests/wpt) somewhere
and update tests database in `tests/tree_construction` with this command:
```sh
./tests/tree_construction/update.sh PATH_TO_YOUR_WPT_DIRECTORY
```

## Python tests

```sh
PYTHONPATH=python python3 -m gumbo.gumboc_test
PYTHONPATH=python python3 -m gumbo.soup_adapter_test  # requires bs4
PYTHONPATH=python python3 -m gumbo.html5lib_adapter_test  # requires html5lib
```
alternatively
```sh
python3 -m unittest discover -s python -p '*test.py'
```

## Fuzzing

```
export CC=clang CXX=clang++
meson setup --wipe buildfuzz --buildtype=debugoptimized -Dfuzz=true
meson compile -C buildfuzz
mkdir -p fuzz_artifacts
UBSAN_OPTIONS=print_stacktrace=1 ./buildfuzz/gumbo_fuzz -jobs=4 -artifact_prefix=fuzz_artifacts -print_final_stats=1 fuzz/corpus
```

## Benchmarking

`gumbo_perf` parses each file it's given repeatedly and reports the best and
median times, the number of allocations, the peak heap usage, and a hash of
everything the parser output (nodes, attributes, source positions and errors):

```sh
meson setup buildperf --buildtype=release -Dbenchmarks=true
meson compile -C buildperf
./buildperf/gumbo_perf benchmarks/*.html
```

To check a change, save the results before making it and compare after:

```sh
./buildperf/gumbo_perf -s before.tsv benchmarks/*.html
# ... make the change ...
meson compile -C buildperf
./buildperf/gumbo_perf -c before.tsv benchmarks/*.html
```
