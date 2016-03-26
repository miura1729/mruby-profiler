/* Profiler for ruby */
#include "mruby.h"
#include "mruby/irep.h"
#include "mruby/array.h"
#include "mruby/class.h"
#include "mruby/value.h"
#include "mruby/debug.h"
#include "mruby/opcode.h"
#include "mruby/string.h"
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
  mrb_sym mname;
  mrb_value klass;
  struct prof_counter *cnt;
  int child_num;
  int child_capa;
  struct prof_irep **child;
  int *ccall_num;
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
  new->mname = mrb->c->ci->mid;
  new->klass = mrb_obj_value(mrb_class(mrb, mrb->c->stack[0]));
  irep->refcnt++;
  new->cnt = mrb_malloc(mrb, irep->ilen * sizeof(struct prof_counter));
  for (i = 0; i < irep->ilen; i++) {
    new->cnt[i].num = 0;
    new->cnt[i].time = 0.0;
  }

  new->child_num = 0;
  new->child_capa = 4;
  new->child = mrb_malloc(mrb, new->child_capa * sizeof(struct prof_irep *));
  new->ccall_num = mrb_malloc(mrb, new->child_capa * sizeof(int));

  if (result.irep_capa <= result.irep_num) {
    int size = result.irep_capa * 2;
    result.irep_tab = mrb_realloc(mrb, result.irep_tab, size * sizeof(struct prof_irep *));
    result.irep_capa = size;
  }
  result.irep_tab[result.irep_num] = new;
  result.irep_num++;
  
  return new;
}

static inline double
prof_curtime()
{
  double curtime;

#ifdef __i386__
  unsigned long ctimehi;
  unsigned long ctimelo;

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
  struct timeval tv;

  gettimeofday(&tv, NULL);
  curtime = ((double)tv.tv_sec) + ((double)tv.tv_usec * 1e-6);
#endif

  return curtime;
}

