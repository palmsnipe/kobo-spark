// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "dashboard_data.h"
#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* A bounded JSON reader. Each token records its entire subtree's end so that
 * unfamiliar nested fields can be skipped without confusing sibling fields. */
typedef struct { int start, len, end, size; char type; } Token;
typedef struct { const char *s; size_t pos, len; int used; Token *tokens; } Json;
#define MAX_TOKENS 20000
#define MAX_JSON (256U * 1024U)
static void whitespace(Json *j) { while (j->pos < j->len && strchr(" \r\n\t", j->s[j->pos])) j->pos++; }
static int value(Json *j, unsigned depth) {
    whitespace(j);
    if (depth > 32 || j->pos >= j->len || j->used >= MAX_TOKENS) return -1;
    int n = j->used++;
    Token *t = &j->tokens[n];
    t->start = (int)j->pos; t->type = j->s[j->pos++];
    if (t->type == '{' || t->type == '[') {
        char close = t->type == '{' ? '}' : ']';
        whitespace(j);
        if (j->pos < j->len && j->s[j->pos] == close) j->pos++;
        else for (;;) {
            if (t->type == '{') {
                whitespace(j);
                if (j->pos >= j->len || j->s[j->pos] != '"' || value(j, depth + 1) < 0) return -1;
                whitespace(j);
                if (j->pos >= j->len || j->s[j->pos++] != ':') return -1;
            }
            if (value(j, depth + 1) < 0) return -1;
            t->size++;
            whitespace(j);
            if (j->pos >= j->len) return -1;
            char c = j->s[j->pos++];
            if (c == close) break;
            if (c != ',') return -1;
        }
    } else if (t->type == '"') {
        for (;;) {
            if (j->pos >= j->len) return -1;
            unsigned char c = (unsigned char)j->s[j->pos++];
            if (c == '"') break;
            if (c < 32) return -1;
            if (c == '\\') {
                if (j->pos >= j->len) return -1;
                c = (unsigned char)j->s[j->pos++];
                if (!strchr("\"\\/bfnrtu", c)) return -1;
                if (c == 'u') for (int i = 0; i < 4; i++) {
                    if (j->pos >= j->len || !isxdigit((unsigned char)j->s[j->pos++])) return -1;
                }
            }
        }
    } else {
        j->pos--;
        const char *literal = t->type == 't' ? "true" : t->type == 'f' ? "false" : t->type == 'n' ? "null" : NULL;
        if (literal) {
            size_t length = strlen(literal);
            if (j->len - j->pos < length || strncmp(j->s + j->pos, literal, length)) return -1;
            j->pos += length;
        } else {
            size_t p = j->pos;
            if (j->s[p] == '-') p++;
            if (p >= j->len || !isdigit((unsigned char)j->s[p])) return -1;
            if (j->s[p] == '0') p++;
            else while (p < j->len && isdigit((unsigned char)j->s[p])) p++;
            if (p < j->len && j->s[p] == '.') {
                p++;
                if (p >= j->len || !isdigit((unsigned char)j->s[p])) return -1;
                while (p < j->len && isdigit((unsigned char)j->s[p])) p++;
            }
            if (p < j->len && (j->s[p] == 'e' || j->s[p] == 'E')) {
                p++;
                if (p < j->len && (j->s[p] == '+' || j->s[p] == '-')) p++;
                if (p >= j->len || !isdigit((unsigned char)j->s[p])) return -1;
                while (p < j->len && isdigit((unsigned char)j->s[p])) p++;
            }
            j->pos = p; t->type = '#';
        }
    }
    t->len = (int)j->pos - t->start; t->end = j->used;
    return n;
}
static int parse(Json *j, const char *s) {
    memset(j, 0, sizeof(*j)); j->s = s; j->len = strlen(s);
    if (j->len > MAX_JSON) return -1;
    j->tokens = calloc(MAX_TOKENS, sizeof(Token));
    if (!j->tokens) return -1;
    int root = value(j, 0);
    whitespace(j);
    if (root < 0 || j->pos != j->len) { free(j->tokens); j->tokens = NULL; return -1; }
    return 0;
}
static bool eq(Json *j, int n, const char *s) {
    return n >= 0 && j->tokens[n].type == '"' && j->tokens[n].len == (int)strlen(s) + 2 &&
        !strncmp(j->s + j->tokens[n].start + 1, s, strlen(s));
}
static int field(Json *j, int n, const char *s) {
    if (n < 0 || j->tokens[n].type != '{') return -1;
    for (int i = n + 1; i < j->tokens[n].end;) {
        int v = i + 1;
        if (eq(j, i, s)) return v;
        i = j->tokens[v].end;
    }
    return -1;
}
static double number(Json *j, int n) {
    if (n < 0 || j->tokens[n].type != '#') return NAN;
    double d = strtod(j->s + j->tokens[n].start, NULL);
    return isfinite(d) && fabs(d) < 1e15 ? d : NAN;
}
static double num(Json *j, int n, const char *key) { return number(j, field(j, n, key)); }
static bool boolean(Json *j, int n, const char *key) {
    int v = field(j, n, key); return v >= 0 && j->tokens[v].type == 't';
}
static void string(Json *j, int n, char *dest, size_t size) {
    if (size == 0) return;
    dest[0] = 0;
    if (n < 0 || j->tokens[n].type != '"') return;
    size_t out = 0;
    int end = j->tokens[n].start + j->tokens[n].len - 1;
    for (int i = j->tokens[n].start + 1; i < end && out + 1 < size; i++) {
        unsigned char c = (unsigned char)j->s[i];
        if (c == '\\') {
            c = (unsigned char)j->s[++i];
            if (c == 'u') { i += 4; c = '?'; }
            else if (strchr("bfnrt", c)) c = ' ';
        }
        dest[out++] = c < 32 ? ' ' : (char)c;
    }
    dest[out] = 0;
}
static void str(Json *j, int n, const char *key, char *dest, size_t size) { string(j, field(j, n, key), dest, size); }
static void blank(Spark *s) {
    memset(s, 0, sizeof(*s));
    s->rate = s->prefill = s->temp = s->power = s->system_power = s->gpu = s->cpu = NAN;
    s->memory = s->available = s->running = s->waiting = s->context = NAN;
}
int dash_parse_sparks(const char *json, Fleet *fleet) {
    Json j;
    if (parse(&j, json) < 0) return -1;
    int list = j.tokens[0].type == '[' ? 0 : field(&j, 0, "sparks");
    if (list < 0 || j.tokens[list].type != '[') { free(j.tokens); return -1; }
    fleet->count = 0;
    for (int i = list + 1; i < j.tokens[list].end && fleet->count < DASH_UNITS; i = j.tokens[i].end) {
        Spark *s = &fleet->units[fleet->count]; blank(s);
        str(&j, i, "id", s->id, sizeof(s->id)); str(&j, i, "name", s->name, sizeof(s->name));
        if (!s->id[0]) { free(j.tokens); return -1; }
        /* Refuse an identifier too long to represent without aliasing another unit. */
        int id = field(&j, i, "id");
        if (j.tokens[id].len > (int)sizeof(s->id) + 1) { free(j.tokens); return -1; }
        for (int n = 0; n < fleet->count; n++) if (!strcmp(fleet->units[n].id, s->id)) { free(j.tokens); return -1; }
        fleet->count++;
    }
    free(j.tokens);
    return fleet->count ? 0 : -1;
}
int dash_parse_metrics(const char *json, Spark *s) {
    Json j;
    if (parse(&j, json) < 0) return -1;
    char id[64]; str(&j, 0, "id", id, sizeof(id));
    int rootmetrics = field(&j, 0, "metrics");
    int online = field(&j, 0, "online");
    if (strcmp(id, s->id) || online < 0 || (j.tokens[online].type != 't' && j.tokens[online].type != 'f') ||
        (boolean(&j, 0, "online") && (rootmetrics < 0 || j.tokens[rootmetrics].type != '{'))) { free(j.tokens); return -1; }
    Spark next; blank(&next);
    snprintf(next.id, sizeof(next.id), "%s", s->id);
    str(&j, 0, "name", next.name, sizeof(next.name));
    if (!next.name[0]) snprintf(next.name, sizeof(next.name), "%s", s->name);
    str(&j, 0, "role", next.role, sizeof(next.role));
    str(&j, 0, "workerHeadId", next.head, sizeof(next.head));
    next.online = boolean(&j, 0, "online");
    next.worker = boolean(&j, 0, "workerNode") || !strcmp(next.role, "worker");
    next.fetched = true; next.updated = dash_wall();
    if (next.worker) str(&j, 0, "workerDerivedLabel", next.model, sizeof(next.model));
    int health = field(&j, 0, "health");
    if (health >= 0 && j.tokens[health].type == '[' && j.tokens[health].size)
        str(&j, health + 1, "title", next.health, sizeof(next.health));
    if (!next.online) str(&j, 0, "offlineReason", next.health, sizeof(next.health));
    if (next.online) {
        int gpu = field(&j, rootmetrics, "gpu"), cpu = field(&j, rootmetrics, "cpu");
        int power = field(&j, gpu, "power"), memory = field(&j, rootmetrics, "unifiedMemory");
        next.temp = num(&j, gpu, "temperature"); next.gpu = num(&j, gpu, "usage");
        next.power = num(&j, power, "draw"); next.system_power = num(&j, power, "systemDraw");
        next.cpu = num(&j, cpu, "usage"); next.memory = num(&j, memory, "percentage");
        next.available = num(&j, memory, "available");
        int llm = field(&j, rootmetrics, "llm");
        if (!next.worker && llm >= 0 && j.tokens[llm].type == '[') {
            for (int i = llm + 1; i < j.tokens[llm].end; i = j.tokens[i].end) {
                if (!boolean(&j, i, "available")) continue;
                double rate = num(&j, i, "generationTps");
                if (!next.llm) {
                    next.rate = next.prefill = next.running = next.waiting = 0;
                    str(&j, i, "modelId", next.model, sizeof(next.model));
                    next.context = num(&j, i, "contextLength");
                } else snprintf(next.model, sizeof(next.model), "Multiple model endpoints");
                next.llm = true;
                next.rate = isfinite(next.rate) && isfinite(rate) && rate >= 0 ? next.rate + rate : NAN;
                double prefill = num(&j, i, "prefillTps"), running = num(&j, i, "requestsRunning"), waiting = num(&j, i, "requestsWaiting");
                next.prefill = isfinite(next.prefill) && isfinite(prefill) ? next.prefill + prefill : NAN;
                next.running = isfinite(next.running) && isfinite(running) ? next.running + running : NAN;
                next.waiting = isfinite(next.waiting) && isfinite(waiting) ? next.waiting + waiting : NAN;
            }
        }
    }
    *s = next;
    free(j.tokens); return 0;
}
int dash_parse_history(const char *json, Spark *s) {
    Json j;
    if (parse(&j, json) < 0) return -1;
    int a[4] = {field(&j, 0, "t"), field(&j, 0, "c"), field(&j, 0, "p"), field(&j, 0, "u")};
    for (int k = 0; k < 4; k++) if (a[k] < 0 || j.tokens[a[k]].type != '[' || j.tokens[a[k]].size != j.tokens[a[0]].size) {
        free(j.tokens); return -1;
    }
    int size = j.tokens[a[0]].size;
    double last = size ? number(&j, j.tokens[a[0]].end - 1) : 0;
    if (!isfinite(last) || last < 0) { free(j.tokens); return -1; }
    GpuPoint points[DASH_POINTS];
    double sums[DASH_POINTS][3] = {{0}};
    int counts[DASH_POINTS][3] = {{0}};
    bool used[DASH_POINTS] = {false};
    for (int k = 0; k < DASH_POINTS; k++) points[k] = (GpuPoint){.temp=NAN, .power=NAN, .usage=NAN};
    int it[4]; for (int k = 0; k < 4; k++) it[k] = a[k] + 1;
    double previous = -1;
    for (int i = 0; i < size; i++) {
        double t = number(&j, it[0]);
        if (!isfinite(t) || t < 0 || t <= previous) { free(j.tokens); return -1; }
        previous = t;
        if (t >= last - DASH_WINDOW_MS) {
            int bucket = (int)((t - (last - DASH_WINDOW_MS)) * DASH_POINTS / DASH_WINDOW_MS);
            if (bucket >= DASH_POINTS) bucket = DASH_POINTS - 1;
            points[bucket].t = (int64_t)t; used[bucket] = true;
            for (int k = 0; k < 3; k++) {
                double d = number(&j, it[k + 1]);
                if (isfinite(d) && d >= 0) { sums[bucket][k] += d; counts[bucket][k]++; }
            }
        }
        for (int k = 0; k < 4; k++) it[k] = j.tokens[it[k]].end;
    }
    s->points = 0;
    for (int b = 0; b < DASH_POINTS; b++) if (used[b]) {
        points[b].temp = counts[b][0] ? sums[b][0] / counts[b][0] : NAN;
        points[b].power = counts[b][1] ? sums[b][1] / counts[b][1] : NAN;
        points[b].usage = counts[b][2] ? sums[b][2] / counts[b][2] : NAN;
        s->history[s->points++] = points[b];
    }
    s->history_ok = true;
    free(j.tokens); return 0;
}
int64_t dash_monotonic(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
int64_t dash_wall(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
/* The server retains UTC calendar days and hourly counters, not individual
 * token timestamps. Only the boundary hour of the rolling window is estimated. */
static int64_t bucket_time(const char *key, bool hour) {
    if (strlen(key) != (hour ? 13U : 10U)) return -1;
    struct tm tm = {0};
    char *end = strptime(key, hour ? "%Y-%m-%dT%H" : "%Y-%m-%d", &tm);
    if (!end || *end || tm.tm_year < 100 || tm.tm_year > 199) return -1;
    time_t t = timegm(&tm);
    struct tm check; char normalized[24];
    gmtime_r(&t, &check);
    strftime(normalized, sizeof(normalized), hour ? "%Y-%m-%dT%H" : "%Y-%m-%d", &check);
    return strcmp(key, normalized) ? -1 : (int64_t)t * 1000;
}
int dash_parse_tokens(const char *json, TokenTotals *totals) {
    Json j;
    if (parse(&j, json) < 0) return -1;
    TokenTotals next = {0};
    double generated = num(&j, 0, "generatedAt");
    int days = field(&j, 0, "day"), hours = field(&j, 0, "hour");
    if (!isfinite(generated) || generated < 946684800000.0 || generated >= 4102444800000.0 ||
        days < 0 || hours < 0 || j.tokens[days].type != '[' || j.tokens[hours].type != '[') goto invalid;
    next.updated = (int64_t)generated;
    int64_t midnight = next.updated / 86400000 * 86400000;
    int64_t since = next.updated - 86400000;
    for (int which = 0; which < 2; which++) {
        int list = which ? hours : days;
        for (int i = list + 1; i < j.tokens[list].end; i = j.tokens[i].end) {
            char key[24]; str(&j, i, "t", key, sizeof(key));
            int64_t t = bucket_time(key, which != 0);
            double count = num(&j, i, "completionTokens");
            if (t < 0 || t > next.updated || !isfinite(count) || count < 0 || floor(count) != count) goto invalid;
            if (!which) {
                if (t == midnight) next.today += count;
                if (t >= midnight - 6 * 86400000LL) next.days7 += count;
            } else if (t + 3600000 > since) {
                double weight = t < since ? (double)(t + 3600000 - since) / 3600000.0 : 1.0;
                next.hours24 += count * weight;
            }
            if (next.today >= 1e15 || next.days7 >= 1e15 || next.hours24 >= 1e15) goto invalid;
        }
    }
    char first[24]; str(&j, 0, "firstDay", first, sizeof(first));
    int64_t first_day = bucket_time(first, false);
    str(&j, 0, "firstHour", first, sizeof(first));
    int64_t first_hour = bucket_time(first, true);
    next.partial_day = first_day < 0 || first_day > midnight - 6 * 86400000LL;
    next.partial_hours = first_hour < 0 || first_hour > since / 3600000 * 3600000;
    next.fetched = true; *totals = next;
    free(j.tokens); return 0;
invalid:
    free(j.tokens); return -1;
}
typedef struct { char *body; size_t bytes; } Body;
static size_t receive(char *ptr, size_t size, size_t nmemb, void *userdata) {
    Body *b = userdata;
    if (size && nmemb > MAX_JSON / size) return 0;
    size_t n = size * nmemb;
    if (n > MAX_JSON - b->bytes || memchr(ptr, 0, n)) return 0;
    memcpy(b->body + b->bytes, ptr, n); b->bytes += n; b->body[b->bytes] = 0;
    return n;
}
static int get(CURL *curl, const char *url, char *body, int64_t deadline, char *error, size_t error_size) {
    int64_t left = deadline - dash_monotonic();
    if (left <= 0) { snprintf(error, error_size, "Dashboard request timed out"); return -1; }
    Body b = {.body=body}; body[0] = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)(left < 5000 ? left : 5000));
    CURLcode code = curl_easy_perform(curl);
    long status = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (code != CURLE_OK || status != 200) {
        if (code) snprintf(error, error_size, "Connection: %s", curl_easy_strerror(code));
        else snprintf(error, error_size, "Dashboard HTTP %ld", status);
        return -1;
    }
    return 0;
}
static bool valid_base(const char *base) {
    if (strncmp(base, "http://", 7) && strncmp(base, "https://", 8)) return false;
    const char *host = strstr(base, "://") + 3;
    if (!*host || strlen(base) > 200 || strpbrk(host, "\r\n\t ?#@")) return false;
    return true;
}
int dash_config(const char *path, char *base, size_t size) {
    snprintf(base, size, "%s", DASH_BASE);
    FILE *f = fopen(path, "r");
    if (!f) return errno == ENOENT ? 0 : -1;
    char line[256];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f); line[strcspn(line, "\r\n")] = 0;
    size_t len = strlen(line);
    while (len && line[len - 1] == '/') line[--len] = 0;
    if (!valid_base(line) || len >= size) return -1;
    memcpy(base, line, len + 1); return 0;
}
static void fetch_mode(Fleet *fleet, const char *base, bool history) {
    memset(fleet, 0, sizeof(*fleet));
    if (!valid_base(base)) { snprintf(fleet->error, sizeof(fleet->error), "Invalid dashboard address"); return; }
    CURL *curl = NULL; char *body = NULL;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK || !(curl = curl_easy_init()) || !(body = malloc(MAX_JSON + 1))) {
        snprintf(fleet->error, sizeof(fleet->error), "Cannot allocate dashboard request"); goto cleanup;
    }
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L); curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, DASH_DIR "/cacert.pem");
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "KoboSparkDash/1.0");
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    char url[512], error[128] = "";
    int64_t deadline = dash_monotonic() + 22000;
    snprintf(url, sizeof(url), "%s/api/sparks", base);
    if (get(curl, url, body, deadline, fleet->error, sizeof(fleet->error)) < 0) goto cleanup;
    if (dash_parse_sparks(body, fleet) < 0) { snprintf(fleet->error, sizeof(fleet->error), "Invalid unit list from dashboard"); goto cleanup; }
    for (int i = 0; i < fleet->count; i++) {
        Spark *s = &fleet->units[i]; char *id = curl_easy_escape(curl, s->id, 0);
        if (!id) continue;
        snprintf(url, sizeof(url), "%s/api/sparks/%s/metrics", base, id);
        if (get(curl, url, body, deadline, error, sizeof(error)) == 0) {
            if (dash_parse_metrics(body, s) < 0) snprintf(error, sizeof(error), "Invalid metrics for %.60s", s->name);
        }
        if (s->fetched && history) {
            snprintf(url, sizeof(url), "%s/api/sparks/%s/gpu-history?windowMs=1800000", base, id);
            if (get(curl, url, body, deadline, error, sizeof(error)) == 0 && dash_parse_history(body, s) < 0)
                snprintf(error, sizeof(error), "Invalid history for %.60s", s->name);
        }
        if (error[0]) snprintf(fleet->error, sizeof(fleet->error), "%s", error);
        error[0] = 0; curl_free(id);
    }
    if (history) {
        snprintf(url, sizeof(url), "%s/api/llm-token-totals/history", base);
        if (get(curl, url, body, deadline, error, sizeof(error)) < 0)
            snprintf(fleet->tokens.error, sizeof(fleet->tokens.error), "Token history: %.70s", error);
        else if (dash_parse_tokens(body, &fleet->tokens) < 0)
            snprintf(fleet->tokens.error, sizeof(fleet->tokens.error), "Invalid token history from server");
    }
