// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "fbink.h"
#include "dashboard_data.h"
#include <math.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define MAX_INPUTS 32
typedef struct {
    int fd, slot;
    int raw_x, raw_y, xmin, xmax, ymin, ymax;
    bool btn_touch, active, dropped, mt_axes, have_x, have_y;
    int64_t started;
} Input;
static FBInkState view;
static volatile sig_atomic_t interrupted;
static void on_signal(int sig) { (void)sig; interrupted = 1; }
static int64_t milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static bool bit(const unsigned char *data, unsigned code) {
    return (data[code / 8] & (1U << (code % 8))) != 0;
}
static int find_inputs(Input *inputs, bool grab) {
    int count = 0;
    for (int i = 0; i < MAX_INPUTS; i++) {
        char path[64], name[128] = "";
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned char keys[(KEY_MAX + 8) / 8] = {0};
        unsigned char axes[(ABS_MAX + 8) / 8] = {0};
        (void)ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
        (void)ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(axes)), axes);
        bool touch = bit(keys, BTN_TOUCH) || bit(axes, ABS_MT_TRACKING_ID);
        bool buttons = bit(keys, KEY_POWER) || bit(keys, KEY_F23) ||
                       bit(keys, KEY_F24) || bit(keys, KEY_PAGEUP) || bit(keys, KEY_PAGEDOWN);
        if (!touch && !buttons) { close(fd); continue; }
        if (grab && ioctl(fd, EVIOCGRAB, 1) < 0) {
            fprintf(stderr, "Cannot claim %s: %s\n", path, strerror(errno));
            close(fd); continue;
        }
        (void)ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        Input input = {.fd = fd, .btn_touch = bit(keys, BTN_TOUCH),
                       .mt_axes = bit(axes, ABS_MT_POSITION_X)};
        if (touch) {
            struct input_absinfo x = {0}, y = {0};
            (void)ioctl(fd, EVIOCGABS(input.mt_axes ? ABS_MT_POSITION_X : ABS_X), &x);
            (void)ioctl(fd, EVIOCGABS(input.mt_axes ? ABS_MT_POSITION_Y : ABS_Y), &y);
            input.xmin = x.minimum; input.xmax = x.maximum;
            input.ymin = y.minimum; input.ymax = y.maximum;
            printf("touch ranges: x=%d..%d y=%d..%d\n", x.minimum, x.maximum, y.minimum, y.maximum);
        }
        inputs[count++] = input;
        printf("input: %s (%s), touch=%d buttons=%d\n", path, name, touch, buttons);
    }
    return count;
}
static void capture(int fd, const FBInkState *state, const char *path) {
    FBInkDump dump = {0};
    if (fbink_dump(fd, &dump) < 0) return;
    FILE *file = fopen(path, "wb");
    if (file) {
        fprintf(file, "%u %u %u %zu %u %u\n", dump.area.width, dump.area.height,
                dump.bpp, dump.stride, state->pixel_format, dump.rota);
        if (fwrite(dump.data, 1, dump.size, file) != dump.size)
            fprintf(stderr, "Screenshot write failed\n");
        fclose(file);
    }
    (void)fbink_free_dump_data(&dump);
}
static Fleet fleet;
static RateHistory rates[DASH_UNITS];
static DashJob job = {.fd=-1};
static int page, metric;
#define TOTALS_PAGE (DASH_UNITS + 1)
static const int intervals[] = {1, 2, 5, 10, 15, 30};
static int refresh_index = 2;
static bool redraw, quit, request_refresh, rates_loaded;
static FBInkConfig paint = {.no_refresh=true, .is_quiet=true, .is_bgless=true};
typedef struct { uint8_t r, g, b; } Color;
static const Color BLUE={0,72,160}, ORANGE={172,68,12}, PURPLE={116,38,166};
static const Color GREEN={20,116,60}, RED={180,30,30};
static const Color BLUE_TINT={198,218,248}, ORANGE_TINT={250,216,184}, PURPLE_TINT={226,204,246};
static Color metric_color(int which) { return which==0?BLUE:which==1?ORANGE:PURPLE; }
static void box(int fd, int x, int y, int w, int h, unsigned char gray) {
    FBInkRect r = {.left=(unsigned short)x, .top=(unsigned short)y, .width=(unsigned short)w, .height=(unsigned short)h};
    if (fbink_fill_rect_gray(fd, &paint, &r, false, gray) < 0) quit = true;
}
static void color_box(int fd, int x, int y, int w, int h, Color color) {
    FBInkRect r={.left=(unsigned short)x,.top=(unsigned short)y,.width=(unsigned short)w,.height=(unsigned short)h};
    if (fbink_fill_rect_rgba(fd,&paint,&r,false,color.r,color.g,color.b,255)<0) quit=true;
}
static void label(int fd, int x, int y, int width, int height, int size, bool bold, const char *text) {
    if (!*text) return;
    FBInkOTConfig ot = {.margins={.top=(short)y, .bottom=(short)(1680-y-height),
        .left=(short)x, .right=(short)(1264-x-width)}, .style=bold ? FNT_BOLD : FNT_REGULAR,
        .size_px=(unsigned short)size};
    if (fbink_print_ot(fd, text, &ot, &paint, NULL) < 0) quit = true;
}
static void color_label(int fd,int x,int y,int width,int height,int size,bool bold,const char *text,Color color) {
    /* Do not quantize: FBInk's grayscale palette would discard RGB here. */
    if (fbink_set_fg_pen_rgba(color.r,color.g,color.b,255,false,false)<0) quit=true;
    label(fd,x,y,width,height,size,bold,text);
    if (fbink_set_fg_pen_gray(0,false,false)<0) quit=true;
}
static void color_button(int fd,int x,int y,int width,const char *name,bool selected,Color accent,Color tint) {
    if (selected) color_box(fd,x,y,width,84,tint);
    else box(fd,x,y,width,84,245);
    color_box(fd,x,y+78,width,6,accent);
    label(fd, x+18, y+22, width-36, 54, 32, selected, name);
}
static void button(int fd,int x,int y,int width,const char *name,bool selected) {
    color_button(fd,x,y,width,name,selected,BLUE,BLUE_TINT);
}
static void fmt(char *buffer, size_t size, double v, const char *unit, int decimals) {
    if (!isfinite(v)) snprintf(buffer, size, "-- %s", unit);
    else snprintf(buffer, size, "%.*f %s", decimals, v, unit);
}
static void line(int fd, int x0, int y0, int x1, int y1, Color color) {
    int dx=abs(x1-x0), sx=x0<x1?1:-1, dy=-abs(y1-y0), sy=y0<y1?1:-1, err=dx+dy;
    for (;;) {
        color_box(fd,x0,y0,6,6,color);
        if (x0==x1 && y0==y1) break;
        int e=2*err;
        if (e>=dy) {err+=dy; x0+=sx;}
        if (e<=dx) {err+=dx; y0+=sy;}
    }
}
static RateHistory *rate_history(const Spark *s) {
    for (int i=0;i<DASH_UNITS;i++) if (!strcmp(rates[i].id,s->id)) return &rates[i];
    return NULL;
}
static void sample_rates(void) {
    bool used[DASH_UNITS]={false};
    for (int i=0;i<fleet.count;i++) {
        int slot=-1;
        for (int k=0;k<DASH_UNITS;k++) if (!strcmp(rates[k].id,fleet.units[i].id)) slot=k;
        if (slot<0) for (int k=0;k<DASH_UNITS;k++) {
            bool exists=false;
            for (int n=0;n<fleet.count;n++) if (!strcmp(rates[k].id,fleet.units[n].id)) exists=true;
            if (!used[k] && !exists) {slot=k; break;}
        }
        if (slot>=0) { dash_sample(&rates[slot],&fleet.units[i],dash_wall()); used[slot]=true; }
    }
}
/* The rolling token history is local, never reconstructed from cumulative token
 * totals. Store only valid numeric samples and retain timestamp gaps on reload. */