void
prof_code_fetch_hook(struct mrb_state *mrb, struct mrb_irep *irep, mrb_code *pc, mrb_value *regs)
{
  double curtime;
  struct prof_irep *newirep;
  
  int off;

  curtime = prof_curtime();

  if (irep->ilen == 1) {
    /* CALL ISEQ */
    return;
  }

  if (current_prof_irep) {
    newirep = current_prof_irep;
    if (current_prof_irep->irep != irep) {
      int i;

      for (i = 0; i < current_prof_irep->child_num; i++) {
	if (current_prof_irep->child[i]->irep == irep) {
	  current_prof_irep->ccall_num[i]++;
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
	int *ccall;
	int size = current_prof_irep->child_capa * 2;

	current_prof_irep->child_capa = size;
	tab = mrb_realloc(mrb, current_prof_irep->child, size * sizeof(struct prof_irep *));
	current_prof_irep->child = tab;
	ccall = mrb_realloc(mrb, current_prof_irep->ccall_num, size * sizeof(int));
	current_prof_irep->ccall_num = ccall;
      }

      newirep = mrb_profiler_alloc_prof_irep(mrb, irep, current_prof_irep);
      off = current_prof_irep->child_num;
      current_prof_irep->child[off] = newirep;
      current_prof_irep->ccall_num[off] = 1;
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
  current_prof_irep = newirep;
  old_time = prof_curtime();
}

static mrb_value
mrb_mruby_profiler_irep_num(mrb_state *mrb, mrb_value self)
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
mrb_mruby_profiler_disasm_once(mrb_state *mrb, mrb_irep *irep, mrb_code c)
{
  int i = 0;
  char buf[256];

  switch (GET_OPCODE(c)) {
  case OP_NOP:
    sprintf(buf, "OP_NOP\n");
    break;
  case OP_MOVE:
    sprintf(buf, "OP_MOVE\tR%d\tR%d", GETARG_A(c), GETARG_B(c));
    //print_lv(mrb, irep, c, RAB);
    break;
  case OP_LOADL:
    {
      mrb_value v = irep->pool[GETARG_Bx(c)];
      mrb_value s = mrb_inspect(mrb, v);
      sprintf(buf, "OP_LOADL\tR%d\tL(%d)\t; %s", GETARG_A(c), GETARG_Bx(c), RSTRING_PTR(s));
    }
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_LOADI:
    sprintf(buf, "OP_LOADI\tR%d\t%d", GETARG_A(c), GETARG_sBx(c));
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_LOADSYM:
    sprintf(buf, "OP_LOADSYM\tR%d\t:%s", GETARG_A(c),
	   mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]));
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_LOADNIL:
    sprintf(buf, "OP_LOADNIL\tR%d\t", GETARG_A(c));
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_LOADSELF:
    sprintf(buf, "OP_LOADSELF\tR%d\t", GETARG_A(c));
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_LOADT:
    sprintf(buf, "OP_LOADT\tR%d\t", GETARG_A(c));
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_LOADF:
    sprintf(buf, "OP_LOADF\tR%d\t", GETARG_A(c));
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_GETGLOBAL:
    sprintf(buf, "OP_GETGLOBAL\tR%d\t:%s", GETARG_A(c),
	   mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]));
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_SETGLOBAL:
    sprintf(buf, "OP_SETGLOBAL\t:%s\tR%d",
	   mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]),
	   GETARG_A(c));
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_GETCONST:
    sprintf(buf, "OP_GETCONST\tR%d\t:%s", GETARG_A(c),
	   mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]));
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_SETCONST:
    sprintf(buf, "OP_SETCONST\t:%s\tR%d",
	   mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]),
	   GETARG_A(c));
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_GETMCNST:
    sprintf(buf, "OP_GETMCNST\tR%d\tR%d::%s", GETARG_A(c), GETARG_A(c),
	   mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]));
    //print_lv(mrb, irep, c, RAB);
    break;
  case OP_SETMCNST:
    sprintf(buf, "OP_SETMCNST\tR%d::%s\tR%d", GETARG_A(c)+1,
	   mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]),
	   GETARG_A(c));
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_GETIV:
    sprintf(buf, "OP_GETIV\tR%d\t%s", GETARG_A(c),
	   mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]));
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_SETIV:
    sprintf(buf, "OP_SETIV\t%s\tR%d",
	   mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]),
	   GETARG_A(c));
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_GETUPVAR:
    sprintf(buf, "OP_GETUPVAR\tR%d\t%d\t%d",
	   GETARG_A(c), GETARG_B(c), GETARG_C(c));
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_SETUPVAR:
    sprintf(buf, "OP_SETUPVAR\tR%d\t%d\t%d",
	   GETARG_A(c), GETARG_B(c), GETARG_C(c));
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_GETCV:
    sprintf(buf, "OP_GETCV\tR%d\t%s", GETARG_A(c),
	   mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]));
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_SETCV:
    sprintf(buf, "OP_SETCV\t%s\tR%d",
	   mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]),
	   GETARG_A(c));
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_JMP:
    sprintf(buf, "OP_JMP\t\t%03d", i+GETARG_sBx(c));
    break;
  case OP_JMPIF:
    sprintf(buf, "OP_JMPIF\tR%d\t%03d", GETARG_A(c), i+GETARG_sBx(c));
    break;
  case OP_JMPNOT:
    sprintf(buf, "OP_JMPNOT\tR%d\t%03d", GETARG_A(c), i+GETARG_sBx(c));
    break;
  case OP_SEND:
    sprintf(buf, "OP_SEND\tR%d\t:%s\t%d", GETARG_A(c),
	   mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
	   GETARG_C(c));
    break;
  case OP_SENDB:
    sprintf(buf, "OP_SENDB\tR%d\t:%s\t%d", GETARG_A(c),
	   mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
	   GETARG_C(c));
    break;
  case OP_TAILCALL:
    sprintf(buf, "OP_TAILCALL\tR%d\t:%s\t%d", GETARG_A(c),
	   mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
	   GETARG_C(c));
    break;
  case OP_SUPER:
    sprintf(buf, "OP_SUPER\tR%d\t%d", GETARG_A(c),
	   GETARG_C(c));
    break;
  case OP_ARGARY:
    sprintf(buf, "OP_ARGARY\tR%d\t%d:%d:%d:%d", GETARG_A(c),
	   (GETARG_Bx(c)>>10)&0x3f,
	   (GETARG_Bx(c)>>9)&0x1,
	   (GETARG_Bx(c)>>4)&0x1f,
	   (GETARG_Bx(c)>>0)&0xf);
    //print_lv(mrb, irep, c, RA);
    break;

  case OP_ENTER:
    sprintf(buf, "OP_ENTER\t%d:%d:%d:%d:%d:%d:%d",
	   (GETARG_Ax(c)>>18)&0x1f,
	   (GETARG_Ax(c)>>13)&0x1f,
	   (GETARG_Ax(c)>>12)&0x1,
	   (GETARG_Ax(c)>>7)&0x1f,
	   (GETARG_Ax(c)>>2)&0x1f,
	   (GETARG_Ax(c)>>1)&0x1,
	   GETARG_Ax(c) & 0x1);
    break;
  case OP_RETURN:
    sprintf(buf, "OP_RETURN\tR%d", GETARG_A(c));
    switch (GETARG_B(c)) {
    case OP_R_NORMAL:
    case OP_R_RETURN:
      strcat(buf, "\treturn"); break;
    case OP_R_BREAK:
      strcat(buf, "\tbreak"); break;
    default:
      strcat(buf, "\tbroken"); break;
      break;
    }
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_BLKPUSH:
    sprintf(buf, "OP_BLKPUSH\tR%d\t%d:%d:%d:%d", GETARG_A(c),
	   (GETARG_Bx(c)>>10)&0x3f,
	   (GETARG_Bx(c)>>9)&0x1,
	   (GETARG_Bx(c)>>4)&0x1f,
	   (GETARG_Bx(c)>>0)&0xf);
    //print_lv(mrb, irep, c, RA);
    break;

  case OP_LAMBDA:
    sprintf(buf, "OP_LAMBDA\tR%d\tI(%+d)\t%d", GETARG_A(c), GETARG_b(c)+1, GETARG_c(c));
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_RANGE:
    sprintf(buf, "OP_RANGE\tR%d\tR%d\t%d", GETARG_A(c), GETARG_B(c), GETARG_C(c));
    //print_lv(mrb, irep, c, RAB);
    break;
  case OP_METHOD:
    sprintf(buf, "OP_METHOD\tR%d\t:%s", GETARG_A(c),
	   mrb_sym2name(mrb, irep->syms[GETARG_B(c)]));
    //print_lv(mrb, irep, c, RA);
    break;

  case OP_ADD:
    sprintf(buf, "OP_ADD\tR%d\t:%s\t%d", GETARG_A(c),
	   mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
	   GETARG_C(c));
    break;
  case OP_ADDI:
    sprintf(buf, "OP_ADDI\tR%d\t:%s\t%d", GETARG_A(c),
	   mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
	   GETARG_C(c));
    break;
  case OP_SUB:
    sprintf(buf, "OP_SUB\tR%d\t:%s\t%d", GETARG_A(c),
	   mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
	   GETARG_C(c));
    break;
  case OP_SUBI:
    sprintf(buf, "OP_SUBI\tR%d\t:%s\t%d", GETARG_A(c),
	   mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
	   GETARG_C(c));
    break;
  case OP_MUL:
    sprintf(buf, "OP_MUL\tR%d\t:%s\t%d", GETARG_A(c),
	   mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
	   GETARG_C(c));
    break;
  case OP_DIV:
    sprintf(buf, "OP_DIV\tR%d\t:%s\t%d", GETARG_A(c),
	   mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
	   GETARG_C(c));
    break;
  case OP_LT:
    sprintf(buf, "OP_LT\tR%d\t:%s\t%d", GETARG_A(c),
	   mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
	   GETARG_C(c));
    break;
  case OP_LE:
    sprintf(buf, "OP_LE\tR%d\t:%s\t%d", GETARG_A(c),
	   mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
	   GETARG_C(c));
    break;
  case OP_GT:
    sprintf(buf, "OP_GT\tR%d\t:%s\t%d", GETARG_A(c),
	   mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
	   GETARG_C(c));
    break;
  case OP_GE:
    sprintf(buf, "OP_GE\tR%d\t:%s\t%d", GETARG_A(c),
	   mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
	   GETARG_C(c));
    break;
  case OP_EQ:
    sprintf(buf, "OP_EQ\tR%d\t:%s\t%d", GETARG_A(c),
	   mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
	   GETARG_C(c));
    break;

  case OP_STOP:
    sprintf(buf, "OP_STOP");
    break;

  case OP_ARRAY:
    sprintf(buf, "OP_ARRAY\tR%d\tR%d\t%d", GETARG_A(c), GETARG_B(c), GETARG_C(c));
    //print_lv(mrb, irep, c, RAB);
    break;
  case OP_ARYCAT:
    sprintf(buf, "OP_ARYCAT\tR%d\tR%d", GETARG_A(c), GETARG_B(c));
    //print_lv(mrb, irep, c, RAB);
    break;
  case OP_ARYPUSH:
    sprintf(buf, "OP_ARYPUSH\tR%d\tR%d", GETARG_A(c), GETARG_B(c));
    //print_lv(mrb, irep, c, RAB);
    break;
  case OP_AREF:
    sprintf(buf, "OP_AREF\tR%d\tR%d\t%d", GETARG_A(c), GETARG_B(c), GETARG_C(c));
    //print_lv(mrb, irep, c, RAB);
    break;
  case OP_APOST:
    sprintf(buf, "OP_APOST\tR%d\t%d\t%d", GETARG_A(c), GETARG_B(c), GETARG_C(c));
    //print_lv(mrb, irep, c, RAB);
    break;
  case OP_STRING:
    {
      mrb_value v = irep->pool[GETARG_Bx(c)];
      mrb_value s = mrb_str_dump(mrb, mrb_str_new(mrb, RSTRING_PTR(v), RSTRING_LEN(v)));
      sprintf(buf, "OP_STRING\tR%d\tL(%d)\t; %s", GETARG_A(c), GETARG_Bx(c), RSTRING_PTR(s));
    }
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_STRCAT:
    sprintf(buf, "OP_STRCAT\tR%d\tR%d", GETARG_A(c), GETARG_B(c));
    //print_lv(mrb, irep, c, RAB);
    break;
  case OP_HASH:
    sprintf(buf, "OP_HASH\tR%d\tR%d\t%d", GETARG_A(c), GETARG_B(c), GETARG_C(c));
    //print_lv(mrb, irep, c, RAB);
    break;

  case OP_OCLASS:
    sprintf(buf, "OP_OCLASS\tR%d", GETARG_A(c));
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_CLASS:
    sprintf(buf, "OP_CLASS\tR%d\t:%s", GETARG_A(c),
	   mrb_sym2name(mrb, irep->syms[GETARG_B(c)]));
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_MODULE:
    sprintf(buf, "OP_MODULE\tR%d\t:%s", GETARG_A(c),
	   mrb_sym2name(mrb, irep->syms[GETARG_B(c)]));
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_EXEC:
    sprintf(buf, "OP_EXEC\tR%d\tI(%+d)", GETARG_A(c), GETARG_Bx(c)+1);
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_SCLASS:
    sprintf(buf, "OP_SCLASS\tR%d\tR%d", GETARG_A(c), GETARG_B(c));
    //print_lv(mrb, irep, c, RAB);
    break;
  case OP_TCLASS:
    sprintf(buf, "OP_TCLASS\tR%d", GETARG_A(c));
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_ERR:
    {
      mrb_value v = irep->pool[GETARG_Bx(c)];
      mrb_value s = mrb_str_dump(mrb, mrb_str_new(mrb, RSTRING_PTR(v), RSTRING_LEN(v)));
      sprintf(buf, "OP_ERR\t%s", RSTRING_PTR(s));
    }
    break;
  case OP_EPUSH:
    sprintf(buf, "OP_EPUSH\t:I(%+d)", GETARG_Bx(c)+1);
    break;
  case OP_ONERR:
    sprintf(buf, "OP_ONERR\t%03d", i+GETARG_sBx(c));
    break;
  case OP_RESCUE:
    sprintf(buf, "OP_RESCUE\tR%d", GETARG_A(c));
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_RAISE:
    sprintf(buf, "OP_RAISE\tR%d", GETARG_A(c));
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_POPERR:
    sprintf(buf, "OP_POPERR\t%d", GETARG_A(c));
    //print_lv(mrb, irep, c, RA);
    break;
  case OP_EPOP:
    sprintf(buf, "OP_EPOP\t%d", GETARG_A(c));
    break;

  default:
    sprintf(buf, "OP_unknown %d\t%d\t%d\t%d", GET_OPCODE(c),
	   GETARG_A(c), GETARG_B(c), GETARG_C(c));
    break;
  }

  return mrb_str_new_cstr(mrb, buf);
}

