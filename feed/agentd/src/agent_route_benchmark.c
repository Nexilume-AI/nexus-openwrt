#include "route_table.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#define BENCH_CANDIDATES_PER_INTENT 10U
#define BENCH_DEFAULT_ROUTES 10000U
#define BENCH_DEFAULT_LOOKUPS 50000U
#define BENCH_LOOKUP_P95_BUDGET_US 1000U
#define BENCH_MUTATION_P95_BUDGET_US 1000U
#define BENCH_BUILD_BUDGET_MS 10000U
#define BENCH_MEMORY_BUDGET_BYTES (20U * 1024U * 1024U)

static uint64_t monotonic_ns(void)
{
#ifdef _WIN32
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return ((uint64_t)counter.QuadPart * UINT64_C(1000000000)) /
           (uint64_t)frequency.QuadPart;
#else
    struct timespec value;
    (void)clock_gettime(CLOCK_MONOTONIC, &value);
    return ((uint64_t)value.tv_sec * UINT64_C(1000000000)) +
           (uint64_t)value.tv_nsec;
#endif
}

static int compare_u64(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static uint64_t percentile(uint64_t *samples, size_t count, size_t value)
{
    size_t index;

    qsort(samples, count, sizeof(*samples), compare_u64);
    index = ((count - 1U) * value) / 100U;
    return samples[index];
}

static bool parse_size(const char *value, size_t *result)
{
    char *end = NULL;
    unsigned long long parsed;

    if (value == NULL || value[0] == '\0' || result == NULL) return false;
    parsed = strtoull(value, &end, 10);
    if (end == value || *end != '\0' || parsed == 0U ||
        parsed > (unsigned long long)SIZE_MAX) return false;
    *result = (size_t)parsed;
    return true;
}

static void fill_route(struct agent_route *route, size_t index)
{
    size_t intent = index / BENCH_CANDIDATES_PER_INTENT;

    memset(route, 0, sizeof(*route));
    (void)snprintf(route->route_id, sizeof(route->route_id),
                   "%032" PRIx64, (uint64_t)index + 1U);
    (void)snprintf(route->intent, sizeof(route->intent),
                   "chip.p72.capability.%05" PRIu64, (uint64_t)intent);
    route->version = 1U;
    (void)snprintf(route->origin, sizeof(route->origin),
                   "agent://tenant-p72/worker-%" PRIu64, (uint64_t)index);
    (void)snprintf(route->endpoint, sizeof(route->endpoint),
                   "http://127.0.0.1:19000/invoke/%" PRIu64,
                   (uint64_t)index);
    (void)snprintf(route->tenant, sizeof(route->tenant), "tenant-p72");
    (void)snprintf(route->region, sizeof(route->region), "local");
    route->cost_microunits = (uint64_t)(index % 1000U);
    route->latency_ms = (uint32_t)(index % 200U) + 1U;
    route->trust_level = (uint8_t)(80U + (index % 21U));
    route->load_permille = (uint16_t)(index % 1001U);
    route->healthy = true;
    route->lease_expires_ms = UINT64_MAX;
    route->sequence = 1U;
    route->source = AGENT_ROUTE_SOURCE_LOCAL;
}

static void fill_query(struct route_query *query, size_t intent)
{
    memset(query, 0, sizeof(*query));
    (void)snprintf(query->intent, sizeof(query->intent),
                   "chip.p72.capability.%05" PRIu64, (uint64_t)intent);
    query->version = 1U;
    (void)snprintf(query->tenant, sizeof(query->tenant), "tenant-p72");
    (void)snprintf(query->region, sizeof(query->region), "local");
    query->min_trust_level = 0U;
}

int main(int argc, char **argv)
{
    size_t route_count = BENCH_DEFAULT_ROUTES;
    size_t lookup_count = BENCH_DEFAULT_LOOKUPS;
    size_t intent_count;
    size_t mutation_count;
    struct route_table table;
    struct agent_route route;
    struct route_query *queries;
    struct route_selection selection;
    struct route_renewal renewal = {0};
    uint64_t *lookup_samples;
    uint64_t *renew_samples;
    uint64_t *withdraw_samples;
    uint64_t started;
    uint64_t elapsed;
    uint64_t lookup_p50;
    uint64_t lookup_p95;
    uint64_t lookup_p99;
    uint64_t renew_p95;
    uint64_t withdraw_p95;
    uint64_t checksum = 0U;
    size_t memory_bytes;
    size_t index;
    bool passed;

    for (index = 1U; index < (size_t)argc; index++) {
        if (strcmp(argv[index], "--routes") == 0 && index + 1U < (size_t)argc) {
            if (!parse_size(argv[++index], &route_count)) return 2;
        } else if (strcmp(argv[index], "--lookups") == 0 &&
                   index + 1U < (size_t)argc) {
            if (!parse_size(argv[++index], &lookup_count)) return 2;
        } else {
            fprintf(stderr, "usage: %s [--routes N] [--lookups N]\n", argv[0]);
            return 2;
        }
    }
    intent_count = (route_count + BENCH_CANDIDATES_PER_INTENT - 1U) /
                   BENCH_CANDIDATES_PER_INTENT;
    mutation_count = route_count < 1000U ? route_count : 1000U;
    queries = calloc(intent_count, sizeof(*queries));
    lookup_samples = calloc(lookup_count, sizeof(*lookup_samples));
    renew_samples = calloc(mutation_count, sizeof(*renew_samples));
    withdraw_samples = calloc(mutation_count, sizeof(*withdraw_samples));
    if (queries == NULL || lookup_samples == NULL || renew_samples == NULL ||
        withdraw_samples == NULL) {
        fprintf(stderr, "benchmark allocation failed\n");
        free(queries);
        free(lookup_samples);
        free(renew_samples);
        free(withdraw_samples);
        return 2;
    }
    for (index = 0U; index < intent_count; index++) {
        fill_query(&queries[index], index);
    }

    route_table_init(&table, route_count);
    started = monotonic_ns();
    for (index = 0U; index < route_count; index++) {
        fill_route(&route, index);
        if (route_table_upsert(&table, &route) != ROUTE_TABLE_OK) {
            fprintf(stderr, "route population failed at %" PRIu64 "\n",
                    (uint64_t)index);
            route_table_destroy(&table);
            return 2;
        }
    }
    elapsed = monotonic_ns() - started;
    memory_bytes = route_table_memory_bytes(&table);

    for (index = 0U; index < lookup_count; index++) {
        started = monotonic_ns();
        if (!route_table_lookup(&table, &queries[index % intent_count],
                                1U, &selection)) {
            fprintf(stderr, "lookup failed at %" PRIu64 "\n",
                    (uint64_t)index);
            route_table_destroy(&table);
            return 2;
        }
        lookup_samples[index] = monotonic_ns() - started;
        checksum ^= selection.score;
    }
    lookup_p50 = percentile(lookup_samples, lookup_count, 50U);
    lookup_p95 = percentile(lookup_samples, lookup_count, 95U);
    lookup_p99 = percentile(lookup_samples, lookup_count, 99U);

    renewal.lease_expires_ms = UINT64_MAX;
    renewal.update_load = true;
    for (index = 0U; index < mutation_count; index++) {
        fill_route(&route, index);
        renewal.load_permille = (uint16_t)((index + 1U) % 1001U);
        started = monotonic_ns();
        if (route_table_renew(&table, route.route_id, &renewal) !=
            ROUTE_TABLE_OK) return 2;
        renew_samples[index] = monotonic_ns() - started;
    }
    renew_p95 = percentile(renew_samples, mutation_count, 95U);

    for (index = 0U; index < mutation_count; index++) {
        fill_route(&route, route_count - 1U - index);
        started = monotonic_ns();
        if (route_table_remove(&table, route.route_id) != ROUTE_TABLE_OK)
            return 2;
        withdraw_samples[index] = monotonic_ns() - started;
    }
    withdraw_p95 = percentile(withdraw_samples, mutation_count, 95U);

    passed = route_table_index_enabled(&table) &&
             elapsed / UINT64_C(1000000) <= BENCH_BUILD_BUDGET_MS &&
             lookup_p95 / UINT64_C(1000) <= BENCH_LOOKUP_P95_BUDGET_US &&
             renew_p95 / UINT64_C(1000) <= BENCH_MUTATION_P95_BUDGET_US &&
             withdraw_p95 / UINT64_C(1000) <= BENCH_MUTATION_P95_BUDGET_US &&
             memory_bytes <= BENCH_MEMORY_BUDGET_BYTES;

    printf("{\n");
    printf("  \"phase\": \"P7.2\",\n");
    printf("  \"routes\": %" PRIu64 ",\n", (uint64_t)route_count);
    printf("  \"lookups\": %" PRIu64 ",\n", (uint64_t)lookup_count);
    printf("  \"index_enabled\": %s,\n",
           route_table_index_enabled(&table) ? "true" : "false");
    printf("  \"index_buckets\": %" PRIu64 ",\n",
           (uint64_t)route_table_index_buckets(&table));
    printf("  \"route_struct_bytes\": %" PRIu64 ",\n",
           (uint64_t)sizeof(struct agent_route));
    printf("  \"route_table_memory_bytes\": %" PRIu64 ",\n",
           (uint64_t)memory_bytes);
    printf("  \"population_ms\": %.3f,\n", (double)elapsed / 1000000.0);
    printf("  \"lookup_p50_us\": %.3f,\n", (double)lookup_p50 / 1000.0);
    printf("  \"lookup_p95_us\": %.3f,\n", (double)lookup_p95 / 1000.0);
    printf("  \"lookup_p99_us\": %.3f,\n", (double)lookup_p99 / 1000.0);
    printf("  \"renew_p95_us\": %.3f,\n", (double)renew_p95 / 1000.0);
    printf("  \"withdraw_p95_us\": %.3f,\n", (double)withdraw_p95 / 1000.0);
    printf("  \"checksum\": \"%" PRIu64 "\",\n", checksum);
    printf("  \"passed\": %s\n", passed ? "true" : "false");
    printf("}\n");

    route_table_destroy(&table);
    free(queries);
    free(lookup_samples);
    free(renew_samples);
    free(withdraw_samples);
    return passed ? 0 : 1;
}
