/* SPDX-License-Identifier: LGPL-2.0-or-later */
#ifndef DTFILE_SYNC_DATES_H
#define DTFILE_SYNC_DATES_H
#include <ck_fs_plan.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* UI input translation only: local calendar dates, or exact UTC timestamps for
 * saved preferences. Filesystem comparison and policy remain in the engine. */
static inline int SyncParseDate(const char *text,ckfsp_timestamp *out,int *present)
{
    memset(out,0,sizeof(*out)); *present=text && *text;
    if (!*present) return 1;
    size_t length=strlen(text); int y=0,m=0,d=0,used=0;
    if (length<10 || sscanf(text,"%4d-%2d-%2d%n",&y,&m,&d,&used)!=3 || used!=10 ||
        y<1 || y>9999 || m<1 || m>12 || d<1) return 0;
    for (unsigned i=0;i<10;++i) if (i!=4 && i!=7 && (text[i]<'0' || text[i]>'9')) return 0;
    static const unsigned days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    int leap=y%4==0 && (y%100!=0 || y%400==0);
    if ((unsigned)d>days[m-1]+(m==2 && leap)) return 0;
    if (length==10) {
        struct tm tm; memset(&tm,0,sizeof(tm)); tm.tm_year=y-1900; tm.tm_mon=m-1; tm.tm_mday=d; tm.tm_isdst=-1;
        errno=0; time_t value=mktime(&tm);
        if (errno==EOVERFLOW || tm.tm_year!=y-1900 || tm.tm_mon!=m-1 || tm.tm_mday!=d ||
            tm.tm_hour || tm.tm_min || tm.tm_sec) return 0;
        out->seconds=(int64_t)value; return 1;
    }
    int h=0,minute=0,second=0;
    if (length<20 || length>30 || text[10]!='T' || text[length-1]!='Z' ||
        sscanf(text+11,"%2d:%2d:%2d%n",&h,&minute,&second,&used)!=3 || used!=8 ||
        h<0 || h>23 || minute<0 || minute>59 || second<0 || second>59) return 0;
    for (unsigned i=11;i<19;++i) if (i!=13 && i!=16 && (text[i]<'0' || text[i]>'9')) return 0;
    if (length!=20) {
        if (text[19]!='.' || length<22) return 0;
        unsigned digits=(unsigned)length-21; uint32_t fraction=0;
        for (unsigned i=0;i<digits;++i) {
            if (text[20+i]<'0' || text[20+i]>'9') return 0;
            fraction=fraction*10+(uint32_t)(text[20+i]-'0');
        }
        while (digits++<9) fraction*=10;
        out->nanoseconds=fraction;
    }
    int64_t previous=y-1;
    int64_t n=(int64_t)365*(y-1970)+(previous/4-1969/4)-(previous/100-1969/100)+(previous/400-1969/400);
    for (int month=1;month<m;++month) n+=days[month-1]+(month==2 && leap);
    out->seconds=(n+d-1)*86400+h*3600+minute*60+second; return 1;
}
static inline int SyncFormatDate(const ckfsp_timestamp *date,char out[40])
{
    time_t value=(time_t)date->seconds; struct tm tm;
    if ((int64_t)value!=date->seconds || !gmtime_r(&value,&tm) || tm.tm_year< -1899 || tm.tm_year>8099 ||
        date->nanoseconds>=1000000000 || date->reserved || tm.tm_mon<0 || tm.tm_mon>11 ||
        tm.tm_mday<1 || tm.tm_mday>31 || tm.tm_hour<0 || tm.tm_hour>23 ||
        tm.tm_min<0 || tm.tm_min>59 || tm.tm_sec<0 || tm.tm_sec>59) return 0;
    char base[80];
    int length=snprintf(base,sizeof(base),"%04d-%02d-%02dT%02d:%02d:%02d",tm.tm_year+1900,tm.tm_mon+1,
        tm.tm_mday,tm.tm_hour,tm.tm_min,tm.tm_sec);
    if (length!=19) return 0;
    memcpy(out,base,19);
    if (date->nanoseconds) {
        out[19]='.'; uint32_t fraction=date->nanoseconds;
        for (unsigned i=29;i>20;--i) { out[i-1]=(char)('0'+fraction%10); fraction/=10; }
        out[29]='Z'; out[30]=0;
    } else { out[19]='Z'; out[20]=0; }
    return 1;
}
#endif
