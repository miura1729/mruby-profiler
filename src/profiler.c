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
  int child_num;
  int child_capa;
  struct prof_irep **child;
  struct prof_irep *parent;
};

struct prof_result {
  struct prof_irep *irep_root;
  int irep_num;
  int irep_capa;
  struct prof_irep **irep_tab;
};

static struct prof_result result;
static struct prof_irep *current_prof_irep = NULL;
static mrb_code *old_pc = NULL;
static double old_time = 0.0;
static mrb_value prof_module;

struct prof_irep *
mrb_profiler_alloc_prof_irep(mrb_state* mrb, struct mrb_irep *irep, struct prof_irep *parent)
{
  int i;
  struct prof_irep *new;

  new = mrb_malloc(mrb, sizeof(struct prof_irep));
  
  new->parent = parent;
  new->irep = irep;
  irep->refcnt++;
  new->cnt = mrb_malloc(mrb, irep->ilen * sizeof(struct prof_counter));
  for (i = 0; i < irep->ilen; i++) {
    new->cnt[i].num = 0;
    new->cnt[i].time = 0.0;
  }

  new->child_num = 0;
  new->child_capa = 4;
  new->child = mrb_malloc(mrb, new->child_capa * sizeof(struct prof_irep *));

  if (result.irep_capa <= result.irep_num) {
    int size = result.irep_capa * 2;
    result.irep_tab = mrb_realloc(mrb, result.irep_tab, size * sizeof(struct prof_irep *));
    result.irep_capa = size;
  }
  result.irep_tab[result.irep_num] = new;
  result.irep_num++;
  
  return new;
}

void
prof_code_fetch_hook(struct mrb_state *mrb, struct mrb_irep *irep, mrb_code *pc, mrb_value *regs)
{
  struct timeval tv;
  double curtime;
  unsigned long ctimehi;
  unsigned long ctimelo;
  struct prof_irep *newirep;
  
  int off;

  if (irep->ilen == 1) {
    /* CALL ISEQ */
    return;
  }

#ifdef __i386__
  asm volatile ("rdtsc\n\t"
		:
		:
		: "%eax", "%edx");
  asm volatile ("mov %%eax, %0\n\t"
		:"=r"(ctimelo));
  asm volatile ("mov %%edx, %0\n\t"
		:"=r"(ctimehi));
  curtime = ((double)ctimehi) * 256.0;
  curtime += ((double)ctimelo / (65536.0 * 256.0));
#else
  gettimeofday(&tv, NULL);
  curtime = ((double)tv.tv_sec) + ((double)tv.tv_usec * 1e-6);
#endif

  if (current_prof_irep) {
    newirep = current_prof_irep;
    if (current_prof_irep->irep != irep) {
      int i;

      for (i = 0; i < current_prof_irep->child_num; i++) {
	if (current_prof_irep->child[i]->irep == irep) {
	  newirep = current_prof_irep->child[i];
	  goto finish;
	}
      }

      for (newirep= current_prof_irep->parent; 
	   newirep && newirep->irep != irep;
	   newirep = newirep->parent);
      if (newirep) {
	goto finish;
      }

      if (current_prof_irep->child_capa <= current_prof_irep->child_num) {
	struct prof_irep **tab;
	int size = current_prof_irep->child_capa * 2;

	current_prof_irep->child_capa = size;
	tab = mrb_realloc(mrb, current_prof_irep->child, size * sizeof(struct prof_irep *));
	current_prof_irep->child = tab;
      }

      newirep = mrb_profiler_alloc_prof_irep(mrb, irep, current_prof_irep);
      current_prof_irep->child[current_prof_irep->child_num] = newirep;
      current_prof_irep->child_num++;
    }
  }
  else {
    newirep = mrb_profiler_alloc_prof_irep(mrb, irep, NULL);
    current_prof_irep = result.irep_root = newirep;
    old_pc = irep->iseq;
    old_time = curtime;
  }

 finish:
  off = old_pc - current_prof_irep->irep->iseq;
  current_prof_irep->cnt[off].time += (curtime - old_time);
  current_prof_irep->cnt[off].num++;
  old_pc = pc;
  old_time = curtime;
  current_prof_irep = newirep;
}

