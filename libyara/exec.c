/*
Copyright (c) 2013-2014. The YARA Authors. All Rights Reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#define _GNU_SOURCE

#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#include <yara/arena.h>
#include <yara/endian.h>
#include <yara/exec.h>
#include <yara/limits.h>
#include <yara/error.h>
#include <yara/object.h>
#include <yara/modules.h>
#include <yara/re.h>
#include <yara/strutils.h>
#include <yara/utils.h>
#include <yara/mem.h>
#include <yara/stopwatch.h>


#include <yara.h>


#define MEM_SIZE   MAX_LOOP_NESTING * LOOP_LOCAL_VARS


#define push(x)  \
    if (sp < stack_size) \
    { \
      stack[sp++] = (x); \
    } \
    else \
    { \
      result = ERROR_EXEC_STACK_OVERFLOW; \
      stop = true; \
      break; \
    } \


#define pop(x) { assert(sp > 0); x = stack[--sp]; }

#define is_undef(x) IS_UNDEFINED((x).i)

#define ensure_defined(x) \
    if (is_undef(x)) \
    { \
      r1.i = UNDEFINED; \
      push(r1); \
      break; \
    }


#define little_endian_uint8_t(x)     (x)
#define little_endian_int8_t(x)      (x)
#define little_endian_uint16_t(x)    yr_le16toh(x)
#define little_endian_int16_t(x)     yr_le16toh(x)
#define little_endian_uint32_t(x)    yr_le32toh(x)
#define little_endian_int32_t(x)     yr_le32toh(x)

#define big_endian_uint8_t(x)        (x)
#define big_endian_int8_t(x)         (x)
#define big_endian_uint16_t(x)       yr_be16toh(x)
#define big_endian_int16_t(x)        yr_be16toh(x)
#define big_endian_uint32_t(x)       yr_be32toh(x)
#define big_endian_int32_t(x)        yr_be32toh(x)


#define function_read(type, endianess) \
    int64_t read_##type##_##endianess(YR_MEMORY_BLOCK_ITERATOR* iterator, size_t offset) \
    { \
      YR_MEMORY_BLOCK* block = iterator->first(iterator); \
      while (block != NULL) \
      { \
        if (offset >= block->base && \
            block->size >= sizeof(type) && \
            offset <= block->base + block->size - sizeof(type)) \
        { \
          type result; \
          const uint8_t* data = block->fetch_data(block); \
          if (data == NULL) \
            return UNDEFINED; \
          result = *(type *)(data + offset - block->base); \
          result = endianess##_##type(result); \
          return result; \
        } \
        block = iterator->next(iterator); \
      } \
      return UNDEFINED; \
    };


function_read(uint8_t, little_endian)
function_read(uint16_t, little_endian)
function_read(uint32_t, little_endian)
function_read(int8_t, little_endian)
function_read(int16_t, little_endian)
function_read(int32_t, little_endian)
function_read(uint8_t, big_endian)
function_read(uint16_t, big_endian)
function_read(uint32_t, big_endian)
function_read(int8_t, big_endian)
function_read(int16_t, big_endian)
function_read(int32_t, big_endian)


static const uint8_t* jmp_if(
    int condition,
    const uint8_t* ip)
{
  const uint8_t* result;

  if (condition)
  {
    result = *(const uint8_t**)(ip);
  }
  else
  {
    result = ip + sizeof(uint64_t);
  }

  return result;
}


int yr_execute_code(
    YR_SCAN_CONTEXT* context)
{
  int64_t mem[MEM_SIZE];
  int32_t sp = 0;

  const uint8_t* ip = context->rules->code_start;

  YR_VALUE args[MAX_FUNCTION_ARGS];
  YR_VALUE *stack;
  YR_VALUE r1;
  YR_VALUE r2;
  YR_VALUE r3;

  uint64_t elapsed_time;

  #ifdef PROFILING_ENABLED
  uint64_t start_time;
  YR_RULE* current_rule = NULL;
  #endif

  YR_INIT_RULE_ARGS init_rule_args;

  YR_RULE* rule;
  YR_MATCH* match;
  YR_OBJECT_FUNCTION* function;
  YR_OBJECT** obj_ptr;
  YR_ARENA* obj_arena;

  char* identifier;
  char* args_fmt;

  int i;
  int found;
  int count;
  int result = ERROR_SUCCESS;
  int cycle = 0;
  int tidx = context->tidx;
  int stack_size;

  bool stop = false;

  uint8_t opcode;

  yr_get_configuration(YR_CONFIG_STACK_SIZE, (void*) &stack_size);

  stack = (YR_VALUE*) yr_malloc(stack_size * sizeof(YR_VALUE));

  if (stack == NULL)
    return ERROR_INSUFFICIENT_MEMORY;

  FAIL_ON_ERROR_WITH_CLEANUP(
      yr_arena_create(1024, 0, &obj_arena),
      yr_free(stack));

  #ifdef PROFILING_ENABLED
  start_time = yr_stopwatch_elapsed_us(&context->stopwatch);
  #endif

  while(!stop)
  {
    opcode = *ip;
    ip++;

    switch(opcode)
    {
      case OP_NOP:
        break;

      case OP_HALT:
        assert(sp == 0); // When HALT is reached the stack should be empty.
        stop = true;
        break;

      case OP_PUSH:
        r1.i = *(uint64_t*)(ip);
        ip += sizeof(uint64_t);
        push(r1);
        break;

      case OP_POP:
        pop(r1);
        break;

      case OP_CLEAR_M:
        r1.i = *(uint64_t*)(ip);
        ip += sizeof(uint64_t);
        mem[r1.i] = 0;
        break;

      case OP_ADD_M:
        r1.i = *(uint64_t*)(ip);
        ip += sizeof(uint64_t);
        pop(r2);
        if (!is_undef(r2))
          mem[r1.i] += r2.i;
        break;

      case OP_INCR_M:
        r1.i = *(uint64_t*)(ip);
        ip += sizeof(uint64_t);
        mem[r1.i]++;
        break;

      case OP_PUSH_M:
        r1.i = *(uint64_t*)(ip);
        ip += sizeof(uint64_t);
        r1.i = mem[r1.i];
        push(r1);
        break;

      case OP_POP_M:
        r1.i = *(uint64_t*)(ip);
        ip += sizeof(uint64_t);
        pop(r2);
        mem[r1.i] = r2.i;
        break;

      case OP_SWAPUNDEF:
        r1.i = *(uint64_t*)(ip);
        ip += sizeof(uint64_t);
        pop(r2);

        if (is_undef(r2))
        {
          r1.i = mem[r1.i];
          push(r1);
        }
        else
        {
          push(r2);
        }
        break;

      case OP_JNUNDEF:
        pop(r1);
        push(r1);

        ip = jmp_if(!is_undef(r1), ip);
        break;

      case OP_JLE:
        pop(r2);
        pop(r1);
        push(r1);
        push(r2);

        ip = jmp_if(r1.i <= r2.i, ip);
        break;

      case OP_JTRUE:
        pop(r1);
        push(r1);

        ip = jmp_if(!is_undef(r1) && r1.i, ip);
        break;

      case OP_JFALSE:
        pop(r1);
        push(r1);

        ip = jmp_if(is_undef(r1) || !r1.i, ip);
        break;

      case OP_AND:
        pop(r2);
        pop(r1);

        if (is_undef(r1) || is_undef(r2))
          r1.i = 0;
        else
          r1.i = r1.i && r2.i;

        push(r1);
        break;

      case OP_OR:
        pop(r2);
        pop(r1);

        if (is_undef(r1))
        {
          push(r2);
        }
        else if (is_undef(r2))
        {
          push(r1);
        }
        else
        {
          r1.i = r1.i || r2.i;
          push(r1);
        }
        break;

      case OP_NOT:
        pop(r1);

        if (is_undef(r1))
          r1.i = UNDEFINED;
        else
          r1.i= !r1.i;

        push(r1);
        break;

      case OP_MOD:
        pop(r2);
        pop(r1);
        ensure_defined(r2);
        ensure_defined(r1);
        if (r2.i != 0)
          r1.i = r1.i % r2.i;
        else
          r1.i = UNDEFINED;
        push(r1);
        break;

      case OP_SHR:
        pop(r2);
        pop(r1);
        ensure_defined(r2);
        ensure_defined(r1);
        if (r2.i < 0)
          r1.i = UNDEFINED;
        else if (r2.i < 64)
          r1.i = r1.i >> r2.i;
        else
          r1.i = 0;
        push(r1);
        break;

      case OP_SHL:
        pop(r2);
        pop(r1);
        ensure_defined(r2);
        ensure_defined(r1);
        if (r2.i < 0)
          r1.i = UNDEFINED;
        else if (r2.i < 64)
          r1.i = r1.i << r2.i;
        else
          r1.i = 0;
        push(r1);
        break;

      case OP_BITWISE_NOT:
        pop(r1);
        ensure_defined(r1);
        r1.i = ~r1.i;
        push(r1);
        break;

      case OP_BITWISE_AND:
        pop(r2);
        pop(r1);
        ensure_defined(r2);
        ensure_defined(r1);
        r1.i = r1.i & r2.i;
        push(r1);
        break;

      case OP_BITWISE_OR:
        pop(r2);
        pop(r1);
        ensure_defined(r2);
        ensure_defined(r1);
        r1.i = r1.i | r2.i;
        push(r1);
        break;

      case OP_BITWISE_XOR:
        pop(r2);
        pop(r1);
        ensure_defined(r2);
        ensure_defined(r1);
        r1.i = r1.i ^ r2.i;
        push(r1);
        break;

      case OP_PUSH_RULE:
        rule = *(YR_RULE**)(ip);
        ip += sizeof(uint64_t);
        if (RULE_IS_DISABLED(rule))
          r1.i = UNDEFINED;
        else
          r1.i = rule->t_flags[tidx] & RULE_TFLAGS_MATCH ? 1 : 0;
        push(r1);
        break;

      case OP_INIT_RULE:
        memcpy(&init_rule_args, ip, sizeof(init_rule_args));
        #ifdef PROFILING_ENABLED
        current_rule = init_rule_args.rule;
        #endif
        if (RULE_IS_DISABLED(init_rule_args.rule))
          ip = init_rule_args.jmp_addr;
        else
          ip += sizeof(init_rule_args);
        break;

      case OP_MATCH_RULE:
        pop(r1);
        rule = *(YR_RULE**)(ip);
        ip += sizeof(uint64_t);

        if (!is_undef(r1) && r1.i)
          rule->t_flags[tidx] |= RULE_TFLAGS_MATCH;
        else if (RULE_IS_GLOBAL(rule))
          rule->ns->t_flags[tidx] |= NAMESPACE_TFLAGS_UNSATISFIED_GLOBAL;

        #ifdef PROFILING_ENABLED
        elapsed_time = yr_stopwatch_elapsed_us(&context->stopwatch);
        rule->time_cost += (elapsed_time - start_time);
        start_time = elapsed_time;
        #endif

        assert(sp == 0); // at this point the stack should be empty.
        break;

      case OP_OBJ_LOAD:
        identifier = *(char**)(ip);
        ip += sizeof(uint64_t);

        r1.o = (YR_OBJECT*) yr_hash_table_lookup(
            context->objects_table,
            identifier,
            NULL);

        assert(r1.o != NULL);
        push(r1);
        break;

      case OP_OBJ_FIELD:
        identifier = *(char**)(ip);
        ip += sizeof(uint64_t);

        pop(r1);
        ensure_defined(r1);

        r1.o = yr_object_lookup_field(r1.o, identifier);

        assert(r1.o != NULL);
        push(r1);
        break;

      case OP_OBJ_VALUE:
        pop(r1);
        ensure_defined(r1);

        switch(r1.o->type)
        {
          case OBJECT_TYPE_INTEGER:
            r1.i = r1.o->value.i;
            break;

          case OBJECT_TYPE_FLOAT:
            if (isnan(r1.o->value.d))
              r1.i = UNDEFINED;
            else
              r1.d = r1.o->value.d;
            break;

          case OBJECT_TYPE_STRING:
            if (r1.o->value.ss == NULL)
              r1.i = UNDEFINED;
            else
              r1.ss = r1.o->value.ss;
            break;

          default:
            assert(false);
        }

        push(r1);
        break;

      case OP_INDEX_ARRAY:
        pop(r1);  // index
        pop(r2);  // array

        ensure_defined(r1);
        ensure_defined(r2);
        assert(r2.o->type == OBJECT_TYPE_ARRAY);

        r1.o = yr_object_array_get_item(r2.o, 0, (int) r1.i);

        if (r1.o == NULL)
          r1.i = UNDEFINED;

        push(r1);
        break;

      case OP_LOOKUP_DICT:
        pop(r1);  // key
        pop(r2);  // dictionary

        ensure_defined(r1);
        ensure_defined(r2);
        assert(r2.o->type == OBJECT_TYPE_DICTIONARY);

        r1.o = yr_object_dict_get_item(
            r2.o, 0, r1.ss->c_string);

        if (r1.o == NULL)
          r1.i = UNDEFINED;

        push(r1);
        break;

      case OP_CALL:
        args_fmt = *(char**)(ip);
        ip += sizeof(uint64_t);

        i = (int) strlen(args_fmt);
        count = 0;

        // pop arguments from stack and copy them to args array

        while (i > 0)
        {
          pop(r1);

          if (is_undef(r1))  // count the number of undefined args
            count++;

          args[i - 1] = r1;
          i--;
        }

        pop(r2);
        ensure_defined(r2);

        if (count > 0)
        {
          // if there are undefined args, result for function call
          // is undefined as well.

          r1.i = UNDEFINED;
          push(r1);
          break;
        }

        function = object_as_function(r2.o);
        result = ERROR_INTERNAL_FATAL_ERROR;

        for (i = 0; i < MAX_OVERLOADED_FUNCTIONS; i++)
        {
          if (function->prototypes[i].arguments_fmt == NULL)
            break;

          if (strcmp(function->prototypes[i].arguments_fmt, args_fmt) == 0)
          {
            result = function->prototypes[i].code(args, context, function);
            break;
          }
        }

        // if i == MAX_OVERLOADED_FUNCTIONS at this point no matching
        // prototype was found, but this shouldn't happen.

        assert(i < MAX_OVERLOADED_FUNCTIONS);

        // make a copy of the returned object and push the copy into the stack
        // function->return_obj can't be pushed because it can change in
        // subsequent calls to the same function.

        if (result == ERROR_SUCCESS)
          result = yr_object_copy(function->return_obj, &r1.o);

        // a pointer to the copied object is stored in a arena in order to
        // free the object before exiting yr_execute_code

        if (result == ERROR_SUCCESS)
          result = yr_arena_write_data(obj_arena, &r1.o, sizeof(r1.o), NULL);

        stop = (result != ERROR_SUCCESS);
        push(r1);
        break;

      case OP_FOUND:
        pop(r1);
        r1.i = r1.s->matches[tidx].tail != NULL ? 1 : 0;
        push(r1);
        break;

      case OP_FOUND_AT:
        pop(r2);
        pop(r1);

        if (is_undef(r1))
        {
          r1.i = 0;
          push(r1);
          break;
        }

        match = r2.s->matches[tidx].head;
        r3.i = false;

        while (match != NULL)
        {
          if (r1.i == match->base + match->offset)
          {
            r3.i = true;
            break;
          }

          if (r1.i < match->base + match->offset)
            break;

          match = match->next;
        }

        push(r3);
        break;

      case OP_FOUND_IN:
        pop(r3);
        pop(r2);
        pop(r1);

        ensure_defined(r1);
        ensure_defined(r2);

        match = r3.s->matches[tidx].head;
        r3.i = false;

        while (match != NULL && !r3.i)
        {
          if (match->base + match->offset >= r1.i &&
              match->base + match->offset <= r2.i)
          {
            r3.i = true;
          }

          if (match->base + match->offset > r2.i)
            break;

          match = match->next;
        }

        push(r3);
        break;

      case OP_COUNT:
        pop(r1);
        r1.i = r1.s->matches[tidx].count;
        push(r1);
        break;

      case OP_OFFSET:
        pop(r2);
        pop(r1);

        ensure_defined(r1);

        match = r2.s->matches[tidx].head;
        i = 1;
        r3.i = UNDEFINED;

        while (match != NULL && r3.i == UNDEFINED)
        {
          if (r1.i == i)
            r3.i = match->base + match->offset;

          i++;
          match = match->next;
        }

        push(r3);
        break;

      case OP_LENGTH:
        pop(r2);
        pop(r1);

        ensure_defined(r1);

        match = r2.s->matches[tidx].head;
        i = 1;
        r3.i = UNDEFINED;

        while (match != NULL && r3.i == UNDEFINED)
        {
          if (r1.i == i)
            r3.i = match->match_length;

          i++;
          match = match->next;
        }

        push(r3);
        break;

      case OP_OF:
        found = 0;
        count = 0;
        pop(r1);

        while (!is_undef(r1))
        {
          if (r1.s->matches[tidx].tail != NULL)
            found++;
          count++;
          pop(r1);
        }

        pop(r2);

        if (is_undef(r2))
          r1.i = found >= count ? 1 : 0;
        else
          r1.i = found >= r2.i ? 1 : 0;

        push(r1);
        break;

      case OP_FILESIZE:
        r1.i = context->file_size;
        push(r1);
        break;

      case OP_ENTRYPOINT:
        r1.i = context->entry_point;
        push(r1);
        break;

      case OP_INT8:
        pop(r1);
        r1.i = read_int8_t_little_endian(context->iterator, (size_t) r1.i);
        push(r1);
        break;

      case OP_INT16:
        pop(r1);
        r1.i = read_int16_t_little_endian(context->iterator, (size_t) r1.i);
        push(r1);
        break;

      case OP_INT32:
        pop(r1);
        r1.i = read_int32_t_little_endian(context->iterator, (size_t) r1.i);
        push(r1);
        break;

      case OP_UINT8:
        pop(r1);
        r1.i = read_uint8_t_little_endian(context->iterator, (size_t) r1.i);
        push(r1);
        break;

      case OP_UINT16:
        pop(r1);
        r1.i = read_uint16_t_little_endian(context->iterator, (size_t) r1.i);
        push(r1);
        break;

      case OP_UINT32:
        pop(r1);
        r1.i = read_uint32_t_little_endian(context->iterator, (size_t) r1.i);
        push(r1);
        break;

      case OP_INT8BE:
        pop(r1);
        r1.i = read_int8_t_big_endian(context->iterator, (size_t) r1.i);
        push(r1);
        break;

      case OP_INT16BE:
        pop(r1);
        r1.i = read_int16_t_big_endian(context->iterator, (size_t) r1.i);
        push(r1);
        break;

      case OP_INT32BE:
        pop(r1);
        r1.i = read_int32_t_big_endian(context->iterator, (size_t) r1.i);
        push(r1);
        break;

      case OP_UINT8BE:
        pop(r1);
        r1.i = read_uint8_t_big_endian(context->iterator, (size_t) r1.i);
        push(r1);
        break;

      case OP_UINT16BE:
        pop(r1);
        r1.i = read_uint16_t_big_endian(context->iterator, (size_t) r1.i);
        push(r1);
        break;

      case OP_UINT32BE:
        pop(r1);
        r1.i = read_uint32_t_big_endian(context->iterator, (size_t) r1.i);
        push(r1);
        break;

      case OP_CONTAINS:
        pop(r2);
        pop(r1);

        ensure_defined(r1);
        ensure_defined(r2);

        r1.i = memmem(r1.ss->c_string, r1.ss->length,
                      r2.ss->c_string, r2.ss->length) != NULL;
        push(r1);
        break;

      case OP_IMPORT:
        r1.i = *(uint64_t*)(ip);
        ip += sizeof(uint64_t);

        result = yr_modules_load((char*) r1.p, context);

        if (result != ERROR_SUCCESS)
          stop = true;

        break;

      case OP_MATCHES:

        pop(r2);
        pop(r1);

        ensure_defined(r2);
        ensure_defined(r1);

        if (r1.ss->length == 0)
        {
          r1.i = false;
          push(r1);
          break;
        }

        result = yr_re_exec(
          context,
          (uint8_t*) r2.re->code,
          (uint8_t*) r1.ss->c_string,
          r1.ss->length,
          0,
          r2.re->flags | RE_FLAGS_SCAN,
          NULL,
          NULL,
          &found);

        if (result != ERROR_SUCCESS)
          stop = true;

        r1.i = found >= 0;
        push(r1);
        break;

      case OP_INT_TO_DBL:
        r1.i = *(uint64_t*)(ip);
        ip += sizeof(uint64_t);
        r2 = stack[sp - r1.i];
        if (is_undef(r2))
          stack[sp - r1.i].i = UNDEFINED;
        else
          stack[sp - r1.i].d = (double) r2.i;
        break;

      case OP_STR_TO_BOOL:
        pop(r1);
        ensure_defined(r1);
        r1.i = r1.ss->length > 0;
        push(r1);
        break;

      case OP_INT_EQ:
        pop(r2);
        pop(r1);
        ensure_defined(r2);
        ensure_defined(r1);
        r1.i = r1.i == r2.i;
        push(r1);
        break;

      case OP_INT_NEQ:
        pop(r2);
        pop(r1);
        ensure_defined(r2);
        ensure_defined(r1);
        r1.i = r1.i != r2.i;
        push(r1);
        break;

      case OP_INT_LT:
        pop(r2);
        pop(r1);
        ensure_defined(r2);
        ensure_defined(r1);
        r1.i = r1.i < r2.i;
        push(r1);
        break;

      case OP_INT_GT:
        pop(r2);
        pop(r1);
        ensure_defined(r2);
        ensure_defined(r1);
        r1.i = r1.i > r2.i;
        push(r1);
        break;

      case OP_INT_LE:
        pop(r2);
        pop(r1);
        ensure_defined(r2);
        ensure_defined(r1);
        r1.i = r1.i <= r2.i;
        push(r1);
        break;

      case OP_INT_GE:
        pop(r2);
        pop(r1);
        ensure_defined(r2);
        ensure_defined(r1);
        r1.i = r1.i >= r2.i;
        push(r1);
        break;

      case OP_INT_ADD:
        pop(r2);
        pop(r1);
        ensure_defined(r2);
        ensure_defined(r1);
        r1.i = r1.i + r2.i;
        push(r1);
        break;

      case OP_INT_SUB:
        pop(r2);
        pop(r1);
        ensure_defined(r2);
        ensure_defined(r1);
        r1.i = r1.i - r2.i;
        push(r1);
        break;

      case OP_INT_MUL:
        pop(r2);
        pop(r1);
        ensure_defined(r2);
        ensure_defined(r1);
        r1.i = r1.i * r2.i;
        push(r1);
        break;

      case OP_INT_DIV:
        pop(r2);
        pop(r1);
        ensure_defined(r2);
        ensure_defined(r1);
        if (r2.i != 0)
          r1.i = r1.i / r2.i;
        else
          r1.i = UNDEFINED;
        push(r1);
        break;

      case OP_INT_MINUS:
        pop(r1);
        ensure_defined(r1);
        r1.i = -r1.i;
        push(r1);
        break;

      case OP_DBL_LT:
        pop(r2);
        pop(r1);
        ensure_defined(r2);
        ensure_defined(r1);
        r1.i = r1.d < r2.d;
        push(r1);
        break;

      case OP_DBL_GT:
        pop(r2);
        pop(r1);
        ensure_defined(r2);
        ensure_defined(r1);
        r1.i = r1.d > r2.d;
        push(r1);
        break;

      case OP_DBL_LE:
        pop(r2);
        pop(r1);
        ensure_defined(r2);
        ensure_defined(r1);
        r1.i = r1.d <= r2.d;
        push(r1);
        break;

      case OP_DBL_GE:
        pop(r2);
        pop(r1);
        ensure_defined(r2);
        ensure_defined(r1);
        r1.i = r1.d >= r2.d;
        push(r1);
        break;

      case OP_DBL_EQ:
        pop(r2);
        pop(r1);
        ensure_defined(r2);
        ensure_defined(r1);
        r1.i = r1.d == r2.d;
        push(r1);
        break;

      case OP_DBL_NEQ:
        pop(r2);
        pop(r1);
        ensure_defined(r2);
        ensure_defined(r1);
        r1.i = r1.d != r2.d;
        push(r1);
        break;

      case OP_DBL_ADD:
        pop(r2);
        pop(r1);
        ensure_defined(r2);
        ensure_defined(r1);
        r1.d = r1.d + r2.d;
        push(r1);
        break;

      case OP_DBL_SUB:
        pop(r2);
        pop(r1);
        ensure_defined(r2);
        ensure_defined(r1);
        r1.d = r1.d - r2.d;
        push(r1);
        break;

      case OP_DBL_MUL:
        pop(r2);
        pop(r1);
        ensure_defined(r2);
        ensure_defined(r1);
        r1.d = r1.d * r2.d;
        push(r1);
        break;

      case OP_DBL_DIV:
        pop(r2);
        pop(r1);
        ensure_defined(r2);
        ensure_defined(r1);
        r1.d = r1.d / r2.d;
        push(r1);
        break;

      case OP_DBL_MINUS:
        pop(r1);
        ensure_defined(r1);
        r1.d = -r1.d;
        push(r1);
        break;

      case OP_STR_EQ:
      case OP_STR_NEQ:
      case OP_STR_LT:
      case OP_STR_LE:
      case OP_STR_GT:
      case OP_STR_GE:

        pop(r2);
        pop(r1);

        ensure_defined(r1);
        ensure_defined(r2);

        switch(opcode)
        {
          case OP_STR_EQ:
            r1.i = (sized_string_cmp(r1.ss, r2.ss) == 0);
            break;
          case OP_STR_NEQ:
            r1.i = (sized_string_cmp(r1.ss, r2.ss) != 0);
            break;
          case OP_STR_LT:
            r1.i = (sized_string_cmp(r1.ss, r2.ss) < 0);
            break;
          case OP_STR_LE:
            r1.i = (sized_string_cmp(r1.ss, r2.ss) <= 0);
            break;
          case OP_STR_GT:
            r1.i = (sized_string_cmp(r1.ss, r2.ss) > 0);
            break;
          case OP_STR_GE:
            r1.i = (sized_string_cmp(r1.ss, r2.ss) >= 0);
            break;
        }

        push(r1);
        break;

      default:
        // Unknown instruction, this shouldn't happen.
        assert(false);
    }

    // Check for timeout every 10 instruction cycles. If timeout == 0 it means
    // no timeout at all.

    if (context->timeout > 0L && ++cycle == 10)
    {
      elapsed_time = yr_stopwatch_elapsed_us(&context->stopwatch);

      if (elapsed_time > context->timeout)
      {
        #ifdef PROFILING_ENABLED
        assert(current_rule != NULL);
        current_rule->time_cost += elapsed_time - start_time;
        #endif
        result = ERROR_SCAN_TIMEOUT;
        stop = true;
      }

      cycle = 0;
    }
  }

  obj_ptr = (YR_OBJECT**) yr_arena_base_address(obj_arena);

  while (obj_ptr != NULL)
  {
    yr_object_destroy(*obj_ptr);

    obj_ptr = (YR_OBJECT**) yr_arena_next_address(
        obj_arena, obj_ptr, sizeof(YR_OBJECT*));
  }

  yr_arena_destroy(obj_arena);
  yr_modules_unload_all(context);
  yr_free(stack);

  return result;
}