static void save_rates(void) {
    FILE *f=fopen(DASH_DIR "/rates.csv.tmp","w");
    if (!f) return;
    for (int k=0;k<DASH_UNITS;k++) for (int i=0;i<rates[k].count;i++) {
        RatePoint p=rates[k].points[i];
        if (isfinite(p.rate) && !strpbrk(rates[k].id,",\r\n")) fprintf(f,"%s,%lld,%.6f\n",rates[k].id,(long long)p.t,p.rate);
    }
    bool good=!ferror(f);
    if (fclose(f)) good=false;
    if (good) rename(DASH_DIR "/rates.csv.tmp",DASH_DIR "/rates.csv");
}
static void load_rates(void) {
    FILE *f=fopen(DASH_DIR "/rates.csv","r");
    if (!f) return;
    char linebuf[256], id[64]; long long t; double rate;
    int64_t now=dash_wall();
    for (int lines=0;lines<DASH_UNITS*DASH_POINTS && fgets(linebuf,sizeof(linebuf),f);lines++) {
        if (sscanf(linebuf,"%63[^,],%lld,%lf",id,&t,&rate)!=3 || !isfinite(rate) || rate<0 ||
            rate>1e9 || t>now || t<now-DASH_WINDOW_MS) continue;
        int k;
        for (k=0;k<DASH_UNITS;k++) if (!rates[k].id[0] || !strcmp(rates[k].id,id)) break;
        if (k==DASH_UNITS || rates[k].count==DASH_POINTS) continue;
        if (rates[k].count && rates[k].points[rates[k].count-1].t>=t) continue;
        snprintf(rates[k].id,sizeof(rates[k].id),"%s",id);
        rates[k].points[rates[k].count++]=(RatePoint){.t=t,.rate=rate};
    }
    fclose(f);
}
static void load_interval(void) {
    FILE *f = fopen(DASH_DIR "/refresh-seconds", "r");
    int seconds;
    if (f) {
        if (fscanf(f, "%d", &seconds) == 1)
            for (unsigned i=0; i<sizeof(intervals)/sizeof(intervals[0]); i++)
                if (intervals[i] == seconds) refresh_index = (int)i;
        fclose(f);
    }
}
static void save_interval(void) {
    FILE *f = fopen(DASH_DIR "/refresh-seconds.tmp", "w");
    if (f) {
        bool good = fprintf(f, "%d\n", intervals[refresh_index]) > 0;
        if (fclose(f)) good = false;
        if (good) rename(DASH_DIR "/refresh-seconds.tmp", DASH_DIR "/refresh-seconds");
    }
}
static void graph(int fd, const Spark *s, int which, int x, int y, int w, int h, bool mini) {
    int64_t times[DASH_POINTS]; double values[DASH_POINTS]; int count=0;
    const char *unit=which==0?"tok/s":which==1?"C":"W";
    if (which==0) {
        RateHistory *history=rate_history(s);
        if (history) for (int i=0;i<history->count;i++) {
            times[count]=history->points[i].t; values[count++]=history->points[i].rate;
        }
    } else for (int i=0;i<s->points;i++) {
        times[count]=s->history[i].t;
        values[count++]=which==1?s->history[i].temp:s->history[i].power;
    }
    double low=INFINITY, high=-INFINITY; int valid=0;
    int64_t end=dash_wall(), start=end-DASH_WINDOW_MS;
    for (int i=0;i<count;i++) if (times[i]>=start && times[i]<=end && isfinite(values[i])) {
        if (values[i]<low) low=values[i];
        if (values[i]>high) high=values[i];
        valid++;
    }
    if (!valid) {
        box(fd,x,y+h-3,w,2,175);
        label(fd,x+12,y+h/2-24,w-24,65,mini?26:36,false,which==0 && s->worker?
            "Throughput is reported by the head node":which==0?"Recording token history on this Kobo...":"No history available yet");
        return;
    }
    if (which==1) {low=floor(low)-2; high=ceil(high)+2;}
    else {low=0; high=fmax(1,ceil(high*1.15));}
    int left=x+(mini?0:88), top=y+24, width=w-(mini?0:88), height=h-62;
    for (int i=0;i<=2;i++) {
        int yy=top+i*height/2;
        box(fd,left,yy,width,1,200);
        if (!mini) {
            char v[32]; snprintf(v,sizeof(v),"%.0f",high-i*(high-low)/2);
            label(fd,x,yy-16,78,44,26,false,v);
        }
    }
    int prevx=0,prevy=0; int64_t prevt=0; bool connected=false;
    for (int i=0;i<count;i++) {
        if (times[i]<start || times[i]>end || !isfinite(values[i])) {connected=false; continue;}
        int xx=left+(int)((times[i]-start)*(width-4)/DASH_WINDOW_MS);
        int yy=top+height-(int)((values[i]-low)*height/(high-low));
        if (yy<top) yy=top;
        if (yy>top+height) yy=top+height;
        if (connected && times[i]-prevt<=45000) line(fd,prevx,prevy,xx,yy,metric_color(which));
        else color_box(fd,xx,yy,7,7,metric_color(which));
        prevx=xx; prevy=yy; prevt=times[i]; connected=true;
    }
    label(fd,left,y+h-32,width/2,35,mini?22:26,false,"30 min ago");
    label(fd,left+width-110,y+h-32,110,35,mini?22:26,false,"now");
    if (!mini) label(fd,x,y-7,78,38,24,false,unit);
}
static void footer(int fd, bool overview) {
    button(fd,48,1490,268,overview?"History":"Overview",false);
    button(fd,348,1490,268,"Token totals",page==TOTALS_PAGE);
    button(fd,648,1490,268,job.pid?"Updating...":"Refresh",false);
    color_button(fd,948,1490,268,"Exit",false,RED,BLUE_TINT);
    label(fd,48,1600,1168,50,25,false,"Page buttons: change Spark   /   Power or hold touch: exit");
}
static void card(int fd,const Spark *s,int index) {
    int y=250+index*570;
    color_box(fd,48,y,1168,8,index?PURPLE:BLUE);
    label(fd,64,y+20,700,70,48,true,s->name);
    const char *status=!s->fetched?"STALE":s->online?"ONLINE":"OFFLINE";
    Color state=!s->fetched?ORANGE:s->online?GREEN:RED;
    color_box(fd,959,y+42,20,20,state);
    color_label(fd,995,y+32,210,46,28,true,status,state);
    char model[168];
    if (s->worker) snprintf(model,sizeof(model),"Worker for %s  /  %s",s->head,s->model);
    else snprintf(model,sizeof(model),"%s",s->model[0]?s->model:"No model endpoint available");
    label(fd,64,y+95,1130,66,28,false,model);
    if (s->health[0]) color_label(fd,64,y+153,1130,40,23,true,s->health,ORANGE);
    char value[64];
    if (s->worker) snprintf(value,sizeof(value),"Worker"); else fmt(value,sizeof(value),s->rate,"",1);
    color_label(fd,64,y+182,430,108,78,true,value,BLUE);
    label(fd,64,y+293,430,44,27,false,s->worker?"Uses head-node throughput":"tokens / sec");
    fmt(value,sizeof(value),s->temp,"C",0); color_label(fd,520,y+195,320,85,58,true,value,ORANGE);
    label(fd,520,y+293,300,44,27,false,"GPU temperature");
    fmt(value,sizeof(value),s->power,"W",1); color_label(fd,895,y+195,305,85,58,true,value,PURPLE);
    label(fd,895,y+293,300,44,27,false,"GPU power");
    label(fd,64,y+360,430,40,25,false,"GPU power / last 30 minutes");
    graph(fd,s,2,64,y+394,1116,148,true);
}
static void draw(int fd, bool flash) {
    if (fbink_cls(fd,&paint,NULL,false)<0) {quit=true; return;}
    label(fd,48,40,1080,94,58,true,page==TOTALS_PAGE?"Generated tokens":page && page<=fleet.count?fleet.units[page-1].name:"Spark dashboard");
    char status[160];
    time_t now=time(NULL); struct tm tm; localtime_r(&now,&tm);
    if (fleet.error[0]) snprintf(status,sizeof(status),"%.112s",fleet.error);
    else snprintf(status,sizeof(status),"%02d:%02d:%02d  /  %s",tm.tm_hour,tm.tm_min,tm.tm_sec,job.pid?"updating":"live");
    label(fd,48,139,830,70,28,false,status);
    char speed[48]; snprintf(speed,sizeof(speed),"Interval: %d s",intervals[refresh_index]);
    button(fd,924,126,292,speed,false);
    if (page==TOTALS_PAGE) {
        TokenTotals *t=&fleet.tokens;
        if (!t->fetched) {
            label(fd,64,320,1130,110,46,true,"Token totals unavailable");
            label(fd,64,470,1130,170,30,false,t->error[0]?t->error:"This sparkDash server needs the token-history endpoint.");
        } else {
            const char *names[]={"Today / since 00:00 UTC", "Past 24 hours / approximate", "7 days / today + 6 UTC dates"};
            double counts[]={t->today,t->hours24,t->days7};
            for (int i=0;i<3;i++) {
                int y=270+i*325; char count[64];
                Color accent=i==0?BLUE:i==1?GREEN:PURPLE;
                color_box(fd,48,y,1168,8,accent);
                label(fd,64,y+28,1130,60,36,true,names[i]);
                snprintf(count,sizeof(count),i==1?"~ %.0f":"%.0f",counts[i]);
                color_label(fd,64,y+110,1130,130,90,true,count,accent);
                label(fd,64,y+246,1130,45,28,false,"generated output tokens");
            }
            label(fd,64,1270,1130,90,28,false,"All recorded models / server counters continue while the Kobo app is closed. Ledger samples every 15 seconds.");
            long age=(long)((dash_wall()-t->updated)/1000);
            snprintf(status,sizeof(status),"Ledger age: %ld s%s",age<0?0:age,
                t->partial_day || t->partial_hours?" / history may cover only part of the window":"");
            label(fd,64,1378,1130,82,26,true,t->error[0]?t->error:status);
        }
    } else if (!fleet.count) {
        label(fd,64,360,1100,140,46,true,job.pid?"Connecting to sparkDash...":"Dashboard unavailable");
        label(fd,64,540,1100,200,34,false,"Enable Wi-Fi before opening the dashboard. Refresh to retry. The dashboard connects to your sparkDash service.");
    } else if (!page) {
        for (int i=0;i<fleet.count && i<2;i++) card(fd,&fleet.units[i],i);
        label(fd,64,1400,1130,55,27,false,"Tap a Spark for details and history");
    } else {
        if (page>fleet.count) page=fleet.count;
        Spark *s=&fleet.units[page-1];
        color_button(fd,48,222,360,"Tokens / sec",metric==0,BLUE,BLUE_TINT);
        color_button(fd,448,222,366,"GPU temp",metric==1,ORANGE,ORANGE_TINT);
        color_button(fd,848,222,368,"GPU power",metric==2,PURPLE,PURPLE_TINT);
        char v[96], note[168];
        double current=metric==0?s->rate:metric==1?s->temp:s->power;
        fmt(v,sizeof(v),current,metric==0?"tok/s":metric==1?"C":"W",metric==1?0:1);
        if (metric==0 && s->worker) snprintf(v,sizeof(v),"Head: %s",s->head);
        color_label(fd,64,352,1136,134,90,true,v,metric_color(metric));
        if (!s->fetched) {
            long age=(long)((dash_wall()-s->updated)/1000);
            snprintf(note,sizeof(note),"Last reading %ld sec ago / refresh to retry",age<0?0:age);
        } else if (!s->online) snprintf(note,sizeof(note),"Spark offline");
        else snprintf(note,sizeof(note),"%s",metric==0?(s->worker?"Model throughput is measured on the head node":"Generation throughput / history recorded by this Kobo"):
            "Last 30 minutes / averaged into 15-second samples");
        label(fd,64,492,1136,70,28,false,note);
        graph(fd,s,metric,64,585,1136,420,false);
        label(fd,64,1058,1130,84,34,true,s->model[0]?s->model:"No model endpoint available");
        char a[48],b[48];
        fmt(a,sizeof(a),s->gpu,"%",0); fmt(b,sizeof(b),s->cpu,"%",0);
        snprintf(v,sizeof(v),"GPU %s   /   CPU %s",a,b); label(fd,64,1160,1130,60,32,false,v);
        fmt(a,sizeof(a),s->memory,"%",0); fmt(b,sizeof(b),s->available/1024.0,"GB free",1);
        snprintf(v,sizeof(v),"Memory %s   /   %s",a,b); label(fd,64,1230,1130,60,32,false,v);
        fmt(a,sizeof(a),s->system_power,"W",1);
        snprintf(v,sizeof(v),"System power %s",a); label(fd,64,1300,1130,60,32,false,v);
        label(fd,64,1378,1130,88,28,true,s->health[0]?s->health:s->online?"No active health alerts":"Waiting for this Spark");
    }
    footer(fd,page==0);
    FBInkConfig refresh={.is_quiet=true,.is_flashing=flash};
    /* The default AUTO/GC16 waveforms render grayscale even with RGB pixels.
     * Request Kaleido color processing for both clearing and regular updates. */
    if (view.has_color_panel) refresh.wfm_mode=flash?WFM_GCC16:WFM_GLRC16;
    if (fbink_refresh(fd,0,0,0,0,&refresh)<0 || fbink_wait_for_complete(fd,LAST_MARKER)<0) quit=true;
}
static void touch(Input *input,bool active) {
    if (active==input->active) return;
    input->active=active;
    if (active) {input->started=milliseconds(); return;}
    if (input->dropped) {input->dropped=false; return;}
    if (milliseconds()-input->started>=1200) {quit=true; return;}
    if (!input->have_x || !input->have_y || input->xmax<=input->xmin || input->ymax<=input->ymin) return;
    int width=view.view_width,height=view.view_height;
    int xextent=view.touch_swap_axes?height:width,yextent=view.touch_swap_axes?width:height;
    int x=(int)((int64_t)(input->raw_x-input->xmin)*(xextent-1)/(input->xmax-input->xmin));
    int y=(int)((int64_t)(input->raw_y-input->ymin)*(yextent-1)/(input->ymax-input->ymin));
    if (view.touch_swap_axes) {int swap=x; x=y; y=swap;}
    if (view.touch_mirror_x) x=width-1-x;
    if (view.touch_mirror_y) y=height-1-y;
    if (view.rotation_map[view.current_rota]==2) {x=width-1-x; y=height-1-y;}
    printf("dashboard tap: %d,%d\n",x,y);
    if (x<48 || x>=1216) return;
    if (y>=126 && y<=210 && x>=924) {
        refresh_index=(refresh_index+1)%(int)(sizeof(intervals)/sizeof(intervals[0]));
        save_interval(); request_refresh=true;
    } else if (y>=1490 && y<=1574) {
        if (x<316) page=page?0:fleet.count?1:0;
        else if (x>=948) quit=true;
        else if (x>=648 && x<916) request_refresh=true;
        else if (x>=348 && x<616) page=TOTALS_PAGE;
    } else if (!page && y>=250 && y<1390) {
        int index=(y-250)/570;
        if (index<fleet.count) {page=index+1; metric=0;}
    } else if (page && page!=TOTALS_PAGE && y>=222 && y<=306) metric=x<414?0:x<814?1:2;
    else return;
    redraw=true;
}
static void event(Input *input,const struct input_event *e) {
    if (e->type==EV_SYN && e->code==SYN_DROPPED) {input->dropped=true; return;}
    if (e->type==EV_KEY) {
        if (e->code==BTN_TOUCH && input->btn_touch) touch(input,e->value!=0);
        else if (e->code==KEY_POWER && e->value==0) quit=true;
        else if (e->value==1 && fleet.count) {
            if (e->code==KEY_F23 || e->code==KEY_PAGEUP) {page=(page+fleet.count)%(fleet.count+1); redraw=true;}
            else if (e->code==KEY_F24 || e->code==KEY_PAGEDOWN) {page=(page+1)%(fleet.count+1); redraw=true;}
        }
    } else if (e->type==EV_ABS) {
        if (e->code==ABS_MT_SLOT) input->slot=e->value;
        if (!input->slot) {
            if (e->code==(input->mt_axes?ABS_MT_POSITION_X:ABS_X)) {input->raw_x=e->value; input->have_x=true;}
            if (e->code==(input->mt_axes?ABS_MT_POSITION_Y:ABS_Y)) {input->raw_y=e->value; input->have_y=true;}
            if (!input->btn_touch && e->code==ABS_MT_TRACKING_ID) touch(input,e->value>=0);
        }
    }
}
static void heartbeat(void) {
    FILE *f=fopen("/tmp/kobo-spark.lock/heartbeat","w");
    if (f) {fputs("alive\n",f); fclose(f);}
}
static void wake(bool hold) {
    FILE *f=fopen(hold?"/sys/power/wake_lock":"/sys/power/wake_unlock","w");
    if (f) {fputs(hold?"kobo-spark 90000000000\n":"kobo-spark\n",f); fclose(f);}
}
int main(int argc,char **argv) {
    char base[256]; bool probe=false; int run_seconds=0;
    const char *screenshot=NULL,*config=DASH_DIR "/dashboard.conf";
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--version")) {puts("Kobo Spark 1.2"); return 0;}
        else if (!strcmp(argv[i],"--probe-dashboard")) probe=true;
        else if (!strcmp(argv[i],"--config") && i+1<argc) config=argv[++i];
        else if (!strcmp(argv[i],"--capture") && i+1<argc) screenshot=argv[++i];
        else if (!strcmp(argv[i],"--page") && i+1<argc) {page=atoi(argv[++i]); if(page<0 || page>TOTALS_PAGE) return 2;}
        else if (!strcmp(argv[i],"--metric") && i+1<argc) {metric=atoi(argv[++i]); if(metric<0 || metric>2) return 2;}
        else if (!strcmp(argv[i],"--run-seconds") && i+1<argc) {run_seconds=atoi(argv[++i]); if(run_seconds<1 || run_seconds>3600) return 2;}
        else {fprintf(stderr,"Unknown argument: %s\n",argv[i]); return 2;}
    }
    if (dash_config(config,base,sizeof(base))<0) {fputs("Invalid dashboard.conf\n",stderr); return 2;}
    if (probe) {
        dash_fetch(&fleet,base);
        for (int i=0;i<fleet.count;i++) {
            Spark *s=&fleet.units[i];
            printf("%s: fetched=%d online=%d role=%s model=%s tok/s=%.1f temp=%.0fC GPU=%.1fW history=%d\n",
                s->name,s->fetched,s->online,s->role,s->model,s->rate,s->temp,s->power,s->points);
        }
        if (fleet.error[0]) printf("error: %s\n",fleet.error);
        if (fleet.tokens.fetched) printf("Output tokens: today(UTC)=%.0f past24h~=%.0f 7UTCdays=%.0f\n",fleet.tokens.today,fleet.tokens.hours24,fleet.tokens.days7);
        else printf("Token totals: %s\n",fleet.tokens.error);
        return fleet.count && !fleet.error[0]?0:1;
    }
    setvbuf(stdout,NULL,_IOLBF,0);
    signal(SIGINT,on_signal); signal(SIGTERM,on_signal); signal(SIGHUP,on_signal);
    int fd=fbink_open(),count=0,result=1;
    if (fd<0) return 1;
    Input inputs[MAX_INPUTS]={{0}}; struct pollfd pfds[MAX_INPUTS]={{0}};
    if (fbink_init(fd,&paint)<0) goto cleanup;
    fbink_get_state(&paint,&view);
    if (view.device_id!=390 || view.view_width!=1264 || view.view_height!=1680) {fputs("Launch in portrait on Libra Colour\n",stderr); goto cleanup;}
    const char *fontdir=DASH_DIR "/fonts/";
    char path[256];
    snprintf(path,sizeof(path),"%sAtkinsonHyperlegible-Regular.ttf",fontdir);
    if (fbink_add_ot_font(path,FNT_REGULAR)<0) goto cleanup;
    snprintf(path,sizeof(path),"%sAtkinsonHyperlegible-Bold.ttf",fontdir);
    if (fbink_add_ot_font(path,FNT_BOLD)<0) goto cleanup;
    count=find_inputs(inputs,true);
    if (!count) goto cleanup;
    for (int i=0;i<count;i++) pfds[i]=(struct pollfd){.fd=inputs[i].fd,.events=POLLIN};
    load_interval(); load_rates(); rates_loaded=true; wake(true); heartbeat();
    if (dash_start(&job,base)<0) snprintf(fleet.error,sizeof(fleet.error),"Cannot start dashboard request");
    draw(fd,true);
    if (screenshot) capture(fd,&view,screenshot);
    int64_t start=milliseconds(),next=start+intervals[refresh_index]*1000,lastbeat=start,lastwake=start,lastsave=start,lastslow=start,lastflash=start;
    while (!quit && !interrupted && (!run_seconds || milliseconds()-start<run_seconds*1000LL)) {
        int64_t now=milliseconds();
        if (now-lastbeat>=1000) {heartbeat(); lastbeat=now;}
        if (now-lastwake>=30000) {wake(true); lastwake=now;}
        int polled=poll(pfds,count,100);
        if (polled<0 && errno!=EINTR) goto cleanup;
        for (int i=0;i<count;i++) {
            if (pfds[i].revents&(POLLERR|POLLHUP|POLLNVAL)) goto cleanup;
            if (!(pfds[i].revents&POLLIN)) continue;
            struct input_event events[32]; ssize_t bytes;
            while ((bytes=read(inputs[i].fd,events,sizeof(events)))>0) {
                if ((size_t)bytes%sizeof(events[0])) goto cleanup;
                for (size_t n=0;n<(size_t)bytes/sizeof(events[0]);n++) event(&inputs[i],&events[n]);
            }
            if (bytes<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR) goto cleanup;
        }
        if (quit || interrupted) break;
        bool done=dash_poll(&job);
        if (done) {
            dash_merge(&fleet,&job.result); sample_rates(); redraw=true;
            printf("dashboard update: units=%d error=%s\n",fleet.count,fleet.error);
        }
        /* Only a refresh-button tap should request early polling. Page changes
         * also redraw, but should not flood the server. */
        if (!job.pid && (request_refresh || now>=next)) {
            bool slow=request_refresh || now-lastslow>=15000;
            if (dash_start_mode(&job,base,slow)<0) snprintf(fleet.error,sizeof(fleet.error),"Cannot start dashboard request");
            else if (slow) lastslow=now;
            next=now+intervals[refresh_index]*1000;
            request_refresh=false;
        }
        if (now-lastsave>=60000) {save_rates(); lastsave=now;}
        if (redraw && !quit) {
            bool flash=now-lastflash>=300000;
            draw(fd,flash); if (flash) lastflash=now; redraw=false;
            if (screenshot) capture(fd,&view,screenshot);
        }
    }
    result=0;
cleanup:
    /* Release input before any disk/network cleanup. Nickel redraws its own UI;
     * waiting for a flashing restore here could hang while input stayed grabbed. */
    for (int i=0;i<count;i++) {(void)ioctl(inputs[i].fd,EVIOCGRAB,0); close(inputs[i].fd);}
    dash_cancel(&job); if (rates_loaded) save_rates();
    /* The launcher keeps the wake lease through reader recovery. */
    if (access("/tmp/kobo-spark.lock/active",F_OK)<0) wake(false);
    (void)fbink_free_ot_fonts(); (void)fbink_close(fd);
    puts("dashboard exited; returning to reader"); return result;
}
