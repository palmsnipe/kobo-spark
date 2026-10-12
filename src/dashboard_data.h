// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DASHBOARD_DATA_H
#define DASHBOARD_DATA_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#define DASH_UNITS 4
#define DASH_POINTS 120
#define DASH_WINDOW_MS 1800000LL
#define DASH_BASE "http://192.168.1.100:5555"
#define DASH_DIR "/mnt/onboard/.adds/kobo-spark"
typedef struct { int64_t t; double temp, power, usage; } GpuPoint;
typedef struct { int64_t t; double rate; } RatePoint;
typedef struct {
    char id[64], name[64], role[24], head[64], model[128], health[128];
    bool fetched, online, worker, llm, history_ok;
    int64_t updated;
    double rate, prefill, temp, power, system_power, gpu, cpu, memory, available;
    double running, waiting, context;
    int points;
    GpuPoint history[DASH_POINTS];
} Spark;
typedef struct { int count; Spark units[DASH_UNITS]; char error[128]; } Fleet;
typedef struct {
    int fd;
    pid_t pid;
    int64_t started;
    size_t received;
    Fleet result;
} DashJob;
typedef struct { char id[64]; int count; RatePoint points[DASH_POINTS]; } RateHistory;
int dash_parse_sparks(const char *json, Fleet *fleet);
int dash_parse_metrics(const char *json, Spark *spark);
int dash_parse_history(const char *json, Spark *spark);
void dash_fetch(Fleet *fleet, const char *base);
int dash_start(DashJob *job, const char *base);
bool dash_poll(DashJob *job);
void dash_cancel(DashJob *job);
void dash_merge(Fleet *current, const Fleet *incoming);
void dash_sample(RateHistory *history, const Spark *spark, int64_t now);
int64_t dash_monotonic(void);
int64_t dash_wall(void);
int dash_config(const char *path, char *base, size_t size);
#endif
