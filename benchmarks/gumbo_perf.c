// gumbo_perf: parses each file repeatedly and reports the parse time, the
// number of allocations, the peak heap usage and a hash of everything the
// parser outputs (nodes, attributes, source positions and errors).
//
//   gumbo_perf -s before.tsv benchmarks/*.html
//   ... change something, rebuild ...
//   gumbo_perf -c before.tsv benchmarks/*.html
//
// Exits with status 2 if the output differs from the saved run, or if a parse
// leaked memory.

#define _POSIX_C_SOURCE 200809L

#include <err.h>
#include <inttypes.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "error.h"
#include "gumbo.h"

enum { kMinReps = 5, kMaxReps = 100000 };

static const double kDefaultSecondsPerFile = 1.0;
static const char kResultsHeader[] = "# gumbo_perf 1: name size_bytes best_s median_s allocations peak_bytes hash\n";

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void* xrealloc(void* ptr, size_t size) {
  ptr = realloc(ptr, size);
  if (!ptr) {
    err(EXIT_FAILURE, "realloc");
  }
  return ptr;
}

typedef struct {
  size_t allocations;
  size_t live_bytes;
  size_t peak_bytes;
} AllocStats;

static void* counting_alloc(void* userdata, size_t size) {
  AllocStats* stats = userdata;
  void* ptr = xrealloc(NULL, size);
  ++stats->allocations;
  stats->live_bytes += malloc_usable_size(ptr);
  if (stats->live_bytes > stats->peak_bytes) {
    stats->peak_bytes = stats->live_bytes;
  }
  return ptr;
}

static void counting_free(void* userdata, void* ptr) {
  AllocStats* stats = userdata;
  stats->live_bytes -= malloc_usable_size(ptr);
  free(ptr);
}

typedef struct {
  uint64_t hash;
  const char* input;
  size_t input_bytes;
} Hasher;

static void hash_bytes(Hasher* h, const void* data, size_t size) {
  const unsigned char* bytes = data;
  for (size_t i = 0; i < size; ++i) {
    h->hash = (h->hash ^ bytes[i]) * UINT64_C(0x100000001b3);
  }
}

static void hash_int(Hasher* h, uint64_t value) {
  hash_bytes(h, &value, sizeof(value));
}

static void hash_string(Hasher* h, const char* str) {
  if (str) {
    hash_bytes(h, str, strlen(str) + 1);
  } else {
    hash_int(h, UINT64_MAX);
  }
}

static void hash_input_pointer(Hasher* h, const char* ptr) {
  uintptr_t offset = (uintptr_t) ptr - (uintptr_t) h->input;
  if (!ptr) {
    hash_int(h, UINT64_MAX);
  } else if (offset <= h->input_bytes) {
    hash_int(h, offset);
  } else {
    hash_int(h, UINT32_MAX);
  }
}

static void hash_piece(Hasher* h, GumboStringPiece piece) {
  hash_input_pointer(h, piece.data);
  hash_int(h, piece.length);
}

static void hash_position(Hasher* h, GumboSourcePosition pos) {
  hash_int(h, pos.line);
  hash_int(h, pos.column);
  hash_int(h, pos.offset);
}

static void hash_attribute(Hasher* h, const GumboAttribute* attr) {
  hash_int(h, attr->attr_namespace);
  hash_string(h, attr->name);
  hash_string(h, attr->value);
  hash_piece(h, attr->original_name);
  hash_piece(h, attr->original_value);
  hash_position(h, attr->name_start);
  hash_position(h, attr->name_end);
  if (attr->original_value.data != attr->original_name.data) {
    hash_position(h, attr->value_start);
    hash_position(h, attr->value_end);
  }
}

static void hash_node(Hasher* h, const GumboNode* node) {
  hash_int(h, node->type);
  hash_int(h, node->parse_flags);
  hash_int(h, node->index_within_parent);

  const GumboVector* children;
  if (node->type == GUMBO_NODE_DOCUMENT) {
    const GumboDocument* doc = &node->v.document;
    hash_int(h, doc->has_doctype);
    hash_string(h, doc->name);
    hash_string(h, doc->public_identifier);
    hash_string(h, doc->system_identifier);
    hash_int(h, doc->doc_type_quirks_mode);
    children = &doc->children;
  } else if (node->type == GUMBO_NODE_ELEMENT || node->type == GUMBO_NODE_TEMPLATE) {
    const GumboElement* element = &node->v.element;
    hash_int(h, element->tag);
    hash_int(h, element->tag_namespace);
    hash_piece(h, element->original_tag);
    hash_piece(h, element->original_end_tag);
    hash_position(h, element->start_pos);
    hash_position(h, element->end_pos);
    hash_int(h, element->attributes.length);
    for (unsigned int i = 0; i < element->attributes.length; ++i) {
      hash_attribute(h, element->attributes.data[i]);
    }
    children = &element->children;
  } else {
    hash_string(h, node->v.text.text);
    hash_piece(h, node->v.text.original_text);
    hash_position(h, node->v.text.start_pos);
    return;
  }

  hash_int(h, children->length);
  for (unsigned int i = 0; i < children->length; ++i) {
    const GumboNode* child = children->data[i];
    hash_int(h, child->parent == node);
    hash_node(h, child);
  }
}