static mrb_value
mrb_mruby_profiler_irep_len(mrb_state *mrb, mrb_value self)
{
  return mrb_fixnum_value(result.irep_num);
}

static mrb_value
mrb_mruby_profiler_ilen(mrb_state *mrb, mrb_value self)
{
  mrb_int irepno;
  mrb_get_args(mrb, "i", &irepno);
  
  return mrb_fixnum_value(result.irep_tab[irepno]->irep->ilen);
}

static mrb_value
mrb_mruby_profiler_read(mrb_state *mrb, mrb_value self)
{
  char *fn;
  int len;
  mrb_value res;
  FILE *fp;
  char buf[256];

  mrb_get_args(mrb, "s", &fn, &len);
  fn[len] = '\0';

  res = mrb_ary_new_capa(mrb, 5);
  fp = fopen(fn, "r");
  while (fgets(buf, 255, fp)) {
    int ai = mrb_gc_arena_save(mrb);
    mrb_value ele = mrb_str_new(mrb, buf, strlen(buf));
    
    mrb_ary_push(mrb, res, ele);
    mrb_gc_arena_restore(mrb, ai);
  }
  fclose(fp);
  return res;
}

static mrb_value
mrb_mruby_profiler_get_prof_info(mrb_state *mrb, mrb_value self)
{
  mrb_int irepno;
  mrb_int iseqoff;
  mrb_value res;
  const char *str;
  mrb_get_args(mrb, "ii", &irepno, &iseqoff);

  res = mrb_ary_new_capa(mrb, 7);
  str = result.irep_tab[irepno]->irep->filename;
  if (str) {
    mrb_ary_push(mrb, res, mrb_str_new(mrb, str, strlen(str)));
  }
  else {
    mrb_ary_push(mrb, res, mrb_nil_value());
  }

  if (result.irep_tab[irepno]->irep->lines) {
    mrb_ary_push(mrb, res, 
		 mrb_fixnum_value(result.irep_tab[irepno]->irep->lines[iseqoff]));
  }
  else {
    mrb_ary_push(mrb, res, mrb_nil_value());
  }

  mrb_ary_push(mrb, res, 
	       mrb_fixnum_value(result.irep_tab[irepno]->cnt[iseqoff].num));
  mrb_ary_push(mrb, res, 
	       mrb_float_value(mrb, result.irep_tab[irepno]->cnt[iseqoff].time));
  
  /* Address */
  mrb_ary_push(mrb, res, mrb_fixnum_value(&result.irep_tab[irepno]->irep->iseq[iseqoff]));
  /* code   */
  mrb_ary_push(mrb, res, mrb_fixnum_value(result.irep_tab[irepno]->irep->iseq[iseqoff]));

  return res;
}

void
mrb_mruby_profiler_gem_init(mrb_state* mrb) {
  struct RObject *m;

  result.irep_capa = 64;
  result.irep_tab = mrb_realloc(mrb, result.irep_tab, result.irep_capa * sizeof(struct prof_irep *));
  result.irep_num = 0;

  m = (struct RObject *)mrb_define_module(mrb, "Profiler");
  prof_module = mrb_obj_value(m);
  mrb->code_fetch_hook = prof_code_fetch_hook;
  mrb_define_singleton_method(mrb, m, "get_prof_info",  
			      mrb_mruby_profiler_get_prof_info, MRB_ARGS_REQ(2));
  mrb_define_singleton_method(mrb, m, "irep_len", mrb_mruby_profiler_irep_len, MRB_ARGS_NONE());
  mrb_define_singleton_method(mrb, m, "ilen", mrb_mruby_profiler_ilen, MRB_ARGS_REQ(1));
  mrb_define_singleton_method(mrb, m, "read", mrb_mruby_profiler_read, MRB_ARGS_REQ(1));
}

void
mrb_mruby_profiler_gem_final(mrb_state* mrb) {
  mrb_funcall(mrb, prof_module, "analyze", 0);
}
