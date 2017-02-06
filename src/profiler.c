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
#include <assert.h>

struct prof_counter {
  double time;  //Total execution time in seconds
  uint32_t num; //Total number of executions
};

struct prof_irep {
  mrb_irep *irep;           //VM instructions
  mrb_sym name;        //Method name
  mrb_value klass;        //Class implementing method
  struct prof_counter *cnt; //Profiler results

  int child_num;            //Number of called methods
  int child_capa;           //Child array capacity
  struct prof_irep **child; //Children VM instructions/methods [child_num elements]

  int *ccall_num;           //Number of calls to each child [child_num elements]
  struct prof_irep *parent;
};

struct prof_result {
  struct prof_irep *irep_root; //First irep profiled
  int irep_num;                //Number of ireps profiled
  int irep_capa;               //Capacity of irep array
  struct prof_irep **irep_tab; //Profiler results, one irep each
};

//Profiling results
static struct prof_result result;
//Current method
static struct prof_irep *current_prof_irep = NULL;
//Last profiled instruction
static mrb_code *old_pc = NULL;
//Time that last instruction was fetched at
static double old_time = 0.0;
//Profiler module
static mrb_value prof_module;

//Allocate a new set of profiler metadata for a new method's irep
//
//Arguments:
// - mrb:    Mruby state
// - irep:   Method irep
// - parent: Calling method
struct prof_irep *
mrb_profiler_alloc_prof_irep(mrb_state* mrb,
                             struct mrb_irep *irep,
                             struct prof_irep *parent)
{
  size_t i;
  struct prof_irep *res;

  res = (struct prof_irep*)mrb_malloc(mrb, sizeof(struct prof_irep));

  res->parent = parent;
  res->irep = irep;
  res->mname = mrb->c->ci->mid;
  res->klass = mrb_obj_value(mrb_class(mrb, mrb->c->stack[0]));
  irep->refcnt++;

  //Allocate per instruction counters
  res->cnt = (struct prof_counter*)
      mrb_malloc(mrb, irep->ilen * sizeof(struct prof_counter));
  for (i = 0; i < irep->ilen; i++) {
    res->cnt[i].num = 0;
    res->cnt[i].time = 0.0;
  }

  //Preallocate child array
  res->child_num  = 0;
  res->child_capa = 4;
  res->child      = (struct prof_irep**)
      mrb_malloc(mrb, res->child_capa * sizeof(struct prof_irep *));
  res->ccall_num  = (int*)mrb_malloc(mrb, res->child_capa * sizeof(int));

  //Add to the global profiler results
  if (result.irep_capa <= result.irep_num) {
    int size = result.irep_capa * 2;
    result.irep_tab = (struct prof_irep**)
        mrb_realloc(mrb, result.irep_tab, size * sizeof(struct prof_irep *));
    result.irep_capa = size;
  }
  result.irep_tab[result.irep_num] = res;
  result.irep_num++;

  return res;
}

//Get current time in seconds
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