static uint64_t hash_output(const GumboOutput* output, const char* input, size_t input_bytes) {
  Hasher h = {UINT64_C(0xcbf29ce484222325), input, input_bytes};
  hash_int(&h, output->errors.length);
  for (unsigned int i = 0; i < output->errors.length; ++i) {
    const GumboError* error = output->errors.data[i];
    hash_int(&h, error->type);
    hash_position(&h, error->position);
    hash_input_pointer(&h, error->original_text);
  }
  hash_node(&h, output->document);
  return h.hash;
}

typedef struct {
  char name[256];
  size_t size_bytes;
  double best_s;
  double median_s;
  size_t allocations;
  size_t peak_bytes;
  uint64_t hash;
} Result;

static void save_results(const char* path, const Result* results, int count) {
  FILE* f = fopen(path, "w");
  if (!f) {
    err(EXIT_FAILURE, "%s", path);
  }
  fputs(kResultsHeader, f);
  for (int i = 0; i < count; ++i) {
    const Result* r = &results[i];
    fprintf(f, "%s\t%zu\t%.9f\t%.9f\t%zu\t%zu\t%016" PRIx64 "\n",
        r->name,
        r->size_bytes,
        r->best_s,
        r->median_s,
        r->allocations,
        r->peak_bytes,
        r->hash);
  }
  if (fclose(f) != 0) {
    err(EXIT_FAILURE, "%s", path);
  }
}

static Result* load_results(const char* path, int* count) {
  FILE* f = fopen(path, "r");
  if (!f) {
    err(EXIT_FAILURE, "%s", path);
  }
  char line[1024];
  if (!fgets(line, sizeof(line), f) || strcmp(line, kResultsHeader) != 0) {
    errx(EXIT_FAILURE, "%s: not a gumbo_perf results file", path);
  }
  Result* results = NULL;
  *count = 0;
  while (fgets(line, sizeof(line), f)) {
    results = xrealloc(results, (*count + 1) * sizeof(Result));
    Result* r = &results[*count];
    if (sscanf(line, "%255[^\t]\t%zu\t%lf\t%lf\t%zu\t%zu\t%" SCNx64,
        r->name,
        &r->size_bytes,
        &r->best_s,
        &r->median_s,
        &r->allocations,
        &r->peak_bytes,
        &r->hash) != 7)
    {
      errx(EXIT_FAILURE, "%s: malformed line: %s", path, line);
    }
    ++*count;
  }
  if (ferror(f)) {
    err(EXIT_FAILURE, "%s", path);
  }
  fclose(f);
  return results;
}

static const Result* find_result(const Result* results, int count, const char* name) {
  for (int i = 0; i < count; ++i) {
    if (strcmp(results[i].name, name) == 0) {
      return &results[i];
    }
  }
  return NULL;
}

static char* read_file(const char* path, size_t* size_bytes) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    err(EXIT_FAILURE, "%s", path);
  }
  size_t capacity = 1 << 16, size = 0, n;
  char* data = xrealloc(NULL, capacity);
  while ((n = fread(data + size, 1, capacity - size, f)) > 0) {
    size += n;
    if (size == capacity) {
      capacity *= 2;
      data = xrealloc(data, capacity);
    }
  }
  if (ferror(f)) {
    err(EXIT_FAILURE, "%s", path);
  }
  fclose(f);
  *size_bytes = size;
  return data;
}

static int compare_doubles(const void* a, const void* b) {
  double x = *(const double*) a, y = *(const double*) b;
  return (x > y) - (x < y);
}

static double times_s[kMaxReps];

static bool measure(const char* path, double seconds_per_file, Result* r) {
  size_t size_bytes;
  char* input = read_file(path, &size_bytes);
  const char* slash = strrchr(path, '/');
  snprintf(r->name, sizeof(r->name), "%s", slash ? slash + 1 : path);
  r->size_bytes = size_bytes;

  AllocStats stats = {0, 0, 0};
  GumboOptions options = kGumboDefaultOptions;
  options.allocator = counting_alloc;
  options.deallocator = counting_free;
  options.userdata = &stats;
  GumboOutput* output = gumbo_parse_with_options(&options, input, size_bytes);
  r->hash = hash_output(output, input, size_bytes);
  gumbo_destroy_output(&options, output);
  r->allocations = stats.allocations;
  r->peak_bytes = stats.peak_bytes;

  int reps = 0;
  double total_s = 0;
  while (reps < kMinReps || (total_s < seconds_per_file && reps < kMaxReps)) {
    double start_s = now_s();
    output = gumbo_parse_with_options(&kGumboDefaultOptions, input, size_bytes);
    gumbo_destroy_output(&kGumboDefaultOptions, output);
    times_s[reps] = now_s() - start_s;
    total_s += times_s[reps++];
  }
  qsort(times_s, reps, sizeof(double), compare_doubles);
  r->best_s = times_s[0];
  r->median_s = times_s[reps / 2];

  free(input);
  return stats.live_bytes == 0; // false if parsing leaked memory
}

