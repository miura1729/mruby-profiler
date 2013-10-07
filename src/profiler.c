/* Profiler for ruby */
#include "mruby.h"
#include "mruby/irep.h"
#include "mruby/array.h"
#include <time.h>
#include <sys/time.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct prof_counter {
  double time;
  uint32_t num;
};

struct prof_irep {
  mrb_irep *irep;
  struct prof_counter *cnt;
};

struct prof_root {
  int irep_len;
  struct prof_irep *pirep;
};

static struct prof_root result;
static mrb_irep *old_irep = NULL;
static mrb_code *old_pc = NULL;
static double old_time = 0.0;

void
mrb_profiler_reallocinfo(mrb_state* mrb)
{
  int i;
  int j;
  result.pirep = mrb_realloc(mrb, result.pirep, mrb->irep_len * sizeof(struct prof_irep));

  for (i = result.irep_len; i < mrb->irep_len; i++) {
    static struct prof_irep *rirep;
    mrb_irep *irep;

    rirep = &result.pirep[i];
    irep = rirep->irep = mrb->irep[i];
    rirep->cnt = mrb_malloc(mrb, irep->ilen * sizeof(struct prof_counter));
    for (j = 0; j < irep->ilen; j++) {
      rirep->cnt[j].num = 0;
      rirep->cnt[j].time = 0.0;
    }
  }
  result.irep_len = mrb->irep_len;
}

void
prof_code_fetch_hook(struct mrb_state *mrb, struct mrb_irep *irep, mrb_code *pc, mrb_value *regs)
{
  struct timeval tv;
  double curtime;
  
  int off;

  if (irep->idx == -1) {
    /* CALL ISEQ */
    return;
  }

  if (irep->idx >= result.irep_len) {
    mrb_profiler_reallocinfo(mrb);
  }
  gettimeofday(&tv, NULL);
  curtime = (double)tv.tv_sec + ((double)tv.tv_usec / 1e-6);
  if (old_irep) {
    off = old_pc - old_irep->iseq;
    result.pirep[old_irep->idx].cnt[off].time = curtime - old_time;
  }
  
  off = pc - irep->iseq;
  result.pirep[irep->idx].cnt[off].num++;
  old_irep = irep;
  old_pc = pc;
  old_time = curtime;
}

static mrb_value
mrb_mruby_profiler_irep_len(mrb_state *mrb, mrb_value self)
{
  return mrb_fixnum_value(result.irep_len);
}

static mrb_value
mrb_mruby_profiler_ilen(mrb_state *mrb, mrb_value self)
{
  mrb_int irepno;
  mrb_get_args(mrb, "i", &irepno);
  
  return mrb_fixnum_value(result.pirep[irepno].irep->ilen);
}

static mrb_value
mrb_mruby_profiler_get_prof_info(mrb_state *mrb, mrb_value self)
{
  mrb_int irepno;
  mrb_int iseqoff;
  mrb_value res;
  const char *str;
  mrb_get_args(mrb, "ii", &irepno, &iseqoff);

  str = result.pirep[irepno].irep->filename;
  if (str) {
    res = mrb_ary_new_capa(mrb, 5);
    mrb_ary_push(mrb, res, mrb_str_new(mrb, str, strlen(str)));
  }
  else {
    mrb_ary_push(mrb, res, mrb_nil_value());
  }

  if (result.pirep[irepno].irep->lines) {
    mrb_ary_push(mrb, res, 
		 mrb_fixnum_value(result.pirep[irepno].irep->lines[iseqoff]));
  }
  else {
    mrb_ary_push(mrb, res, mrb_nil_value());
  }

  mrb_ary_push(mrb, res, 
	       mrb_fixnum_value(result.pirep[irepno].cnt[iseqoff].num));
  mrb_ary_push(mrb, res, 
	       mrb_float_value(mrb, result.pirep[irepno].cnt[iseqoff].time));
  
  return res;
}

void
mrb_mruby_profiler_gem_init(mrb_state* mrb) {
  struct RObject *m;

  mrb_profiler_reallocinfo(mrb);
  m = (struct RObject *)mrb_define_module(mrb, "Profiler");
  mrb->code_fetch_hook = prof_code_fetch_hook;
  mrb_define_singleton_method(mrb, m, "get_prof_info",  
			      mrb_mruby_profiler_get_prof_info, MRB_ARGS_REQ(2));
  mrb_define_singleton_method(mrb, m, "irep_len", mrb_mruby_profiler_irep_len, MRB_ARGS_NONE());
  mrb_define_singleton_method(mrb, m, "ilen", mrb_mruby_profiler_ilen, MRB_ARGS_REQ(1));
}

void
mrb_mruby_profiler_gem_final(mrb_state* mrb) {
  int i;
  int j;
  for (i = 0; i < result.irep_len; i++) {
    mrb_irep *irep;
    int *nums;
    int maxlines = 0;
    int minlines = 99999;
    FILE *fp;

    irep = mrb->irep[i];
    if (irep->filename == NULL) {
      continue;
    }
    printf("   %s \n", irep->filename);

    if (irep->lines == NULL) {
      continue;
    }

    for (j = 0; j < irep->ilen; j++) {
      maxlines = (maxlines < irep->lines[j]) ? irep->lines[j] : maxlines;
      minlines = (minlines > irep->lines[j]) ? irep->lines[j] : minlines;
    }
    nums = alloca((maxlines + 1) * sizeof(int));
    for (j = 0; j < irep->ilen; j++) {
      nums[j] = 0;
    }

    for (j = 0; j < irep->ilen; j++) {
      nums[irep->lines[j]] += result.pirep[i].cnt[j].num;
    }

    fp = fopen(irep->filename, "r");
    for (j = minlines; j < maxlines; j++) {
      char buf[256];
      fgets(buf, 256, fp);
      printf("%d   %d  %s", j, nums[j], buf);
    }
    fclose(fp);
  }
}