//VM Execution Hook
//
//This function is called before the VM executes each instruction
//
//Arguments:
// - mrb: mruby state
// - irep: current instruction context
// - pc:   current VM instruction
// - regs: current VM registers (unused)
void
prof_code_fetch_hook(struct mrb_state *mrb,
                     struct mrb_irep *irep,
                     mrb_code *pc,
                     mrb_value *regs)
{
  double curtime;
  struct prof_irep *newirep;

  int off;
  (void) regs;

  curtime = prof_curtime();

  if (irep->ilen == 1) {
    /* CALL ISEQ */
    return;
  }

  if (current_prof_irep) {
    newirep = current_prof_irep;

    //Executing a new method
    if (current_prof_irep->irep != irep) {
      int i;

      //Update info from calling instruction
      //XXX - wouldn't it be simplier to store the last fetched
      //instruction and assume that it was used to get to the new irep?
      //Though, there would be the issue of 'return' statements...
      for (i = 0; i < current_prof_irep->child_num; i++) {
        if (current_prof_irep->child[i]->irep == irep) {
          current_prof_irep->ccall_num[i]++;
          newirep = current_prof_irep->child[i];
          goto finish;
        }
      }

      //XXX - check for circular dependency?
      for (newirep = current_prof_irep->parent;
          newirep && newirep->irep != irep;
          newirep = newirep->parent);
      if (newirep) {
        goto finish;
      }

      //Extend child irep list
      if (current_prof_irep->child_capa <= current_prof_irep->child_num) {
        struct prof_irep **tab;
        int *ccall;
        int size = current_prof_irep->child_capa * 2;

        current_prof_irep->child_capa = size;
        tab = (struct prof_irep**)
          mrb_realloc(mrb, current_prof_irep->child,
                      size * sizeof(struct prof_irep *));
        current_prof_irep->child = tab;
        ccall = (int*)
          mrb_realloc(mrb, current_prof_irep->ccall_num, size * sizeof(int));
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
    //First VM instruction
    newirep = mrb_profiler_alloc_prof_irep(mrb, irep, NULL);
    current_prof_irep = result.irep_root = newirep;
    old_pc = irep->iseq;
    old_time = curtime;
  }

finish:
  //Update instruction level profilt info
  off = old_pc - current_prof_irep->irep->iseq;
  current_prof_irep->cnt[off].time += (curtime - old_time);
  current_prof_irep->cnt[off].num++;
  old_pc = pc;
  current_prof_irep = newirep;
  old_time = prof_curtime();
}

//Get total number of profiled ireps
static mrb_value
mrb_mruby_profiler_irep_num(mrb_state *mrb, mrb_value self)
{
  (void) mrb;
  (void) self;
  return mrb_fixnum_value(result.irep_num);
}

//Get number of instructions in a given irep/method
static mrb_value
mrb_mruby_profiler_ilen(mrb_state *mrb, mrb_value self)
{
  mrb_int irepno;
  mrb_get_args(mrb, "i", &irepno);
  (void) self;

  return mrb_fixnum_value(result.irep_tab[irepno]->irep->ilen);
}

//Read source file
//
//Arguments:
// - file name
//Returns:
// - Array of files's contents split by newline
static mrb_value
mrb_mruby_profiler_read(mrb_state *mrb, mrb_value self)
{
  char *fn;
  int len;
  mrb_value res;
  FILE *fp;
  char buf[256];
  (void) self;

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

//Produce String representation of provided irep code
//Arguments
//  - mrb:  mruby state
//  - irep: VM instruction's irep
//  - c:    VM instruction
static mrb_value
mrb_mruby_profiler_disasm_once(mrb_state *mrb, mrb_irep *irep, mrb_code c)
{
  int    i = 0;
  char   buf[256] = {0};
  size_t len = sizeof(buf);

  switch (GET_OPCODE(c)) {
  case OP_NOP:
    snprintf(buf, len, "OP_NOP\n");
    break;
  case OP_MOVE:
    snprintf(buf, len, "OP_MOVE\tR%d\tR%d", GETARG_A(c), GETARG_B(c));
    break;
  case OP_LOADL:
    {
      mrb_value v = irep->pool[GETARG_Bx(c)];
      mrb_value s = mrb_inspect(mrb, v);
      snprintf(buf, len, "OP_LOADL\tR%d\tL(%d)\t; %s",
          GETARG_A(c), GETARG_Bx(c), RSTRING_PTR(s));
    }
    break;
  case OP_LOADI:
    snprintf(buf, len, "OP_LOADI\tR%d\t%d", GETARG_A(c), GETARG_sBx(c));
    break;
  case OP_LOADSYM:
    snprintf(buf, len, "OP_LOADSYM\tR%d\t:%s", GETARG_A(c),
            mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]));
    break;
  case OP_LOADNIL:
    snprintf(buf, len, "OP_LOADNIL\tR%d\t", GETARG_A(c));
    break;
  case OP_LOADSELF:
    snprintf(buf, len, "OP_LOADSELF\tR%d\t", GETARG_A(c));
    break;
  case OP_LOADT:
    snprintf(buf, len, "OP_LOADT\tR%d\t", GETARG_A(c));
    break;
  case OP_LOADF:
    snprintf(buf, len, "OP_LOADF\tR%d\t", GETARG_A(c));
    break;
  case OP_GETGLOBAL:
    snprintf(buf, len, "OP_GETGLOBAL\tR%d\t:%s", GETARG_A(c),
            mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]));
    break;
  case OP_SETGLOBAL:
    snprintf(buf, len, "OP_SETGLOBAL\t:%s\tR%d",
            mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]),
            GETARG_A(c));
    break;
  case OP_GETCONST:
    snprintf(buf, len, "OP_GETCONST\tR%d\t:%s", GETARG_A(c),
            mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]));
    break;
  case OP_SETCONST:
    snprintf(buf, len, "OP_SETCONST\t:%s\tR%d",
            mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]),
            GETARG_A(c));
    break;
  case OP_GETMCNST:
    snprintf(buf, len, "OP_GETMCNST\tR%d\tR%d::%s", GETARG_A(c), GETARG_A(c),
            mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]));
    break;
  case OP_SETMCNST:
    snprintf(buf, len, "OP_SETMCNST\tR%d::%s\tR%d", GETARG_A(c)+1,
            mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]),
            GETARG_A(c));
    break;
  case OP_GETIV:
    snprintf(buf, len, "OP_GETIV\tR%d\t%s", GETARG_A(c),
            mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]));
    break;
  case OP_SETIV:
    snprintf(buf, len, "OP_SETIV\t%s\tR%d",
            mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]),
            GETARG_A(c));
    break;
  case OP_GETUPVAR:
    snprintf(buf, len, "OP_GETUPVAR\tR%d\t%d\t%d",
            GETARG_A(c), GETARG_B(c), GETARG_C(c));
    break;
  case OP_SETUPVAR:
    snprintf(buf, len, "OP_SETUPVAR\tR%d\t%d\t%d",
            GETARG_A(c), GETARG_B(c), GETARG_C(c));
    break;
  case OP_GETCV:
    snprintf(buf, len, "OP_GETCV\tR%d\t%s", GETARG_A(c),
            mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]));
    break;
  case OP_SETCV:
    snprintf(buf, len, "OP_SETCV\t%s\tR%d",
            mrb_sym2name(mrb, irep->syms[GETARG_Bx(c)]),
            GETARG_A(c));
    break;
  case OP_JMP:
    snprintf(buf, len, "OP_JMP\t\t%03d", i+GETARG_sBx(c));
    break;
  case OP_JMPIF:
    snprintf(buf, len, "OP_JMPIF\tR%d\t%03d", GETARG_A(c), i+GETARG_sBx(c));
    break;
  case OP_JMPNOT:
    snprintf(buf, len, "OP_JMPNOT\tR%d\t%03d", GETARG_A(c), i+GETARG_sBx(c));
    break;
  case OP_SEND:
    snprintf(buf, len, "OP_SEND\tR%d\t:%s\t%d", GETARG_A(c),
            mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
            GETARG_C(c));
    break;
  case OP_SENDB:
    snprintf(buf, len, "OP_SENDB\tR%d\t:%s\t%d", GETARG_A(c),
            mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
            GETARG_C(c));
    break;
  case OP_TAILCALL:
    snprintf(buf, len, "OP_TAILCALL\tR%d\t:%s\t%d", GETARG_A(c),
            mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
            GETARG_C(c));
    break;
  case OP_SUPER:
    snprintf(buf, len, "OP_SUPER\tR%d\t%d", GETARG_A(c),
            GETARG_C(c));
    break;
  case OP_ARGARY:
    snprintf(buf, len, "OP_ARGARY\tR%d\t%d:%d:%d:%d", GETARG_A(c),
            (GETARG_Bx(c)>>10)&0x3f,
            (GETARG_Bx(c)>>9)&0x1,
            (GETARG_Bx(c)>>4)&0x1f,
            (GETARG_Bx(c)>>0)&0xf);
    break;

  case OP_ENTER:
    snprintf(buf, len, "OP_ENTER\t%d:%d:%d:%d:%d:%d:%d",
            (GETARG_Ax(c)>>18)&0x1f,
            (GETARG_Ax(c)>>13)&0x1f,
            (GETARG_Ax(c)>>12)&0x1,
            (GETARG_Ax(c)>>7)&0x1f,
            (GETARG_Ax(c)>>2)&0x1f,
            (GETARG_Ax(c)>>1)&0x1,
            GETARG_Ax(c) & 0x1);
    break;
  case OP_RETURN:
    snprintf(buf, len, "OP_RETURN\tR%d", GETARG_A(c));
    switch (GETARG_B(c)) {
    case OP_R_NORMAL:
    case OP_R_RETURN:
      strncat(buf, "\treturn", len); break;
    case OP_R_BREAK:
      strncat(buf, "\tbreak", len); break;
    default:
      strncat(buf, "\tbroken", len); break;
      break;
    }
    break;
  case OP_BLKPUSH:
    snprintf(buf, len, "OP_BLKPUSH\tR%d\t%d:%d:%d:%d", GETARG_A(c),
            (GETARG_Bx(c)>>10)&0x3f,
            (GETARG_Bx(c)>>9)&0x1,
            (GETARG_Bx(c)>>4)&0x1f,
            (GETARG_Bx(c)>>0)&0xf);
    break;

  case OP_LAMBDA:
    snprintf(buf, len, "OP_LAMBDA\tR%d\tI(%+d)\t%d",
        GETARG_A(c), GETARG_b(c)+1, GETARG_c(c));
    break;
  case OP_RANGE:
    snprintf(buf, len, "OP_RANGE\tR%d\tR%d\t%d",
        GETARG_A(c), GETARG_B(c), GETARG_C(c));
    break;
  case OP_METHOD:
    snprintf(buf, len, "OP_METHOD\tR%d\t:%s", GETARG_A(c),
            mrb_sym2name(mrb, irep->syms[GETARG_B(c)]));
    break;

  case OP_ADD:
    snprintf(buf, len, "OP_ADD\tR%d\t:%s\t%d", GETARG_A(c),
            mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
            GETARG_C(c));
    break;
  case OP_ADDI:
    snprintf(buf, len, "OP_ADDI\tR%d\t:%s\t%d", GETARG_A(c),
            mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
            GETARG_C(c));
    break;
  case OP_SUB:
    snprintf(buf, len, "OP_SUB\tR%d\t:%s\t%d", GETARG_A(c),
            mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
            GETARG_C(c));
    break;
  case OP_SUBI:
    snprintf(buf, len, "OP_SUBI\tR%d\t:%s\t%d", GETARG_A(c),
            mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
            GETARG_C(c));
    break;
  case OP_MUL:
    snprintf(buf, len, "OP_MUL\tR%d\t:%s\t%d", GETARG_A(c),
            mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
            GETARG_C(c));
    break;
  case OP_DIV:
    snprintf(buf, len, "OP_DIV\tR%d\t:%s\t%d", GETARG_A(c),
            mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
            GETARG_C(c));
    break;
  case OP_LT:
    snprintf(buf, len, "OP_LT\tR%d\t:%s\t%d", GETARG_A(c),
            mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
            GETARG_C(c));
    break;
  case OP_LE:
    snprintf(buf, len, "OP_LE\tR%d\t:%s\t%d", GETARG_A(c),
            mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
            GETARG_C(c));
    break;
  case OP_GT:
    snprintf(buf, len, "OP_GT\tR%d\t:%s\t%d", GETARG_A(c),
            mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
            GETARG_C(c));
    break;
  case OP_GE:
    snprintf(buf, len, "OP_GE\tR%d\t:%s\t%d", GETARG_A(c),
            mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
            GETARG_C(c));
    break;
  case OP_EQ:
    snprintf(buf, len, "OP_EQ\tR%d\t:%s\t%d", GETARG_A(c),
            mrb_sym2name(mrb, irep->syms[GETARG_B(c)]),
            GETARG_C(c));
    break;

  case OP_STOP:
    snprintf(buf, len, "OP_STOP");
    break;

  case OP_ARRAY:
    snprintf(buf, len, "OP_ARRAY\tR%d\tR%d\t%d",
        GETARG_A(c), GETARG_B(c), GETARG_C(c));
    break;
  case OP_ARYCAT:
    snprintf(buf, len, "OP_ARYCAT\tR%d\tR%d", GETARG_A(c), GETARG_B(c));
    break;
  case OP_ARYPUSH:
    snprintf(buf, len, "OP_ARYPUSH\tR%d\tR%d", GETARG_A(c), GETARG_B(c));
    break;
  case OP_AREF:
    snprintf(buf, len, "OP_AREF\tR%d\tR%d\t%d",
        GETARG_A(c), GETARG_B(c), GETARG_C(c));
    break;
  case OP_APOST:
    snprintf(buf, len, "OP_APOST\tR%d\t%d\t%d",
        GETARG_A(c), GETARG_B(c), GETARG_C(c));
    break;
  case OP_STRING:
    {
      mrb_value v = irep->pool[GETARG_Bx(c)];
      mrb_value s = mrb_str_dump(mrb,
          mrb_str_new(mrb, RSTRING_PTR(v), RSTRING_LEN(v)));
      snprintf(buf, len, "OP_STRING\tR%d\tL(%d)\t; %s",
          GETARG_A(c), GETARG_Bx(c), RSTRING_PTR(s));
    }
    break;
  case OP_STRCAT:
    snprintf(buf, len, "OP_STRCAT\tR%d\tR%d", GETARG_A(c), GETARG_B(c));
    break;
  case OP_HASH:
    snprintf(buf, len, "OP_HASH\tR%d\tR%d\t%d",
        GETARG_A(c), GETARG_B(c), GETARG_C(c));
    break;

  case OP_OCLASS:
    snprintf(buf, len, "OP_OCLASS\tR%d", GETARG_A(c));
    break;
  case OP_CLASS:
    snprintf(buf, len, "OP_CLASS\tR%d\t:%s", GETARG_A(c),
            mrb_sym2name(mrb, irep->syms[GETARG_B(c)]));
    break;
  case OP_MODULE:
    snprintf(buf, len, "OP_MODULE\tR%d\t:%s", GETARG_A(c),
            mrb_sym2name(mrb, irep->syms[GETARG_B(c)]));
    break;
  case OP_EXEC:
    snprintf(buf, len, "OP_EXEC\tR%d\tI(%+d)", GETARG_A(c), GETARG_Bx(c)+1);
    break;
  case OP_SCLASS:
    snprintf(buf, len, "OP_SCLASS\tR%d\tR%d", GETARG_A(c), GETARG_B(c));
    break;
  case OP_TCLASS:
    snprintf(buf, len, "OP_TCLASS\tR%d", GETARG_A(c));
    break;
  case OP_ERR:
    {
      mrb_value v = irep->pool[GETARG_Bx(c)];
      mrb_value s =
        mrb_str_dump(mrb, mrb_str_new(mrb, RSTRING_PTR(v), RSTRING_LEN(v)));
      snprintf(buf, len, "OP_ERR\t%s", RSTRING_PTR(s));
    }
    break;
  case OP_EPUSH:
    snprintf(buf, len, "OP_EPUSH\t:I(%+d)", GETARG_Bx(c)+1);
    break;
  case OP_ONERR:
    snprintf(buf, len, "OP_ONERR\t%03d", i+GETARG_sBx(c));
    break;
  case OP_RESCUE:
    snprintf(buf, len, "OP_RESCUE\tR%d", GETARG_A(c));
    break;
  case OP_RAISE:
    snprintf(buf, len, "OP_RAISE\tR%d", GETARG_A(c));
    break;
  case OP_POPERR:
    snprintf(buf, len, "OP_POPERR\t%d", GETARG_A(c));
    break;
  case OP_EPOP:
    snprintf(buf, len, "OP_EPOP\t%d", GETARG_A(c));
    break;

  default:
    snprintf(buf, len, "OP_unknown %d\t%d\t%d\t%d", GET_OPCODE(c),
            GETARG_A(c), GETARG_B(c), GETARG_C(c));
    break;
  }
  assert(strlen(buf) < sizeof(buf));

  return mrb_str_new_cstr(mrb, buf);
}