static mrb_value
mrb_mruby_profiler_get_inst_info(mrb_state *mrb, mrb_value self)
{
  mrb_int irepno;
  mrb_int iseqoff;
  mrb_value res;
  const char *str;
  mrb_get_args(mrb, "ii", &irepno, &iseqoff);

  res = mrb_ary_new_capa(mrb, 7);
  /* 0 fine name or method name */
  str = result.irep_tab[irepno]->irep->filename;
  if (str) {
    mrb_ary_push(mrb, res, mrb_str_new(mrb, str, strlen(str)));
  }
  else {
    mrb_value cls_mname = mrb_ary_new_capa(mrb, 2);
    mrb_ary_push(mrb, cls_mname, result.irep_tab[irepno]->klass);
    mrb_ary_push(mrb, cls_mname, mrb_symbol_value(result.irep_tab[irepno]->mname));

    mrb_ary_push(mrb, res, cls_mname);
  }

  /* 1 Line no */
  if (result.irep_tab[irepno]->irep->lines) {
    mrb_ary_push(mrb, res, 
		 mrb_fixnum_value(result.irep_tab[irepno]->irep->lines[iseqoff]));
  }
  else {
    mrb_ary_push(mrb, res, mrb_nil_value());
  }

  /* 2 Execution Count */
  mrb_ary_push(mrb, res, 
	       mrb_fixnum_value(result.irep_tab[irepno]->cnt[iseqoff].num));
  /* 3 Execution Time */
  mrb_ary_push(mrb, res, 
	       mrb_float_value(mrb, result.irep_tab[irepno]->cnt[iseqoff].time));
  
  /* 4 Address */
  mrb_ary_push(mrb, res, mrb_fixnum_value((mrb_int)&result.irep_tab[irepno]->irep->iseq[iseqoff]));
  /* 5 code   */
  //  mrb_ary_push(mrb, res, mrb_fixnum_value(result.irep_tab[irepno]->irep->iseq[iseqoff]));
  mrb_ary_push(mrb, res, mrb_mruby_profiler_disasm_once(mrb, result.irep_tab[irepno]->irep, result.irep_tab[irepno]->irep->iseq[iseqoff]));

  return res;
}