static void print_change(double now, double before) {
  if (before > 0) {
    printf(" %+7.1f%%", 100 * (now - before) / before);
  } else {
    printf(" %8s", "");
  }
}

static void print_row(const Result* r, const Result* base, const char* output, bool leaked) {
  printf("%-24.24s %10.3f", r->name, r->best_s * 1e3);
  print_change(r->best_s, base ? base->best_s : 0);
  printf(" %10.3f %10zu", r->median_s * 1e3, r->allocations);
  print_change(r->allocations, base ? base->allocations : 0);
  printf(" %9zu", r->peak_bytes / 1024);
  print_change(r->peak_bytes, base ? base->peak_bytes : 0);
  printf("  %s%s\n", output, leaked ? " LEAKED" : "");
  fflush(stdout);
}

static void add_to_total(Result* total, const Result* r) {
  total->size_bytes += r->size_bytes;
  total->best_s += r->best_s;
  total->median_s += r->median_s;
  total->allocations += r->allocations;
  if (r->peak_bytes > total->peak_bytes) {
    total->peak_bytes = r->peak_bytes;
  }
}

static void usage(void) {
  fprintf(stderr,
      "usage: gumbo_perf [-t SECONDS] [-s SAVE.tsv] [-c BASELINE.tsv] "
      "FILE...\n"
      "  -t SECONDS  time to spend parsing each file (default %g)\n"
      "  -s FILE     save the results to FILE\n"
      "  -c FILE     compare with results saved earlier with -s\n",
      kDefaultSecondsPerFile);
  exit(EXIT_FAILURE);
}

int main(int argc, char** argv) {
  double seconds_per_file = kDefaultSecondsPerFile;
  const char* save_path = NULL;
  const char* baseline_path = NULL;
  int opt;
  while ((opt = getopt(argc, argv, "t:s:c:")) != -1) {
    char* end;
    switch (opt) {
      case 't':
        seconds_per_file = strtod(optarg, &end);
        if (*end || end == optarg || seconds_per_file < 0) {
          usage();
        }
        break;
      case 's':
        save_path = optarg;
        break;
      case 'c':
        baseline_path = optarg;
        break;
      default:
        usage();
    }
  }
  char** files = argv + optind;
  int file_count = argc - optind;
  if (file_count == 0) {
    usage();
  }

  Result* baseline = NULL;
  int baseline_count = 0;
  if (baseline_path) {
    baseline = load_results(baseline_path, &baseline_count);
  }

  printf("%-24s %10s %8s %10s %10s %8s %9s %8s  %s\n",
    "file",
    "best ms",
    "change",
    "median ms",
    "allocs",
    "change",
    "peak KiB",
    "change",
    baseline_path ? "output" : "output hash");

  Result* results = xrealloc(NULL, file_count * sizeof(Result));
  Result total = {"total", 0, 0, 0, 0, 0, 0};
  Result baseline_total = total;
  bool all_in_baseline = baseline_path != NULL;
  bool output_changed = false, any_leaked = false;
  for (int i = 0; i < file_count; ++i) {
    Result* r = &results[i];
    bool leaked = !measure(files[i], seconds_per_file, r);
    any_leaked |= leaked;

    const Result* base = find_result(baseline, baseline_count, r->name);
    char hash_hex[17];
    const char* output;
    if (!baseline_path) {
      snprintf(hash_hex, sizeof(hash_hex), "%016" PRIx64, r->hash);
      output = hash_hex;
    } else if (!base) {
      output = "not in baseline";
    } else if (base->size_bytes != r->size_bytes) {
      output = "different input";
      base = NULL;
    } else if (base->hash != r->hash) {
      output = "CHANGED";
      output_changed = true;
    } else {
      output = "same";
    }
    print_row(r, base, output, leaked);

    add_to_total(&total, r);
    if (base) {
      add_to_total(&baseline_total, base);
    } else {
      all_in_baseline = false;
    }
  }

  char throughput[32];
  snprintf(throughput, sizeof(throughput), "%.1f MB/s", total.size_bytes / total.best_s / 1e6);
  print_row(&total, all_in_baseline ? &baseline_total : NULL, throughput, false);

  if (output_changed) {
    printf("\nThe parser's output CHANGED for some files.\n");
  }
  if (baseline_path) {
    printf("\nTiming changes of a few percent are usually noise; rerun or raise -t to check.\n");
  }
  if (save_path) {
    save_results(save_path, results, file_count);
  }
  free(results);
  free(baseline);
  return output_changed || any_leaked ? 2 : 0;
}