//Get instruction profiling information
//Arguments:
// - irepno  - Instruction number
// - iseqoff - Instruction sequence offset
//Returns:
// - Six value array
//  0. File name or method name
//  1. Line number of instruction (if available)
//  2. Execution count of instruction
//  3. Cumulative execution time
//  4. Address
//  5. Code
static mrb_value
mrb_mruby_profiler_get_inst_info(mrb_state *mrb, mrb_value self)
{
  mrb_int irepno=0;
  mrb_int iseqoff=0;
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

//Get irep information
//Arguments:
// - irepno  - Irep number
//Returns:
// - Six value array
//  0. ID of IRep
//  1. Class of method
//  2. Method name
//  3. File name (if available)
//  4. Array of child IDs
//  5. Array of call numbers to children
static mrb_value
mrb_mruby_profiler_get_irep_info(mrb_state *mrb, mrb_value self)
{
  mrb_int           irepno;
  struct prof_irep *profi;
  mrb_value         res;
  mrb_value         ary;
  int i;
  (void) self;

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

  /* 4 Children */
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

//Map C methods onto ruby
void
mrb_mruby_profiler_gem_init(mrb_state* mrb) {
  struct RObject *m;

  //Preallocate results
  result.irep_capa = 64;
  result.irep_tab = (struct prof_irep**)
    mrb_realloc(mrb, result.irep_tab,
        result.irep_capa * sizeof(struct prof_irep *));
  result.irep_num = 0;

  m = (struct RObject *)mrb_define_module(mrb, "Profiler");
  prof_module = mrb_obj_value(m);
  mrb->code_fetch_hook = prof_code_fetch_hook;
  mrb_define_singleton_method(mrb, m, "get_inst_info",
      mrb_mruby_profiler_get_inst_info, MRB_ARGS_REQ(2));
  mrb_define_singleton_method(mrb, m, "get_irep_info",
      mrb_mruby_profiler_get_irep_info, MRB_ARGS_REQ(1));
  mrb_define_singleton_method(mrb, m, "irep_num",
      mrb_mruby_profiler_irep_num, MRB_ARGS_NONE());
  mrb_define_singleton_method(mrb, m, "ilen",
      mrb_mruby_profiler_ilen, MRB_ARGS_REQ(1));
  mrb_define_singleton_method(mrb, m, "read",
      mrb_mruby_profiler_read, MRB_ARGS_REQ(1));
}

void
mrb_mruby_profiler_gem_final(mrb_state* mrb) {
  mrb_funcall(mrb, prof_module, "analyze", 0);
}