#define IREP_ID(prof) (mrb_fixnum_value((mrb_int)((prof)->irep)))

static mrb_value
mrb_mruby_profiler_get_irep_info(mrb_state *mrb, mrb_value self)
{
  mrb_int irepno;
  struct prof_irep *profi;
  mrb_value res;
  mrb_value ary;
  int i;

  mrb_get_args(mrb, "i", &irepno);

  res = mrb_ary_new_capa(mrb, 3);
  profi = result.irep_tab[irepno];
  /* 0 id of irep */
  mrb_ary_push(mrb, res, IREP_ID(profi));

  /* 1 Class of method */
  mrb_ary_push(mrb, res, profi->klass);

  /* 2 method name */
  mrb_ary_push(mrb, res, mrb_symbol_value(profi->mname));

  /* 3 file name */
  if (profi->irep->filename) {
    mrb_ary_push(mrb, res, mrb_str_new(mrb, profi->irep->filename, strlen(profi->irep->filename)));
  }
  else {
    mrb_ary_push(mrb, res, mrb_nil_value());
  }

  /* 4 Childs */
  ary = mrb_ary_new_capa(mrb, profi->child_num);
  for (i = 0; i < profi->child_num; i++) {
    mrb_ary_push(mrb, ary, IREP_ID(profi->child[i]));
  }
  mrb_ary_push(mrb, res, ary);

  /* 5 Call num to Childs */
  ary = mrb_ary_new_capa(mrb, profi->child_num);
  for (i = 0; i < profi->child_num; i++) {
    mrb_ary_push(mrb, ary, mrb_fixnum_value(profi->ccall_num[i]));
  }
  mrb_ary_push(mrb, res, ary);

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
  mrb_define_singleton_method(mrb, m, "get_inst_info",  
			      mrb_mruby_profiler_get_inst_info, MRB_ARGS_REQ(2));
  mrb_define_singleton_method(mrb, m, "get_irep_info",  
			      mrb_mruby_profiler_get_irep_info, MRB_ARGS_REQ(1));
  mrb_define_singleton_method(mrb, m, "irep_num", mrb_mruby_profiler_irep_num, MRB_ARGS_NONE());
  mrb_define_singleton_method(mrb, m, "ilen", mrb_mruby_profiler_ilen, MRB_ARGS_REQ(1));
  mrb_define_singleton_method(mrb, m, "read", mrb_mruby_profiler_read, MRB_ARGS_REQ(1));
}

void
mrb_mruby_profiler_gem_final(mrb_state* mrb) {
  mrb_funcall(mrb, prof_module, "analyze", 0);
}
