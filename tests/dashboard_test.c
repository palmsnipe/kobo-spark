// SPDX-License-Identifier: GPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "dashboard_data.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char *read_file(const char *path) {
    FILE *f=fopen(path,"rb"); assert(f);
    assert(!fseek(f,0,SEEK_END)); long len=ftell(f); assert(len>=0 && len<256*1024);
    rewind(f); char *s=calloc((size_t)len+1,1); assert(s);
    assert(fread(s,1,(size_t)len,f)==(size_t)len); fclose(f); return s;
}
int main(int argc,char **argv) {
    Fleet fleet={0};
    assert(!dash_parse_sparks("{\"sparks\":[{\"id\":\"a\",\"name\":\"Head\"},{\"id\":\"b\",\"name\":\"Worker\"}]}",&fleet));
    assert(fleet.count==2 && !strcmp(fleet.units[0].name,"Head"));
    assert(dash_parse_sparks("{\"sparks\":[{\"id\":\"a\"},{\"id\":\"a\"}]}",&fleet)<0);
    assert(dash_parse_sparks("{\"sparks\":[{\"id\":\"a\"}],}",&fleet)<0);
    assert(dash_parse_sparks("{\"sparks\":null}",&fleet)<0);
    assert(dash_parse_sparks("[{\"id\":\"\\uZZZZ\"}]",&fleet)<0);
    assert(!dash_parse_sparks("[{\"id\":\"a\"},{\"id\":\"b\"}]",&fleet));
    const char *head="{\"id\":\"a\",\"online\":true,\"role\":\"head\",\"metrics\":{\"gpu\":{\"temperature\":43,\"power\":{\"draw\":10.5}},\"unifiedMemory\":{\"available\":1024},\"llm\":[{\"available\":true,\"modelId\":\"Test model\",\"generationTps\":0}]}}";
    assert(!dash_parse_metrics(head,&fleet.units[0]));
    Spark *s=&fleet.units[0];
    assert(s->llm && s->rate==0 && s->temp==43 && s->power==10.5 && isnan(s->gpu));
    assert(dash_parse_metrics("{\"id\":\"wrong\",\"online\":true,\"metrics\":{}}",s)<0);
    assert(dash_parse_metrics("{\"id\":\"a\",\"metrics\":{}}",s)<0);
    assert(dash_parse_metrics("{\"id\":\"a\",\"online\":true,\"metrics\":{\"gpu\":{\"usage\":01}}}",s)<0);
    assert(!dash_parse_history("{\"t\":[1000,2000,3000],\"c\":[43,null,45],\"p\":[10,11,12],\"u\":[0,1,2]}",s));
    assert(s->points>=1 && s->history_ok);
    assert(dash_parse_history("{\"t\":[1000,1000],\"c\":[43,44],\"p\":[10,11],\"u\":[0,1]}",s)<0);
    assert(dash_parse_history("{\"t\":[1],\"c\":[43],\"p\":[],\"u\":[0]}",s)<0);
    assert(dash_parse_history("{\"t\":[null],\"c\":[43],\"p\":[11],\"u\":[0]}",s)<0);
    Fleet old=fleet, failed={0}; snprintf(failed.error,sizeof(failed.error),"No Wi-Fi");
    dash_merge(&fleet,&failed); assert(!fleet.units[0].fetched && fleet.units[0].temp==43 && fleet.error[0]);
    assert(!dash_parse_metrics("{\"id\":\"b\",\"online\":true,\"role\":\"worker\",\"workerHeadId\":\"a\",\"metrics\":{\"llm\":[{\"available\":true,\"generationTps\":42}]}}",&fleet.units[1]));
    assert(fleet.units[1].worker && !fleet.units[1].llm && isnan(fleet.units[1].rate));
    RateHistory rate={0}; dash_sample(&rate,&old.units[0],100000); assert(rate.count==1 && rate.points[0].rate==0);
    dash_sample(&rate,&fleet.units[0],115000); assert(rate.count==2 && isnan(rate.points[1].rate));
    dash_sample(&rate,&old.units[0],115001); assert(rate.count==2);
    dash_sample(&rate,&old.units[0],2000000); assert(rate.count==1);
    assert(!dash_parse_metrics("{\"id\":\"a\",\"online\":false,\"metrics\":null,\"offlineReason\":\"Disconnected\"}",s));
    assert(!s->online && s->fetched && isnan(s->rate) && !strcmp(s->health,"Disconnected"));
    assert(!dash_parse_metrics("{\"id\":\"a\",\"online\":true,\"metrics\":{\"llm\":[{\"available\":true,\"generationTps\":null}]}}",s));
    assert(s->llm && isnan(s->rate));
    /* 12:30 UTC: the oldest hour overlaps the 24-hour window by one half.
     * Sum multiple models/ports, exclude prompt tokens and old buckets. */
    TokenTotals totals={0};
    const char *tokens="{\"generatedAt\":1791808200000,\"firstDay\":\"2026-10-01\",\"firstHour\":\"2026-10-10T00\","
        "\"day\":[{\"t\":\"2026-10-12\",\"completionTokens\":100,\"promptTokens\":999999},"
        "{\"t\":\"2026-10-12\",\"completionTokens\":50},{\"t\":\"2026-10-11\",\"completionTokens\":200},"
        "{\"t\":\"2026-10-06\",\"completionTokens\":300},{\"t\":\"2026-10-05\",\"completionTokens\":900}],"
        "\"hour\":[{\"t\":\"2026-10-11T12\",\"completionTokens\":80},{\"t\":\"2026-10-11T13\",\"completionTokens\":20},"
        "{\"t\":\"2026-10-12T12\",\"completionTokens\":40},{\"t\":\"2026-10-11T11\",\"completionTokens\":500}]}";
    assert(!dash_parse_tokens(tokens,&totals));
    assert(totals.fetched && totals.today==150 && totals.days7==650 && totals.hours24==100);
    assert(!totals.partial_day && !totals.partial_hours);
    TokenTotals saved=totals;
    assert(dash_parse_tokens("{\"generatedAt\":1791808200000,\"day\":[{\"t\":\"2026-02-31\",\"completionTokens\":1}],\"hour\":[]}",&totals)<0);
    assert(totals.today==saved.today);
    assert(dash_parse_tokens("{\"generatedAt\":1791808200000,\"day\":[],\"hour\":[{\"t\":\"2026-10-12T12\",\"completionTokens\":-2}]}",&totals)<0);
    assert(dash_parse_tokens("{\"generatedAt\":1791808200000,\"day\":[],\"hour\":[{\"t\":\"2026-10-12T12\",\"completionTokens\":null}]}",&totals)<0);
    assert(dash_parse_tokens("{\"generatedAt\":1791808200000,\"day\":[],\"hour\":[{\"t\":\"2026-10-13T12\",\"completionTokens\":1}]}",&totals)<0);
    assert(dash_parse_tokens("{}",&totals)<0);
    assert(!dash_parse_tokens("{\"generatedAt\":1791808200000,\"day\":[],\"hour\":[],\"firstDay\":null,\"firstHour\":null}",&totals));
    assert(totals.today==0 && totals.days7==0 && totals.hours24==0 && totals.partial_hours);
    old.tokens=saved;
    Fleet fast=old; memset(&fast.tokens,0,sizeof(fast.tokens));
    dash_merge(&old,&fast); assert(old.tokens.fetched && old.tokens.today==150);
    snprintf(fast.tokens.error,sizeof(fast.tokens.error),"HTTP 404");
    dash_merge(&old,&fast); assert(old.tokens.today==150 && old.tokens.error[0]);
    if (argc==2) {
        const char *names[]={"sparks.json","metrics-0.json","metrics-1.json","history-spark1.json","history-spark2.json"};
        for (int i=0;i<5;i++) {
            char path[512]; snprintf(path,sizeof(path),"%s/%s",argv[1],names[i]); char *json=read_file(path);
            int r=i==0?dash_parse_sparks(json,&fleet):i<3?dash_parse_metrics(json,&fleet.units[i-1]):dash_parse_history(json,&fleet.units[i-3]);
            assert(r==0); free(json);
        }
        assert(fleet.count==2 && fleet.units[0].llm && fleet.units[1].worker && fleet.units[0].points>100);
        puts("Actual sparkDash fixtures: both Sparks and full 30-minute history parsed");
    }
    if (argc==3 && !strcmp(argv[1],"--live")) {
        DashJob job={.fd=-1}; assert(!dash_start(&job,argv[2])); assert(dash_start(&job,argv[2])<0);
        int64_t start=dash_monotonic();
        while (!dash_poll(&job) && dash_monotonic()-start<26000) {struct timespec t={.tv_nsec=10000000}; nanosleep(&t,NULL);}
        assert(!job.pid && job.fd==-1 && job.result.count>0 && !job.result.error[0]);
        for (int i=0;i<job.result.count;i++) assert(job.result.units[i].fetched && job.result.units[i].history_ok);
        assert(job.result.tokens.fetched || job.result.tokens.error[0]);
        Fleet full=job.result;
        assert(!dash_start_mode(&job,argv[2],false));
        start=dash_monotonic();
        while (!dash_poll(&job) && dash_monotonic()-start<26000) {struct timespec t={.tv_nsec=10000000}; nanosleep(&t,NULL);}
        assert(!job.pid && !job.result.units[0].history_ok && !job.result.tokens.fetched);
        dash_merge(&full,&job.result);
        assert(full.units[0].points>0 && full.tokens.fetched);
        assert(!dash_start(&job,argv[2])); dash_cancel(&job); assert(!job.pid && job.fd==-1);
        puts("Live API worker: complete pipe transfer, duplicate prevention and cancellation passed");
    }
    puts("Dashboard tests passed (nulls, offline, stale retention, worker, history, malformed JSON)");
    return 0;
}