cleanup:
    free(body); if (curl) curl_easy_cleanup(curl); curl_global_cleanup();
}
void dash_fetch(Fleet *fleet, const char *base) { fetch_mode(fleet, base, true); }
void dash_cancel(DashJob *job) {
    if (job->pid > 0) {
        kill(job->pid, SIGKILL);
        while (waitpid(job->pid, NULL, 0) < 0 && errno == EINTR) {}
    }
    if (job->fd >= 0) close(job->fd);
    job->pid = 0; job->fd = -1;
}
int dash_start_mode(DashJob *job, const char *base, bool history) {
    if (job->pid > 0) return -1;
    int p[2]; if (pipe(p) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (!pid) {
        signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGHUP, SIG_DFL);
        close(p[0]);
        long maxfd = sysconf(_SC_OPEN_MAX);
        for (int fd = 3; fd < maxfd; fd++) if (fd != p[1]) close(fd);
        Fleet fleet; fetch_mode(&fleet, base, history);
        size_t sent = 0;
        while (sent < sizeof(fleet)) {
            ssize_t n = write(p[1], (char *)&fleet + sent, sizeof(fleet) - sent);
            if (n > 0) sent += (size_t)n;
            else if (n < 0 && errno == EINTR) continue;
            else break;
        }
        close(p[1]); _exit(sent == sizeof(fleet) ? 0 : 1);
    }
    close(p[1]); fcntl(p[0], F_SETFL, O_NONBLOCK);
    job->fd = p[0]; job->pid = pid; job->started = dash_monotonic(); job->received = 0;
    memset(&job->result, 0, sizeof(job->result)); return 0;
}
int dash_start(DashJob *job, const char *base) { return dash_start_mode(job, base, true); }
bool dash_poll(DashJob *job) {
    if (job->pid <= 0) return false;
    while (job->received < sizeof(job->result)) {
        ssize_t n = read(job->fd, (char *)&job->result + job->received, sizeof(job->result) - job->received);
        if (n > 0) job->received += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            memset(&job->result, 0, sizeof(job->result));
            snprintf(job->result.error, sizeof(job->result.error), "Dashboard worker stopped");
            dash_cancel(job); return true;
        } else break;
    }
    if (job->received == sizeof(job->result)) { dash_cancel(job); return true; }
    if (dash_monotonic() - job->started > 25000) {
        dash_cancel(job); memset(&job->result, 0, sizeof(job->result));
        snprintf(job->result.error, sizeof(job->result.error), "Dashboard request timed out"); return true;
    }
    return false;
}
void dash_merge(Fleet *current, const Fleet *incoming) {
    Fleet next = *incoming;
    if (!incoming->tokens.fetched) {
        next.tokens = current->tokens;
        if (incoming->tokens.error[0]) snprintf(next.tokens.error, sizeof(next.tokens.error), "%s", incoming->tokens.error);
    }
    if (!incoming->count) {
        snprintf(current->error, sizeof(current->error), "%s", incoming->error);
        for (int i = 0; i < current->count; i++) current->units[i].fetched = false;
        return;
    }
    for (int i = 0; i < next.count; i++) {
        Spark *s = &next.units[i];
        for (int k = 0; k < current->count; k++) if (!strcmp(s->id, current->units[k].id)) {
            if (!s->fetched) { *s = current->units[k]; s->fetched = false; }
            else if (!s->history_ok) {
                s->points = current->units[k].points;
                memcpy(s->history, current->units[k].history, sizeof(s->history));
            }
            break;
        }
    }
    *current = next;
}
void dash_sample(RateHistory *history, const Spark *s, int64_t now) {
    if (strcmp(history->id, s->id)) { memset(history, 0, sizeof(*history)); snprintf(history->id, sizeof(history->id), "%s", s->id); }
    while (history->count && history->points[0].t < now - DASH_WINDOW_MS) {
        memmove(history->points, history->points + 1, (size_t)(--history->count) * sizeof(RatePoint));
    }
    if (history->count && now - history->points[history->count - 1].t < 15000) return;
    if (history->count == DASH_POINTS) { memmove(history->points, history->points + 1, (DASH_POINTS - 1) * sizeof(RatePoint)); history->count--; }
    history->points[history->count++] = (RatePoint){.t=now, .rate=s->fetched && s->online && s->llm ? s->rate : NAN};
}
