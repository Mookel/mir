/* This file is a part of MIR project.
   Copyright (C) 2018, 2019 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

/* C to MIR compiler.  It is a four pass compiler:
   o preprocessor pass generating tokens
   o parsing pass generating AST
   o context pass checking context constraints and augmenting AST
   o generation pass producing MIR

   The compiler implements C11 standard w/o C11 optional features:
   atomic, complex, variable size arrays. */

#include <assert.h>
#include <string.h>
#include <ctype.h>
#include <float.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <setjmp.h>
#include <math.h>
#include "time.h"

#include "c2mir.h"

#ifdef __x86_64__
#include "x86_64/cx86_64.h"
#else
#error "undefined or unsupported generation target for C"
#endif

typedef enum {
  C_alloc_error,
  C_unfinished_comment,
  C_out_of_range_number,
  C_invalid_char_constant,
  C_no_string_end,
  C_invalid_str_constant,
  C_invalid_char,
} C_error_code_t;

DEF_VARR (char);

typedef struct pos {
  const char *fname;
  int lno, ln_pos;
} pos_t;

static const pos_t no_pos = {NULL, -1, -1};

typedef struct c2m_ctx *c2m_ctx_t;

typedef struct stream {
  FILE *f;                        /* the current file, NULL for top-level or string stream */
  const char *fname;              /* NULL only for preprocessor string stream */
  int (*getc_func) (c2m_ctx_t);   /* get function for top-level or string stream */
  VARR (char) * ln;               /* stream current line in reverse order */
  pos_t pos;                      /* includes file name used for reports */
  fpos_t fpos;                    /* file pos to resume file stream */
  const char *start, *curr;       /* non NULL only for string stream  */
  int ifs_length_at_stream_start; /* length of ifs at the stream start */
} * stream_t;

DEF_VARR (stream_t);

typedef const char *char_ptr_t;
DEF_VARR (char_ptr_t);

typedef void *void_ptr_t;
DEF_VARR (void_ptr_t);

typedef struct {
  const char *s;
  size_t len, key, flags;
} str_t;

DEF_HTAB (str_t);

typedef struct token *token_t;
DEF_VARR (token_t);

typedef struct node *node_t;

enum symbol_mode { S_REGULAR, S_TAG, S_LABEL };

DEF_VARR (node_t);

typedef struct {
  enum symbol_mode mode;
  node_t id;
  node_t scope;
  node_t def_node, aux_node;
  VARR (node_t) * defs;
} symbol_t;

DEF_HTAB (symbol_t);

struct init_object {
  struct type *container_type;
  int designator_p;
  union {
    mir_llong curr_index;
    node_t curr_member;
  } u;
};

typedef struct init_object init_object_t;
DEF_VARR (init_object_t);

struct pre_ctx;
struct parse_ctx;
struct check_ctx;
struct gen_ctx;

struct c2m_ctx {
  jmp_buf env;
  struct c2mir_options *options;
  VARR (char_ptr_t) * headers;
  VARR (char_ptr_t) * system_headers;
  const char **header_dirs, **system_header_dirs;
  void (*error_func) (c2m_ctx_t, C_error_code_t code, const char *message);
  VARR (void_ptr_t) * reg_memory;
  VARR (stream_t) * streams; /* stack of streams */
  stream_t cs, eof_s;        /* current stream and stream corresponding the last EOF */
  HTAB (str_t) * str_tab;
  HTAB (str_t) * str_key_tab;
  str_t empty_str;
  unsigned long curr_uid;
  int (*c_getc) (void); /* c2mir interface get function */
  unsigned n_errors, n_warnings;
  VARR (char) * symbol_text, *temp_string;
  VARR (token_t) * recorded_tokens, *buffered_tokens;
  node_t top_scope;
  HTAB (symbol_t) * symbol_tab;
  VARR (node_t) * call_nodes;
  VARR (node_t) * containing_anon_members;
  VARR (init_object_t) * init_object_path;
  struct pre_ctx *pre_ctx;
  struct parse_ctx *parse_ctx;
  struct check_ctx *check_ctx;
  struct gen_ctx *gen_ctx;
};

typedef struct c2m_ctx *c2m_ctx_t;

#define options c2m_ctx->options
#define headers c2m_ctx->headers
#define system_headers c2m_ctx->system_headers
#define header_dirs c2m_ctx->header_dirs
#define system_header_dirs c2m_ctx->system_header_dirs
#define error_func c2m_ctx->error_func
#define reg_memory c2m_ctx->reg_memory
#define str_tab c2m_ctx->str_tab
#define streams c2m_ctx->streams
#define cs c2m_ctx->cs
#define eof_s c2m_ctx->eof_s
#define str_key_tab c2m_ctx->str_key_tab
#define empty_str c2m_ctx->empty_str
#define curr_uid c2m_ctx->curr_uid
#define c_getc c2m_ctx->c_getc
#define n_errors c2m_ctx->n_errors
#define n_warnings c2m_ctx->n_warnings
#define symbol_text c2m_ctx->symbol_text
#define temp_string c2m_ctx->temp_string
#define recorded_tokens c2m_ctx->recorded_tokens
#define buffered_tokens c2m_ctx->buffered_tokens
#define top_scope c2m_ctx->top_scope
#define symbol_tab c2m_ctx->symbol_tab
#define call_nodes c2m_ctx->call_nodes
#define containing_anon_members c2m_ctx->containing_anon_members
#define init_object_path c2m_ctx->init_object_path

static inline c2m_ctx_t *c2m_ctx_loc (MIR_context_t ctx) {
  return (c2m_ctx_t *) ((void **) ctx + 1);
}

static void alloc_error (c2m_ctx_t c2m_ctx, const char *message) {
  error_func (c2m_ctx, C_alloc_error, message);
}

static const int max_nested_includes = 32;

#define MIR_VARR_ERROR alloc_error
#define MIR_HTAB_ERROR MIR_VARR_ERROR

#define FALSE 0
#define TRUE 1

#include "mir-varr.h"
#include "mir-dlist.h"
#include "mir-hash.h"
#include "mir-htab.h"

static mir_size_t round_size (mir_size_t size, mir_size_t round) {
  return (size + round - 1) / round * round;
}

/* Some abbreviations: */
#define NL_HEAD(list) DLIST_HEAD (node_t, list)
#define NL_TAIL(list) DLIST_TAIL (node_t, list)
#define NL_LENGTH(list) DLIST_LENGTH (node_t, list)
#define NL_NEXT(el) DLIST_NEXT (node_t, el)
#define NL_PREV(el) DLIST_PREV (node_t, el)
#define NL_REMOVE(list, el) DLIST_REMOVE (node_t, list, el)
#define NL_APPEND(list, el) DLIST_APPEND (node_t, list, el)
#define NL_PREPEND(list, el) DLIST_PREPEND (node_t, list, el)
#define NL_EL(list, n) DLIST_EL (node_t, list, n)

enum basic_type {
  TP_UNDEF,
  TP_VOID,
  /* Integer types: the first should be BOOL and the last should be
     ULLONG.  The order is important -- do not change it.  */
  TP_BOOL,
  TP_CHAR,
  TP_SCHAR,
  TP_UCHAR,
  TP_SHORT,
  TP_USHORT,
  TP_INT,
  TP_UINT,
  TP_LONG,
  TP_ULONG,
  TP_LLONG,
  TP_ULLONG,
  TP_FLOAT,
  TP_DOUBLE,
  TP_LDOUBLE,
};

#define ENUM_BASIC_INT_TYPE TP_INT
#define ENUM_MIR_INT mir_int

struct type_qual {
  unsigned int const_p : 1, restrict_p : 1, volatile_p : 1, atomic_p : 1; /* Type qualifiers */
};

static const struct type_qual zero_type_qual = {0, 0, 0, 0};

struct arr_type {
  unsigned int static_p : 1;
  struct type *el_type;
  struct type_qual ind_type_qual;
  node_t size;
};

struct func_type {
  unsigned int dots_p : 1;
  struct type *ret_type;
  node_t param_list; /* w/o N_DOTS */
  MIR_item_t proto_item;
};

enum type_mode {
  TM_UNDEF,
  TM_BASIC,
  TM_ENUM,
  TM_PTR,
  TM_STRUCT,
  TM_UNION,
  TM_ARR,
  TM_FUNC,
};

struct type {
  struct type_qual type_qual;
  node_t pos_node;       /* set up and used only for checking type correctness */
  struct type *arr_type; /* NULL or array type before its adjustment */
  /* Raw type size (w/o alignment type itself requirement but with
     element alignment requirements), undefined if mir_size_max.  */
  mir_size_t raw_size;
  int align; /* type align, undefined if < 0  */
  enum type_mode mode;
  char unnamed_anon_struct_union_member_type_p;
  union {
    enum basic_type basic_type;
    node_t tag_type; /* struct/union/enum */
    struct type *ptr_type;
    struct arr_type *arr_type;
    struct func_type *func_type;
  } u;
};

static const struct type ENUM_INT_TYPE = {.raw_size = MIR_SIZE_MAX,
                                          .align = -1,
                                          .mode = TM_BASIC,
                                          .u = {.basic_type = ENUM_BASIC_INT_TYPE}};
/*!*/ static struct type VOID_TYPE
  = {.raw_size = MIR_SIZE_MAX, .align = -1, .mode = TM_BASIC, .u = {.basic_type = TP_VOID}};

static void set_type_layout (c2m_ctx_t c2m_ctx, struct type *type);

static mir_size_t raw_type_size (c2m_ctx_t c2m_ctx, struct type *type) {
  if (type->raw_size == MIR_SIZE_MAX) set_type_layout (c2m_ctx, type);
  assert (type->raw_size != MIR_SIZE_MAX);
  return type->raw_size;
}

#ifdef __x86_64__
#include "x86_64/cx86_64-code.c"
#else
#error "undefined or unsupported generation target for C"
#endif

static void *reg_malloc (c2m_ctx_t c2m_ctx, size_t s) {
  void *mem = malloc (s);

  if (mem == NULL) alloc_error (c2m_ctx, "no memory");
  VARR_PUSH (void_ptr_t, reg_memory, mem);
  return mem;
}

static void reg_memory_pop (c2m_ctx_t c2m_ctx, size_t mark) {
  while (VARR_LENGTH (void_ptr_t, reg_memory) > mark) free (VARR_POP (void_ptr_t, reg_memory));
}

static size_t reg_memory_mark (c2m_ctx_t c2m_ctx) { return VARR_LENGTH (void_ptr_t, reg_memory); }
static void reg_memory_finish (c2m_ctx_t c2m_ctx) {
  reg_memory_pop (c2m_ctx, 0);
  VARR_DESTROY (void_ptr_t, reg_memory);
}

static void reg_memory_init (c2m_ctx_t c2m_ctx) { VARR_CREATE (void_ptr_t, reg_memory, 4096); }

static int char_is_signed_p (void) { return MIR_CHAR_MAX == MIR_SCHAR_MAX; }

enum str_flag { FLAG_EXT = 1, FLAG_C89, FLAG_EXT89 };

static int str_eq (str_t str1, str_t str2) {
  return str1.len == str2.len && memcmp (str1.s, str2.s, str1.len) == 0;
}
static htab_hash_t str_hash (str_t str) { return mir_hash (str.s, str.len, 0x42); }
static int str_key_eq (str_t str1, str_t str2) { return str1.key == str2.key; }
static htab_hash_t str_key_hash (str_t str) { return mir_hash64 (str.key, 0x24); }

static str_t uniq_cstr (c2m_ctx_t c2m_ctx, const char *str);

static void str_init (c2m_ctx_t c2m_ctx) {
  HTAB_CREATE (str_t, str_tab, 1000, str_hash, str_eq);
  HTAB_CREATE (str_t, str_key_tab, 200, str_key_hash, str_key_eq);
  empty_str = uniq_cstr (c2m_ctx, "");
}

static int str_exists_p (c2m_ctx_t c2m_ctx, const char *s, size_t len, str_t *tab_str) {
  str_t el, str;

  str.s = s;
  str.len = len;
  if (!HTAB_DO (str_t, str_tab, str, HTAB_FIND, el)) return FALSE;
  *tab_str = el;
  return TRUE;
}

static str_t str_add (c2m_ctx_t c2m_ctx, const char *s, size_t len, size_t key, size_t flags,
                      int key_p) {
  char *heap_s;
  str_t el, str;

  if (str_exists_p (c2m_ctx, s, len, &el)) return el;
  heap_s = reg_malloc (c2m_ctx, len);
  memcpy (heap_s, s, len);
  str.s = heap_s;
  str.len = len;
  str.key = key;
  str.flags = flags;
  HTAB_DO (str_t, str_tab, str, HTAB_INSERT, el);
  if (key_p) HTAB_DO (str_t, str_key_tab, str, HTAB_INSERT, el);
  return str;
}

static const char *str_find_by_key (c2m_ctx_t c2m_ctx, size_t key) {
  str_t el, str;

  str.key = key;
  if (!HTAB_DO (str_t, str_key_tab, str, HTAB_FIND, el)) return NULL;
  return el.s;
}

static void str_finish (c2m_ctx_t c2m_ctx) {
  HTAB_DESTROY (str_t, str_tab);
  HTAB_DESTROY (str_t, str_key_tab);
}

static void *c2mir_calloc (MIR_context_t ctx, size_t size) {
  void *res = calloc (1, size);

  if (res == NULL) (*MIR_get_error_func (ctx)) (MIR_alloc_error, "no memory");
  return res;
}

void c2mir_init (MIR_context_t ctx) {
  c2m_ctx_t c2m_ctx, *c2m_ctx_ptr = c2m_ctx_loc (ctx);

  *c2m_ctx_ptr = c2m_ctx = c2mir_calloc (ctx, sizeof (struct c2m_ctx));
  reg_memory_init (c2m_ctx);
  str_init (c2m_ctx);
}

void c2mir_finish (MIR_context_t ctx) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);

  str_finish (c2m_ctx);
  reg_memory_finish (c2m_ctx);
  free (c2m_ctx);
}

/* New Page */

/* ------------------------- Parser Start ------------------------------ */

/* Parser is manually written parser with back-tracing to keep original
   grammar close to C11 standard grammar as possible.  It has a
   rudimentary syntax error recovery based on stop symbols ';' and
   '}'.  The input is parse tokens and the output is the following AST
   nodes (the AST root is transl_unit):

expr : N_I | N_L | N_LL | N_U | N_UL | N_ULL | N_F | N_D | N_LD | N_CH | N_STR | N_ID
     | N_ADD (expr) | N_SUB (expr) | N_ADD (expr, expr) | N_SUB (expr, expr)
     | N_MUL (expr, expr) | N_DIV (expr, expr) | N_MOD (expr, expr)
     | N_LSH (expr, expr) | N_RSH (expr, expr)
     | N_NOT (expr) | N_BITWISE_NOT (expr)
     | N_INC (expr) | N_DEC (expr) | N_POST_INC (expr)| N_POST_DEC (expr)
     | N_ALIGNOF (type_name?) | N_SIZEOF (type_name) | N_EXPR_SIZEOF (expr)
     | N_CAST (type_name, expr) | N_COMMA (expr, expr) | N_ANDAND (expr, expr)
     | N_OROR (expr, expr) | N_EQ (expr, expr) | N_NE (expr, expr)
     | N_LT (expr, expr) | N_LE (expr, expr) | N_GT (expr, expr) | N_GE (expr, expr)
     | N_AND (expr, expr) | N_OR (expr, expr) | N_XOR (expr, expr)
     | N_ASSIGN (expr, expr) | N_ADD_ASSIGN (expr, expr) | N_SUB_ASSIGN (expr, expr)
     | N_MUL_ASSIGN (expr, expr) | N_DIV_ASSIGN (expr, expr) | N_MOD_ASSIGN (expr, expr)
     | N_LSH_ASSIGN (expr, expr) | N_RSH_ASSIGN (expr, expr)
     | N_AND_ASSIGN (expr, expr) | N_OR_ASSIGN (expr, expr) | N_XOR_ASSIGN (expr, expr)
     | N_DEREF (expr) | | N_ADDR (expr) | N_IND (expr, expr) | N_FIELD (expr, N_ID)
     | N_DEREF_FIELD (expr, N_ID) | N_COND (expr, expr, expr)
     | N_COMPOUND_LITERAL (type_name, initializer) | N_CALL (expr, N_LIST:(expr)*)
     | N_GENERIC (expr, N_LIST:(N_GENERIC_ASSOC (type_name?, expr))+ )
label: N_CASE(expr) | N_CASE(expr,expr) | N_DEFAULT | N_LABEL(N_ID)
stmt: compound_stmt | N_IF(N_LIST:(label)*, expr, stmt, stmt?)
    | N_SWITCH(N_LIST:(label)*, expr, stmt) | (N_WHILE|N_DO) (N_LIST:(label)*, expr, stmt)
    | N_FOR(N_LIST:(label)*,(N_LIST: declaration+ | expr)?, expr?, expr?, stmt)
    | N_GOTO(N_LIST:(label)*, N_ID) | (N_CONTINUE|N_BREAK) (N_LIST:(label)*)
    | N_RETURN(N_LIST:(label)*, expr?) | N_EXPR(N_LIST:(label)*, expr)
compound_stmt: N_BLOCK(N_LIST:(label)*, N_LIST:(declaration | stmt)*)
declaration: N_SPEC_DECL(N_SHARE(declaration_specs), declarator?, initializer?) | st_assert
st_assert: N_ST_ASSERT(const_expr, N_STR)
declaration_specs: N_LIST:(align_spec|sc_spec|type_qual|func_spec|type_spec)*
align_spec: N_ALIGNAS(type_name|const_expr)
sc_spec: N_TYPEDEF|N_EXTERN|N_STATIC|N_AUTO|N_REGISTER|N_THREAD_LOCAL
type_qual: N_CONST|N_RESTRICT|N_VOLATILE|N_ATOMIC
func_spec: N_INLINE|N_NO_RETURN
type_spec: N_VOID|N_CHAR|N_SHORT|N_INT|N_LONG|N_FLOAT|N_DOUBLE|N_SIGNED|N_UNSIGNED|N_BOOL
         | (N_STRUCT|N_UNION) (N_ID?, struct_declaration_list?)
         | N_ENUM(N_ID?, N_LIST?: N_ENUM_COST(N_ID, const_expr?)*) | typedef_name
struct_declaration_list: N_LIST: struct_declaration*
struct_declaration: st_assert | N_MEMBER(N_SHARE(spec_qual_list), declarator?, const_expr?)
spec_qual_list: N_LIST:(type_qual|type_spec)*
declarator: the same as direct declarator
direct_declarator: N_DECL(N_ID,
                          N_LIST:(N_POINTER(type_qual_list) | N_FUNC(id_list|parameter_list)
                                            | N_ARR(N_STATIC?, type_qual_list,
                                                    (assign_expr|N_STAR)?))*)
pointer: N_LIST: N_POINTER(type_qual_list)*
type_qual_list : N_LIST: type_qual*
parameter_type_list: N_LIST:(N_SPEC_DECL(declaration_specs, declarator, ignore)
                             | N_TYPE(declaration_specs, abstract_declarator))+ [N_DOTS]
id_list: N_LIST: N_ID*
initializer: assign_expr | initialize_list
initialize_list: N_LIST: N_INIT(N_LIST:(const_expr | N_FIELD_ID (N_ID))* initializer)+
type_name: N_TYPE(spec_qual_list, abstract_declarator)
abstract_declarator: the same as abstract direct declarator
abstract_direct_declarator: N_DECL(ignore,
                                   N_LIST:(N_POINTER(type_qual_list) | N_FUNC(parameter_list)
                                           | N_ARR(N_STATIC?, type_qual_list,
                                                   (assign_expr|N_STAR)?))*)
typedef_name: N_ID
transl_unit: N_MODULE(N_LIST:(declaration
                              | N_FUNC_DEF(declaration_specs, declarator,
                                           N_LIST: declaration*, compound_stmt))*)

Here ? means it can be N_IGNORE, * means 0 or more elements in the list, + means 1 or more.

*/

#define REP_SEP ,
#define T_EL(t) T_##t
typedef enum {
  T_NUMBER = 256,
  REP8 (T_EL, CH, STR, ID, ASSIGN, DIVOP, ADDOP, SH, CMP),
  REP8 (T_EL, EQNE, ANDAND, OROR, INCDEC, ARROW, UNOP, DOTS, BOOL),
  REP8 (T_EL, COMPLEX, ALIGNOF, ALIGNAS, ATOMIC, GENERIC, NO_RETURN, STATIC_ASSERT, THREAD_LOCAL),
  REP8 (T_EL, THREAD, AUTO, BREAK, CASE, CHAR, CONST, CONTINUE, DEFAULT),
  REP8 (T_EL, DO, DOUBLE, ELSE, ENUM, EXTERN, FLOAT, FOR, GOTO),
  REP8 (T_EL, IF, INLINE, INT, LONG, REGISTER, RESTRICT, RETURN, SHORT),
  REP8 (T_EL, SIGNED, SIZEOF, STATIC, STRUCT, SWITCH, TYPEDEF, TYPEOF, UNION),
  REP5 (T_EL, UNSIGNED, VOID, VOLATILE, WHILE, EOFILE),
  /* tokens existing in preprocessor only: */
  T_HEADER,         /* include header */
  T_NO_MACRO_IDENT, /* ??? */
  T_DBLNO,          /* ## */
  T_PLM,
  T_RDBLNO, /* placemarker, ## in replacement list */
  T_BOA,    /* begin of argument */
  T_EOA,
  T_EOR, /* end of argument and macro replacement */
  T_EOP, /* end of processing */
  T_EOU, /* end of translation unit */
} token_code_t;

static token_code_t FIRST_KW = T_BOOL, LAST_KW = T_WHILE;

#define NODE_EL(n) N_##n

typedef enum {
  REP8 (NODE_EL, IGNORE, I, L, LL, U, UL, ULL, F),
  REP8 (NODE_EL, D, LD, CH, STR, ID, COMMA, ANDAND, OROR),
  REP8 (NODE_EL, EQ, NE, LT, LE, GT, GE, ASSIGN, BITWISE_NOT),
  REP8 (NODE_EL, NOT, AND, AND_ASSIGN, OR, OR_ASSIGN, XOR, XOR_ASSIGN, LSH),
  REP8 (NODE_EL, LSH_ASSIGN, RSH, RSH_ASSIGN, ADD, ADD_ASSIGN, SUB, SUB_ASSIGN, MUL),
  REP8 (NODE_EL, MUL_ASSIGN, DIV, DIV_ASSIGN, MOD, MOD_ASSIGN, IND, FIELD, ADDR),
  REP8 (NODE_EL, DEREF, DEREF_FIELD, COND, INC, DEC, POST_INC, POST_DEC, ALIGNOF),
  REP8 (NODE_EL, SIZEOF, EXPR_SIZEOF, CAST, COMPOUND_LITERAL, CALL, GENERIC, GENERIC_ASSOC, IF),
  REP8 (NODE_EL, SWITCH, WHILE, DO, FOR, GOTO, CONTINUE, BREAK, RETURN),
  REP8 (NODE_EL, EXPR, BLOCK, CASE, DEFAULT, LABEL, LIST, SPEC_DECL, SHARE),
  REP8 (NODE_EL, TYPEDEF, EXTERN, STATIC, AUTO, REGISTER, THREAD_LOCAL, DECL, VOID),
  REP8 (NODE_EL, CHAR, SHORT, INT, LONG, FLOAT, DOUBLE, SIGNED, UNSIGNED),
  REP8 (NODE_EL, BOOL, STRUCT, UNION, ENUM, ENUM_CONST, MEMBER, CONST, RESTRICT),
  REP8 (NODE_EL, VOLATILE, ATOMIC, INLINE, NO_RETURN, ALIGNAS, FUNC, STAR, POINTER),
  REP8 (NODE_EL, DOTS, ARR, INIT, FIELD_ID, TYPE, ST_ASSERT, FUNC_DEF, MODULE),
} node_code_t;

#undef REP_SEP

DEF_DLIST_LINK (node_t);
DEF_DLIST_TYPE (node_t);

struct node {
  node_code_t code;
  unsigned long uid;
  pos_t pos;
  DLIST_LINK (node_t) op_link;
  DLIST (node_t) ops;
  union {
    str_t s;
    mir_char ch;
    mir_long l;
    mir_llong ll;
    mir_ulong ul;
    mir_ullong ull;
    mir_float f;
    mir_double d;
    mir_ldouble ld;
    node_t scope;
  } u;
  void *attr;
};

DEF_DLIST_CODE (node_t, op_link);

struct token {
  token_code_t code : 16;
  int processed_p : 16;
  pos_t pos;
  node_code_t node_code;
  node_t node;
  const char *repr;
};

static node_t add_pos (node_t n, pos_t p) {
  if (n->pos.lno < 0) n->pos = p;
  return n;
}

static node_t op_append (node_t n, node_t op) {
  NL_APPEND (n->ops, op);
  return add_pos (n, op->pos);
}

static node_t op_prepend (node_t n, node_t op) {
  NL_PREPEND (n->ops, op);
  return add_pos (n, op->pos);
}

static void op_flat_append (node_t n, node_t op) {
  if (op->code != N_LIST) {
    op_append (n, op);
    return;
  }
  for (node_t next_el, el = NL_HEAD (op->ops); el != NULL; el = next_el) {
    next_el = NL_NEXT (el);
    NL_REMOVE (op->ops, el);
    op_append (n, el);
  }
}

static node_t new_node (c2m_ctx_t c2m_ctx, node_code_t nc) {
  node_t n = reg_malloc (c2m_ctx, sizeof (struct node));

  n->code = nc;
  n->uid = curr_uid++;
  DLIST_INIT (node_t, n->ops);
  n->attr = NULL;
  n->pos = no_pos;
  return n;
}

static node_t copy_node (c2m_ctx_t c2m_ctx, node_t n) {
  node_t r = new_node (c2m_ctx, n->code);

  r->pos = n->pos;
  r->u = n->u;
  return r;
}

static node_t new_pos_node (c2m_ctx_t c2m_ctx, node_code_t nc, pos_t p) {
  return add_pos (new_node (c2m_ctx, nc), p);
}
static node_t new_node1 (c2m_ctx_t c2m_ctx, node_code_t nc, node_t op1) {
  return op_append (new_node (c2m_ctx, nc), op1);
}
static node_t new_pos_node1 (c2m_ctx_t c2m_ctx, node_code_t nc, pos_t p, node_t op1) {
  return add_pos (new_node1 (c2m_ctx, nc, op1), p);
}
static node_t new_node2 (c2m_ctx_t c2m_ctx, node_code_t nc, node_t op1, node_t op2) {
  return op_append (new_node1 (c2m_ctx, nc, op1), op2);
}
static node_t new_pos_node2 (c2m_ctx_t c2m_ctx, node_code_t nc, pos_t p, node_t op1, node_t op2) {
  return add_pos (new_node2 (c2m_ctx, nc, op1, op2), p);
}
static node_t new_node3 (c2m_ctx_t c2m_ctx, node_code_t nc, node_t op1, node_t op2, node_t op3) {
  return op_append (new_node2 (c2m_ctx, nc, op1, op2), op3);
}
static node_t new_pos_node3 (c2m_ctx_t c2m_ctx, node_code_t nc, pos_t p, node_t op1, node_t op2,
                             node_t op3) {
  return add_pos (new_node3 (c2m_ctx, nc, op1, op2, op3), p);
}
static node_t new_node4 (c2m_ctx_t c2m_ctx, node_code_t nc, node_t op1, node_t op2, node_t op3,
                         node_t op4) {
  return op_append (new_node3 (c2m_ctx, nc, op1, op2, op3), op4);
}
static node_t new_pos_node4 (c2m_ctx_t c2m_ctx, node_code_t nc, pos_t p, node_t op1, node_t op2,
                             node_t op3, node_t op4) {
  return add_pos (new_node4 (c2m_ctx, nc, op1, op2, op3, op4), p);
}
static node_t new_ch_node (c2m_ctx_t c2m_ctx, int ch, pos_t p) {
  node_t n = new_pos_node (c2m_ctx, N_CH, p);

  n->u.ch = ch;
  return n;
}
static node_t new_i_node (c2m_ctx_t c2m_ctx, long l, pos_t p) {
  node_t n = new_pos_node (c2m_ctx, N_I, p);

  n->u.l = l;
  return n;
}
static node_t new_l_node (c2m_ctx_t c2m_ctx, long l, pos_t p) {
  node_t n = new_pos_node (c2m_ctx, N_L, p);

  n->u.l = l;
  return n;
}
static node_t new_ll_node (c2m_ctx_t c2m_ctx, long long ll, pos_t p) {
  node_t n = new_pos_node (c2m_ctx, N_LL, p);

  n->u.ll = ll;
  return n;
}
static node_t new_u_node (c2m_ctx_t c2m_ctx, unsigned long ul, pos_t p) {
  node_t n = new_pos_node (c2m_ctx, N_U, p);

  n->u.ul = ul;
  return n;
}
static node_t new_ul_node (c2m_ctx_t c2m_ctx, unsigned long ul, pos_t p) {
  node_t n = new_pos_node (c2m_ctx, N_UL, p);

  n->u.ul = ul;
  return n;
}
static node_t new_ull_node (c2m_ctx_t c2m_ctx, unsigned long long ull, pos_t p) {
  node_t n = new_pos_node (c2m_ctx, N_ULL, p);

  n->u.ull = ull;
  return n;
}
static node_t new_f_node (c2m_ctx_t c2m_ctx, float f, pos_t p) {
  node_t n = new_pos_node (c2m_ctx, N_F, p);

  n->u.f = f;
  return n;
}
static node_t new_d_node (c2m_ctx_t c2m_ctx, double d, pos_t p) {
  node_t n = new_pos_node (c2m_ctx, N_D, p);

  n->u.d = d;
  return n;
}
static node_t new_ld_node (c2m_ctx_t c2m_ctx, long double ld, pos_t p) {
  node_t n = new_pos_node (c2m_ctx, N_LD, p);

  n->u.ld = ld;
  return n;
}
static node_t new_str_node (c2m_ctx_t c2m_ctx, node_code_t nc, str_t s, pos_t p) {
  node_t n = new_pos_node (c2m_ctx, nc, p);

  n->u.s = s;
  return n;
}

static node_t get_op (node_t n, int nop) {
  n = NL_HEAD (n->ops);
  for (; nop > 0; nop--) n = NL_NEXT (n);
  return n;
}

static str_t uniq_cstr (c2m_ctx_t c2m_ctx, const char *str) {
  return str_add (c2m_ctx, str, strlen (str) + 1, T_STR, 0, FALSE);
}
static str_t uniq_str (c2m_ctx_t c2m_ctx, const char *str, size_t len) {
  return str_add (c2m_ctx, str, len, T_STR, 0, FALSE);
}

static token_t new_token (c2m_ctx_t c2m_ctx, pos_t pos, const char *repr, int token_code,
                          node_code_t node_code) {
  token_t token = reg_malloc (c2m_ctx, sizeof (struct token));

  token->code = token_code;
  token->processed_p = FALSE;
  token->pos = pos;
  token->repr = repr;
  token->node_code = node_code;
  token->node = NULL;
  return token;
}

static token_t copy_token (c2m_ctx_t c2m_ctx, token_t t) {
  token_t token = new_token (c2m_ctx, t->pos, t->repr, t->code, t->node_code);

  if (t->node != NULL) token->node = copy_node (c2m_ctx, t->node);
  return token;
}

static token_t new_token_wo_uniq_repr (c2m_ctx_t c2m_ctx, pos_t pos, const char *repr,
                                       int token_code, node_code_t node_code) {
  return new_token (c2m_ctx, pos, uniq_cstr (c2m_ctx, repr).s, token_code, node_code);
}

static token_t new_node_token (c2m_ctx_t c2m_ctx, pos_t pos, const char *repr, int token_code,
                               node_t node) {
  token_t token = new_token_wo_uniq_repr (c2m_ctx, pos, repr, token_code, N_IGNORE);

  token->node = node;
  return token;
}

static void print_pos (FILE *f, pos_t pos, int col_p) {
  if (pos.lno < 0) return;
  fprintf (f, "%s:%d", pos.fname, pos.lno);
  if (col_p) fprintf (f, ":%d: ", pos.ln_pos);
}

static const char *get_token_name (c2m_ctx_t c2m_ctx, int token_code) {
  static char buf[30];
  const char *s;

  switch (token_code) {
  case T_NUMBER: return "number";
  case T_CH: return "char constant";
  case T_STR: return "string";
  case T_ID: return "identifier";
  case T_ASSIGN: return "assign op";
  case T_DIVOP: return "/ or %";
  case T_ADDOP: return "+ or -";
  case T_SH: return "shift op";
  case T_CMP: return "comparison op";
  case T_EQNE: return "equality op";
  case T_ANDAND: return "&&";
  case T_OROR: return "||";
  case T_INCDEC: return "++ or --";
  case T_ARROW: return "->";
  case T_UNOP: return "unary op";
  case T_DOTS: return "...";
  default:
    if ((s = str_find_by_key (c2m_ctx, token_code)) != NULL) return s;
    if (isprint (token_code))
      sprintf (buf, "%c", token_code);
    else
      sprintf (buf, "%d", token_code);
    return buf;
  }
}

static void error (c2m_ctx_t c2m_ctx, pos_t pos, const char *format, ...) {
  va_list args;
  FILE *f;

  if ((f = options->message_file) == NULL) return;
  n_errors++;
  va_start (args, format);
  print_pos (f, pos, TRUE);
  vfprintf (f, format, args);
  va_end (args);
  fprintf (f, "\n");
}

static void warning (c2m_ctx_t c2m_ctx, pos_t pos, const char *format, ...) {
  va_list args;
  FILE *f;

  if ((f = options->message_file) == NULL) return;
  n_warnings++;
  va_start (args, format);
  print_pos (f, pos, TRUE);
  fprintf (f, "warning -- ");
  vfprintf (f, format, args);
  va_end (args);
  fprintf (f, "\n");
}

#define TAB_STOP 8

static void init_streams (c2m_ctx_t c2m_ctx) {
  cs = eof_s = NULL;
  VARR_CREATE (stream_t, streams, 32);
}

static void free_stream (stream_t s) {
  VARR_DESTROY (char, s->ln);
  free (s);
}

static void finish_streams (c2m_ctx_t c2m_ctx) {
  if (eof_s != NULL) free_stream (eof_s);
  if (streams == NULL) return;
  while (VARR_LENGTH (stream_t, streams) != 0) free_stream (VARR_POP (stream_t, streams));
  VARR_DESTROY (stream_t, streams);
}

static stream_t new_stream (FILE *f, const char *fname, int (*getc_func) (c2m_ctx_t)) {
  stream_t s = malloc (sizeof (struct stream));

  VARR_CREATE (char, s->ln, 128);
  s->f = f;
  s->fname = s->pos.fname = fname;
  s->pos.lno = 0;
  s->pos.ln_pos = 0;
  s->ifs_length_at_stream_start = 0;
  s->start = s->curr = NULL;
  s->getc_func = getc_func;
  return s;
}

static void add_stream (c2m_ctx_t c2m_ctx, FILE *f, const char *fname,
                        int (*getc_func) (c2m_ctx_t)) {
  assert (fname != NULL);
  if (cs != NULL && cs->f != NULL && cs->f != stdin) {
    fgetpos (cs->f, &cs->fpos);
    fclose (cs->f);
    cs->f = NULL;
  }
  cs = new_stream (f, fname, getc_func);
  VARR_PUSH (stream_t, streams, cs);
}

static int str_getc (c2m_ctx_t c2m_ctx) {
  if (*cs->curr == '\0') return EOF;
  return *cs->curr++;
}

static void add_string_stream (c2m_ctx_t c2m_ctx, const char *pos_fname, const char *str) {
  pos_t pos;

  add_stream (c2m_ctx, NULL, pos_fname, str_getc);
  cs->start = cs->curr = str;
}

static int string_stream_p (stream_t s) { return s->getc_func != NULL; }

static void change_stream_pos (c2m_ctx_t c2m_ctx, pos_t pos) { cs->pos = pos; }

static void remove_trigraphs (c2m_ctx_t c2m_ctx) {
  int len = VARR_LENGTH (char, cs->ln);
  char *addr = VARR_ADDR (char, cs->ln);
  int i, start, to, ch;

  for (i = to = 0; i < len; i++, to++) {
    addr[to] = addr[i];
    for (start = i; i < len && addr[i] == '?'; i++, to++) addr[to] = addr[i];
    if (i >= len) break;
    if (i < start + 2) {
      addr[to] = addr[i];
      continue;
    }
    switch (addr[i]) {
    case '=': ch = '#'; break;
    case '(': ch = '['; break;
    case '/': ch = '\\'; break;
    case ')': ch = ']'; break;
    case '\'': ch = '^'; break;
    case '<': ch = '{'; break;
    case '!': ch = '|'; break;
    case '>': ch = '}'; break;
    case '-': ch = '~'; break;
    default: addr[to] = addr[i]; continue;
    }
    to -= 2;
    addr[to] = ch;
  }
  VARR_TRUNC (char, cs->ln, to);
}

static int ln_get (c2m_ctx_t c2m_ctx) {
  if (cs->f == NULL) return cs->getc_func (c2m_ctx); /* top level */
  return fgetc (cs->f);
}

static char *reverse (VARR (char) * v) {
  char *addr = VARR_ADDR (char, v);
  int i, j, temp, last = (int) VARR_LENGTH (char, v) - 1;

  if (last >= 0 && addr[last] == '\0') last--;
  for (i = last, j = 0; i > j; i--, j++) {
    temp = addr[i];
    addr[i] = addr[j];
    addr[j] = temp;
  }
  return addr;
}

static int get_line (c2m_ctx_t c2m_ctx) { /* translation phase 1 and 2 */
  int c, eof_p = 0;

  VARR_TRUNC (char, cs->ln, 0);
  for (c = ln_get (c2m_ctx); c != EOF && c != '\n'; c = ln_get (c2m_ctx))
    VARR_PUSH (char, cs->ln, c);
  eof_p = c == EOF;
  if (eof_p) {
    if (VARR_LENGTH (char, cs->ln) == 0) return FALSE;
    if (c != '\n')
      (options->pedantic_p ? error : warning) (c2m_ctx, cs->pos, "no end of line at file end");
  }
  remove_trigraphs (c2m_ctx);
  VARR_PUSH (char, cs->ln, '\n');
  reverse (cs->ln);
  return TRUE;
}

static int cs_get (c2m_ctx_t c2m_ctx) {
  size_t len = VARR_LENGTH (char, cs->ln);

  for (;;) {
    if (len == 2 && VARR_GET (char, cs->ln, 1) == '\\') {
      assert (VARR_GET (char, cs->ln, 0) == '\n');
    } else if (len > 0) {
      cs->pos.ln_pos++;
      return VARR_POP (char, cs->ln);
    }
    if (cs->fname == NULL || !get_line (c2m_ctx)) return EOF;
    len = VARR_LENGTH (char, cs->ln);
    assert (len > 0);
    cs->pos.ln_pos = 0;
    cs->pos.lno++;
  }
}

static void cs_unget (c2m_ctx_t c2m_ctx, int c) {
  cs->pos.ln_pos--;
  VARR_PUSH (char, cs->ln, c);
}

static void set_string_stream (c2m_ctx_t c2m_ctx, const char *str, pos_t pos,
                               void (*transform) (const char *, VARR (char) *)) {
  /* read from string str */
  cs = new_stream (NULL, NULL, NULL);
  VARR_PUSH (stream_t, streams, cs);
  cs->pos = pos;
  if (transform != NULL) {
    transform (str, cs->ln);
  } else {
    for (; *str != '\0'; str++) VARR_PUSH (char, cs->ln, *str);
  }
}

static void remove_string_stream (c2m_ctx_t c2m_ctx) {
  assert (cs->f == NULL && cs->f == NULL);
  free_stream (VARR_POP (stream_t, streams));
  cs = VARR_LAST (stream_t, streams);
}

static void set_string_val (c2m_ctx_t c2m_ctx, token_t t, VARR (char) * temp) {
  int i, str_len, curr_c;
  const char *str;

  assert (t->code == T_STR || t->code == T_CH);
  str = t->repr;
  VARR_TRUNC (char, temp, 0);
  str_len = strlen (str);
  assert (str_len >= 2 && (str[0] == '"' || str[0] == '\'') && str[0] == str[str_len - 1]);
  for (i = 1; i < str_len - 1; i++) {
    curr_c = str[i];
    if (curr_c != '\\') {
      VARR_PUSH (char, temp, curr_c);
      continue;
    }
    curr_c = str[++i];
    switch (curr_c) {
    case 'a': curr_c = '\a'; break;
    case 'b': curr_c = '\b'; break;
    case 'n': curr_c = '\n'; break;
    case 'f': curr_c = '\f'; break;
    case 'r': curr_c = '\r'; break;
    case 't': curr_c = '\t'; break;
    case 'v': curr_c = '\v'; break;
    case '\\':
    case '\'':
    case '\?':
    case '\"': break;
    case 'e':
      (options->pedantic_p ? error : warning) (c2m_ctx, t->pos, "non-standard escape sequence \\e");
      curr_c = '\033';
      break;
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7': {
      unsigned long v = curr_c - '0';

      curr_c = str[++i];
      if (!isdigit (curr_c) || curr_c == '8' || curr_c == '9') {
        i--;
      } else {
        v = v * 8 + curr_c - '0';
        curr_c = str[++i];
        if (!isdigit (curr_c) || curr_c == '8' || curr_c == '9')
          i--;
        else
          v = v * 8 + curr_c - '0';
      }
      curr_c = v;
      break;
    }
    case 'x':
    case 'X': {
      int first_p = TRUE;
      unsigned long v = 0;

      for (i++;; i++) {
        curr_c = str[i];
        if (!isxdigit (curr_c)) break;
        first_p = FALSE;
        v *= 16;
        v += (isdigit (curr_c) ? curr_c - '0'
                               : islower (curr_c) ? curr_c - 'a' + 10 : curr_c - 'A' + 10);
      }
      if (first_p)
        error (c2m_ctx, t->pos, "wrong hexadecimal char %c", curr_c);
      else if (v > MIR_UCHAR_MAX)
        (options->pedantic_p ? error : warning) (c2m_ctx, t->pos, "too big hexadecimal char 0x%x",
                                                 v);
      curr_c = v;
      i--;
      break;
    }
    default: error (c2m_ctx, t->pos, "wrong escape char 0x%x", curr_c); curr_c = -1;
    }
    if (t->repr[0] == '\'' || curr_c >= 0) VARR_PUSH (char, temp, curr_c);
  }
  VARR_PUSH (char, temp, '\0');
  if (t->repr[0] == '"')
    t->node->u.s = uniq_str (c2m_ctx, VARR_ADDR (char, temp), VARR_LENGTH (char, temp));
  else if (VARR_LENGTH (char, temp) == 1)
    error (c2m_ctx, t->pos, "empty char constant");
  else
    t->node->u.ch = VARR_GET (char, temp, 0);
}

static token_t new_id_token (c2m_ctx_t c2m_ctx, pos_t pos, const char *id_str) {
  token_t token;
  str_t str = uniq_cstr (c2m_ctx, id_str);

  token = new_token (c2m_ctx, pos, str.s, T_ID, N_IGNORE);
  token->node = new_str_node (c2m_ctx, N_ID, str, pos);
  return token;
}

static token_t get_next_pptoken_1 (c2m_ctx_t c2m_ctx, int header_p) {
  int start_c, curr_c, nl_p, comment_char;
  pos_t pos;

  if (cs->fname != NULL && VARR_LENGTH (token_t, buffered_tokens) != 0)
    return VARR_POP (token_t, buffered_tokens);
  VARR_TRUNC (char, symbol_text, 0);
  for (;;) {
    curr_c = cs_get (c2m_ctx);
    /* Process sequence of white spaces/comments: */
    for (comment_char = -1, nl_p = FALSE;; curr_c = cs_get (c2m_ctx)) {
      switch (curr_c) {
      case '\t':
        cs->pos.ln_pos = round_size (cs->pos.ln_pos, TAB_STOP);
        /* fall through */
      case ' ':
      case '\f':
      case '\r':
      case '\v': break;
      case '\n':
        cs->pos.ln_pos = 0;
        if (comment_char < 0) {
          nl_p = TRUE;
        } else if (comment_char == '/') {
          comment_char = -1;
          nl_p = TRUE;
        }
        break;
      case '/':
        if (comment_char >= 0) break;
        curr_c = cs_get (c2m_ctx);
        if (curr_c == '/' || curr_c == '*') {
          VARR_PUSH (char, symbol_text, '/');
          comment_char = curr_c;
          break;
        }
        cs_unget (c2m_ctx, curr_c);
        curr_c = '/';
        goto end_ws;
      case '*':
        if (comment_char < 0) goto end_ws;
        if (comment_char != '*') break;
        curr_c = cs_get (c2m_ctx);
        if (curr_c == '/') {
          comment_char = -1;
          VARR_PUSH (char, symbol_text, '*');
        } else {
          cs_unget (c2m_ctx, curr_c);
          curr_c = '*';
        }
        break;
      default:
        if (comment_char < 0) goto end_ws;
        if (curr_c == EOF) {
          error_func (c2m_ctx, C_unfinished_comment, "unfinished comment");
          goto end_ws;
        }
        break;
      }
      VARR_PUSH (char, symbol_text, curr_c);
    }
  end_ws:
    if (VARR_LENGTH (char, symbol_text) != 0) {
      cs_unget (c2m_ctx, curr_c);
      VARR_PUSH (char, symbol_text, '\0');
      return new_token_wo_uniq_repr (c2m_ctx, cs->pos, VARR_ADDR (char, symbol_text),
                                     nl_p ? '\n' : ' ', N_IGNORE);
    }
    if (header_p && (curr_c == '<' || curr_c == '\"')) {
      int i, stop;

      pos = cs->pos;
      VARR_TRUNC (char, temp_string, 0);
      for (stop = curr_c == '<' ? '>' : '\"';;) {
        VARR_PUSH (char, symbol_text, curr_c);
        curr_c = cs_get (c2m_ctx);
        VARR_PUSH (char, temp_string, curr_c);
        if (curr_c == stop || curr_c == '\n' || curr_c == EOF) break;
      }
      if (curr_c == stop) {
        VARR_PUSH (char, symbol_text, curr_c);
        VARR_PUSH (char, symbol_text, '\0');
        VARR_POP (char, temp_string);
        VARR_PUSH (char, temp_string, '\0');
        return new_node_token (c2m_ctx, pos, VARR_ADDR (char, symbol_text), T_HEADER,
                               new_str_node (c2m_ctx, N_STR,
                                             uniq_cstr (c2m_ctx, VARR_ADDR (char, temp_string)),
                                             pos));
      } else {
        VARR_PUSH (char, symbol_text, curr_c);
        for (i = 0; i < VARR_LENGTH (char, symbol_text); i++)
          cs_unget (c2m_ctx, VARR_GET (char, symbol_text, i));
        curr_c = (stop == '>' ? '<' : '\"');
      }
    }
    switch (start_c = curr_c) {
    case '\\':
      curr_c = cs_get (c2m_ctx);
      assert (curr_c != '\n');
      cs_unget (c2m_ctx, curr_c);
      return new_token (c2m_ctx, cs->pos, "\\", '\\', N_IGNORE);
    case '~': return new_token (c2m_ctx, cs->pos, "~", T_UNOP, N_BITWISE_NOT);
    case '+':
    case '-':
      pos = cs->pos;
      curr_c = cs_get (c2m_ctx);
      if (curr_c == start_c) {
        if (start_c == '+')
          return new_token (c2m_ctx, pos, "++", T_INCDEC, N_INC);
        else
          return new_token (c2m_ctx, pos, "--", T_INCDEC, N_DEC);
      } else if (curr_c == '=') {
        if (start_c == '+')
          return new_token (c2m_ctx, pos, "+=", T_ASSIGN, N_ADD_ASSIGN);
        else
          return new_token (c2m_ctx, pos, "-=", T_ASSIGN, N_SUB_ASSIGN);
      } else if (start_c == '-' && curr_c == '>') {
        return new_token (c2m_ctx, pos, "->", T_ARROW, N_DEREF_FIELD);
      } else {
        cs_unget (c2m_ctx, curr_c);
        if (start_c == '+')
          return new_token (c2m_ctx, pos, "+", T_ADDOP, N_ADD);
        else
          return new_token (c2m_ctx, pos, "-", T_ADDOP, N_SUB);
      }
      assert (FALSE);
    case '=':
      pos = cs->pos;
      curr_c = cs_get (c2m_ctx);
      if (curr_c == '=') {
        return new_token (c2m_ctx, pos, "==", T_EQNE, N_EQ);
      } else {
        cs_unget (c2m_ctx, curr_c);
        return new_token (c2m_ctx, pos, "=", '=', N_ASSIGN);
      }
      assert (FALSE);
    case '<':
    case '>':
      pos = cs->pos;
      curr_c = cs_get (c2m_ctx);
      if (curr_c == start_c) {
        curr_c = cs_get (c2m_ctx);
        if (curr_c == '=') {
          if (start_c == '<')
            return new_token (c2m_ctx, pos, "<<=", T_ASSIGN, N_LSH_ASSIGN);
          else
            return new_token (c2m_ctx, pos, ">>=", T_ASSIGN, N_RSH_ASSIGN);
        } else {
          cs_unget (c2m_ctx, curr_c);
          if (start_c == '<')
            return new_token (c2m_ctx, pos, "<<", T_SH, N_LSH);
          else
            return new_token (c2m_ctx, pos, ">>", T_SH, N_RSH);
        }
      } else if (curr_c == '=') {
        if (start_c == '<')
          return new_token (c2m_ctx, pos, "<=", T_CMP, N_LE);
        else
          return new_token (c2m_ctx, pos, ">=", T_CMP, N_GE);
      } else if (start_c == '<' && curr_c == ':') {
        return new_token (c2m_ctx, pos, "<:", '[', N_IGNORE);
      } else if (start_c == '<' && curr_c == '%') {
        return new_token (c2m_ctx, pos, "<%", '{', N_IGNORE);
      } else {
        cs_unget (c2m_ctx, curr_c);
        if (start_c == '<')
          return new_token (c2m_ctx, pos, "<", T_CMP, N_LT);
        else
          return new_token (c2m_ctx, pos, ">", T_CMP, N_GT);
      }
      assert (FALSE);
    case '*':
      pos = cs->pos;
      curr_c = cs_get (c2m_ctx);
      if (curr_c == '=') {
        return new_token (c2m_ctx, pos, "*=", T_ASSIGN, N_MUL_ASSIGN);
      } else {
        cs_unget (c2m_ctx, curr_c);
        return new_token (c2m_ctx, pos, "*", '*', N_MUL);
      }
      assert (FALSE);
    case '/':
      pos = cs->pos;
      curr_c = cs_get (c2m_ctx);
      assert (curr_c != '/' && curr_c != '*');
      if (curr_c == '=') return new_token (c2m_ctx, pos, "/=", T_ASSIGN, N_DIV_ASSIGN);
      assert (curr_c != '*' && curr_c != '/'); /* we already processed comments */
      cs_unget (c2m_ctx, curr_c);
      return new_token (c2m_ctx, pos, "/", T_DIVOP, N_DIV);
    case '%':
      pos = cs->pos;
      curr_c = cs_get (c2m_ctx);
      if (curr_c == '=') {
        return new_token (c2m_ctx, pos, "%=", T_ASSIGN, N_MOD_ASSIGN);
      } else if (curr_c == '>') {
        return new_token (c2m_ctx, pos, "%>", '}', N_IGNORE);
      } else if (curr_c == ':') {
        curr_c = cs_get (c2m_ctx);
        if (curr_c != '%') {
          cs_unget (c2m_ctx, curr_c);
          return new_token (c2m_ctx, pos, "%:", '#', N_IGNORE);
        } else {
          curr_c = cs_get (c2m_ctx);
          if (curr_c == ':')
            return new_token (c2m_ctx, pos, "%:%:", T_DBLNO, N_IGNORE);
          else {
            cs_unget (c2m_ctx, '%');
            cs_unget (c2m_ctx, curr_c);
            return new_token (c2m_ctx, pos, "%:", '#', N_IGNORE);
          }
        }
      } else {
        cs_unget (c2m_ctx, curr_c);
        return new_token (c2m_ctx, pos, "%", T_DIVOP, N_MOD);
      }
      assert (FALSE);
    case '&':
    case '|':
      pos = cs->pos;
      curr_c = cs_get (c2m_ctx);
      if (curr_c == '=') {
        if (start_c == '&')
          return new_token (c2m_ctx, pos, "&=", T_ASSIGN, N_AND_ASSIGN);
        else
          return new_token (c2m_ctx, pos, "|=", T_ASSIGN, N_OR_ASSIGN);
      } else if (curr_c == start_c) {
        if (start_c == '&')
          return new_token (c2m_ctx, pos, "&&", T_ANDAND, N_ANDAND);
        else
          return new_token (c2m_ctx, pos, "||", T_OROR, N_OROR);
      } else {
        cs_unget (c2m_ctx, curr_c);
        if (start_c == '&')
          return new_token (c2m_ctx, pos, "&", start_c, N_AND);
        else
          return new_token (c2m_ctx, pos, "|", start_c, N_OR);
      }
      assert (FALSE);
    case '^':
    case '!':
      pos = cs->pos;
      curr_c = cs_get (c2m_ctx);
      if (curr_c == '=') {
        if (start_c == '^')
          return new_token (c2m_ctx, pos, "^=", T_ASSIGN, N_XOR_ASSIGN);
        else
          return new_token (c2m_ctx, pos, "!=", T_EQNE, N_NE);
      } else {
        cs_unget (c2m_ctx, curr_c);
        if (start_c == '^')
          return new_token (c2m_ctx, pos, "^", '^', N_XOR);
        else
          return new_token (c2m_ctx, pos, "!", T_UNOP, N_NOT);
      }
      assert (FALSE);
    case ';': return new_token (c2m_ctx, cs->pos, ";", curr_c, N_IGNORE);
    case '?': return new_token (c2m_ctx, cs->pos, "?", curr_c, N_IGNORE);
    case '(': return new_token (c2m_ctx, cs->pos, "(", curr_c, N_IGNORE);
    case ')': return new_token (c2m_ctx, cs->pos, ")", curr_c, N_IGNORE);
    case '{': return new_token (c2m_ctx, cs->pos, "{", curr_c, N_IGNORE);
    case '}': return new_token (c2m_ctx, cs->pos, "}", curr_c, N_IGNORE);
    case ']': return new_token (c2m_ctx, cs->pos, "]", curr_c, N_IGNORE);
    case EOF: {
      pos_t pos = cs->pos;

      assert (eof_s != cs);
      if (eof_s != NULL) free_stream (eof_s);
      if (cs->f != stdin && cs->f != NULL) {
        fclose (cs->f);
        cs->f = NULL;
      }
      eof_s = VARR_POP (stream_t, streams);
      if (VARR_LENGTH (stream_t, streams) == 0) {
        return new_token (c2m_ctx, pos, "<EOU>", T_EOU, N_IGNORE);
      }
      cs = VARR_LAST (stream_t, streams);
      if (cs->f == NULL && cs->fname != NULL && !string_stream_p (cs)) {
        if ((cs->f = fopen (cs->fname, "r")) == NULL) {
          if (options->message_file != NULL)
            fprintf (options->message_file, "cannot reopen file %s -- good bye\n", cs->fname);
          longjmp (c2m_ctx->env, 1);  // ???
        }
        fsetpos (cs->f, &cs->fpos);
      }
      return new_token (c2m_ctx, cs->pos, "<EOF>", T_EOFILE, N_IGNORE);
    }
    case ':':
      curr_c = cs_get (c2m_ctx);
      if (curr_c == '>') {
        return new_token (c2m_ctx, cs->pos, ":>", ']', N_IGNORE);
      } else {
        cs_unget (c2m_ctx, curr_c);
        return new_token (c2m_ctx, cs->pos, ":", ':', N_IGNORE);
      }
    case '#':
      curr_c = cs_get (c2m_ctx);
      if (curr_c == '#') {
        return new_token (c2m_ctx, cs->pos, "##", T_DBLNO, N_IGNORE);
      } else {
        cs_unget (c2m_ctx, curr_c);
        return new_token (c2m_ctx, cs->pos, "#", '#', N_IGNORE);
      }
    case ',': return new_token (c2m_ctx, cs->pos, ",", ',', N_COMMA);
    case '[': return new_token (c2m_ctx, cs->pos, "[", '[', N_IND);
    case '.':
      pos = cs->pos;
      curr_c = cs_get (c2m_ctx);
      if (curr_c == '.') {
        curr_c = cs_get (c2m_ctx);
        if (curr_c == '.') {
          return new_token (c2m_ctx, pos, "...", T_DOTS, N_IGNORE);
        } else {
          cs_unget (c2m_ctx, '.');
          cs_unget (c2m_ctx, curr_c);
          return new_token (c2m_ctx, pos, ".", '.', N_FIELD);
        }
      } else if (!isdigit (curr_c)) {
        cs_unget (c2m_ctx, curr_c);
        return new_token (c2m_ctx, pos, ".", '.', N_FIELD);
      }
      cs_unget (c2m_ctx, curr_c);
      curr_c = '.';
      /* Fall through: */
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9': {
      pos = cs->pos;
      VARR_TRUNC (char, symbol_text, 0);
      for (;;) {
        VARR_PUSH (char, symbol_text, curr_c);
        curr_c = cs_get (c2m_ctx);
        if (curr_c == 'e' || curr_c == 'E' || curr_c == 'p' || curr_c == 'P') {
          int c = cs_get (c2m_ctx);

          if (c == '+' || c == '-') {
            VARR_PUSH (char, symbol_text, curr_c);
            curr_c = c;
          } else {
            cs_unget (c2m_ctx, c);
          }
        } else if (!isdigit (curr_c) && !isalpha (curr_c) && curr_c != '_' && curr_c != '.')
          break;
      }
      VARR_PUSH (char, symbol_text, '\0');
      cs_unget (c2m_ctx, curr_c);
      return new_token_wo_uniq_repr (c2m_ctx, pos, VARR_ADDR (char, symbol_text), T_NUMBER,
                                     N_IGNORE);
    }
    case '\'':
    case '\"': { /* ??? unicode and wchar */
      token_t t;
      int stop = curr_c;

      pos = cs->pos;
      VARR_PUSH (char, symbol_text, curr_c);
      for (curr_c = cs_get (c2m_ctx); curr_c != stop && curr_c != '\n' && curr_c != EOF;
           curr_c = cs_get (c2m_ctx)) {
        VARR_PUSH (char, symbol_text, curr_c);
        if (curr_c != '\\') continue;
        curr_c = cs_get (c2m_ctx);
        if (curr_c == '\n' || curr_c == EOF) break;
        VARR_PUSH (char, symbol_text, curr_c);
      }
      VARR_PUSH (char, symbol_text, curr_c);
      if (curr_c == stop) {
        if (stop == '\'' && VARR_LENGTH (char, symbol_text) == 1)
          error (c2m_ctx, pos, "empty character");
      } else {
        error (c2m_ctx, pos, "unterminated %s", stop == '"' ? "string" : "char");
        VARR_PUSH (char, symbol_text, stop);
      }
      VARR_PUSH (char, symbol_text, '\0');
      t = (stop == '\"' ? new_node_token (c2m_ctx, pos, VARR_ADDR (char, symbol_text), T_STR,
                                          new_str_node (c2m_ctx, N_STR, empty_str, pos))
                        : new_node_token (c2m_ctx, pos, VARR_ADDR (char, symbol_text), T_CH,
                                          new_ch_node (c2m_ctx, ' ', pos)));
      set_string_val (c2m_ctx, t, symbol_text);
      return t;
    }
    default:
      if (isalpha (curr_c) || curr_c == '_') {
        pos = cs->pos;
        do {
          VARR_PUSH (char, symbol_text, curr_c);
          curr_c = cs_get (c2m_ctx);
        } while (isalnum (curr_c) || curr_c == '_');
        cs_unget (c2m_ctx, curr_c);
        VARR_PUSH (char, symbol_text, '\0');
        return new_id_token (c2m_ctx, pos, VARR_ADDR (char, symbol_text));
      } else {
        VARR_PUSH (char, symbol_text, curr_c);
        VARR_PUSH (char, symbol_text, '\0');
        return new_token_wo_uniq_repr (c2m_ctx, pos, VARR_ADDR (char, symbol_text), curr_c,
                                       N_IGNORE);
      }
    }
  }
}

static token_t get_next_pptoken (c2m_ctx_t c2m_ctx) { return get_next_pptoken_1 (c2m_ctx, FALSE); }

static token_t get_next_include_pptoken (c2m_ctx_t c2m_ctx) {
  return get_next_pptoken_1 (c2m_ctx, TRUE);
}

#ifdef C2MIR_PREPRO_DEBUG
static const char *get_token_str (token_t t) {
  switch (t->code) {
  case T_EOFILE: return "EOF";
  case T_DBLNO: return "DBLNO";
  case T_PLM: return "PLM";
  case T_RDBLNO: return "RDBLNO";
  case T_BOA: return "BOA";
  case T_EOA: return "EOA";
  case T_EOR: return "EOR";
  case T_EOP: return "EOP";
  case T_EOU: return "EOU";
  default: return t->repr;
  }
}
#endif

static void unget_next_pptoken (c2m_ctx_t c2m_ctx, token_t t) {
  VARR_PUSH (token_t, buffered_tokens, t);
}

static const char *stringify (const char *str, VARR (char) * to) {
  VARR_TRUNC (char, to, 0);
  VARR_PUSH (char, to, '"');
  for (; *str != '\0'; str++) {
    if (*str == '\"' || *str == '\\') VARR_PUSH (char, to, '\\');
    VARR_PUSH (char, to, *str);
  }
  VARR_PUSH (char, to, '"');
  return VARR_ADDR (char, to);
}

static void destringify (const char *repr, VARR (char) * to) {
  int i, repr_len = strlen (repr);

  VARR_TRUNC (char, to, 0);
  if (repr_len == 0) return;
  i = repr[0] == '"' ? 1 : 0;
  if (i == 1 && repr_len == 1) return;
  if (repr[repr_len - 1] == '"') repr_len--;
  for (; i < repr_len; i++)
    if (repr[i] != '\\' || i + 1 >= repr_len || (repr[i + 1] != '\\' && repr[i + 1] != '"'))
      VARR_PUSH (char, to, repr[i]);
}

/* TS - vector, T defines position for empty vector */
static token_t token_stringify (c2m_ctx_t c2m_ctx, token_t t, VARR (token_t) * ts) {
  int i;

  if (VARR_LENGTH (token_t, ts) != 0) t = VARR_GET (token_t, ts, 0);
  t = new_node_token (c2m_ctx, t->pos, "", T_STR, new_str_node (c2m_ctx, N_STR, empty_str, t->pos));
  VARR_TRUNC (char, temp_string, 0);
  for (const char *s = t->repr; *s != 0; s++) VARR_PUSH (char, temp_string, *s);
  VARR_PUSH (char, temp_string, '"');
  for (i = 0; i < VARR_LENGTH (token_t, ts); i++)
    if (VARR_GET (token_t, ts, i)->code == ' ' || VARR_GET (token_t, ts, i)->code == '\n') {
      VARR_PUSH (char, temp_string, ' ');
    } else {
      for (const char *s = VARR_GET (token_t, ts, i)->repr; *s != 0; s++) {
        int c = VARR_LENGTH (token_t, ts) == i + 1 ? '\0' : VARR_GET (token_t, ts, i + 1)->repr[0];

        /* It is an implementation defined behaviour analogous GCC/Clang (see set_string_val): */
        if (*s == '\"'
            || (*s == '\\' && c != '\\' && c != 'a' && c != 'b' && c != 'f' && c != 'n' && c != 'r'
                && c != 'v' && c != 't' && c != '?' && c != 'e' && !('0' <= c && c <= '7')
                && c != 'x' && c != 'X'))
          VARR_PUSH (char, temp_string, '\\');
        VARR_PUSH (char, temp_string, *s);
      }
    }
  VARR_PUSH (char, temp_string, '"');
  VARR_PUSH (char, temp_string, '\0');
  t->repr = uniq_cstr (c2m_ctx, VARR_ADDR (char, temp_string)).s;
  set_string_val (c2m_ctx, t, temp_string);
  return t;
}

static node_t get_int_node_from_repr (c2m_ctx_t c2m_ctx, const char *repr, char **stop, int base,
                                      int uns_p, int long_p, int llong_p, pos_t pos) {
  mir_ullong ull = strtoull (repr, stop, base);

  if (llong_p) {
    if (!uns_p && (base == 10 || ull <= MIR_LLONG_MAX)) return new_ll_node (c2m_ctx, ull, pos);
    return new_ull_node (c2m_ctx, ull, pos);
  }
  if (long_p) {
    if (!uns_p && ull <= MIR_LONG_MAX) return new_l_node (c2m_ctx, ull, pos);
    if (ull <= MIR_ULONG_MAX) return new_ul_node (c2m_ctx, ull, pos);
    if (!uns_p && (base == 10 || ull <= MIR_LLONG_MAX)) return new_ll_node (c2m_ctx, ull, pos);
    return new_ull_node (c2m_ctx, ull, pos);
  }
  if (uns_p) {
    if (ull <= MIR_UINT_MAX) return new_u_node (c2m_ctx, ull, pos);
    if (ull <= MIR_ULONG_MAX) return new_ul_node (c2m_ctx, ull, pos);
    return new_ull_node (c2m_ctx, ull, pos);
  }
  if (ull <= MIR_INT_MAX) return new_i_node (c2m_ctx, ull, pos);
  if (base != 10 && ull <= MIR_UINT_MAX) return new_u_node (c2m_ctx, ull, pos);
  if (ull <= MIR_LONG_MAX) return new_l_node (c2m_ctx, ull, pos);
  if (ull <= MIR_ULONG_MAX) return new_ul_node (c2m_ctx, ull, pos);
  if (base == 10 || ull <= MIR_LLONG_MAX) return new_ll_node (c2m_ctx, ull, pos);
  return new_ull_node (c2m_ctx, ull, pos);
}

static token_t pptoken2token (c2m_ctx_t c2m_ctx, token_t t, int id2kw_p) {
  assert (t->code != T_HEADER && t->code != T_BOA && t->code != T_EOA && t->code != T_EOR
          && t->code != T_EOP && t->code != T_EOFILE && t->code != T_EOU && t->code != T_PLM
          && t->code != T_RDBLNO);
  if (t->code == T_NO_MACRO_IDENT) t->code = T_ID;
  if (t->code == T_ID && id2kw_p) {
    str_t str = str_add (c2m_ctx, t->repr, strlen (t->repr) + 1, T_STR, 0, FALSE);

    if (str.key != T_STR) {
      t->code = str.key;
      t->node_code = N_IGNORE;
      t->node = NULL;
    }
    return t;
  } else if (t->code == ' ' || t->code == '\n') {
    return NULL;
  } else if (t->code == T_NUMBER) {
    int i, base = 10, float_p = FALSE, double_p = FALSE, ldouble_p = FALSE;
    int uns_p = FALSE, long_p = FALSE, llong_p = FALSE;
    const char *repr = t->repr, *start = t->repr;
    char *stop;
    int last = strlen (repr) - 1;

    assert (last >= 0);
    if (repr[0] == '0' && (repr[1] == 'x' || repr[1] == 'X')) {
      base = 16;
    } else if (repr[0] == '0' && (repr[1] == 'b' || repr[1] == 'B')) {
      (options->pedantic_p ? error : warning) (c2m_ctx, t->pos,
                                               "binary number is not a standard: %s", t->repr);
      base = 2;
      start += 2;
    } else if (repr[0] == '0') {
      base = 8;
    }
    for (i = 0; i <= last; i++) {
      if (repr[i] == '.') {
        double_p = TRUE;
      } else if (repr[i] == 'p' || repr[i] == 'P') {
        double_p = TRUE;
      } else if ((repr[i] == 'e' || repr[i] == 'E') && base != 16) {
        double_p = TRUE;
      }
    }
    if (last >= 2
        && (strcmp (&repr[last - 2], "LLU") == 0 || strcmp (&repr[last - 2], "ULL") == 0
            || strcmp (&repr[last - 2], "llu") == 0 || strcmp (&repr[last - 2], "ull") == 0
            || strcmp (&repr[last - 2], "LLu") == 0 || strcmp (&repr[last - 2], "uLL") == 0
            || strcmp (&repr[last - 2], "llU") == 0 || strcmp (&repr[last - 2], "Ull") == 0)) {
      llong_p = uns_p = TRUE;
      last -= 3;
    } else if (last >= 1
               && (strcmp (&repr[last - 1], "LL") == 0 || strcmp (&repr[last - 1], "ll") == 0)) {
      llong_p = TRUE;
      last -= 2;
    } else if (last >= 1
               && (strcmp (&repr[last - 1], "LU") == 0 || strcmp (&repr[last - 1], "UL") == 0
                   || strcmp (&repr[last - 1], "lu") == 0 || strcmp (&repr[last - 1], "ul") == 0
                   || strcmp (&repr[last - 1], "Lu") == 0 || strcmp (&repr[last - 1], "uL") == 0
                   || strcmp (&repr[last - 1], "lU") == 0 || strcmp (&repr[last - 1], "Ul") == 0)) {
      long_p = uns_p = TRUE;
      last -= 2;
    } else if (strcmp (&repr[last], "L") == 0 || strcmp (&repr[last], "l") == 0) {
      long_p = TRUE;
      last--;
    } else if (strcmp (&repr[last], "U") == 0 || strcmp (&repr[last], "u") == 0) {
      uns_p = TRUE;
      last--;
    } else if (double_p && (strcmp (&repr[last], "F") == 0 || strcmp (&repr[last], "f") == 0)) {
      float_p = TRUE;
      double_p = FALSE;
      last--;
    }
    if (double_p) {
      if (uns_p || llong_p) {
        error (c2m_ctx, t->pos, "wrong number: %s", repr);
      } else if (long_p) {
        ldouble_p = TRUE;
        double_p = FALSE;
      }
    }
    errno = 0;
    if (float_p) {
      t->node = new_f_node (c2m_ctx, strtof (start, &stop), t->pos);
    } else if (double_p) {
      t->node = new_d_node (c2m_ctx, strtod (start, &stop), t->pos);
    } else if (ldouble_p) {
      t->node = new_ld_node (c2m_ctx, strtold (start, &stop), t->pos);
    } else {
      t->node
        = get_int_node_from_repr (c2m_ctx, start, &stop, base, uns_p, long_p, llong_p, t->pos);
    }
    if (stop != &repr[last + 1]) {
      if (options->message_file != NULL)
        fprintf (options->message_file, "%s:%s:%s\n", repr, stop, &repr[last + 1]);
      error (c2m_ctx, t->pos, "wrong number: %s", t->repr);
    } else if (errno) {
      if (float_p || double_p || ldouble_p) {
        warning (c2m_ctx, t->pos, "number %s is out of range -- using IEEE infinity", t->repr);
      } else {
        (options->pedantic_p ? error : warning) (c2m_ctx, t->pos, "number %s is out of range",
                                                 t->repr);
      }
    }
  }
  return t;
}

/* --------------------------- Preprocessor -------------------------------- */

typedef struct macro {          /* macro definition: */
  token_t id;                   /* T_ID */
  VARR (token_t) * params;      /* (T_ID)* [N_DOTS], NULL means no params */
  VARR (token_t) * replacement; /* token*, NULL means a standard macro */
  int ignore_p;
} * macro_t;

DEF_VARR (macro_t);
DEF_HTAB (macro_t);

typedef struct ifstate {
  int skip_p, true_p, else_p; /* ??? flags that we are in a else part and in a false part */
  pos_t if_pos;               /* pos for #if and last #else, #elif */
} * ifstate_t;

DEF_VARR (ifstate_t);

typedef VARR (token_t) * token_arr_t;

DEF_VARR (token_arr_t);

typedef struct macro_call {
  macro_t macro;
  /* Var array of arguments, each arg is var array of tokens, NULL for args absence: */
  VARR (token_arr_t) * args;
  int repl_pos;                 /* position in macro replacement */
  VARR (token_t) * repl_buffer; /* LIST:(token nodes)* */
} * macro_call_t;

DEF_VARR (macro_call_t);

struct pre_ctx {
  VARR (token_t) * temp_tokens;
  HTAB (macro_t) * macro_tab;
  VARR (macro_t) * macros;
  VARR (ifstate_t) * ifs; /* stack of ifstates */
  int no_out_p;           /* don't output lexs -- put them into buffer */
  int skip_if_part_p;
  token_t if_id; /* last processed token #if or #elif: used for error messages */
  char date_str[50], time_str[50], date_str_repr[50], time_str_repr[50];
  VARR (token_t) * output_buffer;
  VARR (macro_call_t) * macro_call_stack;
  VARR (token_t) * pre_expr;
  token_t pre_last_token;
  pos_t should_be_pre_pos, actual_pre_pos;
  unsigned long pptokens_num;
};

#define temp_tokens c2m_ctx->pre_ctx->temp_tokens
#define macro_tab c2m_ctx->pre_ctx->macro_tab
#define macros c2m_ctx->pre_ctx->macros
#define ifs c2m_ctx->pre_ctx->ifs
#define no_out_p c2m_ctx->pre_ctx->no_out_p
#define skip_if_part_p c2m_ctx->pre_ctx->skip_if_part_p
#define if_id c2m_ctx->pre_ctx->if_id
#define date_str c2m_ctx->pre_ctx->date_str
#define time_str c2m_ctx->pre_ctx->time_str
#define date_str_repr c2m_ctx->pre_ctx->date_str_repr
#define time_str_repr c2m_ctx->pre_ctx->time_str_repr
#define output_buffer c2m_ctx->pre_ctx->output_buffer
#define macro_call_stack c2m_ctx->pre_ctx->macro_call_stack
#define pre_expr c2m_ctx->pre_ctx->pre_expr
#define pre_last_token c2m_ctx->pre_ctx->pre_last_token
#define should_be_pre_pos c2m_ctx->pre_ctx->should_be_pre_pos
#define actual_pre_pos c2m_ctx->pre_ctx->actual_pre_pos
#define pptokens_num c2m_ctx->pre_ctx->pptokens_num

/* It is a token based prerpocessor.
   It is input preprocessor tokens and output is (parser) tokens */

static void add_to_temp_string (c2m_ctx_t c2m_ctx, const char *str) {
  size_t i, len;

  if ((len = VARR_LENGTH (char, temp_string)) != 0
      && VARR_GET (char, temp_string, len - 1) == '\0') {
    VARR_POP (char, temp_string);
  }
  len = strlen (str);
  for (i = 0; i < len; i++) VARR_PUSH (char, temp_string, str[i]);
  VARR_PUSH (char, temp_string, '\0');
}

static int macro_eq (macro_t macro1, macro_t macro2) {
  return macro1->id->repr == macro2->id->repr;
}

static htab_hash_t macro_hash (macro_t macro) {
  return mir_hash (macro->id->repr, strlen (macro->id->repr), 0x42);
}

static macro_t new_macro (c2m_ctx_t c2m_ctx, token_t id, VARR (token_t) * params,
                          VARR (token_t) * replacement);

static void new_std_macro (c2m_ctx_t c2m_ctx, const char *id_str) {
  new_macro (c2m_ctx, new_id_token (c2m_ctx, no_pos, id_str), NULL, NULL);
}

static void init_macros (c2m_ctx_t c2m_ctx) {
  VARR_CREATE (macro_t, macros, 2048);
  HTAB_CREATE (macro_t, macro_tab, 2048, macro_hash, macro_eq);
  /* Standard macros : */
  new_std_macro (c2m_ctx, "__DATE__");
  new_std_macro (c2m_ctx, "__TIME__");
  new_std_macro (c2m_ctx, "__FILE__");
  new_std_macro (c2m_ctx, "__LINE__");
}

static macro_t new_macro (c2m_ctx_t c2m_ctx, token_t id, VARR (token_t) * params,
                          VARR (token_t) * replacement) {
  macro_t tab_m, m = malloc (sizeof (struct macro));

  m->id = id;
  m->params = params;
  m->replacement = replacement;
  m->ignore_p = FALSE;
  assert (!HTAB_DO (macro_t, macro_tab, m, HTAB_FIND, tab_m));
  HTAB_DO (macro_t, macro_tab, m, HTAB_INSERT, tab_m);
  VARR_PUSH (macro_t, macros, m);
  return m;
}

static void finish_macros (c2m_ctx_t c2m_ctx) {
  if (macros != NULL) {
    while (VARR_LENGTH (macro_t, macros) != 0) {
      macro_t m = VARR_POP (macro_t, macros);

      if (m->params != NULL) VARR_DESTROY (token_t, m->params);
      if (m->replacement != NULL) VARR_DESTROY (token_t, m->replacement);
      free (m);
    }
    VARR_DESTROY (macro_t, macros);
  }
  if (macro_tab != NULL) HTAB_DESTROY (macro_t, macro_tab);
}

static macro_call_t new_macro_call (macro_t m) {
  macro_call_t mc = malloc (sizeof (struct macro_call));

  mc->macro = m;
  mc->repl_pos = 0;
  mc->args = NULL;
  VARR_CREATE (token_t, mc->repl_buffer, 64);
  return mc;
}

static void free_macro_call (macro_call_t mc) {
  VARR_DESTROY (token_t, mc->repl_buffer);
  if (mc->args != NULL) {
    while (VARR_LENGTH (token_arr_t, mc->args) != 0) {
      VARR (token_t) *arg = VARR_POP (token_arr_t, mc->args);
      VARR_DESTROY (token_t, arg);
    }
    VARR_DESTROY (token_arr_t, mc->args);
  }
  free (mc);
}

static ifstate_t new_ifstate (int skip_p, int true_p, int else_p, pos_t if_pos) {
  ifstate_t ifstate = malloc (sizeof (struct ifstate));

  ifstate->skip_p = skip_p;
  ifstate->true_p = true_p;
  ifstate->else_p = else_p;
  ifstate->if_pos = if_pos;
  return ifstate;
}

static void pop_ifstate (c2m_ctx_t c2m_ctx) { free (VARR_POP (ifstate_t, ifs)); }

static void (*pre_out_token_func) (c2m_ctx_t c2m_ctx, token_t);

static void pre_init (MIR_context_t ctx) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);
  time_t t;
  struct tm *tm;

  c2m_ctx->pre_ctx = c2mir_calloc (ctx, sizeof (struct pre_ctx));
  no_out_p = skip_if_part_p = FALSE;
  t = time (NULL);
  tm = localtime (&t);
  if (tm == NULL) {
    strcpy (date_str_repr, "\"Unknown date\"");
    strcpy (time_str_repr, "\"Unknown time\"");
  } else {
    strftime (date_str_repr, sizeof (date_str), "\"%b %d %Y\"", tm);
    strftime (time_str_repr, sizeof (time_str), "\"%H:%M:%S\"", tm);
  }
  strcpy (date_str, date_str_repr + 1);
  date_str[strlen (date_str) - 1] = '\0';
  strcpy (time_str, time_str_repr + 1);
  time_str[strlen (time_str) - 1] = '\0';
  VARR_CREATE (token_t, temp_tokens, 128);
  VARR_CREATE (token_t, output_buffer, 2048);
  init_macros (c2m_ctx);
  VARR_CREATE (ifstate_t, ifs, 512);
  VARR_CREATE (macro_call_t, macro_call_stack, 512);
}

static void pre_finish (c2m_ctx_t c2m_ctx) {
  if (c2m_ctx == NULL || c2m_ctx->pre_ctx == NULL) return;
  if (temp_tokens != NULL) VARR_DESTROY (token_t, temp_tokens);
  if (output_buffer != NULL) VARR_DESTROY (token_t, output_buffer);
  finish_macros (c2m_ctx);
  if (ifs != NULL) {
    while (VARR_LENGTH (ifstate_t, ifs) != 0) pop_ifstate (c2m_ctx);
    VARR_DESTROY (ifstate_t, ifs);
  }
  if (macro_call_stack != NULL) {
    while (VARR_LENGTH (macro_call_t, macro_call_stack) != 0)
      free_macro_call (VARR_POP (macro_call_t, macro_call_stack));
    VARR_DESTROY (macro_call_t, macro_call_stack);
  }
  free (c2m_ctx->pre_ctx);
}

static void add_include_stream (c2m_ctx_t c2m_ctx, const char *fname) {
  FILE *f;

  assert (fname != NULL);
  if ((f = fopen (fname, "r")) == NULL) {
    if (options->message_file != NULL) fprintf (f, "error in opening file %s\n", fname);
    longjmp (c2m_ctx->env, 1);  // ???
  }
  add_stream (c2m_ctx, f, fname, NULL);
  cs->ifs_length_at_stream_start = VARR_LENGTH (ifstate_t, ifs);
}

static void skip_nl (c2m_ctx_t c2m_ctx, token_t t,
                     VARR (token_t) * buffer) { /* skip until new line */
  if (t == NULL) t = get_next_pptoken (c2m_ctx);
  for (; t->code != '\n'; t = get_next_pptoken (c2m_ctx))  // ??>
    if (buffer != NULL) VARR_PUSH (token_t, buffer, t);
  unget_next_pptoken (c2m_ctx, t);
}

static const char *varg = "__VA_ARGS__";

static int find_param (VARR (token_t) * params, const char *name) {
  size_t len;
  token_t param;

  len = VARR_LENGTH (token_t, params);
  if (strcmp (name, varg) == 0 && len != 0 && VARR_LAST (token_t, params)->code == T_DOTS)
    return len - 1;
  for (int i = 0; i < len; i++) {
    param = VARR_GET (token_t, params, i);
    if (strcmp (param->repr, name) == 0) return i;
  }
  return -1;
}

static int params_eq_p (VARR (token_t) * params1, VARR (token_t) * params2) {
  token_t param1, param2;

  if (params1 == NULL || params2 == NULL) return params1 == params2;
  if (VARR_LENGTH (token_t, params1) != VARR_LENGTH (token_t, params2)) return FALSE;
  for (int i = 0; i < VARR_LENGTH (token_t, params1); i++) {
    param1 = VARR_GET (token_t, params1, i);
    param2 = VARR_GET (token_t, params2, i);
    if (strcmp (param1->repr, param2->repr) != 0) return FALSE;
  }
  return TRUE;
}

static int replacement_eq_p (VARR (token_t) * r1, VARR (token_t) * r2) {
  token_t el1, el2;

  if (VARR_LENGTH (token_t, r1) != VARR_LENGTH (token_t, r2)) return FALSE;
  for (int i = 0; i < VARR_LENGTH (token_t, r1); i++) {
    el1 = VARR_GET (token_t, r1, i);
    el2 = VARR_GET (token_t, r2, i);

    if (el1->code == ' ' && el2->code == ' ') return TRUE;
    if (el1->node_code != el2->node_code) return FALSE;
    if (strcmp (el1->repr, el2->repr) != 0) return FALSE;
  }
  return TRUE;
}

static void define (c2m_ctx_t c2m_ctx) {
  VARR (token_t) * repl, *params;
  token_t id, t;
  const char *name;
  macro_t m;
  struct macro macro_struct;

  t = get_next_pptoken (c2m_ctx);                      // ???
  if (t->code == ' ') t = get_next_pptoken (c2m_ctx);  // ??
  if (t->code != T_ID) {
    error (c2m_ctx, t->pos, "no ident after #define: %s", t->repr);
    skip_nl (c2m_ctx, t, NULL);
    return;
  }
  id = t;
  t = get_next_pptoken (c2m_ctx);
  VARR_CREATE (token_t, repl, 64);
  params = NULL;
  if (t->code == '(') {
    VARR_CREATE (token_t, params, 16);
    t = get_next_pptoken (c2m_ctx); /* skip '(' */
    if (t->code == ' ') t = get_next_pptoken (c2m_ctx);
    if (t->code != ')') {
      for (;;) {
        if (t->code == ' ') t = get_next_pptoken (c2m_ctx);
        if (t->code == T_ID) {
          if (find_param (params, t->repr) >= 0)
            error (c2m_ctx, t->pos, "repeated macro parameter %s", t->repr);
          VARR_PUSH (token_t, params, t);
        } else if (t->code == T_DOTS) {
          VARR_PUSH (token_t, params, t);
        } else {
          error (c2m_ctx, t->pos, "macro parameter is expected");
          break;
        }
        t = get_next_pptoken (c2m_ctx);
        if (t->code == ' ') t = get_next_pptoken (c2m_ctx);
        if (t->code == ')') break;
        if (VARR_LAST (token_t, params)->code == T_DOTS) {
          error (c2m_ctx, t->pos, "... is not the last parameter");
          break;
        }
        if (t->code == T_DOTS) continue;
        if (t->code != ',') {
          error (c2m_ctx, t->pos, "missed ,");
          continue;
        }
        t = get_next_pptoken (c2m_ctx);
      }
    }
    for (; t->code != '\n' && t->code != ')';) t = get_next_pptoken (c2m_ctx);
    if (t->code == ')') t = get_next_pptoken (c2m_ctx);
  }
  if (t->code == ' ') t = get_next_pptoken (c2m_ctx);
  for (; t->code != '\n'; t = get_next_pptoken (c2m_ctx)) {
    if (t->code == T_DBLNO) t->code = T_RDBLNO;
    VARR_PUSH (token_t, repl, t);
  }
  unget_next_pptoken (c2m_ctx, t);
  name = id->repr;
  macro_struct.id = id;
  if (!HTAB_DO (macro_t, macro_tab, &macro_struct, HTAB_FIND, m)) {
    if (strcmp (name, "defined") == 0) {
      error (c2m_ctx, id->pos, "macro definition of %s", name);
    } else {
      new_macro (c2m_ctx, id, params, repl);
    }
  } else if (m->replacement == NULL) {
    error (c2m_ctx, id->pos, "standard macro %s redefinition", name);
  } else if (!params_eq_p (m->params, params) || !replacement_eq_p (m->replacement, repl)) {
    error (c2m_ctx, id->pos, "different macro redefinition of %s", name);
  }
}

#ifdef C2MIR_PREPRO_DEBUG
static void print_output_buffer (void) {
  fprintf (stderr, "output buffer:");
  for (size_t i = 0; i < (int) VARR_LENGTH (token_t, output_buffer); i++) {
    fprintf (stderr, " <%s>", get_token_str (VARR_GET (token_t, output_buffer, i)));
  }
  fprintf (stderr, "\n");
}
#endif

static void push_back (c2m_ctx_t c2m_ctx, VARR (token_t) * tokens) {
#ifdef C2MIR_PREPRO_DEBUG
  fprintf (stderr,
           "# push back (macro call depth %d):", VARR_LENGTH (macro_call_t, macro_call_stack));
#endif
  for (int i = (int) VARR_LENGTH (token_t, tokens) - 1; i >= 0; i--) {
#ifdef C2MIR_PREPRO_DEBUG
    fprintf (stderr, " <%s>", get_token_str (VARR_GET (token_t, tokens, i)));
#endif
    unget_next_pptoken (c2m_ctx, VARR_GET (token_t, tokens, i));
  }
#ifdef C2MIR_PREPRO_DEBUG
  fprintf (stderr, "\no");
  print_output_buffer ();
#endif
}

static void copy_and_push_back (c2m_ctx_t c2m_ctx, VARR (token_t) * tokens) {
#ifdef C2MIR_PREPRO_DEBUG
  fprintf (stderr, "# copy & push back (macro call depth %d):",
           VARR_LENGTH (macro_call_t, macro_call_stack));
#endif
  for (int i = (int) VARR_LENGTH (token_t, tokens) - 1; i >= 0; i--) {
#ifdef C2MIR_PREPRO_DEBUG
    fprintf (stderr, " <%s>", get_token_str (VARR_GET (token_t, tokens, i)));
#endif
    unget_next_pptoken (c2m_ctx, copy_token (c2m_ctx, VARR_GET (token_t, tokens, i)));
  }
#ifdef C2MIR_PREPRO_DEBUG
  fprintf (stderr, "\n");
  print_output_buffer ();
#endif
}

static int file_found_p (const char *name) {
  FILE *f;

  if ((f = fopen (name, "r")) == NULL) return FALSE;
  fclose (f);
  return TRUE;
}

static const char *get_full_name (c2m_ctx_t c2m_ctx, const char *base, const char *name,
                                  int dir_base_p) {
  const char *str, *last;
  size_t len;

  VARR_TRUNC (char, temp_string, 0);
  if (base == NULL || *base == '\0') {
    assert (name != NULL && name[0] != '\0');
    return name;
  }
  if (dir_base_p) {
    len = strlen (base);
    assert (len > 0);
    add_to_temp_string (c2m_ctx, base);
    if (base[len - 1] != '/') add_to_temp_string (c2m_ctx, "/");
  } else if ((last = strrchr (base, '/')) == NULL) {
    add_to_temp_string (c2m_ctx, "./");
  } else {
    for (str = base; str <= last; str++) VARR_PUSH (char, temp_string, *str);
    VARR_PUSH (char, temp_string, '\0');
  }
  add_to_temp_string (c2m_ctx, name);
  return VARR_ADDR (char, temp_string);
}

static const char *get_include_fname (c2m_ctx_t c2m_ctx, token_t t) {
  const char *fullname, *name;

  assert (t->code == T_STR || t->code == T_HEADER);
  if ((name = t->node->u.s.s)[0] != '/') {
    if (t->repr[0] == '"') {
      /* Search relative to the current source dir */
      if (cs->fname != NULL) {
        fullname = get_full_name (c2m_ctx, cs->fname, name, FALSE);
        if (file_found_p (fullname)) return uniq_cstr (c2m_ctx, fullname).s;
      }
      for (size_t i = 0; header_dirs[i] != NULL; i++) {
        fullname = get_full_name (c2m_ctx, header_dirs[i], name, TRUE);
        if (file_found_p (fullname)) return uniq_cstr (c2m_ctx, fullname).s;
      }
    }
    for (size_t i = 0; system_header_dirs[i] != NULL; i++) {
      fullname = get_full_name (c2m_ctx, system_header_dirs[i], name, TRUE);
      if (file_found_p (fullname)) return uniq_cstr (c2m_ctx, fullname).s;
    }
  }
  return name;
}

static int digits_p (const char *str) {
  while ('0' <= *str && *str <= '9') str++;
  return *str == '\0';
}

static pos_t check_line_directive_args (c2m_ctx_t c2m_ctx, VARR (token_t) * buffer) {
  size_t i, len = VARR_LENGTH (token_t, buffer);
  token_t *buffer_arr = VARR_ADDR (token_t, buffer);
  const char *fname;
  pos_t pos;
  int lno;
  unsigned long long l;

  if (len == 0) return no_pos;
  i = buffer_arr[0]->code == ' ' ? 1 : 0;
  fname = buffer_arr[i]->pos.fname;
  if (i >= len || buffer_arr[i]->code != T_NUMBER) return no_pos;
  if (!digits_p (buffer_arr[i]->repr)) return no_pos;
  errno = 0;
  lno = l = strtoll (buffer_arr[i]->repr, NULL, 10);
  if (errno || l > ((1ul << 31) - 1))
    error (c2m_ctx, buffer_arr[i]->pos, "#line with too big value: %s", buffer_arr[i]->repr);
  i++;
  if (i < len && buffer_arr[i]->code == ' ') i++;
  if (i < len && buffer_arr[i]->code == T_STR) {
    fname = buffer_arr[i]->node->u.s.s;
    i++;
  }
  if (i == len) {
    pos.fname = fname;
    pos.lno = lno;
    pos.ln_pos = 0;
    return pos;
  }
  return no_pos;
}

static void check_pragma (c2m_ctx_t c2m_ctx, token_t t, VARR (token_t) * tokens) {
  token_t *tokens_arr = VARR_ADDR (token_t, tokens);
  size_t i, tokens_len = VARR_LENGTH (token_t, tokens);

  i = 0;
  if (i < tokens_len && tokens_arr[i]->code == ' ') i++;
  if (i >= tokens_len || tokens_arr[i]->code != T_ID || strcmp (tokens_arr[i]->repr, "STDC") != 0) {
    warning (c2m_ctx, t->pos, "unknown pragma");
    return;
  }
  i++;
  if (i < tokens_len && tokens_arr[i]->code == ' ') i++;
  if (i >= tokens_len || tokens_arr[i]->code != T_ID) {
    error (c2m_ctx, t->pos, "wrong STDC pragma");
    return;
  }
  if (strcmp (tokens_arr[i]->repr, "FP_CONTRACT") != 0
      && strcmp (tokens_arr[i]->repr, "FENV_ACCESS") != 0
      && strcmp (tokens_arr[i]->repr, "CX_LIMITED_RANGE") != 0) {
    error (c2m_ctx, t->pos, "unknown STDC pragma %s", tokens_arr[i]->repr);
    return;
  }
  i++;
  if (i < tokens_len && tokens_arr[i]->code == ' ') i++;
  if (i >= tokens_len || tokens_arr[i]->code != T_ID) {
    error (c2m_ctx, t->pos, "wrong STDC pragma value");
    return;
  }
  if (strcmp (tokens_arr[i]->repr, "ON") != 0 && strcmp (tokens_arr[i]->repr, "OFF") != 0
      && strcmp (tokens_arr[i]->repr, "DEFAULT") != 0) {
    error (c2m_ctx, t->pos, "unknown STDC pragma value", tokens_arr[i]->repr);
    return;
  }
  i++;
  if (i < tokens_len && (tokens_arr[i]->code == ' ' || tokens_arr[i]->code == '\n')) i++;
  if (i < tokens_len) error (c2m_ctx, t->pos, "garbage at STDC pragma end");
}

static void pop_macro_call (c2m_ctx_t c2m_ctx) {
  macro_call_t mc;

  mc = VARR_POP (macro_call_t, macro_call_stack);
#ifdef C2MIR_PREPRO_DEBUG
  fprintf (stderr, "finish macro %s\n", mc->macro->id->repr);
#endif
  mc->macro->ignore_p = FALSE;
  free_macro_call (mc);
}

static void find_args (c2m_ctx_t c2m_ctx, macro_call_t mc) { /* we have just read a parenthesis */
  macro_t m;
  token_t t;
  int va_p, level = 0;
  size_t params_len;
  VARR (token_arr_t) * args;
  VARR (token_t) * arg, *temp_arr;

  m = mc->macro;
  VARR_CREATE (token_arr_t, args, 16);
  VARR_CREATE (token_t, arg, 16);
  params_len = VARR_LENGTH (token_t, m->params);
  va_p = params_len == 1 && VARR_GET (token_t, m->params, 0)->code == T_DOTS;
#ifdef C2MIR_PREPRO_DEBUG
  fprintf (stderr, "# finding args of macro %s call:\n#    arg 0:", m->id->repr);
#endif
  for (;;) {
    t = get_next_pptoken (c2m_ctx);
#ifdef C2MIR_PREPRO_DEBUG
    fprintf (stderr, " <%s>%s", get_token_str (t), t->processed_p ? "*" : "");
#endif
    if (t->code == T_EOR) {
      t = get_next_pptoken (c2m_ctx);
#ifdef C2MIR_PREPRO_DEBUG
      fprintf (stderr, " <%s>", get_token_str (t), t->processed_p ? "*" : "");
#endif
      pop_macro_call (c2m_ctx);
    }
    if (t->code == T_EOFILE || t->code == T_EOU || t->code == T_EOR || t->code == T_BOA
        || t->code == T_EOA)
      break;
    if (level == 0 && t->code == ')') break;
    if (level == 0 && !va_p && t->code == ',') {
      VARR_PUSH (token_arr_t, args, arg);
#ifdef C2MIR_PREPRO_DEBUG
      fprintf (stderr, "\n#    arg %d:", VARR_LENGTH (token_arr_t, args));
#endif
      VARR_CREATE (token_t, arg, 16);
      if (VARR_LENGTH (token_arr_t, args) == params_len - 1
          && strcmp (VARR_GET (token_t, m->params, params_len - 1)->repr, "...") == 0)
        va_p = 1;
    } else {
      VARR_PUSH (token_t, arg, t);
      if (t->code == ')')
        level--;
      else if (t->code == '(')
        level++;
    }
  }
#ifdef C2MIR_PREPRO_DEBUG
  fprintf (stderr, "\n");
#endif
  if (t->code != ')') error (c2m_ctx, t->pos, "unfinished call of macro %s", m->id->repr);
  VARR_PUSH (token_arr_t, args, arg);
  if (params_len == 0 && VARR_LENGTH (token_arr_t, args) == 1) {
    token_arr_t arr = VARR_GET (token_arr_t, args, 0);

    if (VARR_LENGTH (token_t, arr) == 0
        || (VARR_LENGTH (token_t, arr) == 1 && VARR_GET (token_t, arr, 0)->code == ' ')) {
      temp_arr = VARR_POP (token_arr_t, args);
      VARR_DESTROY (token_t, temp_arr);
      mc->args = args;
      return;
    }
  }
  if (VARR_LENGTH (token_arr_t, args) > params_len) {
    t = VARR_GET (token_t, VARR_GET (token_arr_t, args, params_len), 0);
    while (VARR_LENGTH (token_arr_t, args) > params_len) {
      temp_arr = VARR_POP (token_arr_t, args);
      VARR_DESTROY (token_t, temp_arr);
    }
    error (c2m_ctx, t->pos, "too many args for call of macro %s", m->id->repr);
  } else if (VARR_LENGTH (token_arr_t, args) < params_len) {
    for (; VARR_LENGTH (token_arr_t, args) < params_len;) {
      VARR_CREATE (token_t, arg, 16);
      VARR_PUSH (token_arr_t, args, arg);
    }
    error (c2m_ctx, t->pos, "not enough args for call of macro %s", m->id->repr);
  }
  mc->args = args;
}

static token_t token_concat (c2m_ctx_t c2m_ctx, token_t t1, token_t t2) {
  token_t t, next;

  VARR_TRUNC (char, temp_string, 0);
  add_to_temp_string (c2m_ctx, t1->repr);
  add_to_temp_string (c2m_ctx, t2->repr);
  reverse (temp_string);
  set_string_stream (c2m_ctx, VARR_ADDR (char, temp_string), t1->pos, NULL);
  t = get_next_pptoken (c2m_ctx);
  next = get_next_pptoken (c2m_ctx);
  if (next->code != T_EOFILE) {
    error (c2m_ctx, t1->pos, "wrong result of ##: %s", reverse (temp_string));
    remove_string_stream (c2m_ctx);
  }
  return t;
}

static void add_token (VARR (token_t) * to, token_t t) {
  if ((t->code != ' ' && t->code != '\n') || VARR_LENGTH (token_t, to) == 0
      || (VARR_LAST (token_t, to)->code != ' ' && VARR_LAST (token_t, to)->code != '\n'))
    VARR_PUSH (token_t, to, t);
}

static void add_arg_tokens (VARR (token_t) * to, VARR (token_t) * from) {
  int start;

  for (start = VARR_LENGTH (token_t, from) - 1; start >= 0; start--)
    if (VARR_GET (token_t, from, start)->code == T_BOA) break;
  assert (start >= 0);
  for (size_t i = start + 1; i < VARR_LENGTH (token_t, from); i++)
    add_token (to, VARR_GET (token_t, from, i));
  VARR_TRUNC (token_t, from, start);
}

static void add_tokens (VARR (token_t) * to, VARR (token_t) * from) {
  for (size_t i = 0; i < VARR_LENGTH (token_t, from); i++)
    add_token (to, VARR_GET (token_t, from, i));
}

static void del_tokens (VARR (token_t) * tokens, int from, int len) {
  int diff, tokens_len = VARR_LENGTH (token_t, tokens);
  token_t *addr = VARR_ADDR (token_t, tokens);

  if (len < 0) len = tokens_len - from;
  assert (from + len <= tokens_len);
  if ((diff = tokens_len - from - len) > 0)
    memmove (addr + from, addr + from + len, diff * sizeof (token_t));
  VARR_TRUNC (token_t, tokens, tokens_len - len);
}

static VARR (token_t) * do_concat (c2m_ctx_t c2m_ctx, VARR (token_t) * tokens) {
  int i, j, k, empty_j_p, empty_k_p, len = VARR_LENGTH (token_t, tokens);
  token_t t;

  for (i = len - 1; i >= 0; i--)
    if ((t = VARR_GET (token_t, tokens, i))->code == T_RDBLNO) {
      j = i + 1;
      k = i - 1;
      assert (k >= 0 && j < len);
      if (VARR_GET (token_t, tokens, j)->code == ' ' || VARR_GET (token_t, tokens, j)->code == '\n')
        j++;
      if (VARR_GET (token_t, tokens, k)->code == ' ' || VARR_GET (token_t, tokens, k)->code == '\n')
        k--;
      assert (k >= 0 && j < len);
      empty_j_p = VARR_GET (token_t, tokens, j)->code == T_PLM;
      empty_k_p = VARR_GET (token_t, tokens, k)->code == T_PLM;
      if (empty_j_p || empty_k_p) {
        if (!empty_j_p)
          j--;
        else if (j + 1 < len
                 && (VARR_GET (token_t, tokens, j + 1)->code == ' '
                     || VARR_GET (token_t, tokens, j + 1)->code == '\n'))
          j++;
        if (!empty_k_p)
          k++;
        else if (k != 0
                 && (VARR_GET (token_t, tokens, k - 1)->code == ' '
                     || VARR_GET (token_t, tokens, k - 1)->code == '\n'))
          k--;
        if (!empty_j_p || !empty_k_p) {
          del_tokens (tokens, k, j - k + 1);
        } else {
          del_tokens (tokens, k, j - k);
          t = new_token (c2m_ctx, t->pos, "", ' ', N_IGNORE);
          VARR_SET (token_t, tokens, k, t);
        }
      } else {
        t = token_concat (c2m_ctx, VARR_GET (token_t, tokens, k), VARR_GET (token_t, tokens, j));
        del_tokens (tokens, k + 1, j - k);
        VARR_SET (token_t, tokens, k, t);
      }
      i = k;
      len = VARR_LENGTH (token_t, tokens);
    }
  for (i = len - 1; i >= 0; i--) VARR_GET (token_t, tokens, i)->processed_p = TRUE;
  return tokens;
}

static void process_replacement (c2m_ctx_t c2m_ctx, macro_call_t mc) {
  macro_t m;
  token_t t, *m_repl;
  VARR (token_t) * arg;
  int i, m_repl_len, sharp_pos, copy_p;

  m = mc->macro;
  sharp_pos = -1;
  m_repl = VARR_ADDR (token_t, m->replacement);
  m_repl_len = VARR_LENGTH (token_t, m->replacement);
  for (;;) {
    if (mc->repl_pos >= m_repl_len) {
      t = get_next_pptoken (c2m_ctx);
      unget_next_pptoken (c2m_ctx, t);
#ifdef C2MIR_PREPRO_DEBUG
      fprintf (stderr, "# push back <%s>\n", get_token_str (t));
#endif
      unget_next_pptoken (c2m_ctx, new_token (c2m_ctx, t->pos, "", T_EOR, N_IGNORE));
#ifdef C2MIR_PREPRO_DEBUG
      fprintf (stderr, "# push back <EOR>: mc=%lx\n", mc);
#endif
      push_back (c2m_ctx, do_concat (c2m_ctx, mc->repl_buffer));
      m->ignore_p = TRUE;
      return;
    }
    t = m_repl[mc->repl_pos++];
    copy_p = TRUE;
    if (t->code == T_ID) {
      i = find_param (m->params, t->repr);
      if (i >= 0) {
        arg = VARR_GET (token_arr_t, mc->args, i);
        if (sharp_pos >= 0) {
          del_tokens (mc->repl_buffer, sharp_pos, -1);
          if (VARR_LENGTH (token_t, arg) != 0
              && (VARR_GET (token_t, arg, 0)->code == ' '
                  || VARR_GET (token_t, arg, 0)->code == '\n'))
            del_tokens (arg, 0, 1);
          if (VARR_LENGTH (token_t, arg) != 0
              && (VARR_LAST (token_t, arg)->code == ' ' || VARR_LAST (token_t, arg)->code == '\n'))
            VARR_POP (token_t, arg);
          t = token_stringify (c2m_ctx, mc->macro->id, arg);
          copy_p = FALSE;
        } else if ((mc->repl_pos >= 2 && m_repl[mc->repl_pos - 2]->code == T_RDBLNO)
                   || (mc->repl_pos >= 3 && m_repl[mc->repl_pos - 2]->code == ' '
                       && m_repl[mc->repl_pos - 3]->code == T_RDBLNO)
                   || (mc->repl_pos < m_repl_len && m_repl[mc->repl_pos]->code == T_RDBLNO)
                   || (mc->repl_pos + 1 < m_repl_len && m_repl[mc->repl_pos + 1]->code == T_RDBLNO
                       && m_repl[mc->repl_pos]->code == ' ')) {
          if (VARR_LENGTH (token_t, arg) == 0
              || (VARR_LENGTH (token_t, arg) == 1
                  && (VARR_GET (token_t, arg, 0)->code == ' '
                      || VARR_GET (token_t, arg, 0)->code == '\n'))) {
            t = new_token (c2m_ctx, t->pos, "", T_PLM, N_IGNORE);
            copy_p = FALSE;
          } else {
            add_tokens (mc->repl_buffer, arg);
            continue;
          }
        } else {
          unget_next_pptoken (c2m_ctx, new_token (c2m_ctx, t->pos, "", T_EOA, N_IGNORE));
#ifdef C2MIR_PREPRO_DEBUG
          fprintf (stderr, "# push back <EOA> for macro %s call\n", mc->macro->id->repr);
#endif
          copy_and_push_back (c2m_ctx, arg);
          unget_next_pptoken (c2m_ctx, new_token (c2m_ctx, t->pos, "", T_BOA, N_IGNORE));
#ifdef C2MIR_PREPRO_DEBUG
          fprintf (stderr, "# push back <BOA> for macro %s call\n", mc->macro->id->repr);
#endif
          return;
        }
      }
    } else if (t->code == '#') {
      sharp_pos = VARR_LENGTH (token_t, mc->repl_buffer);
    } else if (t->code != ' ') {
      sharp_pos = -1;
    }
    if (copy_p) t = copy_token (c2m_ctx, t);
    add_token (mc->repl_buffer, t);
  }
}

static void prepare_pragma_string (const char *repr, VARR (char) * to) {
  destringify (repr, to);
  reverse (to);
}

static int process_pragma (c2m_ctx_t c2m_ctx, token_t t) {
  token_t t1, t2;

  if (strcmp (t->repr, "_Pragma") != 0) return FALSE;
  VARR_TRUNC (token_t, temp_tokens, 0);
  t1 = get_next_pptoken (c2m_ctx);
  VARR_PUSH (token_t, temp_tokens, t1);
  if (t1->code == ' ' || t1->code == '\n') {
    t1 = get_next_pptoken (c2m_ctx);
    VARR_PUSH (token_t, temp_tokens, t1);
  }
  if (t1->code != '(') {
    push_back (c2m_ctx, temp_tokens);
    return FALSE;
  }
  t1 = get_next_pptoken (c2m_ctx);
  VARR_PUSH (token_t, temp_tokens, t1);
  if (t1->code == ' ' || t1->code == '\n') {
    t1 = get_next_pptoken (c2m_ctx);
    VARR_PUSH (token_t, temp_tokens, t1);
  }
  if (t1->code != T_STR) {
    push_back (c2m_ctx, temp_tokens);
    return FALSE;
  }
  t2 = t1;
  t1 = get_next_pptoken (c2m_ctx);
  VARR_PUSH (token_t, temp_tokens, t1);
  if (t1->code == ' ' || t1->code == '\n') {
    t1 = get_next_pptoken (c2m_ctx);
    VARR_PUSH (token_t, temp_tokens, t1);
  }
  if (t1->code != ')') {
    push_back (c2m_ctx, temp_tokens);
    return FALSE;
  }
  set_string_stream (c2m_ctx, t2->repr, t2->pos, prepare_pragma_string);
  VARR_TRUNC (token_t, temp_tokens, 0);
  for (t1 = get_next_pptoken (c2m_ctx); t1->code != T_EOFILE; t1 = get_next_pptoken (c2m_ctx))
    VARR_PUSH (token_t, temp_tokens, t1);
  check_pragma (c2m_ctx, t2, temp_tokens);
  return TRUE;
}

static void flush_buffer (c2m_ctx_t c2m_ctx) {
  for (size_t i = 0; i < VARR_LENGTH (token_t, output_buffer); i++)
    pre_out_token_func (c2m_ctx, VARR_GET (token_t, output_buffer, i));
  VARR_TRUNC (token_t, output_buffer, 0);
}

static void out_token (c2m_ctx_t c2m_ctx, token_t t) {
  if (no_out_p || VARR_LENGTH (macro_call_t, macro_call_stack) != 0) {
    VARR_PUSH (token_t, output_buffer, t);
    return;
  }
  flush_buffer (c2m_ctx);
  pre_out_token_func (c2m_ctx, t);
}

struct val {
  int uns_p;
  union {
    mir_llong i_val;
    mir_ullong u_val;
  } u;
};

static void move_tokens (VARR (token_t) * to, VARR (token_t) * from) {
  VARR_TRUNC (token_t, to, 0);
  for (int i = 0; i < VARR_LENGTH (token_t, from); i++)
    VARR_PUSH (token_t, to, VARR_GET (token_t, from, i));
  VARR_TRUNC (token_t, from, 0);
}

static void reverse_move_tokens (c2m_ctx_t c2m_ctx, VARR (token_t) * to, VARR (token_t) * from) {
  VARR_TRUNC (token_t, to, 0);
  while (VARR_LENGTH (token_t, from) != 0) VARR_PUSH (token_t, to, VARR_POP (token_t, from));
}

static void transform_to_header (c2m_ctx_t c2m_ctx, VARR (token_t) * buffer) {
  int i, j, k;
  token_t t;
  pos_t pos;

  for (i = 0; i < VARR_LENGTH (token_t, buffer) && VARR_GET (token_t, buffer, i)->code == ' '; i++)
    ;
  if (i >= VARR_LENGTH (token_t, buffer)) return;
  if ((t = VARR_GET (token_t, buffer, i))->node_code != N_LT) return;
  pos = t->pos;
  for (j = i + 1;
       j < VARR_LENGTH (token_t, buffer) && VARR_GET (token_t, buffer, j)->node_code != N_GT; j++)
    ;
  if (j >= VARR_LENGTH (token_t, buffer)) return;
  VARR_TRUNC (char, symbol_text, 0);
  VARR_TRUNC (char, temp_string, 0);
  VARR_PUSH (char, symbol_text, '<');
  for (k = i + 1; k < j; k++) {
    t = VARR_GET (token_t, buffer, k);
    for (const char *s = t->repr; *s != 0; s++) {
      VARR_PUSH (char, symbol_text, *s);
      VARR_PUSH (char, temp_string, *s);
    }
  }
  VARR_PUSH (char, symbol_text, '>');
  VARR_PUSH (char, symbol_text, '\0');
  VARR_PUSH (char, temp_string, '\0');
  del_tokens (buffer, i, j - i);
  t = new_node_token (c2m_ctx, pos, VARR_ADDR (char, symbol_text), T_HEADER,
                      new_str_node (c2m_ctx, N_STR,
                                    uniq_cstr (c2m_ctx, VARR_ADDR (char, temp_string)), pos));
  VARR_SET (token_t, buffer, i, t);
}

static void processing (c2m_ctx_t c2m_ctx, int ignore_directive_p);

static struct val eval_expr (c2m_ctx_t c2m_ctx, VARR (token_t) * buffer, token_t if_token);

static void process_directive (c2m_ctx_t c2m_ctx) {
  token_t t, t1;
  int i, true_p;
  VARR (token_t) * temp_buffer;
  pos_t pos;
  struct macro macro;
  macro_t tab_macro;
  const char *name;

  t = get_next_pptoken (c2m_ctx);
  if (t->code == '\n') return;
  if (t->code == ' ') t = get_next_pptoken (c2m_ctx);
  if (t->code != T_ID) {
    if (!skip_if_part_p) error (c2m_ctx, t->pos, "wrong directive name %s", t->repr);
    skip_nl (c2m_ctx, NULL, NULL);
    return;
  }
  VARR_CREATE (token_t, temp_buffer, 64);
  if (strcmp (t->repr, "ifdef") == 0 || strcmp (t->repr, "ifndef") == 0) {
    t1 = t;
    if (VARR_LENGTH (ifstate_t, ifs) != 0 && VARR_LAST (ifstate_t, ifs)->skip_p) {
      skip_if_part_p = true_p = TRUE;
      skip_nl (c2m_ctx, NULL, NULL);
    } else {
      t = get_next_pptoken (c2m_ctx);
      skip_if_part_p = FALSE;
      if (t->code == ' ') t = get_next_pptoken (c2m_ctx);
      if (t->code != T_ID) {
        error (c2m_ctx, t->pos, "wrong #%s", t1->repr);
      } else {
        macro.id = t;
        skip_if_part_p = HTAB_DO (macro_t, macro_tab, &macro, HTAB_FIND, tab_macro);
      }
      t = get_next_pptoken (c2m_ctx);
      if (t->code != '\n') {
        error (c2m_ctx, t1->pos, "garbage at the end of #%s", t1->repr);
        skip_nl (c2m_ctx, NULL, NULL);
      }
      if (strcmp (t1->repr, "ifdef") == 0) skip_if_part_p = !skip_if_part_p;
      true_p = !skip_if_part_p;
    }
    VARR_PUSH (ifstate_t, ifs, new_ifstate (skip_if_part_p, true_p, FALSE, t1->pos));
  } else if (strcmp (t->repr, "endif") == 0 || strcmp (t->repr, "else") == 0) {
    t1 = t;
    t = get_next_pptoken (c2m_ctx);
    if (t->code != '\n') {
      error (c2m_ctx, t1->pos, "garbage at the end of #%s", t1->repr);
      skip_nl (c2m_ctx, NULL, NULL);
    }
    if (VARR_LENGTH (ifstate_t, ifs) < cs->ifs_length_at_stream_start)
      error (c2m_ctx, t1->pos, "unmatched #%s", t1->repr);
    else if (strcmp (t1->repr, "endif") == 0) {
      pop_ifstate (c2m_ctx);
      skip_if_part_p = VARR_LENGTH (ifstate_t, ifs) == 0 ? 0 : VARR_LAST (ifstate_t, ifs)->skip_p;
    } else if (VARR_LAST (ifstate_t, ifs)->else_p) {
      error (c2m_ctx, t1->pos, "repeated #else");
      VARR_LAST (ifstate_t, ifs)->skip_p = 1;
      skip_if_part_p = TRUE;
    } else {
      skip_if_part_p = VARR_LAST (ifstate_t, ifs)->true_p;
      VARR_LAST (ifstate_t, ifs)->true_p = TRUE;
      VARR_LAST (ifstate_t, ifs)->skip_p = skip_if_part_p;
      VARR_LAST (ifstate_t, ifs)->else_p = FALSE;
    }
  } else if (strcmp (t->repr, "if") == 0 || strcmp (t->repr, "elif") == 0) {
    if_id = t;
    if (strcmp (t->repr, "elif") == 0 && VARR_LENGTH (ifstate_t, ifs) == 0) {
      error (c2m_ctx, t->pos, "#elif without #if");
    } else if (strcmp (t->repr, "elif") == 0 && VARR_LAST (ifstate_t, ifs)->else_p) {
      error (c2m_ctx, t->pos, "#elif after #else");
      skip_if_part_p = TRUE;
    } else if (strcmp (t->repr, "if") == 0 && VARR_LENGTH (ifstate_t, ifs) != 0
               && VARR_LAST (ifstate_t, ifs)->skip_p) {
      skip_if_part_p = true_p = TRUE;
      skip_nl (c2m_ctx, NULL, NULL);
      VARR_PUSH (ifstate_t, ifs, new_ifstate (skip_if_part_p, true_p, FALSE, t->pos));
    } else if (strcmp (t->repr, "elif") == 0 && VARR_LAST (ifstate_t, ifs)->true_p) {
      VARR_LAST (ifstate_t, ifs)->skip_p = skip_if_part_p = TRUE;
      skip_nl (c2m_ctx, NULL, NULL);
    } else {
      struct val val;

      skip_if_part_p = FALSE; /* for eval expr */
      skip_nl (c2m_ctx, NULL, temp_buffer);
      val = eval_expr (c2m_ctx, temp_buffer, t);
      true_p = val.uns_p ? val.u.u_val != 0 : val.u.i_val != 0;
      skip_if_part_p = !true_p;
      if (strcmp (t->repr, "if") == 0)
        VARR_PUSH (ifstate_t, ifs, new_ifstate (skip_if_part_p, true_p, FALSE, t->pos));
      else {
        VARR_LAST (ifstate_t, ifs)->skip_p = skip_if_part_p;
        VARR_LAST (ifstate_t, ifs)->true_p = true_p;
      }
    }
  } else if (skip_if_part_p) {
    skip_nl (c2m_ctx, NULL, NULL);
  } else if (strcmp (t->repr, "define") == 0) {
    define (c2m_ctx);
  } else if (strcmp (t->repr, "include") == 0) {
    t = get_next_include_pptoken (c2m_ctx);
    if (t->code == ' ') t = get_next_include_pptoken (c2m_ctx);
    t1 = get_next_pptoken (c2m_ctx);
    if ((t->code == T_STR || t->code == T_HEADER) && t1->code == '\n')
      name = get_include_fname (c2m_ctx, t);
    else {
      VARR_PUSH (token_t, temp_buffer, t);
      skip_nl (c2m_ctx, t1, temp_buffer);
      unget_next_pptoken (c2m_ctx, new_token (c2m_ctx, t->pos, "", T_EOP, N_IGNORE));
      push_back (c2m_ctx, temp_buffer);
      assert (VARR_LENGTH (macro_call_t, macro_call_stack) == 0 && !no_out_p);
      no_out_p = TRUE;
      processing (c2m_ctx, TRUE);
      no_out_p = FALSE;
      move_tokens (temp_buffer, output_buffer);
      transform_to_header (c2m_ctx, temp_buffer);
      i = 0;
      if (VARR_LENGTH (token_t, temp_buffer) != 0
          && VARR_GET (token_t, temp_buffer, 0)->code == ' ')
        i++;
      if (i != VARR_LENGTH (token_t, temp_buffer) - 1
          || (VARR_GET (token_t, temp_buffer, i)->code != T_STR
              && VARR_GET (token_t, temp_buffer, i)->code != T_HEADER)) {
        error (c2m_ctx, t->pos, "wrong #include");
        goto ret;
      }
      name = get_include_fname (c2m_ctx, VARR_GET (token_t, temp_buffer, i));
    }
    if (VARR_LENGTH (stream_t, streams) >= max_nested_includes + 1) {
      error (c2m_ctx, t->pos, "more %d include levels", VARR_LENGTH (stream_t, streams) - 1);
      goto ret;
    }
    add_include_stream (c2m_ctx, name);
  } else if (strcmp (t->repr, "line") == 0) {
    skip_nl (c2m_ctx, NULL, temp_buffer);
    unget_next_pptoken (c2m_ctx, new_token (c2m_ctx, t->pos, "", T_EOP, N_IGNORE));
    push_back (c2m_ctx, temp_buffer);
    assert (VARR_LENGTH (macro_call_t, macro_call_stack) == 0 && !no_out_p);
    no_out_p = 1;
    processing (c2m_ctx, TRUE);
    no_out_p = 0;
    move_tokens (temp_buffer, output_buffer);
    pos = check_line_directive_args (c2m_ctx, temp_buffer);
    if (pos.lno < 0) {
      error (c2m_ctx, t->pos, "wrong #line");
    } else {
      change_stream_pos (c2m_ctx, pos);
    }
  } else if (strcmp (t->repr, "error") == 0) {
    VARR_TRUNC (char, temp_string, 0);
    add_to_temp_string (c2m_ctx, "#error");
    for (t1 = get_next_pptoken (c2m_ctx); t1->code != '\n'; t1 = get_next_pptoken (c2m_ctx))
      add_to_temp_string (c2m_ctx, t1->repr);
    error (c2m_ctx, t->pos, "%s", VARR_ADDR (char, temp_string));
  } else if (strcmp (t->repr, "pragma") == 0) {
    skip_nl (c2m_ctx, NULL, temp_buffer);
    check_pragma (c2m_ctx, t, temp_buffer);
  } else if (strcmp (t->repr, "undef") == 0) {
    t = get_next_pptoken (c2m_ctx);
    if (t->code == ' ') t = get_next_pptoken (c2m_ctx);
    if (t->code == '\n') {
      error (c2m_ctx, t->pos, "no ident after #undef");
      goto ret;
    }
    if (t->code != T_ID) {
      error (c2m_ctx, t->pos, "no ident after #undef");
      skip_nl (c2m_ctx, t, NULL);
      goto ret;
    }
    if (strcmp (t->repr, "defined") == 0) {
      error (c2m_ctx, t->pos, "#undef of %s", t->repr);
    } else {
      macro.id = t;
      if (HTAB_DO (macro_t, macro_tab, &macro, HTAB_FIND, tab_macro)) {
        if (tab_macro->replacement == NULL)
          error (c2m_ctx, t->pos, "#undef of standard macro %s", t->repr);
        else
          HTAB_DO (macro_t, macro_tab, &macro, HTAB_DELETE, tab_macro);
      }
    }
  }
ret:
  VARR_DESTROY (token_t, temp_buffer);
}

static int pre_match (c2m_ctx_t c2m_ctx, int c, pos_t *pos, node_code_t *node_code, node_t *node) {
  token_t t;

  if (VARR_LENGTH (token_t, pre_expr) == 0) return FALSE;
  t = VARR_LAST (token_t, pre_expr);
  if (t->code != c) return FALSE;
  if (pos != NULL) *pos = t->pos;
  if (node_code != NULL) *node_code = t->node_code;
  if (node != NULL) *node = t->node;
  VARR_POP (token_t, pre_expr);
  return TRUE;
}

static node_t pre_cond_expr (c2m_ctx_t c2m_ctx);

/* Expressions: */
static node_t pre_primary_expr (c2m_ctx_t c2m_ctx) {
  node_t r;

  if (pre_match (c2m_ctx, T_NUMBER, NULL, NULL, &r) || pre_match (c2m_ctx, T_CH, NULL, NULL, &r))
    return r;
  if (pre_match (c2m_ctx, '(', NULL, NULL, NULL)) {
    if ((r = pre_cond_expr (c2m_ctx)) == NULL) return r;
    if (pre_match (c2m_ctx, ')', NULL, NULL, NULL)) return r;
  }
  return NULL;
}

static node_t pre_unary_expr (c2m_ctx_t c2m_ctx) {
  node_t r;
  node_code_t code;
  pos_t pos;

  if (!pre_match (c2m_ctx, T_UNOP, &pos, &code, NULL)
      && !pre_match (c2m_ctx, T_ADDOP, &pos, &code, NULL))
    return pre_primary_expr (c2m_ctx);
  if ((r = pre_unary_expr (c2m_ctx)) == NULL) return r;
  r = new_pos_node1 (c2m_ctx, code, pos, r);
  return r;
}

static node_t pre_left_op (c2m_ctx_t c2m_ctx, int token, int token2,
                           node_t (*f) (c2m_ctx_t c2m_ctx)) {
  node_code_t code;
  node_t r, n;
  pos_t pos;

  if ((r = f (c2m_ctx)) == NULL) return r;
  while (pre_match (c2m_ctx, token, &pos, &code, NULL)
         || (token2 >= 0 && pre_match (c2m_ctx, token2, &pos, &code, NULL))) {
    n = new_pos_node1 (c2m_ctx, code, pos, r);
    if ((r = f (c2m_ctx)) == NULL) return r;
    op_append (n, r);
    r = n;
  }
  return r;
}

static node_t pre_mul_expr (c2m_ctx_t c2m_ctx) {
  return pre_left_op (c2m_ctx, T_DIVOP, '*', pre_unary_expr);
}
static node_t pre_add_expr (c2m_ctx_t c2m_ctx) {
  return pre_left_op (c2m_ctx, T_ADDOP, -1, pre_mul_expr);
}
static node_t pre_sh_expr (c2m_ctx_t c2m_ctx) {
  return pre_left_op (c2m_ctx, T_SH, -1, pre_add_expr);
}
static node_t pre_rel_expr (c2m_ctx_t c2m_ctx) {
  return pre_left_op (c2m_ctx, T_CMP, -1, pre_sh_expr);
}
static node_t pre_eq_expr (c2m_ctx_t c2m_ctx) {
  return pre_left_op (c2m_ctx, T_EQNE, -1, pre_rel_expr);
}
static node_t pre_and_expr (c2m_ctx_t c2m_ctx) {
  return pre_left_op (c2m_ctx, '&', -1, pre_eq_expr);
}
static node_t pre_xor_expr (c2m_ctx_t c2m_ctx) {
  return pre_left_op (c2m_ctx, '^', -1, pre_and_expr);
}
static node_t pre_or_expr (c2m_ctx_t c2m_ctx) {
  return pre_left_op (c2m_ctx, '|', -1, pre_xor_expr);
}
static node_t pre_land_expr (c2m_ctx_t c2m_ctx) {
  return pre_left_op (c2m_ctx, T_ANDAND, -1, pre_or_expr);
}
static node_t pre_lor_expr (c2m_ctx_t c2m_ctx) {
  return pre_left_op (c2m_ctx, T_OROR, -1, pre_land_expr);
}

static node_t pre_cond_expr (c2m_ctx_t c2m_ctx) {
  node_t r, n;
  pos_t pos;

  if ((r = pre_lor_expr (c2m_ctx)) == NULL) return r;
  if (!pre_match (c2m_ctx, '?', &pos, NULL, NULL)) return r;
  n = new_pos_node1 (c2m_ctx, N_COND, pos, r);
  if ((r = pre_cond_expr (c2m_ctx)) == NULL) return r;
  op_append (n, r);
  if (!pre_match (c2m_ctx, ':', NULL, NULL, NULL)) return NULL;
  if ((r = pre_cond_expr (c2m_ctx)) == NULL) return r;
  op_append (n, r);
  return n;
}

static node_t parse_pre_expr (c2m_ctx_t c2m_ctx, VARR (token_t) * expr) {
  node_t r;
  token_t t;

  pre_expr = expr;
  t = VARR_LAST (token_t, expr);
  if ((r = pre_cond_expr (c2m_ctx)) != NULL && VARR_LENGTH (token_t, expr) == 0) return r;
  if (VARR_LENGTH (token_t, expr) != 0) t = VARR_POP (token_t, expr);
  error (c2m_ctx, t->pos, "wrong preprocessor expression");
  return NULL;
}

static struct val eval (c2m_ctx_t c2m_ctx, node_t tree);

static struct val eval_expr (c2m_ctx_t c2m_ctx, VARR (token_t) * expr_buffer, token_t if_token) {
  int i, j, k, len;
  token_t t, id, ppt;
  const char *res;
  struct macro macro_struct;
  macro_t tab_macro;
  VARR (token_t) * temp_buffer;
  node_t tree;

  for (i = 0; i < VARR_LENGTH (token_t, expr_buffer); i++) {
    /* Change defined ident and defined (ident) */
    t = VARR_GET (token_t, expr_buffer, i);
    if (t->code == T_ID && strcmp (t->repr, "defined") == 0) {
      j = i + 1;
      len = VARR_LENGTH (token_t, expr_buffer);
      if (j < len && VARR_GET (token_t, expr_buffer, j)->code == ' ') j++;
      if (j >= len) continue;
      if ((id = VARR_GET (token_t, expr_buffer, j))->code == T_ID) {
        macro_struct.id = id;
        res = HTAB_DO (macro_t, macro_tab, &macro_struct, HTAB_FIND, tab_macro) ? "1" : "0";
        VARR_SET (token_t, expr_buffer, i,
                  new_token (c2m_ctx, t->pos, res, T_NUMBER, N_IGNORE));  // ???
        del_tokens (expr_buffer, i + 1, j - i);
        continue;
      }
      if (j >= len || VARR_GET (token_t, expr_buffer, j)->code != '(') continue;
      j++;
      if (j < len && VARR_GET (token_t, expr_buffer, j)->code == ' ') j++;
      if (j >= len || VARR_GET (token_t, expr_buffer, j)->code != T_ID) continue;
      k = j;
      j++;
      if (j < len && VARR_GET (token_t, expr_buffer, j)->code == ' ') j++;
      if (j >= len || VARR_GET (token_t, expr_buffer, j)->code != ')') continue;
      macro_struct.id = VARR_GET (token_t, expr_buffer, k);
      res = HTAB_DO (macro_t, macro_tab, &macro_struct, HTAB_FIND, tab_macro) ? "1" : "0";
      VARR_SET (token_t, expr_buffer, i,
                new_token (c2m_ctx, t->pos, res, T_NUMBER, N_IGNORE));  // ???
      del_tokens (expr_buffer, i + 1, j - i);
    }
  }
  if (VARR_LENGTH (macro_call_t, macro_call_stack) != 0)
    error (c2m_ctx, if_token->pos, "#if/#elif inside a macro call");
  assert (VARR_LENGTH (token_t, output_buffer) == 0 && !no_out_p);
  /* macro substitution */
  unget_next_pptoken (c2m_ctx, new_token (c2m_ctx, if_token->pos, "", T_EOP, N_IGNORE));
  push_back (c2m_ctx, expr_buffer);
  no_out_p = TRUE;
  processing (c2m_ctx, TRUE);
  no_out_p = FALSE;
  reverse_move_tokens (c2m_ctx, expr_buffer, output_buffer);
  VARR_CREATE (token_t, temp_buffer, VARR_LENGTH (token_t, expr_buffer));
  for (i = j = 0; i < VARR_LENGTH (token_t, expr_buffer); i++) {
    int change_p = TRUE;

    /* changing PP tokens to tokens and idents to "0" */
    ppt = VARR_GET (token_t, expr_buffer, i);
    t = pptoken2token (c2m_ctx, ppt, FALSE);
    if (t == NULL || t->code == ' ' || t->code == '\n') continue;
    if (t->code == T_NUMBER
        && (t->node->code == N_F || t->node->code == N_D || t->node->code == N_LD)) {
      error (c2m_ctx, ppt->pos, "floating point in #if/#elif: %s", ppt->repr);
    } else if (t->code == T_STR) {
      error (c2m_ctx, ppt->pos, "string in #if/#elif: %s", ppt->repr);
    } else if (t->code != T_ID) {
      change_p = FALSE;
    }
    if (change_p)
      t = new_node_token (c2m_ctx, ppt->pos, "0", T_NUMBER, new_ll_node (c2m_ctx, 0, ppt->pos));
    VARR_PUSH (token_t, temp_buffer, t);
  }
  no_out_p = TRUE;
  tree = parse_pre_expr (c2m_ctx, temp_buffer);
  no_out_p = FALSE;
  VARR_DESTROY (token_t, temp_buffer);
  if (tree == NULL) {
    struct val val;

    val.uns_p = FALSE;
    val.u.i_val = 0;
    return val;
  }
  return eval (c2m_ctx, tree);
}

static int eval_binop_operands (c2m_ctx_t c2m_ctx, node_t tree, struct val *v1, struct val *v2) {
  *v1 = eval (c2m_ctx, NL_HEAD (tree->ops));
  *v2 = eval (c2m_ctx, NL_EL (tree->ops, 1));
  if (v1->uns_p && !v2->uns_p) {
    v2->uns_p = TRUE;
    v2->u.u_val = v2->u.i_val;
  } else if (!v1->uns_p && v2->uns_p) {
    v1->uns_p = TRUE;
    v1->u.u_val = v1->u.i_val;
  }
  return v1->uns_p;
}

static struct val eval (c2m_ctx_t c2m_ctx, node_t tree) {
  int cond;
  struct val res, v1, v2;

#define UNOP(op)                              \
  do {                                        \
    v1 = eval (c2m_ctx, NL_HEAD (tree->ops)); \
    res = v1;                                 \
    if (res.uns_p)                            \
      res.u.u_val = op res.u.u_val;           \
    else                                      \
      res.u.i_val = op res.u.i_val;           \
  } while (0)

#define BINOP(op)                                              \
  do {                                                         \
    res.uns_p = eval_binop_operands (c2m_ctx, tree, &v1, &v2); \
    if (res.uns_p)                                             \
      res.u.u_val = v1.u.u_val op v2.u.u_val;                  \
    else                                                       \
      res.u.i_val = v1.u.i_val op v2.u.i_val;                  \
  } while (0)

  switch (tree->code) {
  case N_CH:
    res.uns_p = !char_is_signed_p () || MIR_CHAR_MAX > MIR_INT_MAX;
    if (res.uns_p)
      res.u.u_val = tree->u.ch;
    else
      res.u.i_val = tree->u.ch;
    break;
  case N_I:
  case N_L:
    res.uns_p = FALSE;
    res.u.i_val = tree->u.l;
    break;
  case N_LL:
    res.uns_p = FALSE;
    res.u.i_val = tree->u.ll;
    break;
  case N_U:
  case N_UL:
    res.uns_p = TRUE;
    res.u.u_val = tree->u.ul;
    break;
  case N_ULL:
    res.uns_p = TRUE;
    res.u.u_val = tree->u.ull;
    break;
  case N_BITWISE_NOT: UNOP (~); break;
  case N_NOT: UNOP (!); break;
  case N_EQ: BINOP (==); break;
  case N_NE: BINOP (!=); break;
  case N_LT: BINOP (<); break;
  case N_LE: BINOP (<=); break;
  case N_GT: BINOP (>); break;
  case N_GE: BINOP (>=); break;
  case N_ADD:
    if (NL_EL (tree->ops, 1) == NULL) {
      UNOP (+);
    } else {
      BINOP (+);
    }
    break;
  case N_SUB:
    if (NL_EL (tree->ops, 1) == NULL) {
      UNOP (-);
    } else {
      BINOP (-);
    }
    break;
  case N_AND: BINOP (&); break;
  case N_OR: BINOP (|); break;
  case N_XOR: BINOP (^); break;
  case N_LSH: BINOP (<<); break;
  case N_RSH: BINOP (>>); break;
  case N_MUL: BINOP (*); break;
  case N_DIV:
  case N_MOD: {
    int zero_p;

    res.uns_p = eval_binop_operands (c2m_ctx, tree, &v1, &v2);
    if (res.uns_p) {
      res.u.u_val = ((zero_p = v2.u.u_val == 0)
                       ? 1
                       : tree->code == N_DIV ? v1.u.u_val / v2.u.u_val : v1.u.u_val % v2.u.u_val);
    } else {
      res.u.i_val = ((zero_p = v2.u.i_val == 0)
                       ? 1
                       : tree->code == N_DIV ? v1.u.i_val / v2.u.i_val : v1.u.i_val % v2.u.i_val);
    }
    if (zero_p)
      error (c2m_ctx, tree->pos, "division (%s) by zero in preporocessor",
             tree->code == N_DIV ? "/" : "%");
    break;
  }
  case N_ANDAND:
  case N_OROR:
    v1 = eval (c2m_ctx, NL_HEAD (tree->ops));
    cond = v1.uns_p ? v1.u.u_val != 0 : v1.u.i_val != 0;
    if (tree->code == N_ANDAND ? cond : !cond) {
      v2 = eval (c2m_ctx, NL_EL (tree->ops, 1));
      cond = v2.uns_p ? v2.u.u_val != 0 : v2.u.i_val != 0;
    }
    res.uns_p = FALSE;
    res.u.i_val = cond;
    break;
  case N_COND:
    v1 = eval (c2m_ctx, NL_HEAD (tree->ops));
    cond = v1.uns_p ? v1.u.u_val != 0 : v1.u.i_val != 0;
    res = eval (c2m_ctx, NL_EL (tree->ops, cond ? 1 : 2));
    break;
  default: assert (FALSE);
  }
  return res;
}

static void processing (c2m_ctx_t c2m_ctx, int ignore_directive_p) {
  token_t t, t1, t2;
  struct macro macro_struct;
  macro_t m;
  macro_call_t mc;
  int newln_p;

  for (newln_p = TRUE;;) { /* Main loop. */
    t = get_next_pptoken (c2m_ctx);
    if (t->code == T_EOP) return; /* end of processing */
    if (newln_p && !ignore_directive_p && t->code == '#') {
      process_directive (c2m_ctx);
      continue;
    }
    if (t->code == '\n') {
      newln_p = TRUE;
      out_token (c2m_ctx, t);
      continue;
    } else if (t->code == ' ') {
      out_token (c2m_ctx, t);
      continue;
    } else if (t->code == T_EOFILE || t->code == T_EOU) {
      if (VARR_LENGTH (ifstate_t, ifs) > eof_s->ifs_length_at_stream_start) {
        error (c2m_ctx, VARR_LAST (ifstate_t, ifs)->if_pos, "unfinished #if");
      }
      if (t->code == T_EOU) return;
      while (VARR_LENGTH (ifstate_t, ifs) > eof_s->ifs_length_at_stream_start)
        pop_ifstate (c2m_ctx);
      skip_if_part_p = VARR_LENGTH (ifstate_t, ifs) == 0 ? 0 : VARR_LAST (ifstate_t, ifs)->skip_p;
      newln_p = TRUE;
      continue;
    } else if (skip_if_part_p) {
      skip_nl (c2m_ctx, t, NULL);
      newln_p = TRUE;
      continue;
    }
    newln_p = FALSE;
    if (t->code == T_EOR) {  // finish macro call
      pop_macro_call (c2m_ctx);
      continue;
    } else if (t->code == T_EOA) { /* arg end: add the result to repl_buffer */
      mc = VARR_LAST (macro_call_t, macro_call_stack);
      add_arg_tokens (mc->repl_buffer, output_buffer);
#ifdef C2MIR_PREPRO_DEBUG
      fprintf (stderr, "adding processed arg to output buffer\n");
#endif
      process_replacement (c2m_ctx, mc);
      continue;
    } else if (t->code != T_ID) {
      out_token (c2m_ctx, t);
      continue;
    }
    macro_struct.id = t;
    if (!HTAB_DO (macro_t, macro_tab, &macro_struct, HTAB_FIND, m)) {
      if (!process_pragma (c2m_ctx, t)) out_token (c2m_ctx, t);
      continue;
    }
    if (m->replacement == NULL) { /* standard macro */
      if (strcmp (t->repr, "__STDC__") == 0) {
        out_token (c2m_ctx, new_node_token (c2m_ctx, t->pos, "1", T_NUMBER,
                                            new_i_node (c2m_ctx, 1, t->pos)));
      } else if (strcmp (t->repr, "__STDC_HOSTED__") == 0) {
        out_token (c2m_ctx, new_node_token (c2m_ctx, t->pos, "1", T_NUMBER,
                                            new_i_node (c2m_ctx, 1, t->pos)));
      } else if (strcmp (t->repr, "__STDC_VERSION__") == 0) {
        out_token (c2m_ctx, new_node_token (c2m_ctx, t->pos, "201112L", T_NUMBER,
                                            new_l_node (c2m_ctx, 201112, t->pos)));  // ???
      } else if (strcmp (t->repr, "__FILE__") == 0) {
        stringify (t->pos.fname, temp_string);
        VARR_PUSH (char, temp_string, '\0');
        t = new_node_token (c2m_ctx, t->pos, uniq_cstr (c2m_ctx, VARR_ADDR (char, temp_string)).s,
                            T_STR, new_str_node (c2m_ctx, N_STR, empty_str, t->pos));
        set_string_val (c2m_ctx, t, temp_string);
        out_token (c2m_ctx, t);
      } else if (strcmp (t->repr, "__LINE__") == 0) {
        char str[50];

        sprintf (str, "%d", t->pos.lno);
        out_token (c2m_ctx, new_node_token (c2m_ctx, t->pos, uniq_cstr (c2m_ctx, str).s, T_NUMBER,
                                            new_i_node (c2m_ctx, t->pos.lno, t->pos)));
      } else if (strcmp (t->repr, "__DATE__") == 0) {
        t = new_node_token (c2m_ctx, t->pos, date_str_repr, T_STR,
                            new_str_node (c2m_ctx, N_STR, uniq_cstr (c2m_ctx, date_str), t->pos));
        out_token (c2m_ctx, t);
      } else if (strcmp (t->repr, "__TIME__") == 0) {
        t = new_node_token (c2m_ctx, t->pos, time_str_repr, T_STR,
                            new_str_node (c2m_ctx, N_STR, uniq_cstr (c2m_ctx, time_str), t->pos));
        out_token (c2m_ctx, t);
      } else {
        assert (FALSE);
      }
      continue;
    }
    if (m->ignore_p) {
      t->code = T_NO_MACRO_IDENT;
      out_token (c2m_ctx, t);
      continue;
    }
    if (m->params == NULL) { /* macro without parameters */
      unget_next_pptoken (c2m_ctx, new_token (c2m_ctx, t->pos, "", T_EOR, N_IGNORE));
#ifdef C2MIR_PREPRO_DEBUG
      fprintf (stderr, "# push back <EOR>\n");
#endif
      mc = new_macro_call (m);
      add_tokens (mc->repl_buffer, m->replacement);
      copy_and_push_back (c2m_ctx, do_concat (c2m_ctx, mc->repl_buffer));
      m->ignore_p = TRUE;
      VARR_PUSH (macro_call_t, macro_call_stack, mc);
    } else { /* macro with parameters */
      t2 = NULL;
      t1 = get_next_pptoken (c2m_ctx);
      if (t1->code == T_EOR) {
        pop_macro_call (c2m_ctx);
        t1 = get_next_pptoken (c2m_ctx);
      }
      if (t1->code == ' ' || t1->code == '\n') {
        t2 = t1;
        t1 = get_next_pptoken (c2m_ctx);
      }
      if (t1->code != '(') { /* no args: it is not a macro call */
        unget_next_pptoken (c2m_ctx, t1);
        if (t2 != NULL) unget_next_pptoken (c2m_ctx, t2);
        out_token (c2m_ctx, t);
        continue;
      }
      mc = new_macro_call (m);
      find_args (c2m_ctx, mc);
      VARR_PUSH (macro_call_t, macro_call_stack, mc);
      process_replacement (c2m_ctx, mc);
    }
  }
}

static void pre_text_out (c2m_ctx_t c2m_ctx, token_t t) { /* NULL means end of output */
  int i;
  FILE *f = options->prepro_output_file;

  if (t == NULL && pre_last_token != NULL && pre_last_token->code == '\n') {
    fprintf (f, "\n");
    return;
  }
  pre_last_token = t;
  if (!t->processed_p) should_be_pre_pos = t->pos;
  if (t->code == '\n') return;
  if (actual_pre_pos.fname != should_be_pre_pos.fname
      || actual_pre_pos.lno != should_be_pre_pos.lno) {
    if (actual_pre_pos.fname == should_be_pre_pos.fname
        && actual_pre_pos.lno < should_be_pre_pos.lno
        && actual_pre_pos.lno + 4 >= should_be_pre_pos.lno) {
      for (; actual_pre_pos.lno != should_be_pre_pos.lno; actual_pre_pos.lno++) fprintf (f, "\n");
    } else {
      if (pre_last_token != NULL) fprintf (f, "\n");
      fprintf (f, "#line %d", should_be_pre_pos.lno);
      if (actual_pre_pos.fname != should_be_pre_pos.fname) {
        stringify (t->pos.fname, temp_string);
        VARR_PUSH (char, temp_string, '\0');
        fprintf (f, " %s", VARR_ADDR (char, temp_string));
      }
      fprintf (f, "\n");
    }
    for (i = 0; i < should_be_pre_pos.ln_pos - 1; i++) fprintf (f, " ");
    actual_pre_pos = should_be_pre_pos;
  }
  fprintf (f, "%s", t->code == ' ' ? " " : t->repr);
}

static void pre_out (c2m_ctx_t c2m_ctx, token_t t) {
  if (t == NULL) {
    t = new_token (c2m_ctx, pre_last_token == NULL ? no_pos : pre_last_token->pos, "<EOF>",
                   T_EOFILE, N_IGNORE);
  } else {
    assert (t->code != T_EOU && t->code != EOF);
    pre_last_token = t;
    if ((t = pptoken2token (c2m_ctx, t, TRUE)) == NULL) return;
  }
  if (t->code == T_STR && VARR_LENGTH (token_t, recorded_tokens) != 0
      && VARR_LAST (token_t, recorded_tokens)->code == T_STR) { /* concat strings */
    token_t last_t = VARR_POP (token_t, recorded_tokens);

    VARR_TRUNC (char, temp_string, 0);
    for (const char *s = last_t->repr; *s != 0; s++) VARR_PUSH (char, temp_string, *s);
    assert (VARR_LAST (char, temp_string) == '"' && t->repr[0] == '"');
    VARR_POP (char, temp_string);
    for (const char *s = &t->repr[1]; *s != 0; s++) VARR_PUSH (char, temp_string, *s);
    t = last_t;
    assert (VARR_LAST (char, temp_string) == '"');
    VARR_PUSH (char, temp_string, '\0');
    t->repr = uniq_cstr (c2m_ctx, VARR_ADDR (char, temp_string)).s;
    set_string_val (c2m_ctx, t, temp_string);
  }
  VARR_PUSH (token_t, recorded_tokens, t);
}

static void common_pre_out (c2m_ctx_t c2m_ctx, token_t t) {
  pptokens_num++;
  (options->prepro_only_p ? pre_text_out : pre_out) (c2m_ctx, t);
}

static void pre (c2m_ctx_t c2m_ctx, const char *start_source_name) {
  pre_last_token = NULL;
  actual_pre_pos.fname = NULL;
  should_be_pre_pos.fname = start_source_name;
  should_be_pre_pos.lno = 0;
  should_be_pre_pos.ln_pos = 0;
  pre_out_token_func = common_pre_out;
  pptokens_num = 0;
  if (!options->no_prepro_p) {
    processing (c2m_ctx, FALSE);
  } else {
    for (;;) {
      token_t t = get_next_pptoken (c2m_ctx);

      if (t->code == T_EOFILE || t->code == T_EOU) break;
      pre_out_token_func (c2m_ctx, t);
    }
  }
  pre_out_token_func (c2m_ctx, NULL);
  if (options->verbose_p && options->message_file != NULL)
    fprintf (options->message_file, "    preprocessor tokens -- %lu, parse tokens -- %lu\n",
             pptokens_num, (unsigned long) VARR_LENGTH (token_t, recorded_tokens));
}

/* ------------------------- Preprocessor End ------------------------------ */

struct parse_ctx {
  int record_level;
  size_t next_token_index;
  token_t curr_token;
  node_t curr_scope;
};

#define record_level c2m_ctx->parse_ctx->record_level
#define next_token_index c2m_ctx->parse_ctx->next_token_index
#define next_token_index c2m_ctx->parse_ctx->next_token_index
#define curr_token c2m_ctx->parse_ctx->curr_token
#define curr_scope c2m_ctx->parse_ctx->curr_scope

static struct node err_struct;
static const node_t err_node = &err_struct;

static void read_token (c2m_ctx_t c2m_ctx) {
  curr_token = VARR_GET (token_t, recorded_tokens, next_token_index);
  next_token_index++;
}

static size_t record_start (c2m_ctx_t c2m_ctx) {
  assert (next_token_index > 0 && record_level >= 0);
  record_level++;
  return next_token_index - 1;
}

static void record_stop (c2m_ctx_t c2m_ctx, size_t mark, int restore_p) {
  assert (record_level > 0);
  record_level--;
  if (!restore_p) return;
  next_token_index = mark;
  read_token (c2m_ctx);
}

static void syntax_error (c2m_ctx_t c2m_ctx, const char *expected_name) {
  FILE *f;

  if ((f = options->message_file) == NULL) return;
  print_pos (f, curr_token->pos, TRUE);
  fprintf (f, "syntax error on %s", get_token_name (c2m_ctx, curr_token->code));
  fprintf (f, " (expected '%s'):", expected_name);
#if 0
  {
    static const int context_len = 5;

    for (int i = 0; i < context_len && curr_token->code != T_EOFILE; i++) {
      fprintf (f, " %s", curr_token->repr);
    }
  }
#endif
  fprintf (f, "\n");
  n_errors++;
}

typedef struct {
  node_t id, scope;
} tpname_t;

DEF_HTAB (tpname_t);
static HTAB (tpname_t) * tpname_tab;

static int tpname_eq (tpname_t tpname1, tpname_t tpname2) {
  return tpname1.id->u.s.s == tpname2.id->u.s.s && tpname1.scope == tpname2.scope;
}

static htab_hash_t tpname_hash (tpname_t tpname) {
  return (mir_hash_finish (
    mir_hash_step (mir_hash_step (mir_hash_init (0x42), (uint64_t) tpname.id->u.s.s),
                   (uint64_t) tpname.scope)));
}

static void tpname_init (void) { HTAB_CREATE (tpname_t, tpname_tab, 1000, tpname_hash, tpname_eq); }

static int tpname_find (node_t id, node_t scope, tpname_t *res) {
  int found_p;
  tpname_t el, tpname;

  tpname.id = id;
  tpname.scope = scope;
  found_p = HTAB_DO (tpname_t, tpname_tab, tpname, HTAB_FIND, el);
  if (res != NULL && found_p) *res = el;
  return found_p;
}

static tpname_t tpname_add (node_t id, node_t scope) {
  tpname_t el, tpname;

  tpname.id = id;
  tpname.scope = scope;
  if (HTAB_DO (tpname_t, tpname_tab, tpname, HTAB_FIND, el)) return el;
  HTAB_DO (tpname_t, tpname_tab, tpname, HTAB_INSERT, el);
  return el;
}

static void tpname_finish (void) {
  if (tpname_tab != NULL) HTAB_DESTROY (tpname_t, tpname_tab);
}

#define P(f)                                                 \
  do {                                                       \
    if ((r = (f) (c2m_ctx, no_err_p)) == err_node) return r; \
  } while (0)
#define PA(f, a)                                                \
  do {                                                          \
    if ((r = (f) (c2m_ctx, no_err_p, a)) == err_node) return r; \
  } while (0)
#define PTFAIL(t)                                                               \
  do {                                                                          \
    if (record_level == 0) syntax_error (c2m_ctx, get_token_name (c2m_ctx, t)); \
    return err_node;                                                            \
  } while (0)

#define PT(t)               \
  do {                      \
    if (!M (t)) PTFAIL (t); \
  } while (0)

#define PTP(t, pos)               \
  do {                            \
    if (!MP (t, pos)) PTFAIL (t); \
  } while (0)

#define PTN(t)                  \
  do {                          \
    if (!MN (t, r)) PTFAIL (t); \
  } while (0)

#define PE(f, l)                                           \
  do {                                                     \
    if ((r = (f) (c2m_ctx, no_err_p)) == err_node) goto l; \
  } while (0)
#define PAE(f, a, l)                                          \
  do {                                                        \
    if ((r = (f) (c2m_ctx, no_err_p, a)) == err_node) goto l; \
  } while (0)
#define PTE(t, pos, l)        \
  do {                        \
    if (!MP (t, pos)) goto l; \
  } while (0)

typedef node_t (*nonterm_func_t) (c2m_ctx_t c2m_ctx, int);
typedef node_t (*nonterm_arg_func_t) (c2m_ctx_t c2m_ctx, int, node_t);

#define D(f) static node_t f (c2m_ctx_t c2m_ctx, int no_err_p)
#define DA(f) static node_t f (c2m_ctx_t c2m_ctx, int no_err_p, node_t arg)

#define C(c) (curr_token->code == c)

static int match (c2m_ctx_t c2m_ctx, int c, pos_t *pos, node_code_t *node_code, node_t *node) {
  if (curr_token->code != c) return FALSE;
  if (pos != NULL) *pos = curr_token->pos;
  if (node_code != NULL) *node_code = curr_token->node_code;
  if (node != NULL) *node = curr_token->node;
  read_token (c2m_ctx);
  return TRUE;
}

#define M(c) match (c2m_ctx, c, NULL, NULL, NULL)
#define MP(c, pos) match (c2m_ctx, c, &(pos), NULL, NULL)
#define MC(c, pos, code) match (c2m_ctx, c, &(pos), &(code), NULL)
#define MN(c, node) match (c2m_ctx, c, NULL, NULL, &(node))

static node_t try_f (c2m_ctx_t c2m_ctx, nonterm_func_t f) {
  size_t mark = record_start (c2m_ctx);
  node_t r = (f) (c2m_ctx, TRUE);

  record_stop (c2m_ctx, mark, r == err_node);
  return r;
}

static node_t try_arg_f (c2m_ctx_t c2m_ctx, nonterm_arg_func_t f, node_t arg) {
  size_t mark = record_start (c2m_ctx);
  node_t r = (f) (c2m_ctx, TRUE, arg);

  record_stop (c2m_ctx, mark, r == err_node);
  return r;
}

#define TRY(f) try_f (c2m_ctx, f)
#define TRY_A(f, arg) try_arg_f (c2m_ctx, f, arg)

/* Expressions: */
D (type_name);
D (expr);
D (assign_expr);
D (initializer_list);

D (par_type_name) {
  node_t r;

  PT ('(');
  P (type_name);
  PT (')');
  return r;
}

D (primary_expr) {
  node_t r, n, op, gn, list;
  pos_t pos;

  if (MN (T_ID, r) || MN (T_NUMBER, r) || MN (T_CH, r) || MN (T_STR, r)) {
    return r;
  } else if (M ('(')) {
    P (expr);
    if (M (')')) return r;
  } else if (MP (T_GENERIC, pos)) {
    PT ('(');
    P (assign_expr);
    PT (',');
    list = new_node (c2m_ctx, N_LIST);
    n = new_pos_node2 (c2m_ctx, N_GENERIC, pos, r, list);
    for (;;) { /* generic-assoc-list , generic-association */
      if (MP (T_DEFAULT, pos)) {
        op = new_node (c2m_ctx, N_IGNORE);
      } else {
        P (type_name);
        op = r;
        pos = op->pos;
      }
      PT (':');
      P (assign_expr);
      gn = new_pos_node2 (c2m_ctx, N_GENERIC_ASSOC, pos, op, r);
      op_append (list, gn);
      if (!M (',')) break;
    }
    PT (')');
    return n;
  }
  return err_node;
}

DA (post_expr_part) {
  node_t r, n, op, list;
  node_code_t code;
  pos_t pos;

  r = arg;
  for (;;) {
    if (MC (T_INCDEC, pos, code)) {
      code = code == N_INC ? N_POST_INC : N_POST_DEC;
      op = r;
      r = NULL;
    } else if (MC ('.', pos, code) || MC (T_ARROW, pos, code)) {
      op = r;
      if (!MN (T_ID, r)) return err_node;
    } else if (MC ('[', pos, code)) {
      op = r;
      P (expr);
      PT (']');
    } else if (!MP ('(', pos)) {
      break;
    } else {
      op = r;
      r = NULL;
      code = N_CALL;
      list = new_node (c2m_ctx, N_LIST);
      if (!C (')')) {
        for (;;) {
          P (assign_expr);
          op_append (list, r);
          if (!M (',')) break;
        }
      }
      r = list;
      PT (')');
    }
    n = new_pos_node1 (c2m_ctx, code, pos, op);
    if (r != NULL) op_append (n, r);
    r = n;
  }
  return r;
}

D (post_expr) {
  node_t r;

  P (primary_expr);
  PA (post_expr_part, r);
  return r;
}

D (unary_expr) {
  node_t r, t;
  node_code_t code;
  pos_t pos;

  if ((r = TRY (par_type_name)) != err_node) {
    t = r;
    if (!MP ('{', pos)) {
      P (unary_expr);
      r = new_node2 (c2m_ctx, N_CAST, t, r);
    } else {
      P (initializer_list);
      if (!M ('}')) return err_node;
      r = new_pos_node2 (c2m_ctx, N_COMPOUND_LITERAL, pos, t, r);
      PA (post_expr_part, r);
    }
    return r;
  } else if (MP (T_SIZEOF, pos)) {
    if ((r = TRY (par_type_name)) != err_node) {
      r = new_pos_node1 (c2m_ctx, N_SIZEOF, pos, r);
      return r;
    }
    code = N_EXPR_SIZEOF;
  } else if (MP (T_ALIGNOF, pos)) {
    if ((r = TRY (par_type_name)) != err_node) {
      r = new_pos_node1 (c2m_ctx, N_ALIGNOF, pos, r);
    } else {
      P (unary_expr); /* error recovery */
      r = new_pos_node1 (c2m_ctx, N_ALIGNOF, pos, new_node (c2m_ctx, N_IGNORE));
    }
    return r;
  } else if (!MC (T_INCDEC, pos, code) && !MC (T_UNOP, pos, code) && !MC (T_ADDOP, pos, code)
             && !MC ('*', pos, code) && !MC ('&', pos, code)) {
    P (post_expr);
    return r;
  } else if (code == N_AND) {
    code = N_ADDR;
  } else if (code == N_MUL) {
    code = N_DEREF;
  }
  P (unary_expr);
  r = new_pos_node1 (c2m_ctx, code, pos, r);
  return r;
}

static node_t left_op (c2m_ctx_t c2m_ctx, int no_err_p, int token, int token2, nonterm_func_t f) {
  node_code_t code;
  node_t r, n;
  pos_t pos;

  P (f);
  while (MC (token, pos, code) || (token2 >= 0 && MC (token2, pos, code))) {
    n = new_pos_node1 (c2m_ctx, code, pos, r);
    P (f);
    op_append (n, r);
    r = n;
  }
  return r;
}

static node_t right_op (c2m_ctx_t c2m_ctx, int no_err_p, int token, int token2, nonterm_func_t left,
                        nonterm_func_t right) {
  node_code_t code;
  node_t r, n;
  pos_t pos;

  P (left);
  if (MC (token, pos, code) || (token2 >= 0 && MC (token2, pos, code))) {
    n = new_pos_node1 (c2m_ctx, code, pos, r);
    P (right);
    op_append (n, r);
    r = n;
  }
  return r;
}

D (mul_expr) { return left_op (c2m_ctx, no_err_p, T_DIVOP, '*', unary_expr); }
D (add_expr) { return left_op (c2m_ctx, no_err_p, T_ADDOP, -1, mul_expr); }
D (sh_expr) { return left_op (c2m_ctx, no_err_p, T_SH, -1, add_expr); }
D (rel_expr) { return left_op (c2m_ctx, no_err_p, T_CMP, -1, sh_expr); }
D (eq_expr) { return left_op (c2m_ctx, no_err_p, T_EQNE, -1, rel_expr); }
D (and_expr) { return left_op (c2m_ctx, no_err_p, '&', -1, eq_expr); }
D (xor_expr) { return left_op (c2m_ctx, no_err_p, '^', -1, and_expr); }
D (or_expr) { return left_op (c2m_ctx, no_err_p, '|', -1, xor_expr); }
D (land_expr) { return left_op (c2m_ctx, no_err_p, T_ANDAND, -1, or_expr); }
D (lor_expr) { return left_op (c2m_ctx, no_err_p, T_OROR, -1, land_expr); }

D (cond_expr) {
  node_t r, n;
  pos_t pos;

  P (lor_expr);
  if (!MP ('?', pos)) return r;
  n = new_pos_node1 (c2m_ctx, N_COND, pos, r);
  P (expr);
  op_append (n, r);
  if (!M (':')) return err_node;
  P (cond_expr);
  op_append (n, r);
  return n;
}

#define const_expr cond_expr

D (assign_expr) { return right_op (c2m_ctx, no_err_p, T_ASSIGN, '=', cond_expr, assign_expr); }
D (expr) { return right_op (c2m_ctx, no_err_p, ',', -1, assign_expr, expr); }

/* Declarations; */
DA (declaration_specs);
D (sc_spec);
DA (type_spec);
D (struct_declaration_list);
D (struct_declaration);
D (spec_qual_list);
D (type_qual);
D (func_spec);
D (align_spec);
D (declarator);
D (direct_declarator);
D (pointer);
D (type_qual_list);
D (param_type_list);
D (id_list);
D (abstract_declarator);
D (direct_abstract_declarator);
D (typedef_name);
D (initializer);
D (st_assert);

D (declaration) {
  int typedef_p;
  node_t op, list, decl, spec, r;
  pos_t pos;

  if (C (T_STATIC_ASSERT)) {
    P (st_assert);
  } else if (MP (';', pos)) {
    r = new_node (c2m_ctx, N_LIST);
    if (curr_scope == top_scope && options->pedantic_p)
      warning (c2m_ctx, pos, "extra ; outside of a function");
  } else {
    PA (declaration_specs, curr_scope == top_scope ? (node_t) 1 : NULL);
    spec = r;
    list = new_node (c2m_ctx, N_LIST);
    if (C (';')) {
      op_append (list, new_node3 (c2m_ctx, N_SPEC_DECL, spec, new_node (c2m_ctx, N_IGNORE),
                                  new_node (c2m_ctx, N_IGNORE)));
    } else { /* init-declarator-list */
      for (op = NL_HEAD (spec->ops); op != NULL; op = NL_NEXT (op))
        if (op->code == N_TYPEDEF) break;
      typedef_p = op != NULL;
      for (;;) { /* init-declarator */
        P (declarator);
        decl = r;
        assert (decl->code == N_DECL);
        if (typedef_p) {
          op = NL_HEAD (decl->ops);
          tpname_add (op, curr_scope);
        }
        if (M ('=')) {
          P (initializer);
        } else {
          r = new_node (c2m_ctx, N_IGNORE);
        }
        op_append (list, new_pos_node3 (c2m_ctx, N_SPEC_DECL, decl->pos,
                                        new_node1 (c2m_ctx, N_SHARE, spec), decl, r));
        if (!M (',')) break;
      }
    }
    r = list;
    PT (';');
  }
  return r;
}

D (attr) {
  if (C (')') || C (',')) /* empty */
    return NULL;
  if (FIRST_KW <= curr_token->code && curr_token->code <= LAST_KW)
    PT (curr_token->code);
  else
    PT (T_ID);
  if (C ('(')) {
    PT ('(');
    while (!C (')')) {
      if (C (T_NUMBER) || C (T_CH) || C (T_STR))
        PT (curr_token->code);
      else
        PT (T_ID);
      if (!C (')')) PT (',');
    }
    PT (')');
  }
  return NULL;
}

D (attr_spec) {
  node_t r;

  PTN (T_ID);
  if (strcmp (r->u.s.s, "__attribute__") != 0) PTFAIL (T_ID);
  PT ('(');
  PT ('(');
  for (;;) {
    P (attr);
    if (C (')')) break;
    PT (',');
  }
  PT (')');
  PT (')');
  return NULL;
}

DA (declaration_specs) {
  node_t list, r, prev_type_spec = NULL;
  int first_p;
  pos_t pos = curr_token->pos, spec_pos;

  list = new_node (c2m_ctx, N_LIST);
  for (first_p = arg == NULL;; first_p = FALSE) {
    spec_pos = curr_token->pos;
    if (C (T_ALIGNAS)) {
      P (align_spec);
    } else if ((r = TRY (sc_spec)) != err_node) {
    } else if ((r = TRY (type_qual)) != err_node) {
    } else if ((r = TRY (func_spec)) != err_node) {
    } else if (first_p) {
      PA (type_spec, prev_type_spec);
      prev_type_spec = r;
    } else if ((r = TRY_A (type_spec, prev_type_spec)) != err_node) {
      prev_type_spec = r;
    } else if ((r = TRY (attr_spec)) != err_node) {
      if (options->pedantic_p)
        error (c2m_ctx, spec_pos, "GCC attributes are not implemented");
      else
        warning (c2m_ctx, spec_pos, "GCC attributes are not implemented -- ignoring them");
      continue;
    } else
      break;
    op_append (list, r);
  }
  if (prev_type_spec == NULL && arg != NULL) {
    if (options->pedantic_p) warning (c2m_ctx, pos, "type defaults to int");
    r = new_pos_node (c2m_ctx, N_INT, pos);
    op_append (list, r);
  }
  return list;
}

D (sc_spec) {
  node_t r;
  pos_t pos;

  if (MP (T_TYPEDEF, pos)) {
    r = new_pos_node (c2m_ctx, N_TYPEDEF, pos);
  } else if (MP (T_EXTERN, pos)) {
    r = new_pos_node (c2m_ctx, N_EXTERN, pos);
  } else if (MP (T_STATIC, pos)) {
    r = new_pos_node (c2m_ctx, N_STATIC, pos);
  } else if (MP (T_AUTO, pos)) {
    r = new_pos_node (c2m_ctx, N_AUTO, pos);
  } else if (MP (T_REGISTER, pos)) {
    r = new_pos_node (c2m_ctx, N_REGISTER, pos);
  } else if (MP (T_THREAD_LOCAL, pos)) {
    r = new_pos_node (c2m_ctx, N_THREAD_LOCAL, pos);
  } else {
    if (record_level == 0) syntax_error (c2m_ctx, "a storage specifier");
    return err_node;
  }
  return r;
}

DA (type_spec) {
  node_t op1, op2, op3, op4, r;
  int struct_p, id_p = FALSE;
  pos_t pos;

  if (MP (T_VOID, pos)) {
    r = new_pos_node (c2m_ctx, N_VOID, pos);
  } else if (MP (T_CHAR, pos)) {
    r = new_pos_node (c2m_ctx, N_CHAR, pos);
  } else if (MP (T_SHORT, pos)) {
    r = new_pos_node (c2m_ctx, N_SHORT, pos);
  } else if (MP (T_INT, pos)) {
    r = new_pos_node (c2m_ctx, N_INT, pos);
  } else if (MP (T_LONG, pos)) {
    r = new_pos_node (c2m_ctx, N_LONG, pos);
  } else if (MP (T_FLOAT, pos)) {
    r = new_pos_node (c2m_ctx, N_FLOAT, pos);
  } else if (MP (T_DOUBLE, pos)) {
    r = new_pos_node (c2m_ctx, N_DOUBLE, pos);
  } else if (MP (T_SIGNED, pos)) {
    r = new_pos_node (c2m_ctx, N_SIGNED, pos);
  } else if (MP (T_UNSIGNED, pos)) {
    r = new_pos_node (c2m_ctx, N_UNSIGNED, pos);
  } else if (MP (T_BOOL, pos)) {
    r = new_pos_node (c2m_ctx, N_BOOL, pos);
  } else if (MP (T_COMPLEX, pos)) {
    if (record_level == 0) error (c2m_ctx, pos, "complex numbers are not supported");
    return err_node;
  } else if (MP (T_ATOMIC, pos)) { /* atomic-type-specifier */
    PT ('(');
    P (type_name);
    PT (')');
    error (c2m_ctx, pos, "Atomic types are not supported");
  } else if ((struct_p = MP (T_STRUCT, pos)) || MP (T_UNION, pos)) {
    /* struct-or-union-specifier, struct-or-union */
    if (!MN (T_ID, op1)) {
      op1 = new_node (c2m_ctx, N_IGNORE);
    } else {
      id_p = TRUE;
    }
    if (M ('{')) {
      if (!C ('}')) {
        P (struct_declaration_list);
      } else {
        (options->pedantic_p ? error : warning) (c2m_ctx, pos, "empty struct/union");
        r = new_node (c2m_ctx, N_LIST);
      }
      PT ('}');
    } else if (!id_p) {
      return err_node;
    } else {
      r = new_node (c2m_ctx, N_IGNORE);
    }
    r = new_pos_node2 (c2m_ctx, struct_p ? N_STRUCT : N_UNION, pos, op1, r);
  } else if (MP (T_ENUM, pos)) { /* enum-specifier */
    if (!MN (T_ID, op1)) {
      op1 = new_node (c2m_ctx, N_IGNORE);
    } else {
      id_p = TRUE;
    }
    op2 = new_node (c2m_ctx, N_LIST);
    if (M ('{')) { /* enumerator-list */
      for (;;) {   /* enumerator */
        PTN (T_ID);
        op3 = r; /* enumeration-constant */
        if (!M ('=')) {
          op4 = new_node (c2m_ctx, N_IGNORE);
        } else {
          P (const_expr);
          op4 = r;
        }
        op_append (op2, new_node2 (c2m_ctx, N_ENUM_CONST, op3, op4));
        if (!M (',')) break;
        if (C ('}')) break;
      }
      PT ('}');
    } else if (!id_p) {
      return err_node;
    } else {
      op2 = new_node (c2m_ctx, N_IGNORE);
    }
    r = new_pos_node2 (c2m_ctx, N_ENUM, pos, op1, op2);
  } else if (arg == NULL) {
    P (typedef_name);
  } else {
    r = err_node;
  }
  return r;
}

D (struct_declaration_list) {
  node_t r, res, el, next_el;

  res = new_node (c2m_ctx, N_LIST);
  for (;;) {
    P (struct_declaration);
    if (r->code != N_LIST) {
      op_append (res, r);
    } else {
      for (el = NL_HEAD (r->ops); el != NULL; el = next_el) {
        next_el = NL_NEXT (el);
        NL_REMOVE (r->ops, el);
        op_append (res, el);
      }
    }
    if (C ('}')) break;
  }
  return res;
}

D (struct_declaration) {
  node_t list, spec, op, r;

  if (C (T_STATIC_ASSERT)) {
    P (st_assert);
  } else {
    P (spec_qual_list);
    spec = r;
    list = new_node (c2m_ctx, N_LIST);
    if (M (';')) {
      op = new_pos_node3 (c2m_ctx, N_MEMBER, spec->pos, new_node1 (c2m_ctx, N_SHARE, spec),
                          new_node (c2m_ctx, N_IGNORE), new_node (c2m_ctx, N_IGNORE));
      op_append (list, op);
    } else {     /* struct-declarator-list */
      for (;;) { /* struct-declarator */
        if (!C (':')) {
          P (declarator);
          op = r;
        } else {
          op = new_node (c2m_ctx, N_IGNORE);
        }
        if (M (':')) {
          P (const_expr);
        } else {
          r = new_node (c2m_ctx, N_IGNORE);
        }
        op = new_pos_node3 (c2m_ctx, N_MEMBER, op->pos, new_node1 (c2m_ctx, N_SHARE, spec), op, r);
        op_append (list, op);
        if (!M (',')) break;
      }
      PT (';');
    }
    r = list;
  }
  return r;
}

D (spec_qual_list) {
  node_t list, op, r, arg = NULL;
  int first_p;

  list = new_node (c2m_ctx, N_LIST);
  for (first_p = TRUE;; first_p = FALSE) {
    if (C (T_CONST) || C (T_RESTRICT) || C (T_VOLATILE) || C (T_ATOMIC)) {
      P (type_qual);
      op = r;
    } else if ((op = TRY_A (type_spec, arg)) != err_node) {
      arg = op;
    } else if (first_p) {
      return err_node;
    } else {
      break;
    }
    op_append (list, op);
  }
  return list;
}

D (type_qual) {
  node_t r;
  pos_t pos;

  if (MP (T_CONST, pos)) {
    r = new_pos_node (c2m_ctx, N_CONST, pos);
  } else if (MP (T_RESTRICT, pos)) {
    r = new_pos_node (c2m_ctx, N_RESTRICT, pos);
  } else if (MP (T_VOLATILE, pos)) {
    r = new_pos_node (c2m_ctx, N_VOLATILE, pos);
  } else if (MP (T_ATOMIC, pos)) {
    r = new_pos_node (c2m_ctx, N_ATOMIC, pos);
  } else {
    if (record_level == 0) syntax_error (c2m_ctx, "a type qualifier");
    r = err_node;
  }
  return r;
}

D (func_spec) {
  node_t r;
  pos_t pos;

  if (MP (T_INLINE, pos)) {
    r = new_pos_node (c2m_ctx, N_INLINE, pos);
  } else if (MP (T_NO_RETURN, pos)) {
    r = new_pos_node (c2m_ctx, N_NO_RETURN, pos);
  } else {
    if (record_level == 0) syntax_error (c2m_ctx, "a function specifier");
    r = err_node;
  }
  return r;
}

D (align_spec) {
  node_t r;
  pos_t pos;

  PTP (T_ALIGNAS, pos);
  PT ('(');
  if ((r = TRY (type_name)) != err_node) {
  } else {
    P (const_expr);
  }
  PT (')');
  r = new_pos_node1 (c2m_ctx, N_ALIGNAS, pos, r);
  return r;
}

D (declarator) {
  node_t list, p = NULL, r, el, next_el;

  if (C ('*')) {
    P (pointer);
    p = r;
  }
  P (direct_declarator);
  if (p != NULL) {
    list = NL_NEXT (NL_HEAD (r->ops));
    assert (list->code == N_LIST);
    for (el = NL_HEAD (p->ops); el != NULL; el = next_el) {
      next_el = NL_NEXT (el);
      NL_REMOVE (p->ops, el);
      op_append (list, el);
    }
  }
  return r;
}

D (direct_declarator) {
  node_t list, tql, ae, res, r;
  pos_t pos, static_pos;

  if (MN (T_ID, r)) {
    res = new_node2 (c2m_ctx, N_DECL, r, new_node (c2m_ctx, N_LIST));
  } else if (M ('(')) {
    P (declarator);
    res = r;
    PT (')');
  } else {
    return err_node;
  }
  list = NL_NEXT (NL_HEAD (res->ops));
  assert (list->code == N_LIST);
  for (;;) {
    if (MP ('(', pos)) {
      if ((r = TRY (param_type_list)) != err_node) {
      } else {
        P (id_list);
      }
      PT (')');
      op_append (list, new_pos_node1 (c2m_ctx, N_FUNC, pos, r));
    } else if (M ('[')) {
      int static_p = FALSE;

      if (MP (T_STATIC, static_pos)) {
        static_p = TRUE;
      }
      if (!C (T_CONST) && !C (T_RESTRICT) && !C (T_VOLATILE) && !C (T_ATOMIC)) {
        tql = new_node (c2m_ctx, N_LIST);
      } else {
        P (type_qual_list);
        tql = r;
        if (!static_p && M (T_STATIC)) {
          static_p = TRUE;
        }
      }
      if (static_p) {
        P (assign_expr);
        ae = r;
      } else if (MP ('*', pos)) {
        ae = new_pos_node (c2m_ctx, N_STAR, pos);
      } else if (!C (']')) {
        P (assign_expr);
        ae = r;
      } else {
        ae = new_node (c2m_ctx, N_IGNORE);
      }
      PT (']');
      op_append (list, new_node3 (c2m_ctx, N_ARR,
                                  static_p ? new_pos_node (c2m_ctx, N_STATIC, static_pos)
                                           : new_node (c2m_ctx, N_IGNORE),
                                  tql, ae));
    } else
      break;
  }
  return res;
}

D (pointer) {
  node_t op, r;
  pos_t pos;

  PTP ('*', pos);
  if (C (T_CONST) || C (T_RESTRICT) || C (T_VOLATILE) || C (T_ATOMIC)) {
    P (type_qual_list);
  } else {
    r = new_node (c2m_ctx, N_LIST);
  }
  op = new_pos_node1 (c2m_ctx, N_POINTER, pos, r);
  if (C ('*')) {
    P (pointer);
  } else {
    r = new_node (c2m_ctx, N_LIST);
  }
  op_append (r, op);
  return r;
}

D (type_qual_list) {
  node_t list, r;

  list = new_node (c2m_ctx, N_LIST);
  for (;;) {
    P (type_qual);
    op_append (list, r);
    if (!C (T_CONST) && !C (T_RESTRICT) && !C (T_VOLATILE) && !C (T_ATOMIC)) break;
  }
  return list;
}

D (param_type_abstract_declarator) {
  node_t r = err_node;

  P (abstract_declarator);
  if (C (',') || C (')')) return r;
  return err_node;
}

D (param_type_list) {
  node_t list, op1, op2, r = err_node;
  int comma_p;
  pos_t pos;

  list = new_node (c2m_ctx, N_LIST);
  for (;;) { /* parameter-list, parameter-declaration */
    PA (declaration_specs, NULL);
    op1 = r;
    if (C (',') || C (')')) {
      r = new_node2 (c2m_ctx, N_TYPE, op1,
                     new_node2 (c2m_ctx, N_DECL, new_node (c2m_ctx, N_IGNORE),
                                new_node (c2m_ctx, N_LIST)));
    } else if ((op2 = TRY (param_type_abstract_declarator)) != err_node) {
      /* Try param_type_abstract_declarator first for possible func
         type case ("<res_type> (<typedef_name>)") which can conflict with declarator ("<res_type>
         (<new decl identifier>)")  */
      r = new_node2 (c2m_ctx, N_TYPE, op1, op2);
    } else {
      P (declarator);
      r = new_pos_node3 (c2m_ctx, N_SPEC_DECL, op2->pos, op1, r, new_node (c2m_ctx, N_IGNORE));
    }
    op_append (list, r);
    comma_p = FALSE;
    if (!M (',')) break;
    comma_p = TRUE;
    if (C (T_DOTS)) break;
  }
  if (comma_p) {
    PTP (T_DOTS, pos);
    op_append (list, new_pos_node (c2m_ctx, N_DOTS, pos));
  }
  return list;
}

D (id_list) {
  node_t list, r;

  list = new_node (c2m_ctx, N_LIST);
  if (C (')')) return list;
  for (;;) {
    PTN (T_ID);
    op_append (list, r);
    if (!M (',')) break;
  }
  return list;
}

D (abstract_declarator) {
  node_t list, p = NULL, r, el, next_el;

  if (C ('*')) {
    P (pointer);
    p = r;
    if ((r = TRY (direct_abstract_declarator)) == err_node)
      r = new_pos_node2 (c2m_ctx, N_DECL, p->pos, new_node (c2m_ctx, N_IGNORE),
                         new_node (c2m_ctx, N_LIST));
  } else {
    P (direct_abstract_declarator);
  }
  if (p != NULL) {
    list = NL_NEXT (NL_HEAD (r->ops));
    assert (list->code == N_LIST);
    for (el = NL_HEAD (p->ops); el != NULL; el = next_el) {
      next_el = NL_NEXT (el);
      NL_REMOVE (p->ops, el);
      op_append (list, el);
    }
  }
  return r;
}

D (par_abstract_declarator) {
  node_t r;

  PT ('(');
  P (abstract_declarator);
  PT (')');
  return r;
}

D (direct_abstract_declarator) {
  node_t res, list, tql, ae, r;
  pos_t pos, pos2 = no_pos;

  if ((res = TRY (par_abstract_declarator)) != err_node) {
    if (!C ('(') && !C ('[')) return res;
  } else {
    res = new_node2 (c2m_ctx, N_DECL, new_node (c2m_ctx, N_IGNORE), new_node (c2m_ctx, N_LIST));
  }
  list = NL_NEXT (NL_HEAD (res->ops));
  assert (list->code == N_LIST);
  for (;;) {
    if (MP ('(', pos)) {
      P (param_type_list);
      PT (')');
      op_append (list, new_pos_node1 (c2m_ctx, N_FUNC, pos, r));
    } else {
      PTP ('[', pos);
      if (MP ('*', pos2)) {
        r = new_pos_node3 (c2m_ctx, N_ARR, pos, new_node (c2m_ctx, N_IGNORE),
                           new_node (c2m_ctx, N_IGNORE), new_pos_node (c2m_ctx, N_STAR, pos2));
      } else {
        int static_p = FALSE;

        if (MP (T_STATIC, pos2)) {
          static_p = TRUE;
        }
        if (!C (T_CONST) && !C (T_RESTRICT) && !C (T_VOLATILE) && !C (T_ATOMIC)) {
          tql = new_node (c2m_ctx, N_LIST);
        } else {
          P (type_qual_list);
          tql = r;
          if (!static_p && M (T_STATIC)) {
            static_p = TRUE;
          }
        }
        if (!C (']')) {
          P (assign_expr);
          ae = r;
        } else {
          ae = new_node (c2m_ctx, N_IGNORE);
        }
        r = new_pos_node3 (c2m_ctx, N_ARR, pos,
                           static_p ? new_pos_node (c2m_ctx, N_STATIC, pos2)
                                    : new_node (c2m_ctx, N_IGNORE),
                           tql, ae);
      }
      PT (']');
      op_append (list, r);
    }
    if (!C ('(') && !C ('[')) break;
  }
  add_pos (res, list->pos);
  return res;
}

D (typedef_name) {
  node_t scope, r;

  PTN (T_ID);
  for (scope = curr_scope;; scope = scope->u.scope) {
    if (tpname_find (r, scope, NULL)) return r;
    if (scope == NULL) break;
  }
  return err_node;
}

D (initializer) {
  node_t r;

  if (!M ('{')) {
    P (assign_expr);
  } else {
    P (initializer_list);
    if (M (','))
      ;
    PT ('}');
  }
  return r;
}

D (initializer_list) {
  node_t list, list2, r;
  int first_p;

  list = new_node (c2m_ctx, N_LIST);
  for (;;) { /* designation */
    list2 = new_node (c2m_ctx, N_LIST);
    for (first_p = TRUE;; first_p = FALSE) { /* designator-list, designator */
      if (M ('[')) {
        P (const_expr);
        PT (']');
      } else if (M ('.')) {
        PTN (T_ID);
        r = new_node1 (c2m_ctx, N_FIELD_ID, r);
      } else
        break;
      op_append (list2, r);
    }
    if (!first_p) {
      PT ('=');
    }
    P (initializer);
    op_append (list, new_node2 (c2m_ctx, N_INIT, list2, r));
    if (!M (',')) break;
    if (C ('}')) break;
  }
  return list;
}

D (type_name) {
  node_t op, r;

  P (spec_qual_list);
  op = r;
  if (!C (')') && !C (':')) {
    P (abstract_declarator);
  } else {
    r = new_pos_node2 (c2m_ctx, N_DECL, op->pos, new_node (c2m_ctx, N_IGNORE),
                       new_node (c2m_ctx, N_LIST));
  }
  return new_node2 (c2m_ctx, N_TYPE, op, r);
}

D (st_assert) {
  node_t op1, r;
  pos_t pos;

  PTP (T_STATIC_ASSERT, pos);
  PT ('(');
  P (const_expr);
  op1 = r;
  PT (',');
  PTN (T_STR);
  PT (')');
  PT (';');
  r = new_pos_node2 (c2m_ctx, N_ST_ASSERT, pos, op1, r);
  return r;
}

/* Statements: */
D (compound_stmt);

D (label) {
  node_t r, n;
  pos_t pos;

  if (MP (T_CASE, pos)) {
    P (expr);
    n = new_pos_node1 (c2m_ctx, N_CASE, pos, r);
    if (M (T_DOTS)) {
      P (expr);
      op_append (n, r);
    }
    r = n;
  } else if (MP (T_DEFAULT, pos)) {
    r = new_pos_node (c2m_ctx, N_DEFAULT, pos);
  } else {
    PTN (T_ID);
    r = new_node1 (c2m_ctx, N_LABEL, r);
  }
  PT (':');
  return r;
}

D (stmt) {
  node_t l, n, op1, op2, op3, r;
  pos_t pos;

  l = new_node (c2m_ctx, N_LIST);
  while ((op1 = TRY (label)) != err_node) {
    op_append (l, op1);
  }
  if (C ('{')) {
    P (compound_stmt);
    if (NL_HEAD (l->ops) != NULL) { /* replace empty label list */
      assert (NL_HEAD (r->ops)->code == N_LIST && NL_HEAD (NL_HEAD (r->ops)->ops) == NULL);
      NL_REMOVE (r->ops, NL_HEAD (r->ops));
      NL_PREPEND (r->ops, l);
    }
  } else if (MP (T_IF, pos)) { /* selection-statement */
    PT ('(');
    P (expr);
    op1 = r;
    PT (')');
    P (stmt);
    op2 = r;
    if (!M (T_ELSE)) {
      r = new_node (c2m_ctx, N_IGNORE);
    } else {
      P (stmt);
    }
    r = new_pos_node4 (c2m_ctx, N_IF, pos, l, op1, op2, r);
  } else if (MP (T_SWITCH, pos)) { /* selection-statement */
    PT ('(');
    P (expr);
    op1 = r;
    PT (')');
    P (stmt);
    r = new_pos_node3 (c2m_ctx, N_SWITCH, pos, l, op1, r);
  } else if (MP (T_WHILE, pos)) { /* iteration-statement */
    PT ('(');
    P (expr);
    op1 = r;
    PT (')');
    P (stmt);
    r = new_pos_node3 (c2m_ctx, N_WHILE, pos, l, op1, r);
  } else if (M (T_DO)) { /* iteration-statement */
    P (stmt);
    op1 = r;
    PTP (T_WHILE, pos);
    PT ('(');
    P (expr);
    PT (')');
    PT (';');
    r = new_pos_node3 (c2m_ctx, N_DO, pos, l, r, op1);
  } else if (MP (T_FOR, pos)) { /* iteration-statement */
    PT ('(');
    n = new_pos_node (c2m_ctx, N_FOR, pos);
    n->u.scope = curr_scope;
    curr_scope = n;
    if ((r = TRY (declaration)) != err_node) {
      op1 = r;
      curr_scope = n->u.scope;
    } else {
      curr_scope = n->u.scope;
      if (!M (';')) {
        P (expr);
        op1 = r;
        PT (';');
      } else {
        op1 = new_node (c2m_ctx, N_IGNORE);
      }
    }
    if (M (';')) {
      op2 = new_node (c2m_ctx, N_IGNORE);
    } else {
      P (expr);
      op2 = r;
      PT (';');
    }
    if (C (')')) {
      op3 = new_node (c2m_ctx, N_IGNORE);
    } else {
      P (expr);
      op3 = r;
    }
    PT (')');
    P (stmt);
    op_append (n, l);
    op_append (n, op1);
    op_append (n, op2);
    op_append (n, op3);
    op_append (n, r);
    r = n;
  } else if (MP (T_GOTO, pos)) { /* jump-statement */
    PTN (T_ID);
    PT (';');
    r = new_pos_node2 (c2m_ctx, N_GOTO, pos, l, r);
  } else if (MP (T_CONTINUE, pos)) { /* continue-statement */
    PT (';');
    r = new_pos_node1 (c2m_ctx, N_CONTINUE, pos, l);
  } else if (MP (T_BREAK, pos)) { /* break-statement */
    PT (';');
    r = new_pos_node1 (c2m_ctx, N_BREAK, pos, l);
  } else if (MP (T_RETURN, pos)) { /* return-statement */
    if (M (';')) {
      r = new_node (c2m_ctx, N_IGNORE);
    } else {
      P (expr);
      PT (';');
    }
    r = new_pos_node2 (c2m_ctx, N_RETURN, pos, l, r);
  } else { /* expression-statement */
    if (C (';')) {
      r = new_node (c2m_ctx, N_IGNORE);
    } else {
      P (expr);
    }
    PT (';');
    r = new_pos_node2 (c2m_ctx, N_EXPR, r->pos, l, r);
  }
  return r;
}

static void error_recovery (c2m_ctx_t c2m_ctx, int par_lev, const char *expected) {
  syntax_error (c2m_ctx, expected);
  if (options->debug_p) fprintf (stderr, "error recovery: skipping");
  for (;;) {
    if (curr_token->code == T_EOFILE || (par_lev == 0 && curr_token->code == ';')) break;
    if (curr_token->code == '{') {
      par_lev++;
    } else if (curr_token->code == '}') {
      if (--par_lev <= 0) break;
    }
    if (options->debug_p)
      fprintf (stderr, " %s(%d:%d)", get_token_name (c2m_ctx, curr_token->code),
               curr_token->pos.lno, curr_token->pos.ln_pos);
    read_token (c2m_ctx);
  }
  if (options->debug_p) fprintf (stderr, " %s\n", get_token_name (c2m_ctx, curr_token->code));
  if (curr_token->code != T_EOFILE) read_token (c2m_ctx);
}

D (compound_stmt) {
  node_t n, list, r;
  pos_t pos;

  PTE ('{', pos, err0);
  list = new_node (c2m_ctx, N_LIST);
  n = new_pos_node2 (c2m_ctx, N_BLOCK, pos, new_node (c2m_ctx, N_LIST), list);
  n->u.scope = curr_scope;
  curr_scope = n;
  while (!C ('}') && !C (T_EOFILE)) { /* block-item-list, block_item */
    if ((r = TRY (declaration)) != err_node) {
    } else {
      PE (stmt, err1);
    }
    op_flat_append (list, r);
    continue;
  err1:
    error_recovery (c2m_ctx, 1, "<statement>");
  }
  curr_scope = n->u.scope;
  if (!C (T_EOFILE)) PT ('}');
  return n;
err0:
  error_recovery (c2m_ctx, 0, "{");
  return err_node;
}

D (transl_unit) {
  node_t list, ds, d, dl, r;

  // curr_token->code = ';'; /* for error recovery */
  read_token (c2m_ctx);
  list = new_node (c2m_ctx, N_LIST);
  while (!C (T_EOFILE)) { /* external-declaration */
    if ((r = TRY (declaration)) != err_node) {
    } else {
      PAE (declaration_specs, (node_t) 1, err);
      ds = r;
      PE (declarator, err);
      d = r;
      dl = new_node (c2m_ctx, N_LIST);
      d->u.scope = curr_scope;
      curr_scope = d;
      while (!C ('{')) { /* declaration-list */
        PE (declaration, decl_err);
        op_flat_append (dl, r);
      }
      P (compound_stmt);
      r = new_pos_node4 (c2m_ctx, N_FUNC_DEF, d->pos, ds, d, dl, r);
      curr_scope = d->u.scope;
    }
    op_flat_append (list, r);
    continue;
  decl_err:
    curr_scope = d->u.scope;
  err:
    error_recovery (c2m_ctx, 0, "<declarator>");
  }
  return new_node1 (c2m_ctx, N_MODULE, list);
}

static void fatal_error (c2m_ctx_t c2m_ctx, C_error_code_t code, const char *message) {
  if (options->message_file != NULL) fprintf (options->message_file, "%s\n", message);
  longjmp (c2m_ctx->env, 1);
}

static void kw_add (c2m_ctx_t c2m_ctx, const char *name, token_code_t tc, size_t flags) {
  str_add (c2m_ctx, name, strlen (name) + 1, tc, flags, TRUE);
}

static void parse_init (MIR_context_t ctx) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);

  c2m_ctx->parse_ctx = c2mir_calloc (ctx, sizeof (struct parse_ctx));
  error_func = fatal_error;
  record_level = 0;
  curr_uid = 0;
  init_streams (c2m_ctx);
  VARR_CREATE (token_t, recorded_tokens, 32);
  VARR_CREATE (token_t, buffered_tokens, 32);
  pre_init (ctx);
  kw_add (c2m_ctx, "_Bool", T_BOOL, 0);
  kw_add (c2m_ctx, "_Complex", T_COMPLEX, 0);
  kw_add (c2m_ctx, "_Alignas", T_ALIGNAS, 0);
  kw_add (c2m_ctx, "_Alignof", T_ALIGNOF, 0);
  kw_add (c2m_ctx, "_Atomic", T_ATOMIC, 0);
  kw_add (c2m_ctx, "_Generic", T_GENERIC, 0);
  kw_add (c2m_ctx, "_Noreturn", T_NO_RETURN, 0);
  kw_add (c2m_ctx, "_Static_assert", T_STATIC_ASSERT, 0);
  kw_add (c2m_ctx, "_Thread_local", T_THREAD_LOCAL, 0);
  kw_add (c2m_ctx, "auto", T_AUTO, 0);
  kw_add (c2m_ctx, "break", T_BREAK, 0);
  kw_add (c2m_ctx, "case", T_CASE, 0);
  kw_add (c2m_ctx, "char", T_CHAR, 0);
  kw_add (c2m_ctx, "const", T_CONST, 0);
  kw_add (c2m_ctx, "continue", T_CONTINUE, 0);
  kw_add (c2m_ctx, "default", T_DEFAULT, 0);
  kw_add (c2m_ctx, "do", T_DO, 0);
  kw_add (c2m_ctx, "double", T_DOUBLE, 0);
  kw_add (c2m_ctx, "else", T_ELSE, 0);
  kw_add (c2m_ctx, "enum", T_ENUM, 0);
  kw_add (c2m_ctx, "extern", T_EXTERN, 0);
  kw_add (c2m_ctx, "float", T_FLOAT, 0);
  kw_add (c2m_ctx, "for", T_FOR, 0);
  kw_add (c2m_ctx, "goto", T_GOTO, 0);
  kw_add (c2m_ctx, "if", T_IF, 0);
  kw_add (c2m_ctx, "inline", T_INLINE, FLAG_EXT89);
  kw_add (c2m_ctx, "int", T_INT, 0);
  kw_add (c2m_ctx, "long", T_LONG, 0);
  kw_add (c2m_ctx, "register", T_REGISTER, 0);
  kw_add (c2m_ctx, "restrict", T_RESTRICT, FLAG_C89);
  kw_add (c2m_ctx, "return", T_RETURN, 0);
  kw_add (c2m_ctx, "short", T_SHORT, 0);
  kw_add (c2m_ctx, "signed", T_SIGNED, 0);
  kw_add (c2m_ctx, "sizeof", T_SIZEOF, 0);
  kw_add (c2m_ctx, "static", T_STATIC, 0);
  kw_add (c2m_ctx, "struct", T_STRUCT, 0);
  kw_add (c2m_ctx, "switch", T_SWITCH, 0);
  kw_add (c2m_ctx, "typedef", T_TYPEDEF, 0);
  kw_add (c2m_ctx, "typeof", T_TYPEOF, FLAG_EXT);
  kw_add (c2m_ctx, "union", T_UNION, 0);
  kw_add (c2m_ctx, "unsigned", T_UNSIGNED, 0);
  kw_add (c2m_ctx, "void", T_VOID, 0);
  kw_add (c2m_ctx, "volatile", T_VOLATILE, 0);
  kw_add (c2m_ctx, "while", T_WHILE, 0);
  kw_add (c2m_ctx, "__restrict", T_RESTRICT, FLAG_EXT);
  kw_add (c2m_ctx, "__restrict__", T_RESTRICT, FLAG_EXT);
  kw_add (c2m_ctx, "__inline", T_INLINE, FLAG_EXT);
  kw_add (c2m_ctx, "__inline__", T_INLINE, FLAG_EXT);
  tpname_init ();
}

#ifndef SOURCEDIR
#define SOURCEDIR "./"
#endif

#ifndef INSTALLDIR
#define INSTALLDIR "/usr/bin/"
#endif

static void add_standard_includes (c2m_ctx_t c2m_ctx) {
  FILE *f;
  const char *str;

  for (int i = 0; i < sizeof (standard_includes) / sizeof (char *); i++) {
    str = standard_includes[i];
    add_string_stream (c2m_ctx, "<environment>", str);
  }
}

static node_t parse (c2m_ctx_t c2m_ctx) {
  next_token_index = 0;
  return transl_unit (c2m_ctx, FALSE);
}

static void parse_finish (c2m_ctx_t c2m_ctx) {
  if (c2m_ctx == NULL || c2m_ctx->parse_ctx == NULL) return;
  if (recorded_tokens != NULL) VARR_DESTROY (token_t, recorded_tokens);
  if (buffered_tokens != NULL) VARR_DESTROY (token_t, buffered_tokens);
  pre_finish (c2m_ctx);
  tpname_finish ();
  finish_streams (c2m_ctx);
  free (c2m_ctx->parse_ctx);
}

#undef P
#undef PT
#undef PTP
#undef PTN
#undef PE
#undef PTE
#undef D
#undef M
#undef MP
#undef MC
#undef MN
#undef TRY
#undef C

/* ------------------------- Parser End ------------------------------ */
/* New Page */
/* ---------------------- Context Checker Start ---------------------- */

/* The context checker is AST traversing pass which checks C11
   constraints.  It also augmenting AST nodes by type and layout
   information.  Here are the created node attributes:

 1. expr nodes have attribute "struct expr", N_ID not expr context has NULL attribute.
 2. N_SWITCH has attribute "struct switch_attr"
 3. N_SPEC_DECL (only with ID), N_MEMBER, N_FUNC_DEF have attribute "struct decl"
 4. N_GOTO hash attribute node_t (target stmt)
 5. N_STRUCT, N_UNION have attribute "struct node_scope" if they have a decl list
 6. N_MODULE, N_BLOCK, N_FOR, N_FUNC have attribute "struct node_scope"
 7. declaration_specs or spec_qual_list N_LISTs have attribute "struct decl_spec",
    but as a part of N_COMPOUND_LITERAL have attribute "struct decl"
 8. N_ENUM_CONST has attribute "struct enum_value"
 9. N_CASE and N_DEFAULT have attribute "struct case_attr"

*/

typedef struct decl *decl_t;
DEF_VARR (decl_t);

typedef struct case_attr *case_t;
DEF_HTAB (case_t);

struct check_ctx {
  VARR (node_t) * gotos;
  node_t func_block_scope;
  unsigned curr_func_scope_num;
  int in_params_p;
  node_t curr_unnamed_anon_struct_union_member;
  node_t curr_switch;
  VARR (decl_t) * func_decls_for_allocation;
  node_t n_i1_node;
  HTAB (case_t) * case_tab;
  node_t curr_func_def, curr_loop, curr_loop_switch;
  mir_size_t curr_call_arg_area_offset;
  VARR (node_t) * context_stack;
};

#define gotos c2m_ctx->check_ctx->gotos
#define func_block_scope c2m_ctx->check_ctx->func_block_scope
#define curr_func_scope_num c2m_ctx->check_ctx->curr_func_scope_num
#define in_params_p c2m_ctx->check_ctx->in_params_p
#define curr_unnamed_anon_struct_union_member \
  c2m_ctx->check_ctx->curr_unnamed_anon_struct_union_member
#define curr_switch c2m_ctx->check_ctx->curr_switch
#define func_decls_for_allocation c2m_ctx->check_ctx->func_decls_for_allocation
#define n_i1_node c2m_ctx->check_ctx->n_i1_node
#define case_tab c2m_ctx->check_ctx->case_tab
#define curr_func_def c2m_ctx->check_ctx->curr_func_def
#define curr_loop c2m_ctx->check_ctx->curr_loop
#define curr_loop_switch c2m_ctx->check_ctx->curr_loop_switch
#define curr_call_arg_area_offset c2m_ctx->check_ctx->curr_call_arg_area_offset
#define context_stack c2m_ctx->check_ctx->context_stack

static int supported_alignment_p (mir_llong align) { return TRUE; }  // ???

static int symbol_eq (symbol_t s1, symbol_t s2) {
  return s1.mode == s2.mode && s1.id->u.s.s == s2.id->u.s.s && s1.scope == s2.scope;
}

static htab_hash_t symbol_hash (symbol_t s) {
  return (mir_hash_finish (
    mir_hash_step (mir_hash_step (mir_hash_step (mir_hash_init (0x42), (uint64_t) s.mode),
                                  (uint64_t) s.id->u.s.s),
                   (uint64_t) s.scope)));
}

static void symbol_clear (symbol_t sym) { VARR_DESTROY (node_t, sym.defs); }

static void symbol_init (c2m_ctx_t c2m_ctx) {
  HTAB_CREATE_WITH_FREE_FUNC (symbol_t, symbol_tab, 5000, symbol_hash, symbol_eq, symbol_clear);
}

static int symbol_find (c2m_ctx_t c2m_ctx, enum symbol_mode mode, node_t id, node_t scope,
                        symbol_t *res) {
  int found_p;
  symbol_t el, symbol;

  symbol.mode = mode;
  symbol.id = id;
  symbol.scope = scope;
  found_p = HTAB_DO (symbol_t, symbol_tab, symbol, HTAB_FIND, el);
  if (res != NULL && found_p) *res = el;
  return found_p;
}

static void symbol_insert (c2m_ctx_t c2m_ctx, enum symbol_mode mode, node_t id, node_t scope,
                           node_t def_node, node_t aux_node) {
  symbol_t el, symbol;

  symbol.mode = mode;
  symbol.id = id;
  symbol.scope = scope;
  symbol.def_node = def_node;
  symbol.aux_node = aux_node;
  VARR_CREATE (node_t, symbol.defs, 4);
  HTAB_DO (symbol_t, symbol_tab, symbol, HTAB_INSERT, el);
}

static void symbol_finish (c2m_ctx_t c2m_ctx) {
  if (symbol_tab != NULL) HTAB_DESTROY (symbol_t, symbol_tab);
}

enum basic_type get_int_basic_type (size_t s) {
  return (s == sizeof (mir_int)
            ? TP_INT
            : s == sizeof (mir_short)
                ? TP_SHORT
                : s == sizeof (mir_long) ? TP_LONG : s == sizeof (mir_schar) ? TP_SCHAR : TP_LLONG);
}

static int type_qual_eq_p (const struct type_qual *tq1, const struct type_qual *tq2) {
  return (tq1->const_p == tq2->const_p && tq1->restrict_p == tq2->restrict_p
          && tq1->volatile_p == tq2->volatile_p && tq1->atomic_p == tq2->atomic_p);
}

static void clear_type_qual (struct type_qual *tq) {
  tq->const_p = tq->restrict_p = tq->volatile_p = tq->atomic_p = FALSE;
}

static int type_qual_subset_p (const struct type_qual *tq1, const struct type_qual *tq2) {
  return (tq1->const_p <= tq2->const_p && tq1->restrict_p <= tq2->restrict_p
          && tq1->volatile_p <= tq2->volatile_p && tq1->atomic_p <= tq2->atomic_p);
}

static struct type_qual type_qual_union (const struct type_qual *tq1, const struct type_qual *tq2) {
  struct type_qual res;

  res.const_p = tq1->const_p || tq2->const_p;
  res.restrict_p = tq1->restrict_p || tq2->restrict_p;
  res.volatile_p = tq1->volatile_p || tq2->volatile_p;
  res.atomic_p = tq1->atomic_p || tq2->atomic_p;
  return res;
}

static void init_type (struct type *type) {
  clear_type_qual (&type->type_qual);
  type->pos_node = NULL;
  type->arr_type = NULL;
  type->align = -1;
  type->raw_size = MIR_SIZE_MAX;
  type->unnamed_anon_struct_union_member_type_p = FALSE;
}

static void set_type_pos_node (struct type *type, node_t n) {
  if (type->pos_node == NULL) type->pos_node = n;
}

static int char_type_p (const struct type *type) {
  return (type->mode == TM_BASIC
          && (type->u.basic_type == TP_CHAR || type->u.basic_type == TP_SCHAR
              || type->u.basic_type == TP_UCHAR));
}

static int standard_integer_type_p (const struct type *type) {
  return (type->mode == TM_BASIC && type->u.basic_type >= TP_BOOL
          && type->u.basic_type <= TP_ULLONG);
}

static int integer_type_p (const struct type *type) {
  return standard_integer_type_p (type) || type->mode == TM_ENUM;
}

static int signed_integer_type_p (const struct type *type) {
  if (standard_integer_type_p (type)) {
    enum basic_type tp = type->u.basic_type;

    return ((tp == TP_CHAR && char_is_signed_p ()) || tp == TP_SCHAR || tp == TP_SHORT
            || tp == TP_INT || tp == TP_LONG || tp == TP_LLONG);
  }
  if (type->mode == TM_ENUM) return signed_integer_type_p (&ENUM_INT_TYPE);
  return FALSE;
}

static int floating_type_p (const struct type *type) {
  return type->mode == TM_BASIC
         && (type->u.basic_type == TP_FLOAT || type->u.basic_type == TP_DOUBLE
             || type->u.basic_type == TP_LDOUBLE);
}

static int arithmetic_type_p (const struct type *type) {
  return integer_type_p (type) || floating_type_p (type);
}

static int scalar_type_p (const struct type *type) {
  return arithmetic_type_p (type) || type->mode == TM_PTR;
}

static struct type get_ptr_int_type (int signed_p) {
  struct type res;

  init_type (&res);
  res.mode = TM_BASIC;
  if (sizeof (mir_int) == sizeof (mir_size_t)) {
    res.u.basic_type = signed_p ? TP_INT : TP_UINT;
    return res;
  }
  assert (sizeof (mir_long) == sizeof (mir_size_t));
  res.u.basic_type = signed_p ? TP_LONG : TP_ULONG;
  return res;
}

static struct type integer_promotion (const struct type *type) {
  struct type res;

  assert (integer_type_p (type));
  init_type (&res);
  res.mode = TM_BASIC;
  if (type->mode == TM_BASIC && TP_LONG <= type->u.basic_type && type->u.basic_type <= TP_ULLONG) {
    res.u.basic_type = type->u.basic_type;
    return res;
  }
  if (type->mode == TM_BASIC
      && ((type->u.basic_type == TP_CHAR && MIR_CHAR_MAX > MIR_INT_MAX)
          || (type->u.basic_type == TP_UCHAR && MIR_UCHAR_MAX > MIR_INT_MAX)
          || (type->u.basic_type == TP_USHORT && MIR_USHORT_MAX > MIR_INT_MAX)))
    res.u.basic_type = TP_UINT;
  else if (type->mode == TM_ENUM)
    res.u.basic_type = ENUM_BASIC_INT_TYPE;
  else if (type->mode == TM_BASIC && type->u.basic_type == TP_UINT)
    res.u.basic_type = TP_UINT;
  else
    res.u.basic_type = TP_INT;
  return res;
}

#define SWAP(a1, a2, t) \
  do {                  \
    t = a1;             \
    a1 = a2;            \
    a2 = t;             \
  } while (0)

static struct type arithmetic_conversion (const struct type *type1, const struct type *type2) {
  struct type res, t1, t2;

  assert (arithmetic_type_p (type1) && arithmetic_type_p (type2));
  init_type (&res);
  res.mode = TM_BASIC;
  if (floating_type_p (type1) || floating_type_p (type2)) {
    if ((type1->mode == TM_BASIC && type1->u.basic_type == TP_LDOUBLE)
        || (type2->mode == TM_BASIC && type2->u.basic_type == TP_LDOUBLE)) {
      res.u.basic_type = TP_LDOUBLE;
    } else if ((type1->mode == TM_BASIC && type1->u.basic_type == TP_DOUBLE)
               || (type2->mode == TM_BASIC && type2->u.basic_type == TP_DOUBLE)) {
      res.u.basic_type = TP_DOUBLE;
    } else if ((type1->mode == TM_BASIC && type1->u.basic_type == TP_FLOAT)
               || (type2->mode == TM_BASIC && type2->u.basic_type == TP_FLOAT)) {
      res.u.basic_type = TP_FLOAT;
    }
    return res;
  }
  t1 = integer_promotion (type1);
  t2 = integer_promotion (type2);
  if (signed_integer_type_p (&t1) == signed_integer_type_p (&t2)) {
    res.u.basic_type = t1.u.basic_type < t2.u.basic_type ? t2.u.basic_type : t1.u.basic_type;
  } else {
    struct type t;

    if (signed_integer_type_p (&t1)) SWAP (t1, t2, t);
    assert (!signed_integer_type_p (&t1) && signed_integer_type_p (&t2));
    if ((t1.u.basic_type == TP_ULONG && t2.u.basic_type < TP_LONG)
        || (t1.u.basic_type == TP_ULLONG && t2.u.basic_type < TP_LLONG)) {
      res.u.basic_type = t1.u.basic_type;
    } else if ((t1.u.basic_type == TP_UINT && t2.u.basic_type >= TP_LONG
                && MIR_LONG_MAX >= MIR_UINT_MAX)
               || (t1.u.basic_type == TP_ULONG && t2.u.basic_type >= TP_LLONG
                   && MIR_LLONG_MAX >= MIR_ULONG_MAX)) {
      res.u.basic_type = t2.u.basic_type;
    } else {
      res.u.basic_type = t1.u.basic_type;
    }
  }
  return res;
}

struct expr {
  unsigned int const_p : 1, const_addr_p : 1, builtin_call_p : 1;
  node_t lvalue_node;
  node_t def_node;    /* defined for id or const address (ref) */
  struct type *type;  /* type of the result */
  struct type *type2; /* used for assign expr type */
  union {             /* defined for const or const addr (i_val is offset) */
    mir_llong i_val;
    mir_ullong u_val;
    mir_ldouble d_val;
  } u;
};

struct decl_spec {
  unsigned int typedef_p : 1, extern_p : 1, static_p : 1;
  unsigned int auto_p : 1, register_p : 1, thread_local_p : 1;
  unsigned int inline_p : 1, no_return_p : 1; /* func specifiers  */
  int align;                                  // negative value means undefined
  node_t align_node;                          //  strictest valid N_ALIGNAS node
  node_code_t linkage;  // N_IGNORE - none, N_STATIC - internal, N_EXTERN - external
  struct type *type;
};

struct enum_value {
  mir_int val;
};

struct node_scope {
  unsigned char stack_var_p; /* necessity for frame */
  unsigned func_scope_num;
  mir_size_t size, offset, call_arg_area_size;
  node_t scope;
};

struct decl {
  /* true if address is taken, reg can be used or is used: */
  unsigned addr_p : 1, reg_p : 1, used_p : 1;
  int bit_offset, width; /* for bitfields, -1 bit_offset for non bitfields. */
  mir_size_t offset;     /* var offset in frame or bss */
  node_t scope;          /* declaration scope */
  struct decl_spec decl_spec;
  /* Unnamed member if this scope is anon struct/union for the member,
     NULL otherwise: */
  node_t containing_unnamed_anon_struct_union_member;
  MIR_item_t item; /* MIR_item for some declarations */
  c2m_ctx_t c2m_ctx;
};

static struct decl_spec *get_param_decl_spec (node_t param) {
  node_t declarator;

  if (param->code == N_TYPE) return param->attr;
  declarator = NL_EL (param->ops, 1);
  assert (param->code == N_SPEC_DECL && declarator != NULL && declarator->code == N_DECL);
  return &((decl_t) param->attr)->decl_spec;
}

static int type_eq_p (struct type *type1, struct type *type2) {
  if (type1->mode != type2->mode) return FALSE;
  if (!type_qual_eq_p (&type1->type_qual, &type2->type_qual)) return FALSE;
  switch (type1->mode) {
  case TM_BASIC: return type1->u.basic_type == type2->u.basic_type;
  case TM_ENUM:
  case TM_STRUCT:
  case TM_UNION: return type1->u.tag_type == type2->u.tag_type;
  case TM_PTR: return type_eq_p (type1->u.ptr_type, type2->u.ptr_type);
  case TM_ARR: {
    struct expr *cexpr1, *cexpr2;
    struct arr_type *at1 = type1->u.arr_type, *at2 = type2->u.arr_type;

    return (at1->static_p == at2->static_p && type_eq_p (at1->el_type, at2->el_type)
            && type_qual_eq_p (&at1->ind_type_qual, &at2->ind_type_qual)
            && at1->size->code != N_IGNORE && at2->size->code != N_IGNORE
            && (cexpr1 = at1->size->attr)->const_p && (cexpr2 = at2->size->attr)->const_p
            && integer_type_p (cexpr2->type) && integer_type_p (cexpr2->type)
            && cexpr1->u.i_val == cexpr2->u.i_val);
  }
  case TM_FUNC: {
    struct func_type *ft1 = type1->u.func_type, *ft2 = type2->u.func_type;
    struct decl_spec *ds1, *ds2;

    if (ft1->dots_p != ft2->dots_p || !type_eq_p (ft1->ret_type, ft2->ret_type)
        || NL_LENGTH (ft1->param_list->ops) != NL_LENGTH (ft2->param_list->ops))
      return FALSE;
    for (node_t p1 = NL_HEAD (ft1->param_list->ops), p2 = NL_HEAD (ft2->param_list->ops);
         p1 != NULL; p1 = NL_NEXT (p1), p2 = NL_NEXT (p2)) {
      ds1 = get_param_decl_spec (p1);
      ds2 = get_param_decl_spec (p2);
      if (!type_eq_p (ds1->type, ds2->type)) return FALSE;
      // ??? other qualifiers
    }
    return TRUE;
  }
  default: return FALSE;
  }
}

static int compatible_types_p (struct type *type1, struct type *type2, int ignore_quals_p) {
  if (type1->mode != type2->mode) {
    if (!ignore_quals_p && !type_qual_eq_p (&type1->type_qual, &type2->type_qual)) return FALSE;
    if (type1->mode == TM_ENUM && type2->mode == TM_BASIC)
      return type2->u.basic_type == ENUM_BASIC_INT_TYPE;
    if (type2->mode == TM_ENUM && type1->mode == TM_BASIC)
      return type1->u.basic_type == ENUM_BASIC_INT_TYPE;
    return FALSE;
  }
  if (type1->mode == TM_BASIC) {
    return (type1->u.basic_type == type2->u.basic_type
            && (ignore_quals_p || type_qual_eq_p (&type1->type_qual, &type2->type_qual)));
  } else if (type1->mode == TM_PTR) {
    return ((ignore_quals_p || type_qual_eq_p (&type1->type_qual, &type2->type_qual))
            && compatible_types_p (type1->u.ptr_type, type2->u.ptr_type, ignore_quals_p));
  } else if (type1->mode == TM_ARR) {
    struct expr *cexpr1, *cexpr2;
    struct arr_type *at1 = type1->u.arr_type, *at2 = type2->u.arr_type;

    if (!compatible_types_p (at1->el_type, at2->el_type, ignore_quals_p)) return FALSE;
    if (at1->size->code == N_IGNORE || at2->size->code == N_IGNORE) return TRUE;
    if ((cexpr1 = at1->size->attr)->const_p && (cexpr2 = at2->size->attr)->const_p
        && integer_type_p (cexpr2->type) && integer_type_p (cexpr2->type))
      return cexpr1->u.i_val == cexpr2->u.i_val;
    return TRUE;
  } else if (type1->mode == TM_FUNC) {
    struct func_type *ft1 = type1->u.func_type, *ft2 = type2->u.func_type;

    if (NL_HEAD (ft1->param_list->ops) != NULL && NL_HEAD (ft2->param_list->ops) != NULL
        && NL_LENGTH (ft1->param_list->ops) != NL_LENGTH (ft2->param_list->ops))
      return FALSE;
    // ??? check parameter types
  } else {
    assert (type1->mode == TM_STRUCT || type1->mode == TM_UNION || type1->mode == TM_ENUM);
    return (type1->u.tag_type == type2->u.tag_type
            && (ignore_quals_p || type_qual_eq_p (&type1->type_qual, &type2->type_qual)));
  }
  return TRUE;
}

static struct type composite_type (c2m_ctx_t c2m_ctx, struct type *tp1, struct type *tp2) {
  struct type t = *tp1;

  assert (compatible_types_p (tp1, tp2, TRUE));
  if (tp1->mode == TM_ARR) {
    struct arr_type *arr_type;

    t.u.arr_type = arr_type = reg_malloc (c2m_ctx, sizeof (struct arr_type));
    *arr_type = *tp1->u.arr_type;
    if (arr_type->size == N_IGNORE) arr_type->size = tp2->u.arr_type->size;
    *arr_type->el_type
      = composite_type (c2m_ctx, tp1->u.arr_type->el_type, tp2->u.arr_type->el_type);
  } else if (tp1->mode == TM_FUNC) { /* ??? */
  }
  return t;
}

static struct type *create_type (c2m_ctx_t c2m_ctx, struct type *copy) {
  struct type *res = reg_malloc (c2m_ctx, sizeof (struct type));

  if (copy == NULL)
    init_type (res);
  else
    *res = *copy;
  return res;
}

DEF_DLIST_LINK (case_t);

struct case_attr {
  node_t case_node, case_target_node;
  DLIST_LINK (case_t) case_link;
};

DEF_DLIST (case_t, case_link);

struct switch_attr {
  struct type type; /* integer promoted type */
  int ranges_p;
  case_t min_val_case, max_val_case;
  DLIST (case_t) case_labels; /* default case is always a tail */
};

static int basic_type_size (enum basic_type bt) {
  switch (bt) {
  case TP_BOOL: return sizeof (mir_bool);
  case TP_CHAR: return sizeof (mir_char);
  case TP_SCHAR: return sizeof (mir_schar);
  case TP_UCHAR: return sizeof (mir_uchar);
  case TP_SHORT: return sizeof (mir_short);
  case TP_USHORT: return sizeof (mir_ushort);
  case TP_INT: return sizeof (mir_int);
  case TP_UINT: return sizeof (mir_uint);
  case TP_LONG: return sizeof (mir_long);
  case TP_ULONG: return sizeof (mir_ulong);
  case TP_LLONG: return sizeof (mir_llong);
  case TP_ULLONG: return sizeof (mir_ullong);
  case TP_FLOAT: return sizeof (mir_float);
  case TP_DOUBLE: return sizeof (mir_double);
  case TP_LDOUBLE: return sizeof (mir_ldouble);
  case TP_VOID: return 1;  // ???
  default: abort ();
  }
}

static int basic_type_align (enum basic_type bt) { return basic_type_size (bt); }

static int type_align (struct type *type) {
  assert (type->align >= 0);
  return type->align;
}

static int incomplete_type_p (c2m_ctx_t c2m_ctx, struct type *type);

static void aux_set_type_align (c2m_ctx_t c2m_ctx, struct type *type) {
  /* Should be called only from set_type_layout. */
  int align, member_align;

  if (type->align >= 0) return;
  if (type->mode == TM_BASIC) {
    align = basic_type_align (type->u.basic_type);
  } else if (type->mode == TM_PTR) {
    align = sizeof (mir_size_t);
  } else if (type->mode == TM_ENUM) {
    align = basic_type_align (ENUM_BASIC_INT_TYPE);
  } else if (type->mode == TM_FUNC) {
    align = sizeof (mir_size_t);
  } else if (type->mode == TM_ARR) {
    align = type_align (type->u.arr_type->el_type);
  } else if (type->mode == TM_UNDEF) {
    align = 0; /* error type */
  } else {
    assert (type->mode == TM_STRUCT || type->mode == TM_UNION);
    if (incomplete_type_p (c2m_ctx, type)) {
      align = -1;
    } else {
      align = 1;
      for (node_t member = NL_HEAD (NL_EL (type->u.tag_type->ops, 1)->ops); member != NULL;
           member = NL_NEXT (member))
        if (member->code == N_MEMBER) {
          decl_t decl = member->attr;
          node_t width = NL_EL (member->ops, 2);
          struct expr *expr;

          if (type->mode == TM_UNION && width->code != N_IGNORE && (expr = width->attr)->const_p
              && expr->u.u_val == 0)
            continue;
          member_align = type_align (decl->decl_spec.type);
          if (align < member_align) align = member_align;
        }
    }
  }
  type->align = align;
}

static mir_size_t type_size (c2m_ctx_t c2m_ctx, struct type *type) {
  mir_size_t size = raw_type_size (c2m_ctx, type);

  return round_size (size, type->align);
}

static mir_size_t var_align (c2m_ctx_t c2m_ctx, struct type *type) {
  int align;

  raw_type_size (c2m_ctx, type); /* check */
  align = type->align;
  assert (align >= 0);
#ifdef ADJUST_VAR_ALIGNMENT
  align = ADJUST_VAR_ALIGNMENT (c2m_ctx, align, type);
#endif
  return align;
}

static mir_size_t var_size (c2m_ctx_t c2m_ctx, struct type *type) {
  mir_size_t size = raw_type_size (c2m_ctx, type);

  return round_size (size, var_align (c2m_ctx, type));
}

/* BOUND_BIT is used only if BF_P.  */
static void update_field_layout (int *bf_p, mir_size_t *overall_size, mir_size_t *offset,
                                 int *bound_bit, mir_size_t prev_size, mir_size_t size, int align,
                                 int bits) {
  mir_size_t prev_field_offset = *offset, bytes = 0;
  int start_bit, diff;

  assert (size > 0);
  if (!*bf_p) { /* transition from field to bit field or field */
    if (bits >= 0 && size > prev_size) {
      *bound_bit = prev_size * MIR_CHAR_BIT;
    } else {
      prev_field_offset += prev_size;
      *offset = prev_field_offset / align * align;
      *bound_bit = (prev_field_offset - *offset) * MIR_CHAR_BIT;
      prev_field_offset = *offset;
    }
  }
  *bf_p = bits >= 0;
  if (bits < 0) {
    bytes = size - 1;
    bits = MIR_CHAR_BIT;
  }
  *offset = prev_field_offset / align * align;
  diff = prev_field_offset - *offset;
  for (;;) {
    start_bit = *bound_bit + diff * MIR_CHAR_BIT;
    if (start_bit < 0) start_bit = 0;
    if ((start_bit + bits - 1) / MIR_CHAR_BIT + 1 + bytes <= size) {
      *bound_bit = start_bit + bits;
      break;
    }
    *offset += align;
    diff -= align;
    if (bytes >= align) bytes -= align;
  }
  if (*overall_size < *offset + size) *overall_size = *offset + size;
}

/* Update offsets inside unnamed anonymous struct/union member. */
static void update_members_offset (struct type *type, mir_size_t offset) {
  assert ((type->mode == TM_STRUCT || type->mode == TM_UNION)
          && type->unnamed_anon_struct_union_member_type_p);
  assert (offset != MIR_SIZE_MAX || type->raw_size == MIR_SIZE_MAX);
  for (node_t el = NL_HEAD (NL_EL (type->u.tag_type->ops, 1)->ops); el != NULL; el = NL_NEXT (el))
    if (el->code == N_MEMBER) {
      decl_t decl = el->attr;

      decl->offset = offset == MIR_SIZE_MAX ? 0 : decl->offset + offset;
      if (decl->decl_spec.type->unnamed_anon_struct_union_member_type_p)
        update_members_offset (decl->decl_spec.type,
                               offset == MIR_SIZE_MAX ? offset : decl->offset);
    }
}

static void set_type_layout (c2m_ctx_t c2m_ctx, struct type *type) {
  mir_size_t overall_size = 0;

  if (type->raw_size != MIR_SIZE_MAX) return; /* defined */
  if (type->mode == TM_BASIC) {
    overall_size = basic_type_size (type->u.basic_type);
  } else if (type->mode == TM_PTR) {
    overall_size = sizeof (mir_size_t);
  } else if (type->mode == TM_ENUM) {
    overall_size = basic_type_size (ENUM_BASIC_INT_TYPE);
  } else if (type->mode == TM_FUNC) {
    overall_size = sizeof (mir_size_t);
  } else if (type->mode == TM_ARR) {
    struct arr_type *arr_type = type->u.arr_type;
    struct expr *cexpr = arr_type->size->attr;
    mir_size_t nel = (arr_type->size->code == N_IGNORE || !cexpr->const_p ? 1 : cexpr->u.i_val);

    set_type_layout (c2m_ctx, arr_type->el_type);
    overall_size = type_size (c2m_ctx, arr_type->el_type) * nel;
  } else if (type->mode == TM_UNDEF) {
    overall_size = sizeof (int); /* error type */
  } else {
    int bf_p = FALSE, bits = -1, bound_bit = 0;
    mir_size_t offset = 0, prev_size = 0;

    assert (type->mode == TM_STRUCT || type->mode == TM_UNION);
    if (incomplete_type_p (c2m_ctx, type)) {
      overall_size = MIR_SIZE_MAX;
    } else {
      for (node_t el = NL_HEAD (NL_EL (type->u.tag_type->ops, 1)->ops); el != NULL;
           el = NL_NEXT (el))
        if (el->code == N_MEMBER) {
          decl_t decl = el->attr;
          int member_align;
          mir_size_t member_size;
          node_t width = NL_EL (el->ops, 2);
          struct expr *expr;
          int anon_process_p = (!type->unnamed_anon_struct_union_member_type_p
                                && decl->decl_spec.type->unnamed_anon_struct_union_member_type_p
                                && decl->decl_spec.type->raw_size == MIR_SIZE_MAX);

          if (anon_process_p) update_members_offset (decl->decl_spec.type, MIR_SIZE_MAX);
          set_type_layout (c2m_ctx, decl->decl_spec.type);
          member_size = type_size (c2m_ctx, decl->decl_spec.type);
          member_align = type_align (decl->decl_spec.type);
          bits = width->code == N_IGNORE || !(expr = width->attr)->const_p ? -1 : expr->u.u_val;
          if (bits != 0) {
            update_field_layout (&bf_p, &overall_size, &offset, &bound_bit, prev_size, member_size,
                                 member_align, bits);
            prev_size = member_size;
            decl->offset = offset;
            decl->bit_offset = bits < 0 ? -1 : bound_bit - bits;
          } else { /* Finish the last unit */
            bf_p = FALSE;
            offset = (offset + member_align - 1) / member_align * member_align;
            /* The offset and bit_offset do not matter, but make
               bit_offset less member_size in bits */
            decl->offset = offset + bound_bit / (member_size * MIR_CHAR_BIT);
            decl->bit_offset = bound_bit % (member_size * MIR_CHAR_BIT);
          }
          decl->width = bits;
          if (type->mode == TM_UNION) {
            offset = prev_size = 0;
            bf_p = FALSE;
            bits = -1;
            bound_bit = 0;
          }
          if (anon_process_p) update_members_offset (decl->decl_spec.type, decl->offset);
        }
    }
  }
  /* we might need raw_size for alignment calculations */
  type->raw_size = overall_size;
  aux_set_type_align (c2m_ctx, type);
  if (type->mode == TM_PTR) /* Visit the pointed but after setting size to avoid looping */
    set_type_layout (c2m_ctx, type->u.ptr_type);
}

static int int_bit_size (struct type *type) {
  assert (type->mode == TM_BASIC || type->mode == TM_ENUM);
  return (basic_type_size (type->mode == TM_ENUM ? ENUM_BASIC_INT_TYPE : type->u.basic_type)
          * MIR_CHAR_BIT);
}

static int void_type_p (struct type *type) {
  return type->mode == TM_BASIC && type->u.basic_type == TP_VOID;
}

static int void_ptr_p (struct type *type) {
  return type->mode == TM_PTR && void_type_p (type->u.ptr_type);
}

static int incomplete_type_p (c2m_ctx_t c2m_ctx, struct type *type) {
  switch (type->mode) {
  case TM_BASIC: return type->u.basic_type == TP_VOID;
  case TM_ENUM:
  case TM_STRUCT:
  case TM_UNION: {
    node_t scope, n = type->u.tag_type;

    if (NL_EL (n->ops, 1)->code == N_IGNORE) return TRUE;
    for (scope = curr_scope; scope != NULL && scope != top_scope && scope != n;
         scope = ((struct node_scope *) scope->attr)->scope)
      ;
    return scope == n;
  }
  case TM_PTR: return FALSE;
  case TM_ARR: {
    struct arr_type *arr_type = type->u.arr_type;

    return (arr_type->size->code == N_IGNORE || incomplete_type_p (c2m_ctx, arr_type->el_type));
  }
  case TM_FUNC:
    return ((type = type->u.func_type->ret_type) == NULL
            || !void_type_p (type) && incomplete_type_p (c2m_ctx, type));
  default: return FALSE;
  }
}

static int null_const_p (struct expr *expr, struct type *type) {
  return ((integer_type_p (type) && expr->const_p && expr->u.u_val == 0)
          || (void_ptr_p (type) && expr->const_p && expr->u.u_val == 0
              && type_qual_eq_p (&type->type_qual, &zero_type_qual)));
}

static void cast_value (struct expr *to_e, struct expr *from_e, struct type *to) {
  assert (to_e->const_p && from_e->const_p);
  struct type *from = from_e->type;

#define CONV(TP, cast, mto, mfrom)        \
  case TP:                                \
    to_e->u.mto = (cast) from_e->u.mfrom; \
    break;
#define BASIC_FROM_CONV(mfrom)                                                           \
  switch (to->u.basic_type) {                                                            \
    CONV (TP_BOOL, mir_bool, u_val, mfrom) CONV (TP_UCHAR, mir_uchar, u_val, mfrom);     \
    CONV (TP_USHORT, mir_ushort, u_val, mfrom) CONV (TP_UINT, mir_uint, u_val, mfrom);   \
    CONV (TP_ULONG, mir_ulong, u_val, mfrom) CONV (TP_ULLONG, mir_ullong, u_val, mfrom); \
    CONV (TP_SCHAR, mir_char, i_val, mfrom);                                             \
    CONV (TP_SHORT, mir_short, i_val, mfrom) CONV (TP_INT, mir_int, i_val, mfrom);       \
    CONV (TP_LONG, mir_long, i_val, mfrom) CONV (TP_LLONG, mir_llong, i_val, mfrom);     \
    CONV (TP_FLOAT, mir_float, d_val, mfrom) CONV (TP_DOUBLE, mir_double, d_val, mfrom); \
    CONV (TP_LDOUBLE, mir_ldouble, d_val, mfrom);                                        \
  case TP_CHAR:                                                                          \
    if (char_is_signed_p ())                                                             \
      to_e->u.i_val = (mir_char) from_e->u.mfrom;                                        \
    else                                                                                 \
      to_e->u.u_val = (mir_char) from_e->u.mfrom;                                        \
    break;                                                                               \
  default: assert (FALSE);                                                               \
  }

#define BASIC_TO_CONV(cast, mto)                                \
  switch (from->u.basic_type) {                                 \
  case TP_BOOL:                                                 \
  case TP_UCHAR:                                                \
  case TP_USHORT:                                               \
  case TP_UINT:                                                 \
  case TP_ULONG:                                                \
  case TP_ULLONG: to_e->u.mto = (cast) from_e->u.u_val; break;  \
  case TP_CHAR:                                                 \
    if (!char_is_signed_p ()) {                                 \
      to_e->u.mto = (cast) from_e->u.u_val;                     \
      break;                                                    \
    }                                                           \
    /* Fall through: */                                         \
  case TP_SCHAR:                                                \
  case TP_SHORT:                                                \
  case TP_INT:                                                  \
  case TP_LONG:                                                 \
  case TP_LLONG: to_e->u.mto = (cast) from_e->u.i_val; break;   \
  case TP_FLOAT:                                                \
  case TP_DOUBLE:                                               \
  case TP_LDOUBLE: to_e->u.mto = (cast) from_e->u.d_val; break; \
  default: assert (FALSE);                                      \
  }

  if (to->mode == from->mode && (from->mode == TM_PTR || from->mode == TM_ENUM)) {
    to_e->u = from_e->u;
  } else if (from->mode == TM_PTR) {
    if (to->mode == TM_ENUM) {
      to_e->u.i_val = (ENUM_MIR_INT) from_e->u.u_val;
    } else {
      BASIC_FROM_CONV (u_val);
    }
  } else if (from->mode == TM_ENUM) {
    if (to->mode == TM_PTR) {
      to_e->u.u_val = (mir_size_t) from_e->u.i_val;
    } else {
      BASIC_FROM_CONV (i_val);
    }
  } else if (to->mode == TM_PTR) {
    BASIC_TO_CONV (mir_size_t, u_val);
  } else if (to->mode == TM_ENUM) {
    BASIC_TO_CONV (ENUM_MIR_INT, i_val);
  } else {
    switch (from->u.basic_type) {
    case TP_BOOL:
    case TP_UCHAR:
    case TP_USHORT:
    case TP_UINT:
    case TP_ULONG:
    case TP_ULLONG: BASIC_FROM_CONV (u_val); break;
    case TP_CHAR:
      if (!char_is_signed_p ()) {
        BASIC_FROM_CONV (u_val);
        break;
      }
      /* Fall through: */
    case TP_SCHAR:
    case TP_SHORT:
    case TP_INT:
    case TP_LONG:
    case TP_LLONG: BASIC_FROM_CONV (i_val); break;
    case TP_FLOAT:
    case TP_DOUBLE:
    case TP_LDOUBLE: BASIC_FROM_CONV (d_val); break;
    default: assert (FALSE);
    }
  }
#undef CONV
#undef BASIC_FROM_CONV
#undef BASIC_TO_CONV
}

static void convert_value (struct expr *e, struct type *to) { cast_value (e, e, to); }

static int non_reg_decl_spec_p (struct decl_spec *ds) {
  return (ds->typedef_p || ds->extern_p || ds->static_p || ds->auto_p || ds->thread_local_p
          || ds->inline_p || ds->no_return_p || ds->align_node);
}

static void create_node_scope (c2m_ctx_t c2m_ctx, node_t node) {
  struct node_scope *ns = reg_malloc (c2m_ctx, sizeof (struct node_scope));

  assert (node != curr_scope);
  ns->func_scope_num = curr_func_scope_num++;
  ns->stack_var_p = FALSE;
  ns->offset = ns->size = ns->call_arg_area_size = 0;
  node->attr = ns;
  ns->scope = curr_scope;
  curr_scope = node;
}

static void finish_scope (c2m_ctx_t c2m_ctx) {
  curr_scope = ((struct node_scope *) curr_scope->attr)->scope;
}

static void set_type_qual (c2m_ctx_t c2m_ctx, node_t r, struct type_qual *tq,
                           enum type_mode tmode) {
  for (node_t n = NL_HEAD (r->ops); n != NULL; n = NL_NEXT (n)) switch (n->code) {
      /* Type qualifiers: */
    case N_CONST: tq->const_p = TRUE; break;
    case N_RESTRICT:
      tq->restrict_p = TRUE;
      if (tmode != TM_PTR && tmode != TM_UNDEF)
        error (c2m_ctx, n->pos, "restrict requires a pointer");
      break;
    case N_VOLATILE: tq->volatile_p = TRUE; break;
    case N_ATOMIC:
      tq->atomic_p = TRUE;
      if (tmode == TM_ARR)
        error (c2m_ctx, n->pos, "_Atomic qualifying array");
      else if (tmode == TM_FUNC)
        error (c2m_ctx, n->pos, "_Atomic qualifying function");
      break;
    default: break; /* Ignore */
    }
}

static void check_type_duplication (c2m_ctx_t c2m_ctx, struct type *type, node_t n,
                                    const char *name, int size, int sign) {
  if (type->mode != TM_BASIC || type->u.basic_type != TP_UNDEF)
    error (c2m_ctx, n->pos, "%s with another type", name);
  else if (type->mode != TM_BASIC && size != 0)
    error (c2m_ctx, n->pos, "size with non-numeric type");
  else if (type->mode != TM_BASIC && sign != 0)
    error (c2m_ctx, n->pos, "sign attribute with non-integer type");
}

static node_t find_def (c2m_ctx_t c2m_ctx, enum symbol_mode mode, node_t id, node_t scope,
                        node_t *aux_node) {
  symbol_t sym;

  for (;;) {
    if (!symbol_find (c2m_ctx, mode, id, scope, &sym)) {
      if (scope == NULL) return NULL;
      scope = ((struct node_scope *) scope->attr)->scope;
    } else {
      if (aux_node) *aux_node = sym.aux_node;
      return sym.def_node;
    }
  }
}

static node_t process_tag (c2m_ctx_t c2m_ctx, node_t r, node_t id, node_t decl_list) {
  symbol_t sym;
  int found_p;
  node_t scope, tab_decl_list;

  if (id->code != N_ID) return r;
  scope = curr_scope;
  while (scope != top_scope && (scope->code == N_STRUCT || scope->code == N_UNION))
    scope = ((struct node_scope *) scope->attr)->scope;
  if (decl_list->code != N_IGNORE) {
    found_p = symbol_find (c2m_ctx, S_TAG, id, scope, &sym);
  } else {
    sym.def_node = find_def (c2m_ctx, S_TAG, id, scope, NULL);
    found_p = sym.def_node != NULL;
  }
  if (!found_p) {
    symbol_insert (c2m_ctx, S_TAG, id, scope, r, NULL);
  } else if (sym.def_node->code != r->code) {
    error (c2m_ctx, id->pos, "kind of tag %s is unmatched with previous declaration", id->u.s.s);
  } else if ((tab_decl_list = NL_EL (sym.def_node->ops, 1))->code != N_IGNORE
             && decl_list->code != N_IGNORE) {
    error (c2m_ctx, id->pos, "tag %s redeclaration", id->u.s.s);
  } else {
    if (decl_list->code != N_IGNORE) { /* swap decl lists */
      DLIST (node_t) temp = r->ops;

      r->ops = sym.def_node->ops;
      sym.def_node->ops = temp;
    }
    r = sym.def_node;
  }
  return r;
}

static void def_symbol (c2m_ctx_t c2m_ctx, enum symbol_mode mode, node_t id, node_t scope,
                        node_t def_node, node_code_t linkage) {
  symbol_t sym;
  struct decl_spec tab_decl_spec, decl_spec;

  if (id->code == N_IGNORE) return;
  assert (id->code == N_ID && scope != NULL);
  assert (scope->code == N_MODULE || scope->code == N_BLOCK || scope->code == N_STRUCT
          || scope->code == N_UNION || scope->code == N_FUNC || scope->code == N_FOR);
  decl_spec = ((decl_t) def_node->attr)->decl_spec;
  if (decl_spec.thread_local_p && !decl_spec.static_p && !decl_spec.extern_p)
    error (c2m_ctx, id->pos, "auto %s is declared as thread local", id->u.s.s);
  if (!symbol_find (c2m_ctx, mode, id, scope, &sym)) {
    symbol_insert (c2m_ctx, mode, id, scope, def_node, NULL);
    return;
  }
  tab_decl_spec = ((decl_t) sym.def_node->attr)->decl_spec;
  if (linkage == N_IGNORE) {
    if (!decl_spec.typedef_p || !tab_decl_spec.typedef_p
        || !type_eq_p (decl_spec.type, tab_decl_spec.type))
      error (c2m_ctx, id->pos, "repeated declaration %s", id->u.s.s);
  } else if (!compatible_types_p (decl_spec.type, tab_decl_spec.type, FALSE)) {
    error (c2m_ctx, id->pos, "incompatible types of %s declarations", id->u.s.s);
  }
  if (tab_decl_spec.thread_local_p != decl_spec.thread_local_p) {
    error (c2m_ctx, id->pos, "thread local and non-thread local declarations of %s", id->u.s.s);
  }
  if ((decl_spec.linkage == N_EXTERN && linkage == N_STATIC)
      || (decl_spec.linkage == N_STATIC && linkage == N_EXTERN))
    warning (c2m_ctx, id->pos, "%s defined with external and internal linkage", id->u.s.s);
  VARR_PUSH (node_t, sym.defs, def_node);
}

static void make_type_complete (c2m_ctx_t c2m_ctx, struct type *type) {
  if (incomplete_type_p (c2m_ctx, type)) return;
  /* The type may become complete: recalculate size: */
  type->raw_size = MIR_SIZE_MAX;
  set_type_layout (c2m_ctx, type);
}

static void check (c2m_ctx_t c2m_ctx, node_t node, node_t context);

static struct decl_spec check_decl_spec (c2m_ctx_t c2m_ctx, node_t r, node_t decl) {
  int n_sc = 0, sign = 0, size = 0, func_p = FALSE;
  struct decl_spec *res;
  struct type *type;

  if (r->attr != NULL) return *(struct decl_spec *) r->attr;
  if (decl->code == N_FUNC_DEF) {
    func_p = TRUE;
  } else if (decl->code == N_SPEC_DECL) {
    node_t declarator = NL_EL (decl->ops, 1);
    node_t list = NL_EL (declarator->ops, 1);

    func_p = list != NULL && NL_HEAD (list->ops) != NULL && NL_HEAD (list->ops)->code == N_FUNC;
  }
  r->attr = res = reg_malloc (c2m_ctx, sizeof (struct decl_spec));
  res->typedef_p = res->extern_p = res->static_p = FALSE;
  res->auto_p = res->register_p = res->thread_local_p = FALSE;
  res->inline_p = res->no_return_p = FALSE;
  res->align = -1;
  res->align_node = NULL;
  res->linkage = N_IGNORE;
  res->type = type = create_type (c2m_ctx, NULL);
  type->pos_node = r;
  type->mode = TM_BASIC;
  type->u.basic_type = TP_UNDEF;
  for (node_t n = NL_HEAD (r->ops); n != NULL; n = NL_NEXT (n))
    if (n->code == N_SIGNED || n->code == N_UNSIGNED) {
      if (sign != 0)
        error (c2m_ctx, n->pos, "more than one sign qualifier");
      else
        sign = n->code == N_SIGNED ? 1 : -1;
    } else if (n->code == N_SHORT) {
      if (size != 0)
        error (c2m_ctx, n->pos, "more than one type");
      else
        size = 1;
    } else if (n->code == N_LONG) {
      if (size == 2)
        size = 3;
      else if (size == 3)
        error (c2m_ctx, n->pos, "more than two long");
      else if (size == 1)
        error (c2m_ctx, n->pos, "short with long");
      else
        size = 2;
    }
  for (node_t n = NL_HEAD (r->ops); n != NULL; n = NL_NEXT (n)) switch (n->code) {
      /* Type qualifiers are already processed. */
    case N_CONST:
    case N_RESTRICT:
    case N_VOLATILE:
    case N_ATOMIC:
      break;
      /* Func specifiers: */
    case N_INLINE:
      if (!func_p)
        error (c2m_ctx, n->pos, "non-function declaration with inline");
      else
        res->inline_p = TRUE;
      break;
    case N_NO_RETURN:
      if (!func_p)
        error (c2m_ctx, n->pos, "non-function declaration with _Noreturn");
      else
        res->no_return_p = TRUE;
      break;
      /* Storage specifiers: */
    case N_TYPEDEF:
    case N_AUTO:
    case N_REGISTER:
      if (n_sc != 0)
        error (c2m_ctx, n->pos, "more than one storage specifier");
      else if (n->code == N_TYPEDEF)
        res->typedef_p = TRUE;
      else if (n->code == N_AUTO)
        res->auto_p = TRUE;
      else
        res->register_p = TRUE;
      n_sc++;
      break;
    case N_EXTERN:
    case N_STATIC:
      if (n_sc != 0 && (n_sc != 1 || !res->thread_local_p))
        error (c2m_ctx, n->pos, "more than one storage specifier");
      else if (n->code == N_EXTERN)
        res->extern_p = TRUE;
      else
        res->static_p = TRUE;
      n_sc++;
      break;
    case N_THREAD_LOCAL:
      if (n_sc != 0 && (n_sc != 1 || (!res->extern_p && !res->static_p)))
        error (c2m_ctx, n->pos, "more than one storage specifier");
      else
        res->thread_local_p = TRUE;
      n_sc++;
      break;
    case N_VOID:
      set_type_pos_node (type, n);
      if (type->mode != TM_BASIC || type->u.basic_type != TP_UNDEF)
        error (c2m_ctx, n->pos, "void with another type");
      else if (sign != 0)
        error (c2m_ctx, n->pos, "void with sign qualifier");
      else if (size != 0)
        error (c2m_ctx, n->pos, "void with short or long");
      else
        type->u.basic_type = TP_VOID;
      break;
    case N_UNSIGNED:
    case N_SIGNED:
    case N_SHORT:
    case N_LONG: set_type_pos_node (type, n); break;
    case N_CHAR:
    case N_INT:
      set_type_pos_node (type, n);
      if (type->mode != TM_BASIC || type->u.basic_type != TP_UNDEF) {
        error (c2m_ctx, n->pos, "char or int with another type");
      } else if (n->code == N_CHAR) {
        if (size != 0)
          error (c2m_ctx, n->pos, "char with short or long");
        else
          type->u.basic_type = sign == 0 ? TP_CHAR : sign < 0 ? TP_UCHAR : TP_SCHAR;
      } else if (size == 0)
        type->u.basic_type = sign >= 0 ? TP_INT : TP_UINT;
      else if (size == 1)
        type->u.basic_type = sign >= 0 ? TP_SHORT : TP_USHORT;
      else if (size == 2)
        type->u.basic_type = sign >= 0 ? TP_LONG : TP_ULONG;
      else
        type->u.basic_type = sign >= 0 ? TP_LLONG : TP_ULLONG;
      break;
    case N_BOOL:
      set_type_pos_node (type, n);
      if (type->mode != TM_BASIC || type->u.basic_type != TP_UNDEF)
        error (c2m_ctx, n->pos, "_Bool with another type");
      else if (sign != 0)
        error (c2m_ctx, n->pos, "_Bool with sign qualifier");
      else if (size != 0)
        error (c2m_ctx, n->pos, "_Bool with short or long");
      type->u.basic_type = TP_BOOL;
      break;
    case N_FLOAT:
      set_type_pos_node (type, n);
      if (type->mode != TM_BASIC || type->u.basic_type != TP_UNDEF)
        error (c2m_ctx, n->pos, "float with another type");
      else if (sign != 0)
        error (c2m_ctx, n->pos, "float with sign qualifier");
      else if (size != 0)
        error (c2m_ctx, n->pos, "float with short or long");
      else
        type->u.basic_type = TP_FLOAT;
      break;
    case N_DOUBLE:
      set_type_pos_node (type, n);
      if (type->mode != TM_BASIC || type->u.basic_type != TP_UNDEF)
        error (c2m_ctx, n->pos, "double with another type");
      else if (sign != 0)
        error (c2m_ctx, n->pos, "double with sign qualifier");
      else if (size == 0)
        type->u.basic_type = TP_DOUBLE;
      else if (size == 2)
        type->u.basic_type = TP_LDOUBLE;
      else
        error (c2m_ctx, n->pos, "double with short");
      break;
    case N_ID: {
      node_t def = find_def (c2m_ctx, S_REGULAR, n, curr_scope, NULL);
      decl_t decl;

      set_type_pos_node (type, n);
      if (def == NULL) {
        error (c2m_ctx, n->pos, "unknown type %s", n->u.s.s);
        init_type (type);
        type->mode = TM_BASIC;
        type->u.basic_type = TP_INT;
      } else {
        assert (def->code == N_SPEC_DECL);
        decl = def->attr;
        decl->used_p = TRUE;
        assert (decl->decl_spec.typedef_p);
        *type = *decl->decl_spec.type;
      }
      break;
    }
    case N_STRUCT:
    case N_UNION: {
      int new_scope_p;
      node_t res, id = NL_HEAD (n->ops);
      node_t decl_list = NL_NEXT (id);
      node_t saved_unnamed_anon_struct_union_member = curr_unnamed_anon_struct_union_member;

      set_type_pos_node (type, n);
      res = process_tag (c2m_ctx, n, id, decl_list);
      check_type_duplication (c2m_ctx, type, n, n->code == N_STRUCT ? "struct" : "union", size,
                              sign);
      type->mode = n->code == N_STRUCT ? TM_STRUCT : TM_UNION;
      type->u.tag_type = res;
      new_scope_p = (id->code != N_IGNORE || decl->code != N_MEMBER
                     || NL_EL (decl->ops, 1)->code != N_IGNORE);
      type->unnamed_anon_struct_union_member_type_p = !new_scope_p;
      curr_unnamed_anon_struct_union_member = new_scope_p ? NULL : decl;
      if (decl_list->code != N_IGNORE) {
        if (new_scope_p) create_node_scope (c2m_ctx, res);
        check (c2m_ctx, decl_list, n);
        if (new_scope_p) finish_scope (c2m_ctx);
        if (res != n) make_type_complete (c2m_ctx, type); /* recalculate size */
      }
      curr_unnamed_anon_struct_union_member = saved_unnamed_anon_struct_union_member;
      break;
    }
    case N_ENUM: {
      node_t res, id = NL_HEAD (n->ops);
      node_t enum_list = NL_NEXT (id);

      set_type_pos_node (type, n);
      res = process_tag (c2m_ctx, n, id, enum_list);
      check_type_duplication (c2m_ctx, type, n, "enum", size, sign);
      type->mode = TM_ENUM;
      type->u.tag_type = res;
      if (enum_list->code == N_IGNORE) {
        if (incomplete_type_p (c2m_ctx, type))
          error (c2m_ctx, n->pos, "enum storage size is unknown");
      } else {
        mir_int curr_val = 0;

        for (node_t en = NL_HEAD (enum_list->ops); en != NULL; en = NL_NEXT (en)) {  // ??? id
          node_t id, const_expr;
          symbol_t sym;
          struct enum_value *enum_value;

          assert (en->code == N_ENUM_CONST);
          id = NL_HEAD (en->ops);
          const_expr = NL_NEXT (id);
          check (c2m_ctx, const_expr, n);
          if (symbol_find (c2m_ctx, S_REGULAR, id, curr_scope, &sym)) {
            error (c2m_ctx, id->pos, "enum constant %s redeclaration", id->u.s.s);
          } else {
            symbol_insert (c2m_ctx, S_REGULAR, id, curr_scope, en, n);
          }
          if (const_expr->code != N_IGNORE) {
            struct expr *cexpr = const_expr->attr;

            if (!cexpr->const_p)
              error (c2m_ctx, const_expr->pos, "non-constant value in enum const expression");
            else if (!integer_type_p (cexpr->type))
              error (c2m_ctx, const_expr->pos, "enum const expression is not of an integer type");
            else if ((signed_integer_type_p (cexpr->type) && cexpr->u.i_val > MIR_INT_MAX)
                     || (!signed_integer_type_p (cexpr->type) && cexpr->u.u_val > MIR_INT_MAX))
              error (c2m_ctx, const_expr->pos, "enum const expression is not represented by int");
            else
              curr_val = cexpr->u.i_val;
          }
          en->attr = enum_value = reg_malloc (c2m_ctx, sizeof (struct enum_value));
          enum_value->val = curr_val;
          curr_val++;
        }
      }
      break;
    }
    case N_ALIGNAS: {
      node_t el;
      int align = -1;

      if (decl->code == N_FUNC_DEF) {
        error (c2m_ctx, n->pos, "_Alignas for function");
      } else if (decl->code == N_MEMBER && (el = NL_EL (decl->ops, 3)) != NULL
                 && el->code != N_IGNORE) {
        error (c2m_ctx, n->pos, "_Alignas for a bit-field");
      } else if (decl->code == N_SPEC_DECL && in_params_p) {
        error (c2m_ctx, n->pos, "_Alignas for a function parameter");
      } else {
        node_t op = NL_HEAD (n->ops);

        check (c2m_ctx, op, n);
        if (op->code == N_TYPE) {
          struct decl_spec *decl_spec = op->attr;

          align = type_align (decl_spec->type);
        } else {
          struct expr *cexpr = op->attr;

          if (!cexpr->const_p) {
            error (c2m_ctx, op->pos, "non-constant value in _Alignas");
          } else if (!integer_type_p (cexpr->type)) {
            error (c2m_ctx, op->pos, "constant value in _Alignas is not of an integer type");
          } else if (!signed_integer_type_p (cexpr->type)
                     || !supported_alignment_p (cexpr->u.i_val)) {
            error (c2m_ctx, op->pos, "constant value in _Alignas specifies unspported alignment");
          } else if (invalid_alignment (cexpr->u.i_val)) {
            error (c2m_ctx, op->pos, "unsupported alignmnent");
          } else {
            align = cexpr->u.i_val;
          }
        }
        if (align != 0 && res->align < align) {
          res->align = align;
          res->align_node = n;
        }
      }
      break;
    }
    default: abort ();
    }
  if (type->mode == TM_BASIC && type->u.basic_type == TP_UNDEF) {
    if (size == 0 && sign == 0) {
      (options->pedantic_p ? error (c2m_ctx, r->pos, "no any type specifier")
                           : warning (c2m_ctx, r->pos, "type defaults to int"));
      type->u.basic_type = TP_INT;
    } else if (size == 0) {
      type->u.basic_type = sign >= 0 ? TP_INT : TP_UINT;
    } else if (size == 1) {
      type->u.basic_type = sign >= 0 ? TP_SHORT : TP_USHORT;
    } else if (size == 2) {
      type->u.basic_type = sign >= 0 ? TP_LONG : TP_ULONG;
    } else {
      type->u.basic_type = sign >= 0 ? TP_LLONG : TP_ULLONG;
    }
  }
  set_type_qual (c2m_ctx, r, &type->type_qual, type->mode);
  if (res->align_node) {
    if (res->typedef_p)
      error (c2m_ctx, res->align_node->pos, "_Alignas in typedef");
    else if (res->register_p)
      error (c2m_ctx, res->align_node->pos, "_Alignas with register");
  }
  return *res;
}

static struct type *append_type (struct type *head, struct type *el) {
  struct type **holder;

  if (head == NULL) return el;
  if (head->mode == TM_PTR) {
    holder = &head->u.ptr_type;
  } else if (head->mode == TM_ARR) {
    holder = &head->u.arr_type->el_type;
  } else {
    assert (head->mode == TM_FUNC);
    holder = &head->u.func_type->ret_type;
  }
  *holder = append_type (*holder, el);
  return head;
}

static int void_param_p (node_t param) {
  struct decl_spec *decl_spec;
  struct type *type;

  if (param != NULL && param->code == N_TYPE) {
    decl_spec = param->attr;
    type = decl_spec->type;
    if (void_type_p (type)) return TRUE;
  }
  return FALSE;
}

static void adjust_param_type (c2m_ctx_t c2m_ctx, struct type **type_ptr) {
  struct type *par_type, *type = *type_ptr;
  struct arr_type *arr_type;

  if (type->mode == TM_ARR) {  // ??? static, old type qual
    arr_type = type->u.arr_type;
    type->mode = TM_PTR;
    type->u.ptr_type = arr_type->el_type;
    type->type_qual = arr_type->ind_type_qual;
    make_type_complete (c2m_ctx, type);
  } else if (type->mode == TM_FUNC) {
    par_type = create_type (c2m_ctx, NULL);
    par_type->mode = TM_PTR;
    par_type->pos_node = type->pos_node;
    par_type->u.ptr_type = type;
    *type_ptr = type = par_type;
    make_type_complete (c2m_ctx, type);
  }
}

static struct type *check_declarator (c2m_ctx_t c2m_ctx, node_t r, int func_def_p) {
  struct type *type, *res = NULL;
  node_t list = NL_EL (r->ops, 1);

  assert (r->code == N_DECL);
  if (NL_HEAD (list->ops) == NULL) return NULL;
  for (node_t n = NL_HEAD (list->ops); n != NULL; n = NL_NEXT (n)) {
    type = create_type (c2m_ctx, NULL);
    type->pos_node = n;
    switch (n->code) {
    case N_POINTER: {
      node_t type_qual = NL_HEAD (n->ops);

      type->mode = TM_PTR;
      type->pos_node = n;
      type->u.ptr_type = NULL;
      set_type_qual (c2m_ctx, type_qual, &type->type_qual, TM_PTR);
      break;
    }
    case N_ARR: {
      struct arr_type *arr_type;
      node_t static_node = NL_HEAD (n->ops);
      node_t type_qual = NL_NEXT (static_node);
      node_t size = NL_NEXT (type_qual);

      type->mode = TM_ARR;
      type->pos_node = n;
      type->u.arr_type = arr_type = reg_malloc (c2m_ctx, sizeof (struct arr_type));
      clear_type_qual (&arr_type->ind_type_qual);
      set_type_qual (c2m_ctx, type_qual, &arr_type->ind_type_qual, TM_UNDEF);
      check (c2m_ctx, size, n);
      arr_type->size = size;
      arr_type->static_p = static_node->code == N_STATIC;
      arr_type->el_type = NULL;
      break;
    }
    case N_FUNC: {
      struct func_type *func_type;
      node_t first_param, param_list = NL_HEAD (n->ops);
      node_t last = NL_TAIL (param_list->ops);
      int saved_in_params_p = in_params_p;

      type->mode = TM_FUNC;
      type->pos_node = n;
      type->u.func_type = func_type = reg_malloc (c2m_ctx, sizeof (struct func_type));
      func_type->ret_type = NULL;
      func_type->proto_item = NULL;
      if ((func_type->dots_p = last != NULL && last->code == N_DOTS))
        NL_REMOVE (param_list->ops, last);
      if (!func_def_p) create_node_scope (c2m_ctx, n);
      func_type->param_list = param_list;
      in_params_p = TRUE;
      first_param = NL_HEAD (param_list->ops);
      if (first_param != NULL && first_param->code != N_ID) check (c2m_ctx, first_param, n);
      if (void_param_p (first_param)) {
        struct decl_spec *ds = first_param->attr;

        if (non_reg_decl_spec_p (ds) || ds->register_p
            || !type_qual_eq_p (&ds->type->type_qual, &zero_type_qual)) {
          error (c2m_ctx, first_param->pos, "qualified void parameter");
        }
        if (NL_NEXT (first_param) != NULL) {
          error (c2m_ctx, first_param->pos, "void must be the only parameter");
        }
      } else {
        for (node_t p = first_param; p != NULL; p = NL_NEXT (p)) {
          struct decl_spec *decl_spec_ptr;

          if (p->code == N_ID) {
            if (!func_def_p)
              error (c2m_ctx, p->pos,
                     "parameters identifier list can be only in function definition");
            break;
          } else {
            if (p != first_param) check (c2m_ctx, p, n);
            decl_spec_ptr = get_param_decl_spec (p);
            adjust_param_type (c2m_ctx, &decl_spec_ptr->type);
          }
        }
      }
      in_params_p = saved_in_params_p;
      if (!func_def_p) finish_scope (c2m_ctx);
      break;
    }
    default: abort ();
    }
    res = append_type (res, type);
  }
  return res;
}

static int check_case_expr (c2m_ctx_t c2m_ctx, node_t case_expr, struct type *type, node_t target) {
  struct expr *expr;

  check (c2m_ctx, case_expr, target);
  expr = case_expr->attr;
  if (!expr->const_p) {
    error (c2m_ctx, case_expr->pos, "case-expr is not a constant expression");
    return FALSE;
  } else if (!integer_type_p (expr->type)) {
    error (c2m_ctx, case_expr->pos, "case-expr is not an integer type expression");
    return FALSE;
  } else {
    convert_value (expr, type);
    return TRUE;
  }
}

static void check_labels (c2m_ctx_t c2m_ctx, node_t labels, node_t target) {
  for (node_t l = NL_HEAD (labels->ops); l != NULL; l = NL_NEXT (l)) {
    if (l->code == N_LABEL) {
      symbol_t sym;
      node_t id = NL_HEAD (l->ops);

      if (symbol_find (c2m_ctx, S_LABEL, id, func_block_scope, &sym)) {
        error (c2m_ctx, id->pos, "label %s redeclaration", id->u.s.s);
      } else {
        symbol_insert (c2m_ctx, S_LABEL, id, func_block_scope, target, NULL);
      }
    } else if (curr_switch == NULL) {
      error (c2m_ctx, l->pos, "%s not within a switch-stmt",
             l->code == N_CASE ? "case label" : "default label");
    } else {
      struct switch_attr *switch_attr = curr_switch->attr;
      struct type *type = &switch_attr->type;
      node_t case_expr = l->code == N_CASE ? NL_HEAD (l->ops) : NULL;
      node_t case_expr2 = l->code == N_CASE ? NL_EL (l->ops, 1) : NULL;
      case_t case_attr, tail = DLIST_TAIL (case_t, switch_attr->case_labels);
      int ok_p = FALSE, default_p = tail != NULL && tail->case_node->code == N_DEFAULT;
      struct expr *expr;

      if (case_expr == NULL) {
        if (default_p) {
          error (c2m_ctx, l->pos, "multiple default labels in one switch");
        } else {
          ok_p = TRUE;
        }
      } else {
        ok_p = check_case_expr (c2m_ctx, case_expr, type, target);
        if (case_expr2 != NULL) {
          ok_p = check_case_expr (c2m_ctx, case_expr2, type, target) && ok_p;
          (options->pedantic_p ? error : warning) (c2m_ctx, l->pos,
                                                   "range cases are not a part of C standard");
        }
      }
      if (ok_p) {
        case_attr = reg_malloc (c2m_ctx, sizeof (struct case_attr));
        case_attr->case_node = l;
        case_attr->case_target_node = target;
        if (default_p) {
          DLIST_INSERT_BEFORE (case_t, switch_attr->case_labels, tail, case_attr);
        } else {
          DLIST_APPEND (case_t, switch_attr->case_labels, case_attr);
        }
      }
    }
  }
}

static node_code_t get_id_linkage (c2m_ctx_t c2m_ctx, int func_p, node_t id, node_t scope,
                                   struct decl_spec decl_spec) {
  node_code_t linkage;
  node_t def = find_def (c2m_ctx, S_REGULAR, id, scope, NULL);

  if (decl_spec.typedef_p) return N_IGNORE;                       // p6: no linkage
  if (decl_spec.static_p && scope == top_scope) return N_STATIC;  // p3: internal linkage
  if (decl_spec.extern_p && def != NULL
      && (linkage = ((decl_t) def->attr)->decl_spec.linkage) != N_IGNORE)
    return linkage;  // p4: previous linkage
  if (decl_spec.extern_p && (def == NULL || ((decl_t) def->attr)->decl_spec.linkage == N_IGNORE))
    return N_EXTERN;  // p4: external linkage
  if (!decl_spec.static_p && !decl_spec.extern_p && (scope == top_scope || func_p))
    return N_EXTERN;                                                          // p5
  if (!decl_spec.extern_p && scope != top_scope && !func_p) return N_IGNORE;  // p6: no linkage
  return N_IGNORE;
}

static void check_type (c2m_ctx_t c2m_ctx, struct type *type, int level, int func_def_p) {
  switch (type->mode) {
  case TM_PTR: check_type (c2m_ctx, type->u.ptr_type, level + 1, FALSE); break;
  case TM_STRUCT:
  case TM_UNION: break;
  case TM_ARR: {
    struct arr_type *arr_type = type->u.arr_type;
    node_t size_node = arr_type->size;
    struct type *el_type = arr_type->el_type;

    if (size_node->code == N_STAR) {
      error (c2m_ctx, size_node->pos, "variable size arrays are not supported");
    } else if (size_node->code != N_IGNORE) {
      struct expr *cexpr = size_node->attr;

      if (!integer_type_p (cexpr->type)) {
        error (c2m_ctx, size_node->pos, "non-integer array size type");
      } else if (!cexpr->const_p) {
        error (c2m_ctx, size_node->pos, "variable size arrays are not supported");
      } else if ((signed_integer_type_p (cexpr->type) && cexpr->u.i_val <= 0)
                 || (!signed_integer_type_p (cexpr->type) && cexpr->u.u_val == 0)) {
        error (c2m_ctx, size_node->pos, "array size should be positive");
      }
    }
    check_type (c2m_ctx, el_type, level + 1, FALSE);
    if (el_type->mode == TM_FUNC) {
      error (c2m_ctx, type->pos_node->pos, "array of functions");
    } else if (incomplete_type_p (c2m_ctx, el_type)) {
      error (c2m_ctx, type->pos_node->pos, "incomplete array element type");
    } else if (!in_params_p || level != 0) {
      if (arr_type->static_p)
        error (c2m_ctx, type->pos_node->pos, "static should be only in parameter outermost");
      else if (!type_qual_eq_p (&arr_type->ind_type_qual, &zero_type_qual))
        error (c2m_ctx, type->pos_node->pos,
               "type qualifiers should be only in parameter outermost array");
    }
    break;
  }
  case TM_FUNC: {
    struct decl_spec decl_spec;
    struct func_type *func_type = type->u.func_type;
    struct type *ret_type = func_type->ret_type;
    node_t first_param, param_list = func_type->param_list;

    check_type (c2m_ctx, ret_type, level + 1, FALSE);
    if (ret_type->mode == TM_FUNC) {
      error (c2m_ctx, ret_type->pos_node->pos, "function returning a function");
    } else if (ret_type->mode == TM_ARR) {
      error (c2m_ctx, ret_type->pos_node->pos, "function returning an array");
    }
    first_param = NL_HEAD (param_list->ops);
    if (!void_param_p (first_param)) {
      for (node_t p = first_param; p != NULL; p = NL_NEXT (p)) {
        if (p->code == N_TYPE) {
          decl_spec = *((struct decl_spec *) p->attr);
          check_type (c2m_ctx, decl_spec.type, level + 1, FALSE);
        } else if (p->code == N_SPEC_DECL) {
          decl_spec = ((decl_t) p->attr)->decl_spec;
          check_type (c2m_ctx, decl_spec.type, level + 1, FALSE);
        } else {
          assert (p->code == N_ID);
          break;
        }
        if (non_reg_decl_spec_p (&decl_spec)) {
          error (c2m_ctx, p->pos, "prohibited specifier in a function parameter");
        } else if (func_def_p) {
          if (p->code == N_TYPE)
            error (c2m_ctx, p->pos, "parameter type without a name in function definition");
          else if (incomplete_type_p (c2m_ctx, decl_spec.type))
            error (c2m_ctx, p->pos, "incomplete parameter type in function definition");
        }
      }
    }
    break;
  }
  default: break;  // ???
  }
}

static void check_assignment_types (c2m_ctx_t c2m_ctx, struct type *left, struct type *right,
                                    struct expr *expr, node_t assign_node) {
  node_code_t code = assign_node->code;
  pos_t pos = assign_node->pos;
  const char *msg;

  if (right == NULL) right = expr->type;
  if (arithmetic_type_p (left)) {
    if (!arithmetic_type_p (right)
        && !(left->mode == TM_BASIC && left->u.basic_type == TP_BOOL && right->mode == TM_PTR)) {
      if (integer_type_p (left) && right->mode == TM_PTR) {
        msg
          = (code == N_CALL ? "using pointer without cast for integer type parameter"
                            : code == N_RETURN ? "returning pointer without cast for integer result"
                                               : "assigning pointer without cast to integer");
        (options->pedantic_p ? error : warning) (c2m_ctx, pos, "%s", msg);
      } else {
        msg = (code == N_CALL
                 ? "incompatible argument type for arithemtic type parameter"
                 : code != N_RETURN
                     ? "incompatible types in assignment to an arithemtic type lvalue"
                     : "incompatible return-expr type in function returning an arithemtic value");
        error (c2m_ctx, pos, "%s", msg);
      }
    }
  } else if (left->mode == TM_STRUCT || left->mode == TM_UNION) {
    if ((right->mode != TM_STRUCT && right->mode != TM_UNION)
        || !compatible_types_p (left, right, TRUE)) {
      msg = (code == N_CALL
               ? "incompatible argument type for struct/union type parameter"
               : code != N_RETURN
                   ? "incompatible types in assignment to struct/union"
                   : "incompatible return-expr type in function returning a struct/union");
      error (c2m_ctx, pos, "%s", msg);
    }
  } else if (left->mode == TM_PTR) {
    if (null_const_p (expr, right)) {
    } else if (right->mode != TM_PTR
               || (!compatible_types_p (left->u.ptr_type, right->u.ptr_type, TRUE)
                   && !void_ptr_p (left) && !void_ptr_p (right))) {
      if (right->mode == TM_PTR && left->u.ptr_type->mode == TM_BASIC
          && right->u.ptr_type->mode == TM_BASIC) {
        msg = (code == N_CALL ? "incompatible pointer types of argument and parameter"
                              : code == N_RETURN
                                  ? "incompatible pointer types of return-expr and function result"
                                  : "incompatible pointer types in assignment");
        (options->pedantic_p ? error : warning) (c2m_ctx, pos, "%s", msg);
      } else if (integer_type_p (right)) {
        msg
          = (code == N_CALL ? "using integer without cast for pointer type parameter"
                            : code == N_RETURN ? "returning integer without cast for pointer result"
                                               : "assigning integer without cast to pointer");
        (options->pedantic_p ? error : warning) (c2m_ctx, pos, "%s", msg);
      } else {
        msg = (code == N_CALL ? "incompatible argument type for pointer type parameter"
                              : code == N_RETURN
                                  ? "incompatible return-expr type in function returning a pointer"
                                  : "incompatible types in assignment to a pointer");
        (options->pedantic_p || right->mode != TM_PTR ? error : warning) (c2m_ctx, pos, "%s", msg);
      }
    } else if (right->u.ptr_type->type_qual.atomic_p) {
      msg = (code == N_CALL ? "passing a pointer of an atomic type"
                            : code == N_RETURN ? "returning a pointer of an atomic type"
                                               : "assignment of pointer of an atomic type");
      error (c2m_ctx, pos, "%s", msg);
    } else if (!type_qual_subset_p (&right->u.ptr_type->type_qual, &left->u.ptr_type->type_qual)) {
      msg = (code == N_CALL
               ? "discarding type qualifiers in passing argument"
               : code == N_RETURN ? "return discards a type qualifier from a pointer"
                                  : "assignment discards a type qualifier from a pointer");
      (options->pedantic_p ? error : warning) (c2m_ctx, pos, "%s", msg);
    }
  }
}

static int anon_struct_union_type_member_p (node_t member) {
  decl_t decl = member->attr;

  return decl != NULL && decl->decl_spec.type->unnamed_anon_struct_union_member_type_p;
}

static node_t get_adjacent_member (node_t member, int next_p) {
  assert (member->code == N_MEMBER);
  while ((member = next_p ? NL_NEXT (member) : NL_PREV (member)) != NULL)
    if (member->code == N_MEMBER
        && (NL_EL (member->ops, 1)->code != N_IGNORE || anon_struct_union_type_member_p (member)))
      break;
  return member;
}

static int update_init_object_path (c2m_ctx_t c2m_ctx, size_t mark, struct type *value_type,
                                    int list_p) {
  init_object_t init_object;
  struct type *el_type;
  node_t size_node;
  mir_llong size_val;
  struct expr *sexpr;

  for (;;) {
    for (;;) {
      if (mark == VARR_LENGTH (init_object_t, init_object_path)) return FALSE;
      init_object = VARR_LAST (init_object_t, init_object_path);
      if (init_object.container_type->mode == TM_ARR) {
        el_type = init_object.container_type->u.arr_type->el_type;
        size_node = init_object.container_type->u.arr_type->size;
        sexpr = size_node->attr;
        size_val = (size_node->code != N_IGNORE && sexpr->const_p && integer_type_p (sexpr->type)
                      ? sexpr->u.i_val
                      : -1);
        init_object.u.curr_index++;
        if (size_val < 0 || init_object.u.curr_index < size_val) break;
        VARR_POP (init_object_t, init_object_path);
      } else {
        assert (init_object.container_type->mode == TM_STRUCT
                || init_object.container_type->mode == TM_UNION);
        if (init_object.u.curr_member == NULL) { /* finding the first named member */
          node_t declaration_list = NL_EL (init_object.container_type->u.tag_type->ops, 1);

          assert (declaration_list != NULL && declaration_list->code == N_LIST);
          for (init_object.u.curr_member = NL_HEAD (declaration_list->ops);
               init_object.u.curr_member != NULL
               && (init_object.u.curr_member->code != N_MEMBER
                   || (NL_EL (init_object.u.curr_member->ops, 1)->code == N_IGNORE
                       && !anon_struct_union_type_member_p (init_object.u.curr_member)));
               init_object.u.curr_member = NL_NEXT (init_object.u.curr_member))
            ;
        } else if (init_object.container_type->mode == TM_UNION
                   && !init_object.designator_p) { /* no next union member: */
          init_object.u.curr_member = NULL;
        } else { /* finding the next named struct member: */
          init_object.u.curr_member = get_adjacent_member (init_object.u.curr_member, TRUE);
        }
        if (init_object.u.curr_member != NULL) {
          init_object.designator_p = FALSE;
          el_type = ((decl_t) init_object.u.curr_member->attr)->decl_spec.type;
          break;
        }
        VARR_POP (init_object_t, init_object_path);
      }
    }
    VARR_SET (init_object_t, init_object_path, VARR_LENGTH (init_object_t, init_object_path) - 1,
              init_object);
    if (list_p || scalar_type_p (el_type)) return TRUE;
    assert (el_type->mode == TM_ARR || el_type->mode == TM_STRUCT || el_type->mode == TM_UNION);
    if (el_type->mode != TM_ARR && value_type != NULL
        && el_type->u.tag_type == value_type->u.tag_type)
      return TRUE;
    init_object.container_type = el_type;
    init_object.designator_p = FALSE;
    if (el_type->mode == TM_ARR) {
      init_object.u.curr_index = -1;
    } else {
      init_object.u.curr_member = NULL;
    }
    VARR_PUSH (init_object_t, init_object_path, init_object);
  }
}

static int update_path_and_do (c2m_ctx_t c2m_ctx,
                               void (*action) (c2m_ctx_t c2m_ctx, decl_t member_decl,
                                               struct type **type_ptr, node_t initializer,
                                               int const_only_p, int top_p),
                               size_t mark, node_t value, int const_only_p, mir_llong *max_index,
                               pos_t pos, const char *detail) {
  init_object_t init_object;
  mir_llong index;
  struct type *el_type;
  struct expr *value_expr = value->attr;

  if (!update_init_object_path (c2m_ctx, mark, value_expr == NULL ? NULL : value_expr->type,
                                value->code == N_LIST || value->code == N_COMPOUND_LITERAL)) {
    error (c2m_ctx, pos, "excess elements in %s initializer", detail);
    return FALSE;
  }
  init_object = VARR_LAST (init_object_t, init_object_path);
  if (init_object.container_type->mode == TM_ARR) {
    el_type = init_object.container_type->u.arr_type->el_type;
    action (c2m_ctx, NULL,
            (value->code == N_STR && char_type_p (el_type)
               ? &init_object.container_type
               : &init_object.container_type->u.arr_type->el_type),
            value, const_only_p, FALSE);
  } else if (init_object.container_type->mode == TM_STRUCT
             || init_object.container_type->mode == TM_UNION) {
    action (c2m_ctx, (decl_t) init_object.u.curr_member->attr,
            &((decl_t) init_object.u.curr_member->attr)->decl_spec.type, value, const_only_p,
            FALSE);
  }
  if (max_index != NULL) {
    init_object = VARR_GET (init_object_t, init_object_path, mark);
    if (init_object.container_type->mode == TM_ARR
        && *max_index < (index = init_object.u.curr_index))
      *max_index = index;
  }
  return TRUE;
}

static int check_const_addr_p (c2m_ctx_t c2m_ctx, node_t r, node_t *base, mir_llong *offset,
                               int *deref) {
  struct expr *e = r->attr;
  struct type *type;
  node_t op1, op2, temp;
  decl_t decl;
  struct decl_spec *decl_spec;
  mir_size_t size;

  if (e->const_p && integer_type_p (e->type)) {
    *base = NULL;
    *offset = (mir_size_t) e->u.u_val;
    *deref = 0;
    return TRUE;
  }
  switch (r->code) {
  case N_STR:
    *base = r;
    *offset = 0;
    *deref = 0;
    return curr_scope == top_scope;
  case N_ID:
    if (e->def_node == NULL)
      return FALSE;
    else if (e->def_node->code == N_FUNC_DEF
             || (e->def_node->code == N_SPEC_DECL
                 && ((decl_t) e->def_node->attr)->decl_spec.type->mode == TM_FUNC)) {
      *base = e->def_node;
      *deref = 0;
    } else if (e->lvalue_node == NULL
               || ((decl = e->lvalue_node->attr)->scope != top_scope
                   && decl->decl_spec.linkage != N_IGNORE)) {
      return FALSE;
    } else {
      *base = e->def_node;
      *deref = e->type->arr_type == NULL;
    }
    *offset = 0;
    return TRUE;
  case N_DEREF:
  case N_ADDR: {
    node_t op = NL_HEAD (r->ops);
    struct expr *e = op->attr;

    if (!check_const_addr_p (c2m_ctx, op, base, offset, deref)) return FALSE;
    if (op->code != N_ID
        || (e->def_node->code != N_FUNC_DEF
            && (e->def_node->code != N_SPEC_DECL
                || ((decl_t) e->def_node->attr)->decl_spec.type->mode != TM_FUNC)))
      r->code == N_DEREF ? (*deref)++ : (*deref)--;
    return TRUE;
  }
  case N_FIELD:
  case N_DEREF_FIELD:
    if (!check_const_addr_p (c2m_ctx, NL_HEAD (r->ops), base, offset, deref)) return FALSE;
    if (*deref != (r->code == N_FIELD ? 1 : 0)) return FALSE;
    *deref = 1;
    e = r->attr;
    decl = e->lvalue_node->attr;
    *offset += decl->offset;
    return TRUE;
  case N_IND:
    if (((struct expr *) NL_HEAD (r->ops)->attr)->type->mode != TM_PTR) return FALSE;
    if (!check_const_addr_p (c2m_ctx, NL_HEAD (r->ops), base, offset, deref)) return FALSE;
    if (!(e = NL_EL (r->ops, 1)->attr)->const_p) return FALSE;
    type = ((struct expr *) r->attr)->type;
    size = type_size (c2m_ctx, type->arr_type != NULL ? type->arr_type : type);
    *deref = 1;
    *offset += e->u.i_val * size;
    return TRUE;
  case N_ADD:
  case N_SUB:
    if ((op2 = NL_EL (r->ops, 1)) == NULL) return FALSE;
    op1 = NL_HEAD (r->ops);
    if (r->code == N_ADD && (e = op1->attr)->const_p) SWAP (op1, op2, temp);
    if (!check_const_addr_p (c2m_ctx, op1, base, offset, deref)) return FALSE;
    if (*deref != 0 && ((struct expr *) op1->attr)->type->arr_type == NULL) return FALSE;
    if (!(e = op2->attr)->const_p) return FALSE;
    type = ((struct expr *) r->attr)->type;
    assert (type->mode == TM_BASIC || type->mode == TM_PTR);
    size = (type->mode == TM_BASIC || type->u.ptr_type->mode == TM_FUNC
              ? 1
              : type_size (c2m_ctx, type->u.ptr_type->arr_type != NULL ? type->u.ptr_type->arr_type
                                                                       : type->u.ptr_type));
    if (r->code == N_ADD)
      *offset += e->u.i_val * size;
    else
      *offset -= e->u.i_val * size;
    return TRUE;
  case N_CAST:
    decl_spec = NL_HEAD (r->ops)->attr;
    if (type_size (c2m_ctx, decl_spec->type) != sizeof (mir_size_t)) return FALSE;
    return check_const_addr_p (c2m_ctx, NL_EL (r->ops, 1), base, offset, deref);
  default: return FALSE;
  }
}

static void setup_const_addr_p (c2m_ctx_t c2m_ctx, node_t r) {
  node_t base;
  mir_llong offset;
  int deref;
  struct expr *e;

  if (!check_const_addr_p (c2m_ctx, r, &base, &offset, &deref) || deref != 0) return;
  e = r->attr;
  e->const_addr_p = TRUE;
  e->def_node = base;
  e->u.i_val = offset;
}

static void process_init_field_designator (c2m_ctx_t c2m_ctx, node_t designator_member,
                                           struct type *container_type) {
  decl_t decl;
  init_object_t init_object;
  node_t curr_member;

  assert (designator_member->code == N_MEMBER);
  /* We can have *partial* path of containing anon members: pop them */
  while (VARR_LENGTH (init_object_t, init_object_path) != 0) {
    init_object = VARR_LAST (init_object_t, init_object_path);
    if ((decl = init_object.u.curr_member->attr) == NULL
        || !decl->decl_spec.type->unnamed_anon_struct_union_member_type_p) {
      break;
    }
    container_type = init_object.container_type;
    VARR_POP (init_object_t, init_object_path);
  }
  /* Now add *full* path to designator_member of containing anon members */
  assert (VARR_LENGTH (node_t, containing_anon_members) == 0);
  decl = designator_member->attr;
  for (curr_member = decl->containing_unnamed_anon_struct_union_member; curr_member != NULL;
       curr_member = decl->containing_unnamed_anon_struct_union_member) {
    decl = curr_member->attr;
    VARR_PUSH (node_t, containing_anon_members, curr_member);
  }
  while (VARR_LENGTH (node_t, containing_anon_members) != 0) {
    init_object.u.curr_member = VARR_POP (node_t, containing_anon_members);
    init_object.container_type = container_type;
    init_object.designator_p = FALSE;
    VARR_PUSH (init_object_t, init_object_path, init_object);
    container_type = (decl = init_object.u.curr_member->attr)->decl_spec.type;
  }
  init_object.u.curr_member = get_adjacent_member (designator_member, FALSE);
  init_object.container_type = container_type;
  init_object.designator_p = TRUE;
  VARR_PUSH (init_object_t, init_object_path, init_object);
}

static node_t get_compound_literal (node_t n, int *addr_p) {
  for (int addr = 0; n != NULL; n = NL_HEAD (n->ops)) {
    switch (n->code) {
    case N_ADDR: addr++; break;
    case N_DEREF: addr--; break;
    case N_CAST: break;  // ???
    case N_STR:
    case N_COMPOUND_LITERAL:
      if (addr < 0) return NULL;
      *addr_p = addr > 0;
      return n;
      break;
    default: return NULL;
    }
    if (addr != -1 && addr != 0 && addr != 1) return NULL;
  }
  return NULL;
}
static void check_initializer (c2m_ctx_t c2m_ctx, decl_t member_decl, struct type **type_ptr,
                               node_t initializer, int const_only_p, int top_p) {
  struct type *type = *type_ptr;
  struct expr *cexpr;
  node_t literal, des_list, curr_des, init, str, value, size_node, temp;
  mir_llong max_index, size_val;
  size_t mark, len;
  symbol_t sym;
  struct expr *sexpr;
  init_object_t init_object;
  int addr_p;

  literal = get_compound_literal (initializer, &addr_p);
  if (literal != NULL && !addr_p && initializer->code != N_STR) {
    cexpr = initializer->attr;
    check_assignment_types (c2m_ctx, type, NULL, cexpr, initializer);
    initializer = NL_EL (literal->ops, 1);
  }
check_one_value:
  if (initializer->code != N_LIST
      && !(initializer->code == N_STR && type->mode == TM_ARR
           && char_type_p (type->u.arr_type->el_type))) {
    if ((cexpr = initializer->attr)->const_p || initializer->code == N_STR || !const_only_p) {
      check_assignment_types (c2m_ctx, type, NULL, cexpr, initializer);
    } else {
      setup_const_addr_p (c2m_ctx, initializer);
      if ((cexpr = initializer->attr)->const_addr_p || (literal != NULL && addr_p))
        check_assignment_types (c2m_ctx, type, NULL, cexpr, initializer);
      else
        error (c2m_ctx, initializer->pos,
               "initializer of non-auto or thread local object"
               " should be a constant expression or address");
    }
    return;
  }
  init = NL_HEAD (initializer->ops);
  if (((str = initializer)->code == N_STR /* string or string in parentheses  */
       || (init->code == N_INIT && NL_EL (initializer->ops, 1) == NULL
           && (des_list = NL_HEAD (init->ops))->code == N_LIST && NL_HEAD (des_list->ops) == NULL
           && NL_EL (init->ops, 1) != NULL && (str = NL_EL (init->ops, 1))->code == N_STR))
      && type->mode == TM_ARR && char_type_p (type->u.arr_type->el_type)) {
    len = str->u.s.len;
    if (incomplete_type_p (c2m_ctx, type)) {
      assert (len < MIR_INT_MAX);
      type->u.arr_type->size = new_i_node (c2m_ctx, len, type->u.arr_type->size->pos);
      check (c2m_ctx, type->u.arr_type->size, NULL);
      make_type_complete (c2m_ctx, type);
    } else if (len > ((struct expr *) type->u.arr_type->size->attr)->u.i_val + 1) {
      error (c2m_ctx, initializer->pos, "string is too long for array initializer");
    }
    return;
  }
  assert (init->code == N_INIT);
  des_list = NL_HEAD (init->ops);
  assert (des_list->code == N_LIST);
  if (type->mode != TM_ARR && type->mode != TM_STRUCT && type->mode != TM_UNION) {
    if ((temp = NL_NEXT (init)) != NULL) {
      error (c2m_ctx, temp->pos, "excess elements in scalar initializer");
      return;
    }
    if ((temp = NL_HEAD (des_list->ops)) != NULL) {
      error (c2m_ctx, temp->pos, "designator in scalar initializer");
      return;
    }
    initializer = NL_NEXT (des_list);
    if (!top_p) {
      error (c2m_ctx, init->pos, "braces around scalar initializer");
      return;
    }
    top_p = FALSE;
    goto check_one_value;
  }
  mark = VARR_LENGTH (init_object_t, init_object_path);
  init_object.container_type = type;
  init_object.designator_p = FALSE;
  if (type->mode == TM_ARR) {
    size_node = type->u.arr_type->size;
    sexpr = size_node->attr;
    size_val = (size_node->code != N_IGNORE && sexpr->const_p && integer_type_p (sexpr->type)
                  ? sexpr->u.i_val
                  : -1);
    init_object.u.curr_index = -1;
  } else {
    init_object.u.curr_member = NULL;
  }
  VARR_PUSH (init_object_t, init_object_path, init_object);
  max_index = -1;
  for (; init != NULL; init = NL_NEXT (init)) {
    assert (init->code == N_INIT);
    des_list = NL_HEAD (init->ops);
    value = NL_NEXT (des_list);
    if ((value->code == N_LIST || value->code == N_COMPOUND_LITERAL) && type->mode != TM_ARR
        && type->mode != TM_STRUCT && type->mode != TM_UNION) {
      error (c2m_ctx, init->pos,
             value->code == N_LIST ? "braces around scalar initializer"
                                   : "compound literal for scalar initializer");
      break;
    }
    if ((curr_des = NL_HEAD (des_list->ops)) == NULL) {
      if (!update_path_and_do (c2m_ctx, check_initializer, mark, value, const_only_p, &max_index,
                               init->pos, "array/struct/union"))
        break;
    } else {
      for (; curr_des != NULL; curr_des = NL_NEXT (curr_des)) {
        VARR_TRUNC (init_object_t, init_object_path, mark + 1);
        init_object = VARR_POP (init_object_t, init_object_path);
        if (curr_des->code == N_FIELD_ID) {
          node_t id = NL_HEAD (curr_des->ops);

          if (type->mode != TM_STRUCT && type->mode != TM_UNION) {
            error (c2m_ctx, curr_des->pos, "field name not in struct or union initializer");
          } else if (!symbol_find (c2m_ctx, S_REGULAR, id, type->u.tag_type, &sym)) {
            error (c2m_ctx, curr_des->pos, "unknown field %s in initializer", id->u.s.s);
          } else {
            process_init_field_designator (c2m_ctx, sym.def_node, init_object.container_type);
            if (!update_path_and_do (c2m_ctx, check_initializer, mark, value, const_only_p, NULL,
                                     init->pos, "struct/union"))
              break;
          }
        } else if (type->mode != TM_ARR) {
          error (c2m_ctx, curr_des->pos, "array index in initializer for non-array");
        } else if (!(cexpr = curr_des->attr)->const_p) {
          error (c2m_ctx, curr_des->pos, "nonconstant array index in initializer");
        } else if (!integer_type_p (cexpr->type)) {
          error (c2m_ctx, curr_des->pos, "array index in initializer not of integer type");
        } else if (incomplete_type_p (c2m_ctx, type) && signed_integer_type_p (cexpr->type)
                   && cexpr->u.i_val < 0) {
          error (c2m_ctx, curr_des->pos,
                 "negative array index in initializer for array without size");
        } else if (size_val >= 0 && size_val <= cexpr->u.u_val) {
          error (c2m_ctx, curr_des->pos, "array index in initializer exceeds array bounds");
        } else {
          init_object.u.curr_index = cexpr->u.i_val - 1; /* previous el */
          init_object.designator_p = FALSE;
          VARR_PUSH (init_object_t, init_object_path, init_object);
          if (!update_path_and_do (c2m_ctx, check_initializer, mark, value, const_only_p,
                                   &max_index, init->pos, "array"))
            break;
        }
      }
    }
  }
  if (type->mode == TM_ARR && size_val < 0 && max_index >= 0) {
    /* Array w/o size: define it.  Copy the type as the incomplete
       type can be shared by declarations with different length
       initializers.  We need only one level of copying as sub-array
       can not have incomplete type with an initializer. */
    struct arr_type *arr_type = reg_malloc (c2m_ctx, sizeof (struct arr_type));

    type = create_type (c2m_ctx, type);
    assert (incomplete_type_p (c2m_ctx, type));
    *arr_type = *type->u.arr_type;
    type->u.arr_type = arr_type;
    size_node = type->u.arr_type->size;
    type->u.arr_type->size
      = (max_index < MIR_INT_MAX
           ? new_i_node (c2m_ctx, max_index + 1, size_node->pos)
           : max_index < MIR_LONG_MAX ? new_l_node (c2m_ctx, max_index + 1, size_node->pos)
                                      : new_ll_node (c2m_ctx, max_index + 1, size_node->pos));
    check (c2m_ctx, type->u.arr_type->size, NULL);
    make_type_complete (c2m_ctx, type);
  }
  VARR_TRUNC (init_object_t, init_object_path, mark);
  *type_ptr = type;
  return;
}

static void check_decl_align (c2m_ctx_t c2m_ctx, struct decl_spec *decl_spec) {
  if (decl_spec->align < 0) return;
  if (decl_spec->align < type_align (decl_spec->type))
    error (c2m_ctx, decl_spec->align_node->pos,
           "requested alignment is less than minimum alignment for the type");
}

static void init_decl (c2m_ctx_t c2m_ctx, decl_t decl) {
  decl->addr_p = FALSE;
  decl->reg_p = decl->used_p = FALSE;
  decl->offset = 0;
  decl->bit_offset = -1;
  decl->scope = curr_scope;
  decl->containing_unnamed_anon_struct_union_member = curr_unnamed_anon_struct_union_member;
  decl->item = NULL;
  decl->c2m_ctx = c2m_ctx;
}

static void create_decl (c2m_ctx_t c2m_ctx, node_t scope, node_t decl_node,
                         struct decl_spec decl_spec, node_t width, node_t initializer,
                         int param_p) {
  int func_def_p = decl_node->code == N_FUNC_DEF, func_p = FALSE;
  node_t id, list_head, declarator;
  struct type *type;
  decl_t decl = reg_malloc (c2m_ctx, sizeof (struct decl));

  assert (decl_node->code == N_MEMBER || decl_node->code == N_SPEC_DECL
          || decl_node->code == N_FUNC_DEF);
  init_decl (c2m_ctx, decl);
  decl->scope = scope;
  decl->decl_spec = decl_spec;
  decl_node->attr = decl;
  declarator = NL_EL (decl_node->ops, 1);
  if (declarator->code == N_IGNORE) {
    assert (decl_node->code == N_MEMBER);
    decl->decl_spec.linkage = N_IGNORE;
  } else {
    assert (declarator->code == N_DECL);
    type = check_declarator (c2m_ctx, declarator, func_def_p);
    decl->decl_spec.type = append_type (type, decl->decl_spec.type);
  }
  check_type (c2m_ctx, decl->decl_spec.type, 0, func_def_p);
  if (declarator->code == N_DECL) {
    id = NL_HEAD (declarator->ops);
    list_head = NL_HEAD (NL_NEXT (id)->ops);
    func_p = !param_p && list_head && list_head->code == N_FUNC;
    decl->decl_spec.linkage = get_id_linkage (c2m_ctx, func_p, id, scope, decl->decl_spec);
  }
  if (declarator->code == N_DECL) {
    def_symbol (c2m_ctx, S_REGULAR, id, scope, decl_node, decl->decl_spec.linkage);
    if (scope != top_scope && decl->decl_spec.linkage == N_EXTERN)
      def_symbol (c2m_ctx, S_REGULAR, id, top_scope, decl_node, N_EXTERN);
    if (func_p && decl->decl_spec.thread_local_p) {
      error (c2m_ctx, id->pos, "thread local function declaration");
      if (options->message_file != NULL) {
        if (id->code != N_IGNORE) fprintf (options->message_file, " of %s", id->u.s.s);
        fprintf (options->message_file, "\n");
      }
    }
  }
  if (decl_node->code != N_MEMBER) {
    set_type_layout (c2m_ctx, decl->decl_spec.type);
    check_decl_align (c2m_ctx, &decl->decl_spec);
    if (!decl->decl_spec.typedef_p && decl->scope != top_scope && decl->scope->code != N_FUNC)
      VARR_PUSH (decl_t, func_decls_for_allocation, decl);
  }
  if (initializer == NULL || initializer->code == N_IGNORE) return;
  if (incomplete_type_p (c2m_ctx, decl->decl_spec.type)
      && (decl->decl_spec.type->mode != TM_ARR
          || incomplete_type_p (c2m_ctx, decl->decl_spec.type->u.arr_type->el_type))) {
    if (decl->decl_spec.type->mode == TM_ARR
        && decl->decl_spec.type->u.arr_type->el_type->mode == TM_ARR)
      error (c2m_ctx, initializer->pos, "initialization of incomplete sub-array");
    else
      error (c2m_ctx, initializer->pos, "initialization of incomplete type variable");
    return;
  }
  if (decl->decl_spec.linkage != N_IGNORE && scope != top_scope) {
    error (c2m_ctx, initializer->pos,
           "initialization of %s in block scope with external or internal linkage", id->u.s.s);
    return;
  }
  check (c2m_ctx, initializer, decl_node);
  check_initializer (c2m_ctx, NULL, &decl->decl_spec.type, initializer,
                     decl->decl_spec.linkage == N_STATIC || decl->decl_spec.linkage == N_EXTERN
                       || decl->decl_spec.thread_local_p || decl->decl_spec.static_p,
                     TRUE);
  if (decl_node->code != N_MEMBER && !decl->decl_spec.typedef_p && decl->scope != top_scope
      && decl->scope->code != N_FUNC)
    /* Process after initilizer because we can make type complete by it. */
    VARR_PUSH (decl_t, func_decls_for_allocation, decl);
}

static struct type *adjust_type (c2m_ctx_t c2m_ctx, struct type *type) {
  struct type *res;

  if (type->mode != TM_ARR && type->mode != TM_FUNC) return type;
  res = create_type (c2m_ctx, NULL);
  res->mode = TM_PTR;
  res->pos_node = type->pos_node;
  if (type->mode == TM_FUNC) {
    res->u.ptr_type = type;
  } else {
    res->arr_type = type;
    res->u.ptr_type = type->u.arr_type->el_type;
    res->type_qual = type->u.arr_type->ind_type_qual;
  }
  set_type_layout (c2m_ctx, res);
  return res;
}

static void process_unop (c2m_ctx_t c2m_ctx, node_t r, node_t *op, struct expr **e, struct type **t,
                          node_t context) {
  *op = NL_HEAD (r->ops);
  check (c2m_ctx, *op, context);
  *e = (*op)->attr;
  *t = (*e)->type;
}

static void process_bin_ops (c2m_ctx_t c2m_ctx, node_t r, node_t *op1, node_t *op2,
                             struct expr **e1, struct expr **e2, struct type **t1, struct type **t2,
                             node_t context) {
  *op1 = NL_HEAD (r->ops);
  *op2 = NL_NEXT (*op1);
  check (c2m_ctx, *op1, context);
  check (c2m_ctx, *op2, context);
  *e1 = (*op1)->attr;
  *e2 = (*op2)->attr;
  *t1 = (*e1)->type;
  *t2 = (*e2)->type;
}

static void process_type_bin_ops (c2m_ctx_t c2m_ctx, node_t r, node_t *op1, node_t *op2,
                                  struct expr **e2, struct type **t2, node_t context) {
  *op1 = NL_HEAD (r->ops);
  *op2 = NL_NEXT (*op1);
  check (c2m_ctx, *op1, context);
  check (c2m_ctx, *op2, context);
  *e2 = (*op2)->attr;
  *t2 = (*e2)->type;
}

static struct expr *create_expr (c2m_ctx_t c2m_ctx, node_t r) {
  struct expr *e = reg_malloc (c2m_ctx, sizeof (struct expr));

  r->attr = e;
  e->type = create_type (c2m_ctx, NULL);
  e->type2 = NULL;
  e->type->pos_node = r;
  e->lvalue_node = NULL;
  e->const_p = e->const_addr_p = e->builtin_call_p = FALSE;
  return e;
}

static struct expr *create_basic_type_expr (c2m_ctx_t c2m_ctx, node_t r, enum basic_type bt) {
  struct expr *e = create_expr (c2m_ctx, r);

  e->type->mode = TM_BASIC;
  e->type->u.basic_type = bt;
  return e;
}

static void get_int_node (c2m_ctx_t c2m_ctx, node_t *op, struct expr **e, struct type **t,
                          mir_size_t i) {
  if (i == 1) {
    *op = n_i1_node;
  } else {
    *op = new_i_node (c2m_ctx, i, no_pos);
    check (c2m_ctx, *op, NULL);
  }
  *e = (*op)->attr;
  *t = (*e)->type;
  init_type (*t);
  (*e)->type->mode = TM_BASIC;
  (*e)->type->u.basic_type = TP_INT;
  (*e)->u.i_val = i;  // ???
}

static struct expr *check_assign_op (c2m_ctx_t c2m_ctx, node_t r, node_t op1, node_t op2,
                                     struct expr *e1, struct expr *e2, struct type *t1,
                                     struct type *t2) {
  struct expr *e, *te;
  struct type t, *tt;

  switch (r->code) {
  case N_AND:
  case N_OR:
  case N_XOR:
  case N_AND_ASSIGN:
  case N_OR_ASSIGN:
  case N_XOR_ASSIGN:
    e = create_expr (c2m_ctx, r);
    e->type->mode = TM_BASIC;
    e->type->u.basic_type = TP_INT;
    if (!integer_type_p (t1) || !integer_type_p (t2)) {
      error (c2m_ctx, r->pos, "bitwise operation operands should be of an integer type");
    } else {
      t = arithmetic_conversion (t1, t2);
      e->type->u.basic_type = t.u.basic_type;
      if (e1->const_p && e2->const_p) {
        convert_value (e1, &t);
        convert_value (e2, &t);
        e->const_p = TRUE;
        if (signed_integer_type_p (&t))
          e->u.i_val = (r->code == N_AND ? e1->u.i_val & e2->u.i_val
                                         : r->code == N_OR ? e1->u.i_val | e2->u.i_val
                                                           : e1->u.i_val ^ e2->u.i_val);
        else
          e->u.u_val = (r->code == N_AND ? e1->u.u_val & e2->u.u_val
                                         : r->code == N_OR ? e1->u.u_val | e2->u.u_val
                                                           : e1->u.u_val ^ e2->u.u_val);
      }
    }
    break;
  case N_LSH:
  case N_RSH:
  case N_LSH_ASSIGN:
  case N_RSH_ASSIGN:
    e = create_expr (c2m_ctx, r);
    e->type->mode = TM_BASIC;
    e->type->u.basic_type = TP_INT;
    if (!integer_type_p (t1) || !integer_type_p (t2)) {
      error (c2m_ctx, r->pos, "shift operands should be of an integer type");
    } else {
      t = integer_promotion (t1);
      e->type->u.basic_type = t.u.basic_type;
      if (e1->const_p && e2->const_p) {
        struct type rt = integer_promotion (t2);

        convert_value (e1, &t);
        convert_value (e2, &rt);
        e->const_p = TRUE;
        if (signed_integer_type_p (&t)) {
          if (signed_integer_type_p (&rt))
            e->u.i_val = r->code == N_LSH ? e1->u.i_val << e2->u.i_val : e1->u.i_val >> e2->u.i_val;
          else
            e->u.i_val = r->code == N_LSH ? e1->u.i_val << e2->u.u_val : e1->u.i_val >> e2->u.u_val;
        } else if (signed_integer_type_p (&rt)) {
          e->u.u_val = r->code == N_LSH ? e1->u.u_val << e2->u.i_val : e1->u.u_val >> e2->u.i_val;
        } else {
          e->u.u_val = r->code == N_LSH ? e1->u.u_val << e2->u.u_val : e1->u.u_val >> e2->u.u_val;
        }
      }
    }
    break;
  case N_INC:
  case N_DEC:
  case N_POST_INC:
  case N_POST_DEC:
  case N_ADD:
  case N_SUB:
  case N_ADD_ASSIGN:
  case N_SUB_ASSIGN: {
    mir_size_t size;
    int add_p
      = (r->code == N_ADD || r->code == N_ADD_ASSIGN || r->code == N_INC || r->code == N_POST_INC);

    e = create_expr (c2m_ctx, r);
    e->type->mode = TM_BASIC;
    e->type->u.basic_type = TP_INT;
    if (arithmetic_type_p (t1) && arithmetic_type_p (t2)) {
      t = arithmetic_conversion (t1, t2);
      e->type->u.basic_type = t.u.basic_type;
      if (e1->const_p && e2->const_p) {
        e->const_p = TRUE;
        convert_value (e1, &t);
        convert_value (e2, &t);
        if (floating_type_p (&t))
          e->u.d_val = (add_p ? e1->u.d_val + e2->u.d_val : e1->u.d_val - e2->u.d_val);
        else if (signed_integer_type_p (&t))
          e->u.i_val = (add_p ? e1->u.i_val + e2->u.i_val : e1->u.i_val - e2->u.i_val);
        else
          e->u.u_val = (add_p ? e1->u.u_val + e2->u.u_val : e1->u.u_val - e2->u.u_val);
      }
    } else if (add_p) {
      if (t2->mode == TM_PTR) {
        SWAP (t1, t2, tt);
        SWAP (e1, e2, te);
      }
      if (t1->mode != TM_PTR || !integer_type_p (t2)) {
        error (c2m_ctx, r->pos, "invalid operand types of +");
      } else if (incomplete_type_p (c2m_ctx, t1->u.ptr_type)) {
        error (c2m_ctx, r->pos, "pointer to incomplete type as an operand of +");
      } else {
        *e->type = *t1;
        if (e1->const_p && e2->const_p) {
          size = type_size (c2m_ctx, t1->u.ptr_type);
          e->const_p = TRUE;
          e->u.u_val = (signed_integer_type_p (t2) ? e1->u.u_val + e2->u.i_val * size
                                                   : e1->u.u_val + e2->u.u_val * size);
        }
      }
    } else if (t1->mode == TM_PTR && integer_type_p (t2)) {
      if (incomplete_type_p (c2m_ctx, t1->u.ptr_type)) {
        error (c2m_ctx, r->pos, "pointer to incomplete type as an operand of -");
      } else {
        *e->type = *t1;
        if (e1->const_p && e2->const_p) {
          size = type_size (c2m_ctx, t1->u.ptr_type);
          e->const_p = TRUE;
          e->u.u_val = (signed_integer_type_p (t2) ? e1->u.u_val - e2->u.i_val * size
                                                   : e1->u.u_val - e2->u.u_val * size);
        }
      }
    } else if (t1->mode == TM_PTR && t2->mode == TM_PTR && compatible_types_p (t1, t2, TRUE)) {
      if (incomplete_type_p (c2m_ctx, t1->u.ptr_type)
          && incomplete_type_p (c2m_ctx, t2->u.ptr_type)) {
        error (c2m_ctx, r->pos, "pointer to incomplete type as an operand of -");
      } else if (t1->u.ptr_type->type_qual.atomic_p || t2->u.ptr_type->type_qual.atomic_p) {
        error (c2m_ctx, r->pos, "pointer to atomic type as an operand of -");
      } else {
        e->type->mode = TM_BASIC;
        e->type->u.basic_type = get_int_basic_type (sizeof (mir_ptrdiff_t));
        set_type_layout (c2m_ctx, e->type);
        if (e1->const_p && e2->const_p) {
          size = type_size (c2m_ctx, t1->u.ptr_type);
          e->const_p = TRUE;
          e->u.i_val
            = (e1->u.u_val > e2->u.u_val ? (mir_ptrdiff_t) ((e1->u.u_val - e2->u.u_val) / size)
                                         : -(mir_ptrdiff_t) ((e2->u.u_val - e1->u.u_val) / size));
        }
      }
    } else {
      error (c2m_ctx, r->pos, "invalid operand types of -");
    }
    break;
  }
  case N_MUL:
  case N_DIV:
  case N_MOD:
  case N_MUL_ASSIGN:
  case N_DIV_ASSIGN:
  case N_MOD_ASSIGN:
    e = create_expr (c2m_ctx, r);
    e->type->mode = TM_BASIC;
    e->type->u.basic_type = TP_INT;
    if (r->code == N_MOD && (!integer_type_p (t1) || !integer_type_p (t2))) {
      error (c2m_ctx, r->pos, "invalid operand types of %%");
    } else if (r->code != N_MOD && (!arithmetic_type_p (t1) || !arithmetic_type_p (t2))) {
      error (c2m_ctx, r->pos, "invalid operand types of %s", r->code == N_MUL ? "*" : "/");
    } else {
      t = arithmetic_conversion (t1, t2);
      e->type->u.basic_type = t.u.basic_type;
      if (e1->const_p && e2->const_p) {
        e->const_p = TRUE;
        convert_value (e1, &t);
        convert_value (e2, &t);
        if (r->code == N_MUL) {
          if (floating_type_p (&t))
            e->u.d_val = e1->u.d_val * e2->u.d_val;
          else if (signed_integer_type_p (&t))
            e->u.i_val = e1->u.i_val * e2->u.i_val;
          else
            e->u.u_val = e1->u.u_val * e2->u.u_val;
        } else if ((floating_type_p (&t) && e1->u.d_val == 0.0 && e2->u.d_val == 0.0)
                   || (signed_integer_type_p (&t) && e2->u.i_val == 0)
                   || (integer_type_p (&t) && !signed_integer_type_p (&t) && e2->u.u_val == 0)) {
          if (floating_type_p (&t)) {
            e->u.d_val = nanl (""); /* Use NaN */
          } else {
            if (signed_integer_type_p (&t))
              e->u.i_val = 0;
            else
              e->u.u_val = 0;
            error (c2m_ctx, r->pos, "Division by zero");
          }
        } else if (r->code != N_MOD && floating_type_p (&t)) {
          e->u.d_val = e1->u.d_val / e2->u.d_val;
        } else if (signed_integer_type_p (&t)) {  // ??? zero
          e->u.i_val = r->code == N_DIV ? e1->u.i_val / e2->u.i_val : e1->u.i_val % e2->u.i_val;
        } else {
          e->u.u_val = r->code == N_DIV ? e1->u.u_val / e2->u.u_val : e1->u.u_val % e2->u.u_val;
        }
      }
    }
    break;
  default: assert (FALSE);
  }
  return e;
}

static unsigned case_hash (case_t el) {
  node_t case_expr = NL_HEAD (el->case_node->ops);
  struct expr *expr;

  assert (el->case_node->code == N_CASE);
  expr = case_expr->attr;
  assert (expr->const_p);
  if (signed_integer_type_p (expr->type))
    return mir_hash (&expr->u.i_val, sizeof (expr->u.i_val), 0x42);
  return mir_hash (&expr->u.u_val, sizeof (expr->u.u_val), 0x42);
}

static int case_eq (case_t el1, case_t el2) {
  node_t case_expr1 = NL_HEAD (el1->case_node->ops);
  node_t case_expr2 = NL_HEAD (el2->case_node->ops);
  struct expr *expr1, *expr2;

  assert (el1->case_node->code == N_CASE && el2->case_node->code == N_CASE);
  expr1 = case_expr1->attr;
  expr2 = case_expr2->attr;
  assert (expr1->const_p && expr2->const_p);
  assert (signed_integer_type_p (expr1->type) == signed_integer_type_p (expr2->type));
  if (signed_integer_type_p (expr1->type)) return expr1->u.i_val == expr2->u.i_val;
  return expr1->u.u_val == expr2->u.u_val;
}

static void update_call_arg_area_offset (c2m_ctx_t c2m_ctx, struct type *type, int update_scope_p) {
  node_t block = NL_EL (curr_func_def->ops, 3);
  struct node_scope *ns = block->attr;

  curr_call_arg_area_offset += round_size (type_size (c2m_ctx, type), MAX_ALIGNMENT);
  if (update_scope_p && ns->call_arg_area_size < curr_call_arg_area_offset)
    ns->call_arg_area_size = curr_call_arg_area_offset;
}

#define NODE_CASE(n) case N_##n:
#define REP_SEP
static void classify_node (node_t n, int *expr_attr_p, int *stmt_p) {
  *expr_attr_p = *stmt_p = FALSE;
  switch (n->code) {
    REP8 (NODE_CASE, I, L, LL, U, UL, ULL, F, D)
    REP8 (NODE_CASE, LD, CH, STR, ID, COMMA, ANDAND, OROR, EQ)
    REP8 (NODE_CASE, NE, LT, LE, GT, GE, ASSIGN, BITWISE_NOT, NOT)
    REP8 (NODE_CASE, AND, AND_ASSIGN, OR, OR_ASSIGN, XOR, XOR_ASSIGN, LSH, LSH_ASSIGN)
    REP8 (NODE_CASE, RSH, RSH_ASSIGN, ADD, ADD_ASSIGN, SUB, SUB_ASSIGN, MUL, MUL_ASSIGN)
    REP8 (NODE_CASE, DIV, DIV_ASSIGN, MOD, MOD_ASSIGN, IND, FIELD, ADDR, DEREF)
    REP8 (NODE_CASE, DEREF_FIELD, COND, INC, DEC, POST_INC, POST_DEC, ALIGNOF, SIZEOF)
    REP6 (NODE_CASE, EXPR_SIZEOF, CAST, COMPOUND_LITERAL, CALL, GENERIC, GENERIC_ASSOC)
    *expr_attr_p = TRUE;
    break;
    REP8 (NODE_CASE, IF, SWITCH, WHILE, DO, FOR, GOTO, CONTINUE, BREAK)
    REP4 (NODE_CASE, RETURN, EXPR, BLOCK, SPEC_DECL) /* SPEC DECL may have an initializer */
    *stmt_p = TRUE;
    break;
    REP8 (NODE_CASE, IGNORE, CASE, DEFAULT, LABEL, LIST, SHARE, TYPEDEF, EXTERN)
    REP8 (NODE_CASE, STATIC, AUTO, REGISTER, THREAD_LOCAL, DECL, VOID, CHAR, SHORT)
    REP8 (NODE_CASE, INT, LONG, FLOAT, DOUBLE, SIGNED, UNSIGNED, BOOL, STRUCT)
    REP8 (NODE_CASE, UNION, ENUM, ENUM_CONST, MEMBER, CONST, RESTRICT, VOLATILE, ATOMIC)
    REP8 (NODE_CASE, INLINE, NO_RETURN, ALIGNAS, FUNC, STAR, POINTER, DOTS, ARR)
    REP6 (NODE_CASE, INIT, FIELD_ID, TYPE, ST_ASSERT, FUNC_DEF, MODULE)
    break;
  default: assert (FALSE);
  }
}
#undef REP_SEP

/* Create "static const char __func__[] = "<func name>" at the
   beginning of func_block if it is necessary.  */
static void add__func__def (c2m_ctx_t c2m_ctx, node_t func_block, str_t func_name) {
  static const char fdecl_name[] = "__func__";
  pos_t pos = func_block->pos;
  node_t list, declarator, decl, decl_specs;
  str_t str;

  if (!str_exists_p (c2m_ctx, fdecl_name, strlen (fdecl_name) + 1, &str)) return;
  decl_specs = new_pos_node (c2m_ctx, N_LIST, pos);
  NL_APPEND (decl_specs->ops, new_pos_node (c2m_ctx, N_STATIC, pos));
  NL_APPEND (decl_specs->ops, new_pos_node (c2m_ctx, N_CONST, pos));
  NL_APPEND (decl_specs->ops, new_pos_node (c2m_ctx, N_CHAR, pos));
  list = new_pos_node (c2m_ctx, N_LIST, pos);
  NL_APPEND (list->ops, new_pos_node3 (c2m_ctx, N_ARR, pos, new_pos_node (c2m_ctx, N_IGNORE, pos),
                                       new_pos_node (c2m_ctx, N_LIST, pos),
                                       new_pos_node (c2m_ctx, N_IGNORE, pos)));
  declarator = new_pos_node2 (c2m_ctx, N_DECL, pos, new_str_node (c2m_ctx, N_ID, str, pos), list);
  decl = new_pos_node3 (c2m_ctx, N_SPEC_DECL, pos, decl_specs, declarator,
                        new_str_node (c2m_ctx, N_STR, func_name, pos));
  NL_PREPEND (NL_EL (func_block->ops, 1)->ops, decl);
}

/* Sort by decl scope nesting (more nested scope has a bigger UID) and decl size. */
static int decl_cmp (const void *v1, const void *v2) {
  const decl_t d1 = *(const decl_t *) v1, d2 = *(const decl_t *) v2;
  struct type *t1 = d1->decl_spec.type, *t2 = d2->decl_spec.type;
  mir_size_t s1 = raw_type_size (d1->c2m_ctx, t1), s2 = raw_type_size (d2->c2m_ctx, t2);

  if (d1->scope->uid < d2->scope->uid) return -1;
  if (d1->scope->uid > d2->scope->uid) return 1;
  if (s1 < s2) return -1;
  if (s1 > s2) return 1;
  return 0;
}

static void process_func_decls_for_allocation (c2m_ctx_t c2m_ctx) {
  size_t i, j;
  decl_t decl;
  struct type *type;
  struct node_scope *ns, *curr_ns;
  node_t scope;
  mir_size_t start_offset;

  /* Exclude decls which will be in regs: */
  for (i = j = 0; i < VARR_LENGTH (decl_t, func_decls_for_allocation); i++) {
    decl = VARR_GET (decl_t, func_decls_for_allocation, i);
    type = decl->decl_spec.type;
    ns = decl->scope->attr;
    if (scalar_type_p (type) && !decl->addr_p) {
      decl->reg_p = TRUE;
      continue;
    }
    VARR_SET (decl_t, func_decls_for_allocation, j, decl);
    j++;
  }
  VARR_TRUNC (decl_t, func_decls_for_allocation, j);
  qsort (VARR_ADDR (decl_t, func_decls_for_allocation), j, sizeof (decl_t), decl_cmp);
  scope = NULL;
  for (i = 0; i < VARR_LENGTH (decl_t, func_decls_for_allocation); i++) {
    decl = VARR_GET (decl_t, func_decls_for_allocation, i);
    type = decl->decl_spec.type;
    ns = decl->scope->attr;
    if (decl->scope != scope) { /* new scope: process upper scopes */
      for (scope = ns->scope; scope != top_scope; scope = curr_ns->scope) {
        curr_ns = scope->attr;
        ns->offset += curr_ns->size;
        curr_ns->stack_var_p = TRUE;
      }
      scope = decl->scope;
      ns->stack_var_p = TRUE;
      start_offset = ns->offset;
    }
    ns->offset = round_size (ns->offset, var_align (c2m_ctx, type));
    decl->offset = ns->offset;
    ns->offset += var_size (c2m_ctx, type);
    ns->size = ns->offset - start_offset;
  }
  scope = NULL;
  for (i = 0; i < VARR_LENGTH (decl_t, func_decls_for_allocation); i++) { /* update scope sizes: */
    decl = VARR_GET (decl_t, func_decls_for_allocation, i);
    ns = decl->scope->attr;
    if (decl->scope == scope) continue;
    /* new scope: update upper scope sizes */
    for (scope = ns->scope; scope != top_scope; scope = curr_ns->scope) {
      curr_ns = scope->attr;
      if (curr_ns->size < ns->offset) curr_ns->size = ns->offset;
      if (ns->stack_var_p) curr_ns->stack_var_p = TRUE;
    }
  }
}

#define BUILTIN_VA_START "__builtin_va_start"
#define BUILTIN_VA_ARG "__builtin_va_arg"
#define ALLOCA "alloca"

static void check (c2m_ctx_t c2m_ctx, node_t r, node_t context) {
  node_t op1, op2;
  struct expr *e = NULL, *e1, *e2;
  struct type t, *t1, *t2, *assign_expr_type;
  int expr_attr_p, stmt_p;

  VARR_PUSH (node_t, context_stack, context);
  classify_node (r, &expr_attr_p, &stmt_p);
  switch (r->code) {
  case N_IGNORE:
  case N_STAR:
  case N_FIELD_ID: break; /* do nothing */
  case N_LIST: {
    for (node_t n = NL_HEAD (r->ops); n != NULL; n = NL_NEXT (n)) check (c2m_ctx, n, r);
    break;
  }
  case N_I:
  case N_L:
    e = create_basic_type_expr (c2m_ctx, r, r->code == N_I ? TP_INT : TP_LONG);
    e->const_p = TRUE;
    e->u.i_val = r->u.l;
    break;
  case N_LL:
    e = create_basic_type_expr (c2m_ctx, r, TP_LLONG);
    e->const_p = TRUE;
    e->u.i_val = r->u.ll;
    break;
  case N_U:
  case N_UL:
    e = create_basic_type_expr (c2m_ctx, r, r->code == N_U ? TP_UINT : TP_ULONG);
    e->const_p = TRUE;
    e->u.u_val = r->u.ul;
    break;
  case N_ULL:
    e = create_basic_type_expr (c2m_ctx, r, TP_ULLONG);
    e->const_p = TRUE;
    e->u.u_val = r->u.ull;
    break;
  case N_F:
    e = create_basic_type_expr (c2m_ctx, r, TP_FLOAT);
    e->const_p = TRUE;
    e->u.d_val = r->u.f;
    break;
  case N_D:
    e = create_basic_type_expr (c2m_ctx, r, TP_DOUBLE);
    e->const_p = TRUE;
    e->u.d_val = r->u.d;
    break;
  case N_LD:
    e = create_basic_type_expr (c2m_ctx, r, TP_LDOUBLE);
    e->const_p = TRUE;
    e->u.d_val = r->u.ld;
    break;
  case N_CH:
    e = create_basic_type_expr (c2m_ctx, r, TP_CHAR);
    e->const_p = TRUE;
    if (char_is_signed_p ())
      e->u.i_val = r->u.ch;
    else
      e->u.u_val = r->u.ch;
    break;
  case N_STR: {
    struct arr_type *arr_type;

    e = create_expr (c2m_ctx, r);
    e->lvalue_node = r;
    e->type->mode = TM_ARR;
    e->type->pos_node = r;
    e->type->u.arr_type = arr_type = reg_malloc (c2m_ctx, sizeof (struct arr_type));
    clear_type_qual (&arr_type->ind_type_qual);
    arr_type->static_p = FALSE;
    arr_type->el_type = create_type (c2m_ctx, NULL);
    arr_type->el_type->pos_node = r;
    arr_type->el_type->mode = TM_BASIC;
    arr_type->el_type->u.basic_type = TP_CHAR;
    arr_type->size = new_i_node (c2m_ctx, r->u.s.len, r->pos);
    check (c2m_ctx, arr_type->size, NULL);
    break;
  }
  case N_ID: {
    node_t aux_node = NULL;
    decl_t decl;

    op1 = find_def (c2m_ctx, S_REGULAR, r, curr_scope, &aux_node);
    e = create_expr (c2m_ctx, r);
    e->def_node = op1;
    if (op1 == NULL) {
      error (c2m_ctx, r->pos, "undeclared identifier %s", r->u.s.s);
    } else if (op1->code == N_IGNORE) {
      e->type->mode = TM_BASIC;
      e->type->u.basic_type = TP_INT;
    } else if (op1->code == N_SPEC_DECL) {
      decl = op1->attr;
      if (decl->decl_spec.typedef_p)
        error (c2m_ctx, r->pos, "typedef name %s as an operand", r->u.s.s);
      decl->used_p = TRUE;
      *e->type = *decl->decl_spec.type;
      if (e->type->mode != TM_FUNC) e->lvalue_node = op1;
    } else if (op1->code == N_FUNC_DEF) {
      decl = op1->attr;
      decl->used_p = TRUE;
      assert (decl->decl_spec.type->mode == TM_FUNC);
      *e->type = *decl->decl_spec.type;
    } else {
      assert (op1->code == N_ENUM_CONST && aux_node && aux_node->code == N_ENUM);
      e->type->mode = TM_ENUM;
      e->type->pos_node = r;
      e->type->u.tag_type = aux_node;
      e->const_p = TRUE;
      e->u.i_val = ((struct enum_value *) op1->attr)->val;
    }
    break;
  }
  case N_COMMA:
    process_bin_ops (c2m_ctx, r, &op1, &op2, &e1, &e2, &t1, &t2, r);
    e = create_expr (c2m_ctx, r);
    *e->type = *e2->type;
    break;
  case N_ANDAND:
  case N_OROR:
    process_bin_ops (c2m_ctx, r, &op1, &op2, &e1, &e2, &t1, &t2, r);
    e = create_expr (c2m_ctx, r);
    e->type->mode = TM_BASIC;
    e->type->u.basic_type = TP_INT;
    if (!scalar_type_p (t1) || !scalar_type_p (t2)) {
      error (c2m_ctx, r->pos, "invalid operand types of %s", r->code == N_ANDAND ? "&&" : "||");
    } else if (e1->const_p) {
      int v;

      if (floating_type_p (t1))
        v = e1->u.d_val != 0.0;
      else if (signed_integer_type_p (t1))
        v = e1->u.i_val != 0;
      else
        v = e1->u.u_val != 0;
      if (v && r->code == N_OROR) {
        e->const_p = TRUE;
        e->u.i_val = v;
      } else if (!v && r->code == N_ANDAND) {
        e->const_p = TRUE;
        e->u.i_val = v;
      } else if (e2->const_p) {
        e->const_p = TRUE;
        if (floating_type_p (t2))
          v = e2->u.d_val != 0.0;
        else if (signed_integer_type_p (t2))
          v = e2->u.i_val != 0;
        else
          v = e2->u.u_val != 0;
        e->u.i_val = v;
      }
    }
    break;
  case N_EQ:
  case N_NE:
  case N_LT:
  case N_LE:
  case N_GT:
  case N_GE:
    process_bin_ops (c2m_ctx, r, &op1, &op2, &e1, &e2, &t1, &t2, r);
    e = create_expr (c2m_ctx, r);
    e->type->mode = TM_BASIC;
    e->type->u.basic_type = TP_INT;
    if ((r->code == N_EQ || r->code == N_NE)
        && ((t1->mode == TM_PTR && null_const_p (e2, t2))
            || (t2->mode == TM_PTR && null_const_p (e1, t1))))
      ;
    else if (t1->mode == TM_PTR && t2->mode == TM_PTR) {
      if (!compatible_types_p (t1, t2, TRUE)
          && ((r->code != N_EQ && r->code != N_NE) || (!void_ptr_p (t1) && !void_ptr_p (t2)))) {
        error (c2m_ctx, r->pos, "incompatible pointer types in comparison");
      } else if (t1->u.ptr_type->type_qual.atomic_p || t2->u.ptr_type->type_qual.atomic_p) {
        error (c2m_ctx, r->pos, "pointer to atomic type as a comparison operand");
      } else if (e1->const_p && e2->const_p) {
        e->const_p = TRUE;
        e->u.i_val = (r->code == N_EQ
                        ? e1->u.u_val == e2->u.u_val
                        : r->code == N_NE
                            ? e1->u.u_val != e2->u.u_val
                            : r->code == N_LT
                                ? e1->u.u_val < e2->u.u_val
                                : r->code == N_LE ? e1->u.u_val <= e2->u.u_val
                                                  : r->code == N_GT ? e1->u.u_val > e2->u.u_val
                                                                    : e1->u.u_val >= e2->u.u_val);
      }
    } else if (arithmetic_type_p (t1) && arithmetic_type_p (t2)) {
      if (e1->const_p && e2->const_p) {
        t = arithmetic_conversion (t1, t2);
        convert_value (e1, &t);
        convert_value (e2, &t);
        e->const_p = TRUE;
        if (floating_type_p (&t))
          e->u.i_val = (r->code == N_EQ
                          ? e1->u.d_val == e2->u.d_val
                          : r->code == N_NE
                              ? e1->u.d_val != e2->u.d_val
                              : r->code == N_LT
                                  ? e1->u.d_val < e2->u.d_val
                                  : r->code == N_LE ? e1->u.d_val <= e2->u.d_val
                                                    : r->code == N_GT ? e1->u.d_val > e2->u.d_val
                                                                      : e1->u.d_val >= e2->u.d_val);
        else if (signed_integer_type_p (&t))
          e->u.i_val = (r->code == N_EQ
                          ? e1->u.i_val == e2->u.i_val
                          : r->code == N_NE
                              ? e1->u.i_val != e2->u.i_val
                              : r->code == N_LT
                                  ? e1->u.i_val < e2->u.i_val
                                  : r->code == N_LE ? e1->u.i_val <= e2->u.i_val
                                                    : r->code == N_GT ? e1->u.i_val > e2->u.i_val
                                                                      : e1->u.i_val >= e2->u.i_val);
        else
          e->u.i_val = (r->code == N_EQ
                          ? e1->u.u_val == e2->u.u_val
                          : r->code == N_NE
                              ? e1->u.u_val != e2->u.u_val
                              : r->code == N_LT
                                  ? e1->u.u_val < e2->u.u_val
                                  : r->code == N_LE ? e1->u.u_val <= e2->u.u_val
                                                    : r->code == N_GT ? e1->u.u_val > e2->u.u_val
                                                                      : e1->u.u_val >= e2->u.u_val);
      }
    } else {
      error (c2m_ctx, r->pos, "invalid types of comparison operands");
    }
    break;
  case N_BITWISE_NOT:
  case N_NOT:
    process_unop (c2m_ctx, r, &op1, &e1, &t1, r);
    e = create_expr (c2m_ctx, r);
    e->type->mode = TM_BASIC;
    e->type->u.basic_type = TP_INT;
    if (r->code == N_BITWISE_NOT && !integer_type_p (t1)) {
      error (c2m_ctx, r->pos, "bitwise-not operand should be of an integer type");
    } else if (r->code == N_NOT && !scalar_type_p (t1)) {
      error (c2m_ctx, r->pos, "not operand should be of a scalar type");
    } else if (r->code == N_BITWISE_NOT) {
      t = integer_promotion (t1);
      e->type->u.basic_type = t.u.basic_type;
      if (e1->const_p) {
        convert_value (e1, &t);
        e->const_p = TRUE;
        if (signed_integer_type_p (&t))
          e->u.i_val = ~e1->u.i_val;
        else
          e->u.u_val = ~e1->u.u_val;
      }
    } else if (e1->const_p) {
      e->const_p = TRUE;
      if (floating_type_p (t1))
        e->u.i_val = e1->u.d_val == 0.0;
      else if (signed_integer_type_p (t1))
        e->u.i_val = e1->u.i_val == 0;
      else
        e->u.i_val = e1->u.u_val == 0;
    }
    break;
  case N_INC:
  case N_DEC:
  case N_POST_INC:
  case N_POST_DEC: {
    struct expr saved_expr;

    process_unop (c2m_ctx, r, &op1, &e1, &t1, NULL);
    saved_expr = *e1;
    t1 = e1->type = adjust_type (c2m_ctx, e1->type);
    get_int_node (c2m_ctx, &op2, &e2, &t2,
                  t1->mode != TM_PTR ? 1 : type_size (c2m_ctx, t1->u.ptr_type));
    e = check_assign_op (c2m_ctx, r, op1, op2, e1, e2, t1, t2);
    t2 = ((struct expr *) r->attr)->type;
    *e1 = saved_expr;
    t1 = e1->type;
    assign_expr_type = create_type (c2m_ctx, NULL);
    *assign_expr_type = *e->type;
    goto assign;
    break;
  }
  case N_ADD:
  case N_SUB:
    if (NL_NEXT (NL_HEAD (r->ops)) == NULL) { /* unary */
      process_unop (c2m_ctx, r, &op1, &e1, &t1, r);
      e = create_expr (c2m_ctx, r);
      e->type->mode = TM_BASIC;
      e->type->u.basic_type = TP_INT;
      if (!arithmetic_type_p (t1)) {
        error (c2m_ctx, r->pos, "unary + or - operand should be of an arithmentic type");
      } else {
        if (e1->const_p) e->const_p = TRUE;
        if (floating_type_p (t1)) {
          e->type->u.basic_type = t1->u.basic_type;
          if (e->const_p) e->u.d_val = (r->code == N_ADD ? +e1->u.d_val : -e1->u.d_val);
        } else {
          t = integer_promotion (t1);
          e->type->u.basic_type = t.u.basic_type;
          if (e1->const_p) {
            convert_value (e1, &t);
            if (signed_integer_type_p (&t))
              e->u.i_val = (r->code == N_ADD ? +e1->u.i_val : -e1->u.i_val);
            else
              e->u.u_val = (r->code == N_ADD ? +e1->u.u_val : -e1->u.u_val);
          }
        }
      }
      break;
    }
    /* Fall through: */
  case N_AND:
  case N_OR:
  case N_XOR:
  case N_LSH:
  case N_RSH:
  case N_MUL:
  case N_DIV:
  case N_MOD:
    process_bin_ops (c2m_ctx, r, &op1, &op2, &e1, &e2, &t1, &t2, r);
    e = check_assign_op (c2m_ctx, r, op1, op2, e1, e2, t1, t2);
    break;
  case N_AND_ASSIGN:
  case N_OR_ASSIGN:
  case N_XOR_ASSIGN:
  case N_LSH_ASSIGN:
  case N_RSH_ASSIGN:
  case N_ADD_ASSIGN:
  case N_SUB_ASSIGN:
  case N_MUL_ASSIGN:
  case N_DIV_ASSIGN:
  case N_MOD_ASSIGN: {
    struct expr saved_expr;

    process_bin_ops (c2m_ctx, r, &op1, &op2, &e1, &e2, &t1, &t2, NULL);
    saved_expr = *e1;
    t1 = e1->type = adjust_type (c2m_ctx, e1->type);
    t2 = e2->type = adjust_type (c2m_ctx, e2->type);
    e = check_assign_op (c2m_ctx, r, op1, op2, e1, e2, t1, t2);
    assign_expr_type = create_type (c2m_ctx, NULL);
    *assign_expr_type = *e->type;
    t2 = ((struct expr *) r->attr)->type;
    *e1 = saved_expr;
    t1 = e1->type;
    goto assign;
    break;
  }
  case N_ASSIGN:
    process_bin_ops (c2m_ctx, r, &op1, &op2, &e1, &e2, &t1, &t2, NULL);
    t2 = e2->type = adjust_type (c2m_ctx, e2->type);
    assign_expr_type = NULL;
  assign:
    e = create_expr (c2m_ctx, r);
    if (!e1->lvalue_node) {
      error (c2m_ctx, r->pos, "lvalue required as left operand of assignment");
    }
    check_assignment_types (c2m_ctx, t1, t2, e2, r);
    *e->type = *t1;
    if ((e->type2 = assign_expr_type) != NULL) set_type_layout (c2m_ctx, assign_expr_type);
    break;
  case N_IND:
    process_bin_ops (c2m_ctx, r, &op1, &op2, &e1, &e2, &t1, &t2, r);
    if (t1->mode != TM_PTR && t1->mode != TM_ARR && (t2->mode == TM_PTR || t2->mode == TM_ARR)) {
      struct type *temp;
      node_t op;

      SWAP (t1, t2, temp);
      SWAP (e1, e2, e);
      SWAP (op1, op2, op);
      NL_REMOVE (r->ops, op1);
      NL_APPEND (r->ops, op1);
    }
    e = create_expr (c2m_ctx, r);
    e->lvalue_node = r;
    e->type->mode = TM_BASIC;
    e->type->u.basic_type = TP_INT;
    if (t1->mode != TM_PTR && t1->mode != TM_ARR) {
      error (c2m_ctx, r->pos, "subscripted value is neither array nor pointer");
    } else if (t1->mode == TM_PTR) {
      *e->type = *t1->u.ptr_type;
      if (incomplete_type_p (c2m_ctx, t1->u.ptr_type)) {
        error (c2m_ctx, r->pos, "pointer to incomplete type in array subscription");
      }
    } else {
      *e->type = *t1->u.arr_type->el_type;
      e->type->type_qual = t1->u.arr_type->ind_type_qual;
      if (incomplete_type_p (c2m_ctx, e->type)) {
        error (c2m_ctx, r->pos, "array type has incomplete element type");
      }
    }
    if (!integer_type_p (t2)) {
      error (c2m_ctx, r->pos, "array subscript is not an integer");
    }
    break;
  case N_ADDR:
    process_unop (c2m_ctx, r, &op1, &e1, &t1, r);
    e = create_expr (c2m_ctx, r);
    if (op1->code == N_DEREF) {
      node_t deref_op = NL_HEAD (op1->ops);

      *e->type = *((struct expr *) deref_op->attr)->type;
      break;
    } else if (e1->type->mode == TM_PTR && e1->type->u.ptr_type->mode == TM_FUNC) {
      *e->type = *e1->type;
      break;
    } else if (!e1->lvalue_node) {
      e->type->mode = TM_BASIC;
      e->type->u.basic_type = TP_INT;
      error (c2m_ctx, r->pos, "lvalue required as unary & operand");
      break;
    }
    if (op1->code == N_IND) {
      t2 = create_type (c2m_ctx, t1);
    } else if (op1->code == N_ID) {
      node_t decl_node = e1->lvalue_node;
      decl_t decl = decl_node->attr;

      decl->addr_p = TRUE;
      if (decl->decl_spec.register_p)
        error (c2m_ctx, r->pos, "address of register variable %s requested", op1->u.s.s);
      t2 = create_type (c2m_ctx, decl->decl_spec.type);
    } else if (e1->lvalue_node->code == N_MEMBER) {
      node_t declarator = NL_EL (e1->lvalue_node->ops, 1);
      node_t width = NL_NEXT (declarator);
      decl_t decl = e1->lvalue_node->attr;

      assert (declarator->code == N_DECL);
      if (width->code != N_IGNORE) {
        error (c2m_ctx, r->pos, "cannot take address of bit-field %s",
               NL_HEAD (declarator->ops)->u.s.s);
      }
      t2 = create_type (c2m_ctx, decl->decl_spec.type);
      if (op1->code == N_DEREF_FIELD && (e2 = NL_HEAD (op1->ops)->attr)->const_p) {
        e->const_p = TRUE;
        e->u.u_val = e2->u.u_val + decl->offset;
      }
    } else if (e1->lvalue_node->code == N_COMPOUND_LITERAL) {
      t2 = t1;
    } else {
      assert (e1->lvalue_node->code == N_STR);
      t2 = t1;
    }
    e->type->mode = TM_PTR;
    e->type->u.ptr_type = t2;
    break;
  case N_DEREF:
    process_unop (c2m_ctx, r, &op1, &e1, &t1, r);
    e = create_expr (c2m_ctx, r);
    e->type->mode = TM_BASIC;
    e->type->u.basic_type = TP_INT;
    if (t1->mode != TM_PTR) {
      error (c2m_ctx, r->pos, "invalid type argument of unary *");
    } else {
      *e->type = *t1->u.ptr_type;
      e->lvalue_node = r;
    }
    break;
  case N_FIELD:
  case N_DEREF_FIELD: {
    symbol_t sym;
    decl_t decl;
    node_t width;
    struct expr *width_expr;

    process_unop (c2m_ctx, r, &op1, &e1, &t1, r);
    e = create_expr (c2m_ctx, r);
    e->type->mode = TM_BASIC;
    e->type->u.basic_type = TP_INT;
    op2 = NL_NEXT (op1);
    assert (op2->code == N_ID);
    if (r->code == N_DEREF_FIELD && t1->mode == TM_PTR) {
      t1 = t1->u.ptr_type;
    }
    if (t1->mode != TM_STRUCT && t1->mode != TM_UNION) {
      error (c2m_ctx, r->pos, "request for member %s in something not a structure or union",
             op2->u.s.s);
    } else if (!symbol_find (c2m_ctx, S_REGULAR, op2, t1->u.tag_type, &sym)) {
      error (c2m_ctx, r->pos, "%s has no member %s", t1->mode == TM_STRUCT ? "struct" : "union",
             op2->u.s.s);
    } else {
      assert (sym.def_node->code == N_MEMBER);
      decl = sym.def_node->attr;
      *e->type = *decl->decl_spec.type;
      e->lvalue_node = sym.def_node;
      if ((width = NL_EL (sym.def_node->ops, 2))->code != N_IGNORE && e->type->mode == TM_BASIC
          && (width_expr = width->attr)->const_p
          && width_expr->u.i_val < sizeof (mir_int) * MIR_CHAR_BIT)
        e->type->u.basic_type = TP_INT;
    }
    break;
  }
  case N_COND: {
    node_t op3;
    struct expr *e3;
    struct type *t3;
    int v = 0;

    process_bin_ops (c2m_ctx, r, &op1, &op2, &e1, &e2, &t1, &t2, r);
    op3 = NL_NEXT (op2);
    check (c2m_ctx, op3, r);
    e3 = op3->attr;
    e3->type = adjust_type (c2m_ctx, e3->type);
    t3 = e3->type;
    e = create_expr (c2m_ctx, r);
    e->type->mode = TM_BASIC;
    e->type->u.basic_type = TP_INT;
    if (!scalar_type_p (t1)) {
      error (c2m_ctx, r->pos, "condition should be of a scalar type");
      break;
    }
    if (e1->const_p) {
      if (floating_type_p (t1))
        v = e1->u.d_val != 0.0;
      else if (signed_integer_type_p (t1))
        v = e1->u.i_val != 0;
      else
        v = e1->u.u_val != 0;
    }
    if (arithmetic_type_p (t2) && arithmetic_type_p (t3)) {
      t = arithmetic_conversion (t2, t3);
      *e->type = t;
      if (e1->const_p) {
        if (v && e2->const_p) {
          e->const_p = TRUE;
          convert_value (e2, &t);
          e->u = e2->u;
        } else if (!v && e3->const_p) {
          e->const_p = TRUE;
          convert_value (e3, &t);
          e->u = e3->u;
        }
      }
      break;
    }
    if (void_type_p (t2) && void_type_p (t3)) {
      e->type->u.basic_type = TP_VOID;
    } else if ((t2->mode == TM_STRUCT || t2->mode == TM_UNION)
               && (t3->mode == TM_STRUCT || t3->mode == TM_UNION)
               && t2->u.tag_type == t3->u.tag_type) {
      *e->type = *t2;
    } else if ((t2->mode == TM_PTR && null_const_p (e3, t3))
               || (t3->mode == TM_PTR && null_const_p (e2, t2))) {
      e->type->mode = TM_PTR;
      e->type->pos_node = r;
      e->type->u.ptr_type = create_type (c2m_ctx, NULL);
      e->type->u.ptr_type->pos_node = r;
      e->type->u.ptr_type->mode = TM_BASIC;
      e->type->u.ptr_type->u.basic_type = TP_VOID;
    } else if (t2->mode != TM_PTR && t3->mode != TM_PTR) {
      error (c2m_ctx, r->pos, "incompatible types in true and false parts of cond-expression");
      break;
    } else if (compatible_types_p (t2, t3, TRUE)) {
      t = composite_type (c2m_ctx, t2->u.ptr_type, t3->u.ptr_type);
      e->type->mode = TM_PTR;
      e->type->pos_node = r;
      e->type->u.ptr_type = create_type (c2m_ctx, &t);
      e->type->u.ptr_type->type_qual
        = type_qual_union (&t2->u.ptr_type->type_qual, &t3->u.ptr_type->type_qual);
      if ((t2->u.ptr_type->type_qual.atomic_p || t3->u.ptr_type->type_qual.atomic_p)
          && !null_const_p (e2, t2) && !null_const_p (e3, t3)) {
        error (c2m_ctx, r->pos, "pointer to atomic type in true or false parts of cond-expression");
      }
    } else if (void_ptr_p (t2) || void_ptr_p (t3)) {
      e->type->mode = TM_PTR;
      e->type->pos_node = r;
      e->type->u.ptr_type = create_type (c2m_ctx, NULL);
      e->type->u.ptr_type->pos_node = r;
      if (null_const_p (e2, t2)) {
        e->type->u.ptr_type = e2->type->u.ptr_type;
      } else if (null_const_p (e3, t3)) {
        e->type->u.ptr_type = e3->type->u.ptr_type;
      } else {
        if (t2->u.ptr_type->type_qual.atomic_p || t3->u.ptr_type->type_qual.atomic_p) {
          error (c2m_ctx, r->pos,
                 "pointer to atomic type in true or false parts of cond-expression");
        }
        e->type->u.ptr_type->mode = TM_BASIC;
        e->type->u.ptr_type->u.basic_type = TP_VOID;
      }
      e->type->u.ptr_type->type_qual
        = type_qual_union (&t2->u.ptr_type->type_qual, &t3->u.ptr_type->type_qual);
    } else {
      error (c2m_ctx, r->pos,
             "incompatible pointer types in true and false parts of cond-expression");
      break;
    }
    if (e1->const_p) {
      if (v && e2->const_p) {
        e->const_p = TRUE;
        e->u = e2->u;
      } else if (!v && e3->const_p) {
        e->const_p = TRUE;
        e->u = e3->u;
      }
    }
    break;
  }
  case N_ALIGNOF:
  case N_SIZEOF: {
    struct decl_spec *decl_spec;

    op1 = NL_HEAD (r->ops);
    check (c2m_ctx, op1, r);
    e = create_expr (c2m_ctx, r);
    *e->type = get_ptr_int_type (FALSE);
    if (r->code == N_ALIGNOF && op1->code == N_IGNORE) {
      error (c2m_ctx, r->pos, "_Alignof of non-type");
      break;
    }
    assert (op1->code == N_TYPE);
    decl_spec = op1->attr;
    if (incomplete_type_p (c2m_ctx, decl_spec->type)) {
      error (c2m_ctx, r->pos, "%s of incomplete type requested",
             r->code == N_ALIGNOF ? "_Alignof" : "sizeof");
    } else if (decl_spec->type->mode == TM_FUNC) {
      error (c2m_ctx, r->pos, "%s of function type requested",
             r->code == N_ALIGNOF ? "_Alignof" : "sizeof");
    } else {
      e->const_p = TRUE;
      e->u.i_val = (r->code == N_SIZEOF ? type_size (c2m_ctx, decl_spec->type)
                                        : type_align (decl_spec->type));
    }
    break;
  }
  case N_EXPR_SIZEOF:
    process_unop (c2m_ctx, r, &op1, &e1, &t1, r);
    e = create_expr (c2m_ctx, r);
    e->type->mode = TM_BASIC;
    e->type->u.basic_type = TP_INT;  // ???
    if (incomplete_type_p (c2m_ctx, t1)) {
      error (c2m_ctx, r->pos, "sizeof of incomplete type requested");
    } else if (t1->mode == TM_FUNC) {
      error (c2m_ctx, r->pos, "sizeof of function type requested");
    } else if (e1->lvalue_node && e1->lvalue_node->code == N_MEMBER) {
      node_t declarator = NL_EL (e1->lvalue_node->ops, 1);
      node_t width = NL_NEXT (declarator);

      assert (declarator->code == N_DECL);
      if (width->code != N_IGNORE) {
        error (c2m_ctx, r->pos, "sizeof applied to a bit-field %s",
               NL_HEAD (declarator->ops)->u.s.s);
      }
    }
    e->const_p = TRUE;
    e->u.i_val = type_size (c2m_ctx, t1);
    break;
  case N_CAST: {
    struct decl_spec *decl_spec;
    int void_p;

    process_type_bin_ops (c2m_ctx, r, &op1, &op2, &e2, &t2, r);
    e = create_expr (c2m_ctx, r);
    assert (op1->code == N_TYPE);
    decl_spec = op1->attr;
    *e->type = *decl_spec->type;
    void_p = void_type_p (decl_spec->type);
    if (!void_p && !scalar_type_p (decl_spec->type)) {
      error (c2m_ctx, r->pos, "conversion to non-scalar type requested");
    } else if (!scalar_type_p (t2)) {
      error (c2m_ctx, r->pos, "conversion of non-scalar value requested");
    } else if (t2->mode == TM_PTR && floating_type_p (decl_spec->type)) {
      error (c2m_ctx, r->pos, "conversion of a pointer to floating value requested");
    } else if (decl_spec->type->mode == TM_PTR && floating_type_p (t2)) {
      error (c2m_ctx, r->pos, "conversion of floating point value to a pointer requested");
    } else if (e2->const_p && !void_p) {
      e->const_p = TRUE;
      cast_value (e, e2, decl_spec->type);
    }
    break;
  }
  case N_COMPOUND_LITERAL: {
    node_t list, n;
    decl_t decl;
    int n_spec_index, addr_p;

    op1 = NL_HEAD (r->ops);
    list = NL_NEXT (op1);
    assert (op1->code == N_TYPE && list->code == N_LIST);
    check (c2m_ctx, op1, r);
    decl = op1->attr;
    t1 = decl->decl_spec.type;
    check (c2m_ctx, list, r);
    decl->addr_p = TRUE;
    if (incomplete_type_p (c2m_ctx, t1)
        && (t1->mode != TM_ARR || t1->u.arr_type->size->code != N_IGNORE
            || incomplete_type_p (c2m_ctx, t1->u.arr_type->el_type))) {
      error (c2m_ctx, r->pos, "compound literal of incomplete type");
      break;
    }
    for (n_spec_index = VARR_LENGTH (node_t, context_stack) - 1;
         n_spec_index >= 0 && (n = VARR_GET (node_t, context_stack, n_spec_index)) != NULL
         && n->code != N_SPEC_DECL;
         n_spec_index--)
      ;
    if (n_spec_index < VARR_LENGTH (node_t, context_stack) - 1
        && (n_spec_index < 0
            || !get_compound_literal (VARR_GET (node_t, context_stack, n_spec_index + 1), &addr_p)
            || addr_p))
      check_initializer (c2m_ctx, NULL, &t1, list,
                         curr_scope == top_scope || decl->decl_spec.static_p
                           || decl->decl_spec.thread_local_p,
                         FALSE);
    decl->decl_spec.type = t1;
    e = create_expr (c2m_ctx, r);
    e->lvalue_node = r;
    *e->type = *t1;
    if (curr_scope != top_scope) VARR_PUSH (decl_t, func_decls_for_allocation, decl);
    break;
  }
  case N_CALL: {
    struct func_type *func_type;
    struct type *ret_type;
    node_t list, spec_list, decl, param_list, start_param, param, arg_list, arg;
    node_t saved_scope = curr_scope;
    struct decl_spec *decl_spec;
    mir_size_t saved_call_arg_area_offset_before_args;
    struct type res_type;
    int builtin_call_p, alloca_p, va_arg_p = FALSE, va_start_p = FALSE;

    op1 = NL_HEAD (r->ops);
    alloca_p = op1->code == N_ID && strcmp (op1->u.s.s, ALLOCA) == 0;
    if (op1->code == N_ID && find_def (c2m_ctx, S_REGULAR, op1, curr_scope, NULL) == NULL) {
      va_arg_p = strcmp (op1->u.s.s, BUILTIN_VA_ARG) == 0;
      va_start_p = strcmp (op1->u.s.s, BUILTIN_VA_START) == 0;
      if (!va_arg_p && !va_start_p && !alloca_p) {
        /* N_SPEC_DECL (N_SHARE (N_LIST (N_INT)), N_DECL (N_ID, N_FUNC (N_LIST)), N_IGNORE) */
        spec_list = new_node (c2m_ctx, N_LIST);
        op_append (spec_list, new_node (c2m_ctx, N_INT));
        list = new_node (c2m_ctx, N_LIST);
        op_append (list, new_node1 (c2m_ctx, N_FUNC, new_node (c2m_ctx, N_LIST)));
        decl
          = new_pos_node3 (c2m_ctx, N_SPEC_DECL, op1->pos, new_node1 (c2m_ctx, N_SHARE, spec_list),
                           new_node2 (c2m_ctx, N_DECL, copy_node (c2m_ctx, op1), list),
                           new_node (c2m_ctx, N_IGNORE));
        curr_scope = top_scope;
        check (c2m_ctx, decl, NULL);
        curr_scope = saved_scope;
        assert (top_scope->code == N_MODULE);
        list = NL_HEAD (top_scope->ops);
        assert (list->code == N_LIST);
        op_prepend (list, decl);
      }
    }
    builtin_call_p = alloca_p || va_arg_p || va_start_p;
    if (!builtin_call_p) VARR_PUSH (node_t, call_nodes, r);
    arg_list = NL_NEXT (op1);
    if (builtin_call_p) {
      for (arg = NL_HEAD (arg_list->ops); arg != NULL; arg = NL_NEXT (arg)) check (c2m_ctx, arg, r);
      init_type (&res_type);
      if (alloca_p) {
        res_type.mode = TM_PTR;
        res_type.u.ptr_type = &VOID_TYPE;
      } else {
        res_type.mode = TM_BASIC;
        res_type.u.basic_type = va_arg_p ? TP_INT : TP_VOID;
      }
      ret_type = &res_type;
      if (va_start_p && NL_LENGTH (arg_list->ops) != 1) {
        error (c2m_ctx, op1->pos, "wrong number of arguments in %s call", BUILTIN_VA_START);
      } else if (alloca_p && NL_LENGTH (arg_list->ops) != 1) {
        error (c2m_ctx, op1->pos, "wrong number of arguments in %s call", ALLOCA);
      } else if (va_arg_p && NL_LENGTH (arg_list->ops) != 2) {
        error (c2m_ctx, op1->pos, "wrong number of arguments in %s call", BUILTIN_VA_ARG);
      } else {
        /* first argument type ??? */
        if (va_arg_p) {
          arg = NL_EL (arg_list->ops, 1);
          e2 = arg->attr;
          t2 = e2->type;
          if (t2->mode != TM_PTR)
            error (c2m_ctx, arg->pos, "wrong type of 2nd argument of %s call", BUILTIN_VA_ARG);
          else
            ret_type = t2->u.ptr_type;
        }
      }
    } else {
      check (c2m_ctx, op1, r);
      e1 = op1->attr;
      t1 = e1->type;
      if (t1->mode != TM_PTR || (t1 = t1->u.ptr_type)->mode != TM_FUNC) {
        error (c2m_ctx, r->pos, "called object is not a function or function pointer");
        break;
      }
      func_type = t1->u.func_type;
      ret_type = func_type->ret_type;
    }
    e = create_expr (c2m_ctx, r);
    *e->type = *ret_type;
    e->builtin_call_p = builtin_call_p;
    if ((ret_type->mode != TM_BASIC || ret_type->u.basic_type != TP_VOID)
        && incomplete_type_p (c2m_ctx, ret_type)) {
      error (c2m_ctx, r->pos, "function return type is incomplete");
    }
    if (ret_type->mode == TM_STRUCT || ret_type->mode == TM_UNION) {
      set_type_layout (c2m_ctx, ret_type);
      if (!builtin_call_p) update_call_arg_area_offset (c2m_ctx, ret_type, TRUE);
    }
    if (builtin_call_p) break;
    param_list = func_type->param_list;
    param = start_param = NL_HEAD (param_list->ops);
    if (void_param_p (start_param)) { /* f(void) */
      if ((arg = NL_HEAD (arg_list->ops)) != NULL) error (c2m_ctx, arg->pos, "too many arguments");
      break;
    }
    saved_call_arg_area_offset_before_args = curr_call_arg_area_offset;
    for (arg = NL_HEAD (arg_list->ops); arg != NULL; arg = NL_NEXT (arg)) {
      check (c2m_ctx, arg, r);
      e2 = arg->attr;
      if (start_param == NULL || start_param->code == N_ID) continue; /* no params or ident list */
      if (param == NULL) {
        if (!func_type->dots_p) error (c2m_ctx, arg->pos, "too many arguments");
        start_param = NULL; /* ignore the rest args */
        continue;
      }
      assert (param->code == N_SPEC_DECL || param->code == N_TYPE);
      decl_spec = get_param_decl_spec (param);
      check_assignment_types (c2m_ctx, decl_spec->type, NULL, e2, r);
      param = NL_NEXT (param);
    }
    curr_call_arg_area_offset = saved_call_arg_area_offset_before_args;
    if (param != NULL) error (c2m_ctx, r->pos, "too few arguments");
    break;
  }
  case N_GENERIC: {
    node_t list, ga, ga2, type_name, type_name2, expr;
    node_t default_case = NULL, ga_case = NULL;
    struct decl_spec *decl_spec;

    op1 = NL_HEAD (r->ops);
    check (c2m_ctx, op1, r);
    e1 = op1->attr;
    t = *e1->type;
    if (integer_type_p (&t)) t = integer_promotion (&t);
    list = NL_NEXT (op1);
    for (ga = NL_HEAD (list->ops); ga != NULL; ga = NL_NEXT (ga)) {
      assert (ga->code == N_GENERIC_ASSOC);
      type_name = NL_HEAD (ga->ops);
      expr = NL_NEXT (type_name);
      check (c2m_ctx, type_name, r);
      check (c2m_ctx, expr, r);
      if (type_name->code == N_IGNORE) {
        if (default_case) error (c2m_ctx, ga->pos, "duplicate default case in _Generic");
        default_case = ga;
        continue;
      }
      assert (type_name->code == N_TYPE);
      decl_spec = type_name->attr;
      if (incomplete_type_p (c2m_ctx, decl_spec->type)) {
        error (c2m_ctx, ga->pos, "_Generic case has incomplete type");
      } else if (compatible_types_p (&t, decl_spec->type, TRUE)) {
        if (ga_case)
          error (c2m_ctx, ga_case->pos,
                 "_Generic expr type is compatible with more than one generic association type");
        ga_case = ga;
      } else {
        for (ga2 = NL_HEAD (list->ops); ga2 != ga; ga2 = NL_NEXT (ga2)) {
          type_name2 = NL_HEAD (ga2->ops);
          if (type_name2->code != N_IGNORE
              && !(incomplete_type_p (c2m_ctx, t2 = ((struct decl_spec *) type_name2->attr)->type))
              && compatible_types_p (t2, decl_spec->type, TRUE)) {
            error (c2m_ctx, ga->pos, "two or more compatible generic association types");
            break;
          }
        }
      }
    }
    e = create_expr (c2m_ctx, r);
    if (default_case == NULL && ga_case == NULL) {
      error (c2m_ctx, r->pos, "expression type is not compatible with generic association");
    } else { /* make compatible association a list head  */
      if (ga_case == NULL) ga_case = default_case;
      NL_REMOVE (list->ops, ga_case);
      NL_PREPEND (list->ops, ga_case);
      t2 = e->type;
      *e = *(struct expr *) NL_EL (ga_case->ops, 1)->attr;
      *t2 = *e->type;
      e->type = t2;
    }
    break;
  }
  case N_SPEC_DECL: {
    node_t specs = NL_HEAD (r->ops);
    node_t declarator = NL_NEXT (specs);
    node_t initializer = NL_NEXT (declarator);
    node_t unshared_specs = specs->code != N_SHARE ? specs : NL_HEAD (specs->ops);
    struct decl_spec decl_spec = check_decl_spec (c2m_ctx, unshared_specs, r);

    if (declarator->code != N_IGNORE) {
      create_decl (c2m_ctx, curr_scope, r, decl_spec, NULL, initializer,
                   context != NULL && context->code == N_FUNC);
    } else if (decl_spec.type->mode == TM_STRUCT || decl_spec.type->mode == TM_UNION) {
      if (NL_HEAD (decl_spec.type->u.tag_type->ops)->code != N_ID)
        error (c2m_ctx, r->pos, "unnamed struct/union with no instances");
    } else if (decl_spec.type->mode != TM_ENUM) {
      error (c2m_ctx, r->pos, "useless declaration");
    }
    /* We have at least one enum constant according to the syntax. */
    break;
  }
  case N_ST_ASSERT: {
    int ok_p;

    op1 = NL_HEAD (r->ops);
    check (c2m_ctx, op1, r);
    e1 = op1->attr;
    t1 = e1->type;
    if (!e1->const_p) {
      error (c2m_ctx, r->pos, "expression in static assertion is not constant");
    } else if (!integer_type_p (t1)) {
      error (c2m_ctx, r->pos, "expression in static assertion is not an integer");
    } else {
      if (signed_integer_type_p (t1))
        ok_p = e1->u.i_val != 0;
      else
        ok_p = e1->u.u_val != 0;
      if (!ok_p) {
        assert (NL_NEXT (op1) != NULL && NL_NEXT (op1)->code == N_STR);
        error (c2m_ctx, r->pos, "static assertion failed: \"%s\"", NL_NEXT (op1)->u.s.s);  // ???
      }
    }
    break;
  }
  case N_MEMBER: {
    struct type *type;
    node_t specs = NL_HEAD (r->ops);
    node_t declarator = NL_NEXT (specs);
    node_t const_expr = NL_NEXT (declarator);
    node_t unshared_specs = specs->code != N_SHARE ? specs : NL_HEAD (specs->ops);
    struct decl_spec decl_spec = check_decl_spec (c2m_ctx, unshared_specs, r);

    create_decl (c2m_ctx, curr_scope, r, decl_spec, const_expr, NULL, FALSE);
    type = ((decl_t) r->attr)->decl_spec.type;
    if (const_expr->code != N_IGNORE) {
      struct expr *cexpr;

      check (c2m_ctx, const_expr, r);
      cexpr = const_expr->attr;
      if (cexpr != NULL) {
        if (type->type_qual.atomic_p) error (c2m_ctx, const_expr->pos, "bit field with _Atomic");
        if (!cexpr->const_p) {
          error (c2m_ctx, const_expr->pos, "bit field is not a constant expr");
        } else if (!integer_type_p (type)
                   && (type->mode != TM_BASIC || type->u.basic_type != TP_BOOL)) {
          error (c2m_ctx, const_expr->pos,
                 "bit field type should be _Bool, a signed integer, or an unsigned integer type");
        } else if (!integer_type_p (cexpr->type)
                   && (cexpr->type->mode != TM_BASIC || cexpr->type->u.basic_type != TP_BOOL)) {
          error (c2m_ctx, const_expr->pos, "bit field width is not of an integer type");
        } else if (signed_integer_type_p (cexpr->type) && cexpr->u.i_val < 0) {
          error (c2m_ctx, const_expr->pos, "bit field width is negative");
        } else if (cexpr->u.i_val == 0 && declarator->code == N_DECL) {
          error (c2m_ctx, const_expr->pos, "zero bit field width for %s",
                 NL_HEAD (declarator->ops)->u.s.s);
        } else if ((!signed_integer_type_p (cexpr->type) && cexpr->u.u_val > int_bit_size (type))
                   || (signed_integer_type_p (cexpr->type)
                       && cexpr->u.i_val > int_bit_size (type))) {
          error (c2m_ctx, const_expr->pos, "bit field width exceeds its type");
        }
      }
    }
    if (declarator->code == N_IGNORE) {
      if (((decl_spec.type->mode != TM_STRUCT && decl_spec.type->mode != TM_UNION)
           || NL_HEAD (decl_spec.type->u.tag_type->ops)->code != N_IGNORE)
          && const_expr->code == N_IGNORE)
        error (c2m_ctx, r->pos, "no declarator in struct or union declaration");
    } else {
      node_t id = NL_HEAD (declarator->ops);

      if (type->mode == TM_FUNC) {
        error (c2m_ctx, id->pos, "field %s is declared as a function", id->u.s.s);
      } else if (incomplete_type_p (c2m_ctx, type)) {
        /* el_type is checked on completness in N_ARR */
        if (type->mode != TM_ARR || type->u.arr_type->size->code != N_IGNORE)
          error (c2m_ctx, id->pos, "field %s has incomplete type", id->u.s.s);
      }
    }
    break;
  }
  case N_INIT: {
    node_t des_list = NL_HEAD (r->ops), initializer = NL_NEXT (des_list);

    check (c2m_ctx, des_list, r);
    check (c2m_ctx, initializer, r);
    break;
  }
  case N_FUNC_DEF: {
    node_t specs = NL_HEAD (r->ops);
    node_t declarator = NL_NEXT (specs);
    node_t declarations = NL_NEXT (declarator);
    node_t block = NL_NEXT (declarations);
    node_t id = NL_HEAD (declarator->ops);
    struct decl_spec decl_spec = check_decl_spec (c2m_ctx, specs, r);
    node_t decl_node, p, next_p, param_list, param_id, param_declarator, func;
    symbol_t sym;
    struct node_scope *ns;

    if (strcmp (id->u.s.s, ALLOCA) == 0 || strcmp (id->u.s.s, BUILTIN_VA_START) == 0
        || strcmp (id->u.s.s, BUILTIN_VA_ARG) == 0) {
      error (c2m_ctx, id->pos, "%s is a builtin function", id->u.s.s);
      break;
    }
    curr_func_scope_num = 0;
    create_node_scope (c2m_ctx, block);
    func_block_scope = curr_scope;
    curr_func_def = r;
    curr_switch = curr_loop = curr_loop_switch = NULL;
    curr_call_arg_area_offset = 0;
    VARR_TRUNC (decl_t, func_decls_for_allocation, 0);
    create_decl (c2m_ctx, top_scope, r, decl_spec, NULL, NULL, FALSE);
    curr_scope = func_block_scope;
    check (c2m_ctx, declarations, r);
    /* Process parameter identifier list:  */
    assert (declarator->code == N_DECL);
    func = NL_HEAD (NL_EL (declarator->ops, 1)->ops);
    assert (func != NULL && func->code == N_FUNC);
    param_list = NL_HEAD (func->ops);
    for (p = NL_HEAD (param_list->ops); p != NULL; p = next_p) {
      next_p = NL_NEXT (p);
      if (p->code != N_ID) break;
      NL_REMOVE (param_list->ops, p);
      if (!symbol_find (c2m_ctx, S_REGULAR, p, curr_scope, &sym)) {
        if (options->pedantic_p) {
          error (c2m_ctx, p->pos, "parameter %s has no type", p->u.s.s);
        } else {
          warning (c2m_ctx, p->pos, "type of parameter %s defaults to int", p->u.s.s);
          decl_node = new_pos_node3 (c2m_ctx, N_SPEC_DECL, p->pos,
                                     new_node1 (c2m_ctx, N_SHARE,
                                                new_node1 (c2m_ctx, N_LIST,
                                                           new_pos_node (c2m_ctx, N_INT, p->pos))),
                                     new_pos_node2 (c2m_ctx, N_DECL, p->pos,
                                                    new_str_node (c2m_ctx, N_ID, p->u.s, p->pos),
                                                    new_node (c2m_ctx, N_LIST)),
                                     new_node (c2m_ctx, N_IGNORE));
          NL_APPEND (param_list->ops, decl_node);
          check (c2m_ctx, decl_node, r);
        }
      } else {
        struct decl_spec decl_spec, *decl_spec_ptr;

        decl_node = sym.def_node;
        assert (decl_node->code == N_SPEC_DECL);
        NL_REMOVE (declarations->ops, decl_node);
        NL_APPEND (param_list->ops, decl_node);
        param_declarator = NL_EL (decl_node->ops, 1);
        assert (param_declarator->code == N_DECL);
        param_id = NL_HEAD (param_declarator->ops);
        if (NL_NEXT (param_declarator)->code != N_IGNORE) {
          error (c2m_ctx, p->pos, "initialized parameter %s", param_id->u.s.s);
        }
        decl_spec_ptr = &((decl_t) decl_node->attr)->decl_spec;
        adjust_param_type (c2m_ctx, &decl_spec_ptr->type);
        decl_spec = *decl_spec_ptr;
        if (decl_spec.typedef_p || decl_spec.extern_p || decl_spec.static_p || decl_spec.auto_p
            || decl_spec.thread_local_p) {
          error (c2m_ctx, param_id->pos, "storage specifier in a function parameter %s",
                 param_id->u.s.s);
        }
      }
    }
    /* Process the rest declarations: */
    for (p = NL_HEAD (declarations->ops); p != NULL; p = NL_NEXT (p)) {
      if (p->code == N_ST_ASSERT) continue;
      assert (p->code == N_SPEC_DECL);
      param_declarator = NL_EL (p->ops, 1);
      assert (param_declarator->code == N_DECL);
      param_id = NL_HEAD (param_declarator->ops);
      assert (param_id->code == N_ID);
      error (c2m_ctx, param_id->pos, "declaration for parameter %s but no such parameter",
             param_id->u.s.s);
    }
    add__func__def (c2m_ctx, block, id->u.s);
    check (c2m_ctx, block, r);
    /* Process all gotos: */
    for (size_t i = 0; i < VARR_LENGTH (node_t, gotos); i++) {
      symbol_t sym;
      node_t n = VARR_GET (node_t, gotos, i);
      node_t id = NL_NEXT (NL_HEAD (n->ops));

      if (!symbol_find (c2m_ctx, S_LABEL, id, func_block_scope, &sym)) {
        error (c2m_ctx, id->pos, "undefined label %s", id->u.s.s);
      } else {
        n->attr = sym.def_node;
      }
    }
    VARR_TRUNC (node_t, gotos, 0);
    assert (curr_scope == top_scope); /* set up in the block */
    func_block_scope = top_scope;
    process_func_decls_for_allocation (c2m_ctx);
    /* Add call arg area */
    ns = block->attr;
    ns->size = round_size (ns->size, MAX_ALIGNMENT);
    ns->size += ns->call_arg_area_size;
    break;
  }
  case N_TYPE: {
    struct type *type;
    node_t specs = NL_HEAD (r->ops);
    node_t abstract_declarator = NL_NEXT (specs);
    struct decl_spec decl_spec = check_decl_spec (c2m_ctx, specs, r); /* only spec_qual_list here */

    type = check_declarator (c2m_ctx, abstract_declarator, FALSE);
    assert (NL_HEAD (abstract_declarator->ops)->code == N_IGNORE);
    decl_spec.type = append_type (type, decl_spec.type);
    if (context && context->code == N_COMPOUND_LITERAL) {
      r->attr = reg_malloc (c2m_ctx, sizeof (struct decl));
      init_decl (c2m_ctx, r->attr);
      ((struct decl *) r->attr)->decl_spec = decl_spec;
      check_type (c2m_ctx, decl_spec.type, 0, FALSE);
      set_type_layout (c2m_ctx, decl_spec.type);
      check_decl_align (c2m_ctx, &((struct decl *) r->attr)->decl_spec);
    } else {
      r->attr = reg_malloc (c2m_ctx, sizeof (struct decl_spec));
      *((struct decl_spec *) r->attr) = decl_spec;
      check_type (c2m_ctx, decl_spec.type, 0, FALSE);
      set_type_layout (c2m_ctx, decl_spec.type);
      check_decl_align (c2m_ctx, r->attr);
    }
    break;
  }
  case N_BLOCK:
    check_labels (c2m_ctx, NL_HEAD (r->ops), r);
    if (curr_scope != r)
      create_node_scope (c2m_ctx, r); /* it happens if it is the top func block */
    check (c2m_ctx, NL_EL (r->ops, 1), r);
    finish_scope (c2m_ctx);
    break;
  case N_MODULE:
    create_node_scope (c2m_ctx, r);
    top_scope = curr_scope;
    check (c2m_ctx, NL_HEAD (r->ops), r);
    finish_scope (c2m_ctx);
    break;
  case N_IF: {
    node_t labels = NL_HEAD (r->ops);
    node_t expr = NL_NEXT (labels);
    node_t if_stmt = NL_NEXT (expr);
    node_t else_stmt = NL_NEXT (if_stmt);

    check_labels (c2m_ctx, labels, r);
    check (c2m_ctx, expr, r);
    e1 = expr->attr;
    t1 = e1->type;
    if (!scalar_type_p (t1)) {
      error (c2m_ctx, expr->pos, "if-expr should be of a scalar type");
    }
    check (c2m_ctx, if_stmt, r);
    check (c2m_ctx, else_stmt, r);
    break;
  }
  case N_SWITCH: {
    node_t saved_switch = curr_switch;
    node_t saved_loop_switch = curr_loop_switch;
    node_t labels = NL_HEAD (r->ops);
    node_t expr = NL_NEXT (labels);
    node_t stmt = NL_NEXT (expr);
    struct type t, *type;
    struct switch_attr *switch_attr;
    case_t el;
    node_t case_expr, case_expr2, another_case_expr, another_case_expr2;
    struct expr *e, *e2, *another_e, *another_e2;
    int signed_p, skip_range_p;

    check_labels (c2m_ctx, labels, r);
    check (c2m_ctx, expr, r);
    type = ((struct expr *) expr->attr)->type;
    if (!integer_type_p (type)) {
      init_type (&t);
      t.mode = TM_BASIC;
      t.u.basic_type = TP_INT;
      error (c2m_ctx, expr->pos, "switch-expr is of non-integer type");
    } else {
      t = integer_promotion (type);
    }
    signed_p = signed_integer_type_p (type);
    curr_switch = curr_loop_switch = r;
    switch_attr = curr_switch->attr = reg_malloc (c2m_ctx, sizeof (struct switch_attr));
    switch_attr->type = t;
    switch_attr->ranges_p = FALSE;
    switch_attr->min_val_case = switch_attr->max_val_case = NULL;
    DLIST_INIT (case_t, ((struct switch_attr *) curr_switch->attr)->case_labels);
    check (c2m_ctx, stmt, r);
    for (case_t c = DLIST_HEAD (case_t, switch_attr->case_labels); c != NULL;
         c = DLIST_NEXT (case_t, c)) { /* process simple cases */
      if (c->case_node->code == N_DEFAULT || NL_EL (c->case_node->ops, 1) != NULL) continue;
      if (HTAB_DO (case_t, case_tab, c, HTAB_FIND, el)) {
        error (c2m_ctx, c->case_node->pos, "duplicate case value");
        continue;
      }
      HTAB_DO (case_t, case_tab, c, HTAB_INSERT, el);
      if (switch_attr->min_val_case == NULL) {
        switch_attr->min_val_case = switch_attr->max_val_case = c;
        continue;
      }
      e = NL_HEAD (c->case_node->ops)->attr;
      e2 = NL_HEAD (switch_attr->min_val_case->case_node->ops)->attr;
      if (signed_p ? e->u.i_val < e2->u.i_val : e->u.u_val < e2->u.u_val)
        switch_attr->min_val_case = c;
      e2 = NL_HEAD (switch_attr->max_val_case->case_node->ops)->attr;
      if (signed_p ? e->u.i_val > e2->u.i_val : e->u.u_val > e2->u.u_val)
        switch_attr->max_val_case = c;
    }
    HTAB_CLEAR (case_t, case_tab);
    /* Check range cases against *all* simple cases or range cases *before* it. */
    for (case_t c = DLIST_HEAD (case_t, switch_attr->case_labels); c != NULL;
         c = DLIST_NEXT (case_t, c)) {
      if (c->case_node->code == N_DEFAULT || (case_expr2 = NL_EL (c->case_node->ops, 1)) == NULL)
        continue;
      switch_attr->ranges_p = TRUE;
      case_expr = NL_HEAD (c->case_node->ops);
      e = case_expr->attr;
      e2 = case_expr2->attr;
      skip_range_p = FALSE;
      for (case_t c2 = DLIST_HEAD (case_t, switch_attr->case_labels); c2 != NULL;
           c2 = DLIST_NEXT (case_t, c2)) {
        if (c2->case_node->code == N_DEFAULT) continue;
        if (c2 == c) {
          skip_range_p = TRUE;
          continue;
        }
        another_case_expr = NL_HEAD (c2->case_node->ops);
        another_case_expr2 = NL_EL (c2->case_node->ops, 1);
        if (skip_range_p && another_case_expr2 != NULL) continue;
        another_e = another_case_expr->attr;
        assert (another_e->const_p && integer_type_p (another_e->type));
        if (another_case_expr2 == NULL) {
          if ((signed_p && e->u.i_val <= another_e->u.i_val && another_e->u.i_val <= e2->u.i_val)
              || (!signed_p && e->u.u_val <= another_e->u.u_val
                  && another_e->u.u_val <= e2->u.u_val)) {
            error (c2m_ctx, c->case_node->pos, "duplicate value in a range case");
            break;
          }
        } else {
          another_e2 = another_case_expr2->attr;
          assert (another_e2->const_p && integer_type_p (another_e2->type));
          if ((signed_p
               && (e->u.i_val <= another_e->u.i_val && another_e->u.i_val <= e2->u.i_val
                   || e->u.i_val <= another_e2->u.i_val && another_e2->u.i_val <= e2->u.i_val))
              || (!signed_p
                  && (e->u.u_val <= another_e->u.u_val && another_e->u.u_val <= e2->u.u_val
                      || e->u.u_val <= another_e2->u.u_val
                           && another_e2->u.u_val <= e2->u.u_val))) {
            error (c2m_ctx, c->case_node->pos, "duplicate value in a range case");
            break;
          }
        }
      }
    }
    curr_switch = saved_switch;
    curr_loop_switch = saved_loop_switch;
    break;
  }
  case N_DO:
  case N_WHILE: {
    node_t labels = NL_HEAD (r->ops);
    node_t expr = NL_NEXT (labels);
    node_t stmt = NL_NEXT (expr);
    node_t saved_loop = curr_loop;
    node_t saved_loop_switch = curr_loop_switch;

    check_labels (c2m_ctx, labels, r);
    check (c2m_ctx, expr, r);
    e1 = expr->attr;
    t1 = e1->type;
    if (!scalar_type_p (t1)) {
      error (c2m_ctx, expr->pos, "while-expr should be of a scalar type");
    }
    curr_loop = curr_loop_switch = r;
    check (c2m_ctx, stmt, r);
    curr_loop_switch = saved_loop_switch;
    curr_loop = saved_loop;
    break;
  }
  case N_FOR: {
    node_t labels = NL_HEAD (r->ops);
    node_t init = NL_NEXT (labels);
    node_t cond = NL_NEXT (init);
    node_t iter = NL_NEXT (cond);
    node_t stmt = NL_NEXT (iter);
    decl_t decl;
    node_t saved_loop = curr_loop;
    node_t saved_loop_switch = curr_loop_switch;

    check_labels (c2m_ctx, labels, r);
    create_node_scope (c2m_ctx, r);
    curr_loop = curr_loop_switch = r;
    check (c2m_ctx, init, r);
    if (init->code == N_LIST) {
      for (node_t spec_decl = NL_HEAD (init->ops); spec_decl != NULL;
           spec_decl = NL_NEXT (spec_decl)) {
        assert (spec_decl->code == N_SPEC_DECL);
        decl = spec_decl->attr;
        if (decl->decl_spec.typedef_p || decl->decl_spec.extern_p || decl->decl_spec.static_p
            || decl->decl_spec.thread_local_p) {
          error (c2m_ctx, spec_decl->pos,
                 "wrong storage specifier of for-loop initial declaration");
          break;
        }
      }
    }
    if (cond->code != N_IGNORE) { /* non-empty condition: */
      check (c2m_ctx, cond, r);
      e1 = cond->attr;
      t1 = e1->type;
      if (!scalar_type_p (t1)) {
        error (c2m_ctx, cond->pos, "for-condition should be of a scalar type");
      }
    }
    check (c2m_ctx, iter, r);
    check (c2m_ctx, stmt, r);
    finish_scope (c2m_ctx);
    curr_loop_switch = saved_loop_switch;
    curr_loop = saved_loop;
    break;
  }
  case N_GOTO: {
    node_t labels = NL_HEAD (r->ops);

    check_labels (c2m_ctx, labels, r);
    VARR_PUSH (node_t, gotos, r);
    break;
  }
  case N_CONTINUE:
  case N_BREAK: {
    node_t labels = NL_HEAD (r->ops);

    if (r->code == N_BREAK && curr_loop_switch == NULL) {
      error (c2m_ctx, r->pos, "break statement not within loop or switch");
    } else if (r->code == N_CONTINUE && curr_loop == NULL) {
      error (c2m_ctx, r->pos, "continue statement not within a loop");
    }
    check_labels (c2m_ctx, labels, r);
    break;
  }
  case N_RETURN: {
    node_t labels = NL_HEAD (r->ops);
    node_t expr = NL_NEXT (labels);
    decl_t decl = curr_func_def->attr;
    struct type *ret_type, *type = decl->decl_spec.type;

    assert (type->mode == TM_FUNC);
    check_labels (c2m_ctx, labels, r);
    check (c2m_ctx, expr, r);
    ret_type = type->u.func_type->ret_type;
    if (expr->code != N_IGNORE && void_type_p (ret_type)) {
      error (c2m_ctx, r->pos, "return with a value in function returning void");
    } else if (expr->code == N_IGNORE
               && (ret_type->mode != TM_BASIC || ret_type->u.basic_type != TP_VOID)) {
      error (c2m_ctx, r->pos, "return with no value in function returning non-void");
    } else if (expr->code != N_IGNORE) {
      check_assignment_types (c2m_ctx, ret_type, NULL, expr->attr, r);
    }
    break;
  }
  case N_EXPR: {
    node_t labels = NL_HEAD (r->ops);
    node_t expr = NL_NEXT (labels);

    check_labels (c2m_ctx, labels, r);
    check (c2m_ctx, expr, r);
    break;
  }
  default: abort ();
  }
  if (e != NULL) {
    node_t base;
    mir_llong offset;
    int deref;

    assert (!stmt_p);
    if (context && context->code != N_ALIGNOF && context->code != N_SIZEOF
        && context->code != N_EXPR_SIZEOF)
      e->type = adjust_type (c2m_ctx, e->type);
    set_type_layout (c2m_ctx, e->type);
    if (!e->const_p && check_const_addr_p (c2m_ctx, r, &base, &offset, &deref) && deref == 0
        && base == NULL) {
      e->const_p = TRUE;
      e->u.i_val = offset;
    }
    if (e->const_p) convert_value (e, e->type);
  } else if (stmt_p) {
    curr_call_arg_area_offset = 0;
  } else if (expr_attr_p) { /* it is an error -- define any expr and type: */
    assert (!stmt_p);
    e = create_expr (c2m_ctx, r);
    e->type->mode = TM_BASIC;
    e->type->u.basic_type = TP_INT;
  }
  VARR_POP (node_t, context_stack);
}

static void do_context (c2m_ctx_t c2m_ctx, node_t r) {
  VARR_TRUNC (node_t, call_nodes, 0);
  check (c2m_ctx, r, NULL);
}

static void context_init (MIR_context_t ctx) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);

  c2m_ctx->check_ctx = c2mir_calloc (ctx, sizeof (struct check_ctx));
  n_i1_node = new_i_node (c2m_ctx, 1, no_pos);
  VARR_CREATE (node_t, context_stack, 64);
  check (c2m_ctx, n_i1_node, NULL);
  func_block_scope = curr_scope = NULL;
  VARR_CREATE (node_t, gotos, 0);
  symbol_init (c2m_ctx);
  in_params_p = FALSE;
  curr_unnamed_anon_struct_union_member = NULL;
  HTAB_CREATE (case_t, case_tab, 100, case_hash, case_eq);
  VARR_CREATE (decl_t, func_decls_for_allocation, 1024);
}

static void context_finish (MIR_context_t ctx) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);

  if (c2m_ctx == NULL || c2m_ctx->check_ctx == NULL) return;
  if (context_stack != NULL) VARR_DESTROY (node_t, context_stack);
  if (gotos != NULL) VARR_DESTROY (node_t, gotos);
  symbol_finish (c2m_ctx);
  if (case_tab != NULL) HTAB_DESTROY (case_t, case_tab);
  if (func_decls_for_allocation != NULL) VARR_DESTROY (decl_t, func_decls_for_allocation);
  free (c2m_ctx->check_ctx);
}

/* ------------------------ Context Checker Finish ---------------------------- */

/* New Page */

/* -------------------------- MIR generator start ----------------------------- */

static const char *FP_NAME = "fp";
static const char *RET_ADDR_NAME = "Ret_Addr";

/* New attribute for non-empty label LIST is a MIR label.  */

/* MIR var naming:
   {I|U|i|u|f|d}_<integer> -- temporary of I64, U64, I32, U32, F, D type
   {I|U|i|u|f|d}<scope_number>_<name> -- variable with of original <name> of the corresponding
                                         type in scope with <scope_number>
*/

#if MIR_PTR64
static const MIR_type_t MIR_POINTER_TYPE = MIR_T_I64;
#else
static const MIR_type_t MIR_POINTER_TYPE = MIR_T_I32;
#endif

struct op {
  decl_t decl;
  MIR_op_t mir_op;
};

typedef struct op op_t;

struct reg_var {
  const char *name;
  MIR_reg_t reg;
};

typedef struct reg_var reg_var_t;

DEF_HTAB (reg_var_t);

static VARR (MIR_var_t) * vars;

struct init_el {
  mir_size_t num, offset;
  decl_t member_decl; /* NULL for non-member initialization  */
  struct type *el_type;
  node_t init;
};

typedef struct init_el init_el_t;
DEF_VARR (init_el_t);

DEF_VARR (MIR_op_t);
DEF_VARR (case_t);

struct gen_ctx {
  op_t zero_op, one_op, minus_one_op;
  MIR_item_t curr_func;
  HTAB (reg_var_t) * reg_var_tab;
  int reg_free_mark;
  MIR_label_t continue_label, break_label;
  VARR (node_t) * mem_params;
  VARR (init_el_t) * init_els;
  MIR_item_t memset_proto, memset_item;
  MIR_item_t memcpy_proto, memcpy_item;
  VARR (MIR_op_t) * call_ops, *switch_ops;
  VARR (case_t) * switch_cases;
  int curr_mir_proto_num;
};

#define zero_op c2m_ctx->gen_ctx->zero_op
#define one_op c2m_ctx->gen_ctx->one_op
#define minus_one_op c2m_ctx->gen_ctx->minus_one_op
#define curr_func c2m_ctx->gen_ctx->curr_func
#define reg_var_tab c2m_ctx->gen_ctx->reg_var_tab
#define reg_free_mark c2m_ctx->gen_ctx->reg_free_mark
#define continue_label c2m_ctx->gen_ctx->continue_label
#define break_label c2m_ctx->gen_ctx->break_label
#define mem_params c2m_ctx->gen_ctx->mem_params
#define init_els c2m_ctx->gen_ctx->init_els
#define memset_proto c2m_ctx->gen_ctx->memset_proto
#define memset_item c2m_ctx->gen_ctx->memset_item
#define memcpy_proto c2m_ctx->gen_ctx->memcpy_proto
#define memcpy_item c2m_ctx->gen_ctx->memcpy_item
#define call_ops c2m_ctx->gen_ctx->call_ops
#define switch_ops c2m_ctx->gen_ctx->switch_ops
#define switch_cases c2m_ctx->gen_ctx->switch_cases
#define curr_mir_proto_num c2m_ctx->gen_ctx->curr_mir_proto_num

static op_t new_op (decl_t decl, MIR_op_t mir_op) {
  op_t res;

  res.decl = decl;
  res.mir_op = mir_op;
  return res;
}

static htab_hash_t reg_var_hash (reg_var_t r) { return mir_hash (r.name, strlen (r.name), 0x42); }
static int reg_var_eq (reg_var_t r1, reg_var_t r2) { return strcmp (r1.name, r2.name) == 0; }

static void init_reg_vars (MIR_context_t ctx) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);

  reg_free_mark = 0;
  HTAB_CREATE (reg_var_t, reg_var_tab, 128, reg_var_hash, reg_var_eq);
}

static void finish_curr_func_reg_vars (MIR_context_t ctx) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);

  reg_free_mark = 0;
  HTAB_CLEAR (reg_var_t, reg_var_tab);
}

static void finish_reg_vars (MIR_context_t ctx) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);

  if (reg_var_tab != NULL) HTAB_DESTROY (reg_var_t, reg_var_tab);
}

static reg_var_t get_reg_var (MIR_context_t ctx, MIR_type_t t, const char *reg_name) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);
  reg_var_t reg_var, el;
  char *str;
  MIR_reg_t reg;

  reg_var.name = reg_name;
  if (HTAB_DO (reg_var_t, reg_var_tab, reg_var, HTAB_FIND, el)) return el;
  t = t == MIR_T_I32 || t == MIR_T_U32 || t == MIR_T_U64 ? MIR_T_I64 : t;
  reg = (t != MIR_T_UNDEF ? MIR_new_func_reg (ctx, curr_func->u.func, t, reg_name)
                          : MIR_reg (ctx, reg_name, curr_func->u.func));
  str = reg_malloc (c2m_ctx, (strlen (reg_name) + 1) * sizeof (char));
  strcpy (str, reg_name);
  reg_var.name = str;
  reg_var.reg = reg;
  HTAB_DO (reg_var_t, reg_var_tab, reg_var, HTAB_INSERT, el);
  return reg_var;
}

static int temp_reg_p (MIR_context_t ctx, MIR_op_t op) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);

  return op.mode == MIR_OP_REG && MIR_reg_name (ctx, op.u.reg, curr_func->u.func)[1] == '_';
}

static MIR_type_t reg_type (MIR_context_t ctx, MIR_reg_t reg) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);
  const char *n = MIR_reg_name (ctx, reg, curr_func->u.func);
  MIR_type_t res;

  if (strcmp (n, FP_NAME) == 0 || strcmp (n, RET_ADDR_NAME) == 0) return MIR_POINTER_TYPE;
  res = (n[0] == 'I'
           ? MIR_T_I64
           : n[0] == 'U'
               ? MIR_T_U64
               : n[0] == 'i' ? MIR_T_I32
                             : n[0] == 'u' ? MIR_T_U32
                                           : n[0] == 'f' ? MIR_T_F
                                                         : n[0] == 'd' ? MIR_T_D
                                                                       : n[0] == 'D' ? MIR_T_LD
                                                                                     : MIR_T_BOUND);
  assert (res != MIR_T_BOUND);
  return res;
}

static op_t get_new_temp (MIR_context_t ctx, MIR_type_t t) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);
  static char reg_name[50];
  MIR_reg_t reg;

  assert (t == MIR_T_I64 || t == MIR_T_U64 || t == MIR_T_I32 || t == MIR_T_U32 || t == MIR_T_F
          || t == MIR_T_D || t == MIR_T_LD);
  sprintf (reg_name,
           t == MIR_T_I64
             ? "I_%u"
             : t == MIR_T_U64
                 ? "U_%u"
                 : t == MIR_T_I32
                     ? "i_%u"
                     : t == MIR_T_U32 ? "u_%u"
                                      : t == MIR_T_F ? "f_%u" : t == MIR_T_D ? "d_%u" : "D_%u",
           reg_free_mark++);
  reg = get_reg_var (ctx, t, reg_name).reg;
  return new_op (NULL, MIR_new_reg_op (ctx, reg));
}

static MIR_type_t get_int_mir_type (size_t size) {
  return size == 1 ? MIR_T_I8 : size == 2 ? MIR_T_I16 : size == 4 ? MIR_T_I32 : MIR_T_I64;
}

static int get_int_mir_type_size (MIR_type_t t) {
  return (t == MIR_T_I8 || t == MIR_T_U8
            ? 1
            : t == MIR_T_I16 || t == MIR_T_U16 ? 2 : t == MIR_T_I32 || t == MIR_T_U32 ? 4 : 8);
}

static MIR_type_t get_mir_type (MIR_context_t ctx, struct type *type) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);
  size_t size = raw_type_size (c2m_ctx, type);
  int int_p = !floating_type_p (type), signed_p = signed_integer_type_p (type);

  if (!scalar_type_p (type)) return MIR_T_UNDEF;
  assert (type->mode == TM_BASIC || type->mode == TM_PTR || type->mode == TM_ENUM);
  if (!int_p) {
    assert (size == 4 || size == 8 || size == sizeof (mir_ldouble));
    return size == 4 ? MIR_T_F : size == 8 ? MIR_T_D : MIR_T_LD;
  }
  assert (size <= 2 || size == 4 || size == 8);
  if (signed_p) return get_int_mir_type (size);
  return size == 1 ? MIR_T_U8 : size == 2 ? MIR_T_U16 : size == 4 ? MIR_T_U32 : MIR_T_U64;
}

static MIR_type_t promote_mir_int_type (MIR_type_t t) {
  return (t == MIR_T_I8 || t == MIR_T_I16 ? MIR_T_I32
                                          : t == MIR_T_U8 || t == MIR_T_U16 ? MIR_T_U32 : t);
}

static MIR_type_t get_op_type (MIR_context_t ctx, op_t op) {
  switch (op.mir_op.mode) {
  case MIR_OP_MEM: return op.mir_op.u.mem.type;
  case MIR_OP_REG: return reg_type (ctx, op.mir_op.u.reg);
  case MIR_OP_INT: return MIR_T_I64;
  case MIR_OP_UINT: return MIR_T_U64;
  case MIR_OP_FLOAT: return MIR_T_F;
  case MIR_OP_DOUBLE: return MIR_T_D;
  case MIR_OP_LDOUBLE: return MIR_T_LD;
  default: assert (FALSE); return MIR_T_I64;
  }
}

static op_t gen (MIR_context_t ctx, node_t r, MIR_label_t true_label, MIR_label_t false_label,
                 int val_p, op_t *desirable_dest);

static int push_const_val (MIR_context_t ctx, node_t r, op_t *res) {
  struct expr *e = (struct expr *) r->attr;
  MIR_type_t mir_type;

  if (!e->const_p) return FALSE;
  if (floating_type_p (e->type)) {
    /* MIR support only IEEE float and double */
    mir_type = get_mir_type (ctx, e->type);
    *res = new_op (NULL, (mir_type == MIR_T_F
                            ? MIR_new_float_op (ctx, e->u.d_val)
                            : mir_type == MIR_T_D ? MIR_new_double_op (ctx, e->u.d_val)
                                                  : MIR_new_ldouble_op (ctx, e->u.d_val)));
  } else {
    assert (integer_type_p (e->type) || e->type->mode == TM_PTR);
    *res = new_op (NULL, (signed_integer_type_p (e->type) ? MIR_new_int_op (ctx, e->u.i_val)
                                                          : MIR_new_uint_op (ctx, e->u.u_val)));
  }
  return TRUE;
}

static MIR_insn_code_t tp_mov (MIR_type_t t) {
  return t == MIR_T_F ? MIR_FMOV : t == MIR_T_D ? MIR_DMOV : t == MIR_T_LD ? MIR_LDMOV : MIR_MOV;
}

static void emit_insn (MIR_context_t ctx, MIR_insn_t insn) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);

  MIR_append_insn (ctx, curr_func, insn);
}

/* Change t1 = expr; v = t1 to v = expr */
static void emit_insn_opt (MIR_context_t ctx, MIR_insn_t insn) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);
  MIR_insn_t tail;
  int out_p;

  if ((insn->code == MIR_MOV || insn->code == MIR_FMOV || insn->code == MIR_DMOV
       || insn->code == MIR_LDMOV)
      && (tail = DLIST_TAIL (MIR_insn_t, curr_func->u.func->insns)) != NULL
      && MIR_insn_nops (ctx, tail) > 0 && temp_reg_p (ctx, insn->ops[1])
      && !temp_reg_p (ctx, insn->ops[0]) && temp_reg_p (ctx, tail->ops[0])
      && insn->ops[1].u.reg == tail->ops[0].u.reg) {
    MIR_insn_op_mode (ctx, tail, 0, &out_p);
    if (out_p) {
      tail->ops[0] = insn->ops[0];
      return;
    }
  }
  MIR_append_insn (ctx, curr_func, insn);
}

static void emit1 (MIR_context_t ctx, MIR_insn_code_t code, MIR_op_t op1) {
  emit_insn_opt (ctx, MIR_new_insn (ctx, code, op1));
}
static void emit2 (MIR_context_t ctx, MIR_insn_code_t code, MIR_op_t op1, MIR_op_t op2) {
  emit_insn_opt (ctx, MIR_new_insn (ctx, code, op1, op2));
}
static void emit3 (MIR_context_t ctx, MIR_insn_code_t code, MIR_op_t op1, MIR_op_t op2,
                   MIR_op_t op3) {
  emit_insn_opt (ctx, MIR_new_insn (ctx, code, op1, op2, op3));
}

static void emit2_noopt (MIR_context_t ctx, MIR_insn_code_t code, MIR_op_t op1, MIR_op_t op2) {
  emit_insn (ctx, MIR_new_insn (ctx, code, op1, op2));
}

static op_t cast (MIR_context_t ctx, op_t op, MIR_type_t t, int new_op_p) {
  op_t res, interm;
  MIR_type_t op_type;
  MIR_insn_code_t insn_code = MIR_INSN_BOUND, insn_code2 = MIR_INSN_BOUND;

  assert (t == MIR_T_I8 || t == MIR_T_U8 || t == MIR_T_I16 || t == MIR_T_U16 || t == MIR_T_I32
          || t == MIR_T_U32 || t == MIR_T_I64 || t == MIR_T_U64 || t == MIR_T_F || t == MIR_T_D
          || t == MIR_T_LD);
  switch (op.mir_op.mode) {
  case MIR_OP_MEM:
    op_type = op.mir_op.u.mem.type;
    if (op_type == MIR_T_UNDEF) { /* ??? it is an array */

    } else if (op_type == MIR_T_I8 || op_type == MIR_T_U8 || op_type == MIR_T_I16
               || op_type == MIR_T_U16 || op_type == MIR_T_I32 || op_type == MIR_T_U32)
      op_type = MIR_T_I64;
    goto all_types;
  case MIR_OP_REG:
    op_type = reg_type (ctx, op.mir_op.u.reg);
  all_types:
    if (op_type == MIR_T_F) goto float_val;
    if (op_type == MIR_T_D) goto double_val;
    if (op_type == MIR_T_LD) goto ldouble_val;
    if (t == MIR_T_I64) {
      insn_code
        = (op_type == MIR_T_I32
             ? MIR_EXT32
             : op_type == MIR_T_U32
                 ? MIR_UEXT32
                 : op_type == MIR_T_F
                     ? MIR_F2I
                     : op_type == MIR_T_D ? MIR_D2I
                                          : op_type == MIR_T_LD ? MIR_LD2I : MIR_INSN_BOUND);
    } else if (t == MIR_T_U64) {
      insn_code
        = (op_type == MIR_T_I32
             ? MIR_EXT32
             : op_type == MIR_T_U32
                 ? MIR_UEXT32
                 : op_type == MIR_T_F
                     ? MIR_F2I
                     : op_type == MIR_T_D ? MIR_D2I
                                          : op_type == MIR_T_LD ? MIR_LD2I : MIR_INSN_BOUND);
    } else if (t == MIR_T_I32) {
      insn_code
        = (op_type == MIR_T_F
             ? MIR_F2I
             : op_type == MIR_T_D ? MIR_D2I : op_type == MIR_T_LD ? MIR_LD2I : MIR_INSN_BOUND);
    } else if (t == MIR_T_U32) {
      insn_code
        = (op_type == MIR_T_F
             ? MIR_F2I
             : op_type == MIR_T_D ? MIR_D2I : op_type == MIR_T_LD ? MIR_LD2I : MIR_INSN_BOUND);
    } else if (t == MIR_T_I16) {
      insn_code
        = (op_type == MIR_T_F
             ? MIR_F2I
             : op_type == MIR_T_D ? MIR_D2I : op_type == MIR_T_LD ? MIR_LD2I : MIR_INSN_BOUND);
      insn_code2 = MIR_EXT16;
    } else if (t == MIR_T_U16) {
      insn_code
        = (op_type == MIR_T_F
             ? MIR_F2I
             : op_type == MIR_T_D ? MIR_D2I : op_type == MIR_T_LD ? MIR_LD2I : MIR_INSN_BOUND);
      insn_code2 = MIR_UEXT16;
    } else if (t == MIR_T_I8) {
      insn_code
        = (op_type == MIR_T_F
             ? MIR_F2I
             : op_type == MIR_T_D ? MIR_D2I : op_type == MIR_T_LD ? MIR_LD2I : MIR_INSN_BOUND);
      insn_code2 = MIR_EXT8;
    } else if (t == MIR_T_U8) {
      insn_code
        = (op_type == MIR_T_F
             ? MIR_F2I
             : op_type == MIR_T_D ? MIR_D2I : op_type == MIR_T_LD ? MIR_LD2I : MIR_INSN_BOUND);
      insn_code2 = MIR_UEXT8;
    } else if (t == MIR_T_F) {
      insn_code
        = (op_type == MIR_T_I32 ? MIR_EXT32 : op_type == MIR_T_U32 ? MIR_UEXT32 : MIR_INSN_BOUND);
      insn_code2 = (op_type == MIR_T_I64 || op_type == MIR_T_I32
                      ? MIR_I2F
                      : op_type == MIR_T_U64 || op_type == MIR_T_U32 ? MIR_UI2F : MIR_INSN_BOUND);
    } else if (t == MIR_T_D) {
      insn_code
        = (op_type == MIR_T_I32 ? MIR_EXT32 : op_type == MIR_T_U32 ? MIR_UEXT32 : MIR_INSN_BOUND);
      insn_code2 = (op_type == MIR_T_I64 || op_type == MIR_T_I32
                      ? MIR_I2D
                      : op_type == MIR_T_U64 || op_type == MIR_T_U32 ? MIR_UI2D : MIR_INSN_BOUND);
    } else if (t == MIR_T_LD) {
      insn_code
        = (op_type == MIR_T_I32 ? MIR_EXT32 : op_type == MIR_T_U32 ? MIR_UEXT32 : MIR_INSN_BOUND);
      insn_code2 = (op_type == MIR_T_I64 || op_type == MIR_T_I32
                      ? MIR_I2LD
                      : op_type == MIR_T_U64 || op_type == MIR_T_U32 ? MIR_UI2LD : MIR_INSN_BOUND);
    }
    break;
  case MIR_OP_INT:
    insn_code
      = (t == MIR_T_I8
           ? MIR_EXT8
           : t == MIR_T_U8
               ? MIR_UEXT8
               : t == MIR_T_I16
                   ? MIR_EXT16
                   : t == MIR_T_U16
                       ? MIR_UEXT16
                       : t == MIR_T_F
                           ? MIR_I2F
                           : t == MIR_T_D ? MIR_I2D : t == MIR_T_LD ? MIR_I2LD : MIR_INSN_BOUND);
    break;
  case MIR_OP_UINT:
    insn_code
      = (t == MIR_T_I8
           ? MIR_EXT8
           : t == MIR_T_U8
               ? MIR_UEXT8
               : t == MIR_T_I16
                   ? MIR_EXT16
                   : t == MIR_T_U16
                       ? MIR_UEXT16
                       : t == MIR_T_F
                           ? MIR_UI2F
                           : t == MIR_T_D ? MIR_UI2D : t == MIR_T_LD ? MIR_UI2LD : MIR_INSN_BOUND);
    break;
  case MIR_OP_FLOAT:
  float_val:
    insn_code = (t == MIR_T_I8 || t == MIR_T_U8 || t == MIR_T_I16 || t == MIR_T_U16
                     || t == MIR_T_I32 || t == MIR_T_U32 || t == MIR_T_I64 || t == MIR_T_U64
                   ? MIR_F2I
                   : t == MIR_T_D ? MIR_F2D : t == MIR_T_LD ? MIR_F2LD : MIR_INSN_BOUND);
    insn_code2 = (t == MIR_T_I8 ? MIR_EXT8
                                : t == MIR_T_U8 ? MIR_UEXT8
                                                : t == MIR_T_I16
                                                    ? MIR_EXT16
                                                    : t == MIR_T_U16 ? MIR_UEXT16 : MIR_INSN_BOUND);
    break;
  case MIR_OP_DOUBLE:
  double_val:
    insn_code = (t == MIR_T_I8 || t == MIR_T_U8 || t == MIR_T_I16 || t == MIR_T_U16
                     || t == MIR_T_I32 || t == MIR_T_U32 || t == MIR_T_I64 || t == MIR_T_U64
                   ? MIR_D2I
                   : t == MIR_T_F ? MIR_D2F : t == MIR_T_LD ? MIR_D2LD : MIR_INSN_BOUND);
    insn_code2 = (t == MIR_T_I8 ? MIR_EXT8
                                : t == MIR_T_U8 ? MIR_UEXT8
                                                : t == MIR_T_I16
                                                    ? MIR_EXT16
                                                    : t == MIR_T_U16 ? MIR_UEXT16 : MIR_INSN_BOUND);
    break;
  case MIR_OP_LDOUBLE:
  ldouble_val:
    insn_code = (t == MIR_T_I8 || t == MIR_T_U8 || t == MIR_T_I16 || t == MIR_T_U16
                     || t == MIR_T_I32 || t == MIR_T_U32 || t == MIR_T_I64 || t == MIR_T_U64
                   ? MIR_LD2I
                   : t == MIR_T_F ? MIR_LD2F : t == MIR_T_D ? MIR_LD2D : MIR_INSN_BOUND);
    insn_code2 = (t == MIR_T_I8 ? MIR_EXT8
                                : t == MIR_T_U8 ? MIR_UEXT8
                                                : t == MIR_T_I16
                                                    ? MIR_EXT16
                                                    : t == MIR_T_U16 ? MIR_UEXT16 : MIR_INSN_BOUND);
    break;
  default: break;
  }
  if (!new_op_p && insn_code == MIR_INSN_BOUND && insn_code2 == MIR_INSN_BOUND) return op;
  res = get_new_temp (ctx, t == MIR_T_I8 || t == MIR_T_U8 || t == MIR_T_I16 || t == MIR_T_U16
                             ? MIR_T_I64
                             : t);
  if (insn_code == MIR_INSN_BOUND && insn_code2 == MIR_INSN_BOUND) {
    emit2 (ctx, tp_mov (t), res.mir_op, op.mir_op);
  } else if (insn_code == MIR_INSN_BOUND) {
    emit2 (ctx, insn_code2, res.mir_op, op.mir_op);
  } else if (insn_code2 == MIR_INSN_BOUND) {
    emit2 (ctx, insn_code, res.mir_op, op.mir_op);
  } else {
    interm = get_new_temp (ctx, MIR_T_I64);
    emit2 (ctx, insn_code, interm.mir_op, op.mir_op);
    emit2 (ctx, insn_code2, res.mir_op, interm.mir_op);
  }
  return res;
}

static op_t promote (MIR_context_t ctx, op_t op, MIR_type_t t, int new_op_p) {
  assert (t == MIR_T_I64 || t == MIR_T_U64 || t == MIR_T_I32 || t == MIR_T_U32 || t == MIR_T_F
          || t == MIR_T_D || t == MIR_T_LD);
  return cast (ctx, op, t, new_op_p);
}

static op_t mem_to_address (MIR_context_t ctx, op_t mem) {
  op_t temp;

  if (mem.mir_op.mode == MIR_OP_STR) return mem;
  assert (mem.mir_op.mode == MIR_OP_MEM);
  if (mem.mir_op.u.mem.base == 0 && mem.mir_op.u.mem.index == 0) {
    mem.mir_op.mode = MIR_OP_INT;
    mem.mir_op.u.i = mem.mir_op.u.mem.disp;
  } else if (mem.mir_op.u.mem.index == 0 && mem.mir_op.u.mem.disp == 0) {
    mem.mir_op.mode = MIR_OP_REG;
    mem.mir_op.u.reg = mem.mir_op.u.mem.base;
  } else if (mem.mir_op.u.mem.index == 0) {
    temp = get_new_temp (ctx, MIR_T_I64);
    emit3 (ctx, MIR_ADD, temp.mir_op, MIR_new_reg_op (ctx, mem.mir_op.u.mem.base),
           MIR_new_int_op (ctx, mem.mir_op.u.mem.disp));
    mem = temp;
  } else {
    temp = get_new_temp (ctx, MIR_T_I64);
    if (mem.mir_op.u.mem.scale != 1)
      emit3 (ctx, MIR_MUL, temp.mir_op, MIR_new_reg_op (ctx, mem.mir_op.u.mem.index),
             MIR_new_int_op (ctx, mem.mir_op.u.mem.scale));
    else
      emit2 (ctx, MIR_MOV, temp.mir_op, MIR_new_reg_op (ctx, mem.mir_op.u.mem.index));
    if (mem.mir_op.u.mem.base != 0)
      emit3 (ctx, MIR_ADD, temp.mir_op, temp.mir_op, MIR_new_reg_op (ctx, mem.mir_op.u.mem.base));
    if (mem.mir_op.u.mem.disp != 0)
      emit3 (ctx, MIR_ADD, temp.mir_op, temp.mir_op, MIR_new_int_op (ctx, mem.mir_op.u.mem.disp));
    mem = temp;
  }
  mem.mir_op.value_mode = MIR_OP_INT;
  return mem;
}

static op_t force_val (MIR_context_t ctx, op_t op, int arr_p) {
  op_t temp_op;
  int sh;

  if (arr_p && op.mir_op.mode == MIR_OP_MEM) {
    /* an array -- use a pointer: */
    return mem_to_address (ctx, op);
  }
  if (op.decl == NULL || op.decl->bit_offset < 0) return op;
  assert (op.mir_op.mode == MIR_OP_MEM);
  temp_op = get_new_temp (ctx, MIR_T_I64);
  emit2 (ctx, MIR_MOV, temp_op.mir_op, op.mir_op);
  sh = 64 - op.decl->bit_offset - op.decl->width;
  if (sh != 0) emit3 (ctx, MIR_LSH, temp_op.mir_op, temp_op.mir_op, MIR_new_int_op (ctx, sh));
  emit3 (ctx,
         signed_integer_type_p (op.decl->decl_spec.type)
             && (op.decl->decl_spec.type->mode != TM_ENUM
                 || op.decl->width >= sizeof (mir_int) * MIR_CHAR_BIT)
           ? MIR_RSH
           : MIR_URSH,
         temp_op.mir_op, temp_op.mir_op, MIR_new_int_op (ctx, 64 - op.decl->width));
  return temp_op;
}

static void gen_unary_op (MIR_context_t ctx, node_t r, op_t *op, op_t *res) {
  MIR_type_t t;

  assert (!((struct expr *) r->attr)->const_p);
  *op = gen (ctx, NL_HEAD (r->ops), NULL, NULL, TRUE, NULL);
  t = get_mir_type (ctx, ((struct expr *) r->attr)->type);
  *op = promote (ctx, *op, t, FALSE);
  *res = get_new_temp (ctx, t);
}

static void gen_assign_bin_op (MIR_context_t ctx, node_t r, struct type *assign_expr_type,
                               op_t *op1, op_t *op2, op_t *var) {
  MIR_type_t t;
  node_t e = NL_HEAD (r->ops);

  assert (!((struct expr *) r->attr)->const_p);
  t = get_mir_type (ctx, assign_expr_type);
  *op1 = gen (ctx, e, NULL, NULL, FALSE, NULL);
  *op2 = gen (ctx, NL_NEXT (e), NULL, NULL, TRUE, NULL);
  *op2 = promote (ctx, *op2, t, FALSE);
  *var = *op1;
  *op1 = force_val (ctx, *op1, ((struct expr *) e->attr)->type->arr_type != NULL);
  *op1 = promote (ctx, *op1, t, TRUE);
}

static void gen_bin_op (MIR_context_t ctx, node_t r, op_t *op1, op_t *op2, op_t *res) {
  struct expr *e = (struct expr *) r->attr;
  MIR_type_t t = get_mir_type (ctx, e->type);

  assert (!e->const_p);
  *op1 = gen (ctx, NL_HEAD (r->ops), NULL, NULL, TRUE, NULL);
  *op2 = gen (ctx, NL_EL (r->ops, 1), NULL, NULL, TRUE, NULL);
  *op1 = promote (ctx, *op1, t, FALSE);
  *op2 = promote (ctx, *op2, t, FALSE);
  *res = get_new_temp (ctx, t);
}

static void gen_cmp_op (MIR_context_t ctx, node_t r, struct type *type, op_t *op1, op_t *op2,
                        op_t *res) {
  MIR_type_t t = get_mir_type (ctx, type), res_t = get_int_mir_type (sizeof (mir_int));

  assert (!((struct expr *) r->attr)->const_p);
  *op1 = gen (ctx, NL_HEAD (r->ops), NULL, NULL, TRUE, NULL);
  *op2 = gen (ctx, NL_EL (r->ops, 1), NULL, NULL, TRUE, NULL);
  *op1 = promote (ctx, *op1, t, FALSE);
  *op2 = promote (ctx, *op2, t, FALSE);
  *res = get_new_temp (ctx, res_t);
}

static MIR_insn_code_t get_mir_type_insn_code (MIR_context_t ctx, struct type *type, node_t r) {
  MIR_type_t t = get_mir_type (ctx, type);

  switch (r->code) {
  case N_INC:
  case N_POST_INC:
  case N_ADD:
  case N_ADD_ASSIGN:
    return (t == MIR_T_F
              ? MIR_FADD
              : t == MIR_T_D
                  ? MIR_DADD
                  : t == MIR_T_LD ? MIR_LDADD
                                  : t == MIR_T_I64 || t == MIR_T_U64 ? MIR_ADD : MIR_ADDS);
  case N_DEC:
  case N_POST_DEC:
  case N_SUB:
  case N_SUB_ASSIGN:
    return (t == MIR_T_F
              ? MIR_FSUB
              : t == MIR_T_D
                  ? MIR_DSUB
                  : t == MIR_T_LD ? MIR_LDSUB
                                  : t == MIR_T_I64 || t == MIR_T_U64 ? MIR_SUB : MIR_SUBS);
  case N_MUL:
  case N_MUL_ASSIGN:
    return (t == MIR_T_F
              ? MIR_FMUL
              : t == MIR_T_D
                  ? MIR_DMUL
                  : t == MIR_T_LD ? MIR_LDMUL
                                  : t == MIR_T_I64 || t == MIR_T_U64 ? MIR_MUL : MIR_MULS);
  case N_DIV:
  case N_DIV_ASSIGN:
    return (t == MIR_T_F
              ? MIR_FDIV
              : t == MIR_T_D
                  ? MIR_DDIV
                  : t == MIR_T_LD
                      ? MIR_LDDIV
                      : t == MIR_T_I64
                          ? MIR_DIV
                          : t == MIR_T_U64 ? MIR_UDIV : t == MIR_T_I32 ? MIR_DIVS : MIR_UDIVS);
  case N_MOD:
  case N_MOD_ASSIGN:
    return (t == MIR_T_I64 ? MIR_MOD
                           : t == MIR_T_U64 ? MIR_UMOD : t == MIR_T_I32 ? MIR_MODS : MIR_UMODS);
  case N_AND:
  case N_AND_ASSIGN: return (t == MIR_T_I64 || t == MIR_T_U64 ? MIR_AND : MIR_ANDS);
  case N_OR:
  case N_OR_ASSIGN: return (t == MIR_T_I64 || t == MIR_T_U64 ? MIR_OR : MIR_ORS);
  case N_XOR:
  case N_XOR_ASSIGN: return (t == MIR_T_I64 || t == MIR_T_U64 ? MIR_XOR : MIR_XORS);
  case N_LSH:
  case N_LSH_ASSIGN: return (t == MIR_T_I64 || t == MIR_T_U64 ? MIR_LSH : MIR_LSHS);
  case N_RSH:
  case N_RSH_ASSIGN:
    return (t == MIR_T_I64 ? MIR_RSH
                           : t == MIR_T_U64 ? MIR_URSH : t == MIR_T_I32 ? MIR_RSHS : MIR_URSHS);
  case N_EQ:
    return (t == MIR_T_F
              ? MIR_FEQ
              : t == MIR_T_D
                  ? MIR_DEQ
                  : t == MIR_T_LD ? MIR_LDEQ : t == MIR_T_I64 || t == MIR_T_U64 ? MIR_EQ : MIR_EQS);
  case N_NE:
    return (t == MIR_T_F
              ? MIR_FNE
              : t == MIR_T_D
                  ? MIR_DNE
                  : t == MIR_T_LD ? MIR_LDNE : t == MIR_T_I64 || t == MIR_T_U64 ? MIR_NE : MIR_NES);
  case N_LT:
    return (t == MIR_T_F
              ? MIR_FLT
              : t == MIR_T_D
                  ? MIR_DLT
                  : t == MIR_T_LD
                      ? MIR_LDLT
                      : t == MIR_T_I64
                          ? MIR_LT
                          : t == MIR_T_U64 ? MIR_ULT : t == MIR_T_I32 ? MIR_LTS : MIR_ULTS);
  case N_LE:
    return (t == MIR_T_F
              ? MIR_FLE
              : t == MIR_T_D
                  ? MIR_DLE
                  : t == MIR_T_LD
                      ? MIR_LDLE
                      : t == MIR_T_I64
                          ? MIR_LE
                          : t == MIR_T_U64 ? MIR_ULE : t == MIR_T_I32 ? MIR_LES : MIR_ULES);
  case N_GT:
    return (t == MIR_T_F
              ? MIR_FGT
              : t == MIR_T_D
                  ? MIR_DGT
                  : t == MIR_T_LD
                      ? MIR_LDGT
                      : t == MIR_T_I64
                          ? MIR_GT
                          : t == MIR_T_U64 ? MIR_UGT : t == MIR_T_I32 ? MIR_GTS : MIR_UGTS);
  case N_GE:
    return (t == MIR_T_F
              ? MIR_FGE
              : t == MIR_T_D
                  ? MIR_DGE
                  : t == MIR_T_LD
                      ? MIR_LDGE
                      : t == MIR_T_I64
                          ? MIR_GE
                          : t == MIR_T_U64 ? MIR_UGE : t == MIR_T_I32 ? MIR_GES : MIR_UGES);
  default: assert (FALSE); return MIR_INSN_BOUND;
  }
}

static MIR_insn_code_t get_mir_insn_code (MIR_context_t ctx,
                                          node_t r) { /* result type is the same as op types */
  return get_mir_type_insn_code (ctx, ((struct expr *) r->attr)->type, r);
}

static MIR_insn_code_t get_compare_branch_code (MIR_insn_code_t code) {
#define B(n)                           \
  case MIR_##n: return MIR_B##n;       \
  case MIR_##n##S: return MIR_B##n##S; \
  case MIR_F##n: return MIR_FB##n;     \
  case MIR_D##n: return MIR_DB##n;     \
  case MIR_LD##n:                      \
    return MIR_LDB##n;
#define BCMP(n)                    \
  B (n)                            \
  case MIR_U##n: return MIR_UB##n; \
  case MIR_U##n##S: return MIR_UB##n##S;
  switch (code) {
    B (EQ) B (NE) BCMP (LT) BCMP (LE) BCMP (GT) BCMP (GE) default : assert (FALSE);
    return MIR_INSN_BOUND;
  }
#undef B
#undef BCMP
}

static op_t force_reg (MIR_context_t ctx, op_t op, MIR_type_t t) {
  op_t res;

  if (op.mir_op.mode == MIR_OP_REG) return op;
  res = get_new_temp (ctx, promote_mir_int_type (t));
  emit2 (ctx, MIR_MOV, res.mir_op, op.mir_op);
  return res;
}

static op_t force_reg_or_mem (MIR_context_t ctx, op_t op, MIR_type_t t) {
  if (op.mir_op.mode == MIR_OP_REG || op.mir_op.mode == MIR_OP_MEM) return op;
  assert (op.mir_op.mode == MIR_OP_REF || op.mir_op.mode == MIR_OP_STR);
  return force_reg (ctx, op, t);
}

static void emit_label (MIR_context_t ctx, node_t r) {
  node_t labels = NL_HEAD (r->ops);

  assert (labels->code == N_LIST);
  if (NL_HEAD (labels->ops) == NULL) return;
  if (labels->attr == NULL) labels->attr = MIR_new_label (ctx);
  emit_insn (ctx, labels->attr);
}

static MIR_label_t get_label (MIR_context_t ctx, node_t target) {
  node_t labels = NL_HEAD (target->ops);

  assert (labels->code == N_LIST && NL_HEAD (labels->ops) != NULL);
  if (labels->attr != NULL) return labels->attr;
  return labels->attr = MIR_new_label (ctx);
}

static void top_gen (MIR_context_t ctx, node_t r, MIR_label_t true_label, MIR_label_t false_label) {
  gen (ctx, r, true_label, false_label, FALSE, NULL);
}

static op_t modify_for_block_move (MIR_context_t ctx, op_t mem, op_t index) {
  op_t base;

  assert (mem.mir_op.u.mem.base != 0 && mem.mir_op.mode == MIR_OP_MEM
          && index.mir_op.mode == MIR_OP_REG);
  if (mem.mir_op.u.mem.index == 0) {
    mem.mir_op.u.mem.index = index.mir_op.u.reg;
    mem.mir_op.u.mem.scale = 1;
  } else {
    base = get_new_temp (ctx, MIR_T_I64);
    if (mem.mir_op.u.mem.scale != 1)
      emit3 (ctx, MIR_MUL, base.mir_op, MIR_new_reg_op (ctx, mem.mir_op.u.mem.index),
             MIR_new_int_op (ctx, mem.mir_op.u.mem.scale));
    else
      emit2 (ctx, MIR_MOV, base.mir_op, MIR_new_reg_op (ctx, mem.mir_op.u.mem.index));
    emit3 (ctx, MIR_ADD, base.mir_op, base.mir_op, MIR_new_reg_op (ctx, mem.mir_op.u.mem.base));
    mem.mir_op.u.mem.base = base.mir_op.u.reg;
    mem.mir_op.u.mem.index = index.mir_op.u.reg;
    mem.mir_op.u.mem.scale = 1;
  }
  return mem;
}

static void gen_memcpy (MIR_context_t ctx, MIR_disp_t disp, MIR_reg_t base, op_t val,
                        mir_size_t len);

static void block_move (MIR_context_t ctx, op_t var, op_t val, mir_size_t size) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);
  MIR_label_t repeat_label;
  op_t index;

  if (MIR_op_eq_p (ctx, var.mir_op, val.mir_op)) return;
  if (size > 5) {
    var = mem_to_address (ctx, var);
    assert (var.mir_op.mode == MIR_OP_REG);
    gen_memcpy (ctx, 0, var.mir_op.u.reg, val, size);
  } else {
    repeat_label = MIR_new_label (ctx);
    index = get_new_temp (ctx, MIR_T_I64);
    emit2 (ctx, MIR_MOV, index.mir_op, MIR_new_int_op (ctx, size));
    val = modify_for_block_move (ctx, val, index);
    var = modify_for_block_move (ctx, var, index);
    emit_insn (ctx, repeat_label);
    emit3 (ctx, MIR_SUB, index.mir_op, index.mir_op, one_op.mir_op);
    assert (var.mir_op.mode == MIR_OP_MEM && val.mir_op.mode == MIR_OP_MEM);
    val.mir_op.u.mem.type = var.mir_op.u.mem.type = MIR_T_I8;
    emit2 (ctx, MIR_MOV, var.mir_op, val.mir_op);
    emit3 (ctx, MIR_BGT, MIR_new_label_op (ctx, repeat_label), index.mir_op, zero_op.mir_op);
  }
}

static const char *get_reg_var_name (MIR_context_t ctx, MIR_type_t promoted_type,
                                     const char *suffix, unsigned func_scope_num) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);
  char prefix[50];

  sprintf (prefix,
           promoted_type == MIR_T_I64
             ? "I%u_"
             : promoted_type == MIR_T_U64
                 ? "U%u_"
                 : promoted_type == MIR_T_I32
                     ? "i%u_"
                     : promoted_type == MIR_T_U32
                         ? "u%u_"
                         : promoted_type == MIR_T_F ? "f%u_"
                                                    : promoted_type == MIR_T_D ? "d%u_" : "D%u_",
           func_scope_num);
  VARR_TRUNC (char, temp_string, 0);
  add_to_temp_string (c2m_ctx, prefix);
  add_to_temp_string (c2m_ctx, suffix);
  return uniq_cstr (c2m_ctx, VARR_ADDR (char, temp_string)).s;
}

static const char *get_func_var_name (MIR_context_t ctx, const char *prefix, const char *suffix) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);

  assert (curr_func != NULL);
  VARR_TRUNC (char, temp_string, 0);
  add_to_temp_string (c2m_ctx, prefix);
  add_to_temp_string (c2m_ctx, curr_func->u.func->name);
  add_to_temp_string (c2m_ctx, "_");
  add_to_temp_string (c2m_ctx, suffix);
  return uniq_cstr (c2m_ctx, VARR_ADDR (char, temp_string)).s;
}

static const char *get_func_static_var_name (MIR_context_t ctx, const char *suffix, decl_t decl) {
  char prefix[50];
  unsigned func_scope_num = ((struct node_scope *) decl->scope->attr)->func_scope_num;

  sprintf (prefix, "S%u_", func_scope_num);
  return get_func_var_name (ctx, prefix, suffix);
}

static const char *get_param_name (MIR_context_t ctx, MIR_type_t *type, struct type *param_type,
                                   const char *name) {
  *type = (param_type->mode == TM_STRUCT || param_type->mode == TM_UNION
             ? MIR_POINTER_TYPE
             : get_mir_type (ctx, param_type));
  return get_reg_var_name (ctx, promote_mir_int_type (*type), name, 0);
}

static void collect_args_and_func_types (MIR_context_t ctx, struct func_type *func_type,
                                         MIR_type_t *ret_type) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);
  node_t declarator, id, first_param, p;
  struct type *param_type;
  MIR_var_t var;
  MIR_type_t type;

  first_param = NL_HEAD (func_type->param_list->ops);
  VARR_TRUNC (MIR_var_t, vars, 0);
  VARR_TRUNC (node_t, mem_params, 0);
  if (func_type->ret_type->mode == TM_STRUCT || func_type->ret_type->mode == TM_UNION) {
    var.name = RET_ADDR_NAME;
    var.type = MIR_POINTER_TYPE;
    VARR_PUSH (MIR_var_t, vars, var);
  }
  if (first_param != NULL && !void_param_p (first_param)) {
    for (p = first_param; p != NULL; p = NL_NEXT (p)) {
      if (p->code == N_TYPE) {
        var.name = "p";
        param_type = ((struct decl_spec *) p->attr)->type;
        type = (param_type->mode == TM_STRUCT || param_type->mode == TM_UNION
                  ? MIR_POINTER_TYPE
                  : get_mir_type (ctx, param_type));
      } else {
        declarator = NL_EL (p->ops, 1);
        assert (p->code == N_SPEC_DECL && declarator != NULL && declarator->code == N_DECL);
        id = NL_HEAD (declarator->ops);
        param_type = ((decl_t) p->attr)->decl_spec.type;
        var.name = get_param_name (ctx, &type, param_type, id->u.s.s);
        if (param_type->mode == TM_STRUCT || param_type->mode == TM_UNION
            || !((decl_t) p->attr)->reg_p)
          VARR_PUSH (node_t, mem_params, p);
      }
      var.type = type;
      VARR_PUSH (MIR_var_t, vars, var);
    }
  }
  set_type_layout (c2m_ctx, func_type->ret_type);
  *ret_type = get_mir_type (ctx, func_type->ret_type);
}

static mir_size_t get_object_path_offset (c2m_ctx_t c2m_ctx) {
  init_object_t init_object;
  size_t offset = 0;

  for (size_t i = 0; i < VARR_LENGTH (init_object_t, init_object_path); i++) {
    init_object = VARR_GET (init_object_t, init_object_path, i);
    if (init_object.container_type->mode == TM_ARR) {  // ??? index < 0
      offset += (init_object.u.curr_index
                 * type_size (c2m_ctx, init_object.container_type->u.arr_type->el_type));
    } else {
      assert (init_object.container_type->mode == TM_STRUCT
              || init_object.container_type->mode == TM_UNION);
      assert (init_object.u.curr_member->code == N_MEMBER);
      if (!anon_struct_union_type_member_p (init_object.u.curr_member))
        /* Members inside anon struct/union already have adjusted offset */
        offset += ((decl_t) init_object.u.curr_member->attr)->offset;
    }
  }
  return offset;
}

/* The function has the same structure as check_initializer.  Keep it this way. */
static void collect_init_els (c2m_ctx_t c2m_ctx, decl_t member_decl, struct type **type_ptr,
                              node_t initializer, int const_only_p, int top_p) {
  struct type *type = *type_ptr;
  struct expr *cexpr;
  node_t literal, des_list, curr_des, str, init, value, size_node;
  mir_llong size_val;
  size_t mark;
  symbol_t sym;
  struct expr *sexpr;
  init_el_t init_el;
  int addr_p, found_p, ok_p;
  init_object_t init_object;

  literal = get_compound_literal (initializer, &addr_p);
  if (literal != NULL && !addr_p && initializer->code != N_STR)
    initializer = NL_EL (literal->ops, 1);
check_one_value:
  if (initializer->code != N_LIST
      && !(initializer->code == N_STR && type->mode == TM_ARR
           && char_type_p (type->u.arr_type->el_type))) {
    cexpr = initializer->attr;
    /* static or thread local object initialization should be const expr or addr: */
    assert (initializer->code == N_STR || !const_only_p || cexpr->const_p || cexpr->const_addr_p
            || (literal != NULL && addr_p));
    init_el.num = VARR_LENGTH (init_el_t, init_els);
    init_el.offset = get_object_path_offset (c2m_ctx);
    init_el.member_decl = member_decl;
    init_el.el_type = type;
    init_el.init = initializer;
    VARR_PUSH (init_el_t, init_els, init_el);
    return;
  }
  init = NL_HEAD (initializer->ops);
  if (((str = initializer)->code == N_STR /* string or string in parentheses  */
       || (init->code == N_INIT && NL_EL (initializer->ops, 1) == NULL
           && (des_list = NL_HEAD (init->ops))->code == N_LIST && NL_HEAD (des_list->ops) == NULL
           && NL_EL (init->ops, 1) != NULL && (str = NL_EL (init->ops, 1))->code == N_STR))
      && type->mode == TM_ARR && char_type_p (type->u.arr_type->el_type)) {
    init_el.num = VARR_LENGTH (init_el_t, init_els);
    init_el.offset = get_object_path_offset (c2m_ctx);
    init_el.el_type = type;
    init_el.init = str;
    VARR_PUSH (init_el_t, init_els, init_el);
    return;
  }
  assert (init->code == N_INIT);
  des_list = NL_HEAD (init->ops);
  assert (des_list->code == N_LIST);
  if (type->mode != TM_ARR && type->mode != TM_STRUCT && type->mode != TM_UNION) {
    assert (NL_NEXT (init) == NULL && NL_HEAD (des_list->ops) == NULL);
    initializer = NL_NEXT (des_list);
    assert (top_p);
    top_p = FALSE;
    goto check_one_value;
  }
  mark = VARR_LENGTH (init_object_t, init_object_path);
  init_object.container_type = type;
  init_object.designator_p = FALSE;
  if (type->mode == TM_ARR) {
    size_node = type->u.arr_type->size;
    sexpr = size_node->attr;
    /* we already figured out the array size during check: */
    assert (size_node->code != N_IGNORE && sexpr->const_p && integer_type_p (sexpr->type));
    size_val = sexpr->u.i_val;
    init_object.u.curr_index = -1;
  } else {
    init_object.u.curr_member = NULL;
  }
  VARR_PUSH (init_object_t, init_object_path, init_object);
  for (; init != NULL; init = NL_NEXT (init)) {
    assert (init->code == N_INIT);
    des_list = NL_HEAD (init->ops);
    value = NL_NEXT (des_list);
    assert ((value->code != N_LIST && value->code != N_COMPOUND_LITERAL) || type->mode == TM_ARR
            || type->mode == TM_STRUCT || type->mode == TM_UNION);
    if ((curr_des = NL_HEAD (des_list->ops)) == NULL) {
      ok_p = update_path_and_do (c2m_ctx, collect_init_els, mark, value, const_only_p, NULL,
                                 init->pos, "");
      assert (ok_p);
    } else {
      for (; curr_des != NULL; curr_des = NL_NEXT (curr_des)) {
        VARR_TRUNC (init_object_t, init_object_path, mark + 1);
        init_object = VARR_POP (init_object_t, init_object_path);
        if (curr_des->code == N_FIELD_ID) {
          node_t id = NL_HEAD (curr_des->ops);

          /* field should be only in struct/union initializer */
          assert (type->mode == TM_STRUCT || type->mode == TM_UNION);
          found_p = symbol_find (c2m_ctx, S_REGULAR, id, type->u.tag_type, &sym);
          assert (found_p); /* field should present */
          process_init_field_designator (c2m_ctx, sym.def_node, init_object.container_type);
          ok_p = update_path_and_do (c2m_ctx, collect_init_els, mark, value, const_only_p, NULL,
                                     init->pos, "");
          assert (ok_p);
        } else {
          cexpr = curr_des->attr;
          /* index should be in array initializer and const expr of right type and value: */
          assert (type->mode == TM_ARR && cexpr->const_p && integer_type_p (cexpr->type)
                  && !incomplete_type_p (c2m_ctx, type) && size_val >= 0
                  && size_val > cexpr->u.u_val);
          init_object.u.curr_index = cexpr->u.i_val - 1;
          init_object.designator_p = FALSE;
          VARR_PUSH (init_object_t, init_object_path, init_object);
          ok_p = update_path_and_do (c2m_ctx, collect_init_els, mark, value, const_only_p, NULL,
                                     init->pos, "");
          assert (ok_p);
        }
      }
    }
  }
  VARR_TRUNC (init_object_t, init_object_path, mark);
}

static int cmp_init_el (const void *p1, const void *p2) {
  const init_el_t *el1 = p1, *el2 = p2;

  if (el1->offset < el2->offset)
    return -1;
  else if (el1->offset > el2->offset)
    return 1;
  else if (el1->member_decl != NULL && el2->member_decl != NULL
           && el1->member_decl->bit_offset < el2->member_decl->bit_offset)
    return -1;
  else if (el1->member_decl != NULL && el2->member_decl != NULL
           && el1->member_decl->bit_offset > el2->member_decl->bit_offset)
    return 1;
  else if (el1->num < el2->num)
    return 1;
  else if (el1->num > el2->num)
    return -1;
  else
    return 0;
}

static void move_item_to_module_start (MIR_module_t module, MIR_item_t item) {
  DLIST_REMOVE (MIR_item_t, module->items, item);
  DLIST_PREPEND (MIR_item_t, module->items, item);
}

static void move_item_forward (MIR_context_t ctx, MIR_item_t item) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);

  assert (curr_func != NULL);
  if (DLIST_TAIL (MIR_item_t, curr_func->module->items) != item) return;
  DLIST_REMOVE (MIR_item_t, curr_func->module->items, item);
  DLIST_INSERT_BEFORE (MIR_item_t, curr_func->module->items, curr_func, item);
}

static void gen_memset (MIR_context_t ctx, MIR_disp_t disp, MIR_reg_t base, mir_size_t len) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);
  MIR_type_t ret_type;
  MIR_var_t vars[3];
  MIR_op_t treg_op, args[6];
  MIR_module_t module;

  if (memset_item == NULL) {
    ret_type = get_int_mir_type (sizeof (mir_size_t));
    vars[0].name = "s";
    vars[0].type = get_int_mir_type (sizeof (mir_size_t));
    vars[1].name = "c";
    vars[1].type = get_int_mir_type (sizeof (mir_int));
    vars[2].name = "n";
    vars[2].type = get_int_mir_type (sizeof (mir_size_t));
    module = curr_func->module;
    memset_proto = MIR_new_proto_arr (ctx, "memset_p", 1, &ret_type, 3, vars);
    memset_item = MIR_new_import (ctx, "memset");
    move_item_to_module_start (module, memset_proto);
    move_item_to_module_start (module, memset_item);
  }
  args[0] = MIR_new_ref_op (ctx, memset_proto);
  args[1] = MIR_new_ref_op (ctx, memset_item);
  args[2] = get_new_temp (ctx, get_int_mir_type (sizeof (mir_size_t))).mir_op;
  if (disp == 0) {
    treg_op = MIR_new_reg_op (ctx, base);
  } else {
    treg_op = get_new_temp (ctx, get_int_mir_type (sizeof (mir_size_t))).mir_op;
    emit3 (ctx, MIR_ADD, treg_op, MIR_new_reg_op (ctx, base), MIR_new_int_op (ctx, disp));
  }
  args[3] = treg_op;
  args[4] = MIR_new_int_op (ctx, 0);
  args[5] = MIR_new_uint_op (ctx, len);
  emit_insn (ctx, MIR_new_insn_arr (ctx, MIR_CALL, 6 /* args + proto + func + res */, args));
}

static void gen_memcpy (MIR_context_t ctx, MIR_disp_t disp, MIR_reg_t base, op_t val,
                        mir_size_t len) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);
  MIR_type_t ret_type;
  MIR_var_t vars[3];
  MIR_op_t treg_op, args[6];
  MIR_module_t module;

  if (val.mir_op.u.mem.index == 0 && val.mir_op.u.mem.disp == disp && val.mir_op.u.mem.base == base)
    return;
  if (memcpy_item == NULL) {
    ret_type = get_int_mir_type (sizeof (mir_size_t));
    vars[0].name = "dest";
    vars[0].type = get_int_mir_type (sizeof (mir_size_t));
    vars[1].name = "src";
    vars[1].type = get_int_mir_type (sizeof (mir_size_t));
    vars[2].name = "n";
    vars[2].type = get_int_mir_type (sizeof (mir_size_t));
    module = curr_func->module;
    memcpy_proto = MIR_new_proto_arr (ctx, "memcpy_p", 1, &ret_type, 3, vars);
    memcpy_item = MIR_new_import (ctx, "memcpy");
    move_item_to_module_start (module, memcpy_proto);
    move_item_to_module_start (module, memcpy_item);
  }
  args[0] = MIR_new_ref_op (ctx, memcpy_proto);
  args[1] = MIR_new_ref_op (ctx, memcpy_item);
  args[2] = get_new_temp (ctx, get_int_mir_type (sizeof (mir_size_t))).mir_op;
  if (disp == 0) {
    treg_op = MIR_new_reg_op (ctx, base);
  } else {
    treg_op = get_new_temp (ctx, get_int_mir_type (sizeof (mir_size_t))).mir_op;
    emit3 (ctx, MIR_ADD, treg_op, MIR_new_reg_op (ctx, base), MIR_new_int_op (ctx, disp));
  }
  args[3] = treg_op;
  args[4] = mem_to_address (ctx, val).mir_op;
  args[5] = MIR_new_uint_op (ctx, len);
  emit_insn (ctx, MIR_new_insn_arr (ctx, MIR_CALL, 6 /* args + proto + func + res */, args));
}

static void emit_scalar_assign (MIR_context_t ctx, op_t var, op_t *val, MIR_type_t t,
                                int ignore_others_p) {
  if (var.decl == NULL || var.decl->bit_offset < 0) {
    emit2_noopt (ctx, tp_mov (t), var.mir_op, val->mir_op);
  } else {
    int width = var.decl->width;
    uint64_t mask, mask2;
    op_t temp_op1, temp_op2, temp_op3, temp_op4;

    assert (var.mir_op.mode == MIR_OP_MEM);
    mask = 0xffffffffffffffff >> (64 - width);
    mask2 = ~(mask << var.decl->bit_offset);
    temp_op1 = get_new_temp (ctx, MIR_T_I64);
    temp_op2 = get_new_temp (ctx, MIR_T_I64);
    temp_op3 = get_new_temp (ctx, MIR_T_I64);
    if (!ignore_others_p) {
      emit2_noopt (ctx, MIR_MOV, temp_op2.mir_op, var.mir_op);
      emit3 (ctx, MIR_AND, temp_op2.mir_op, temp_op2.mir_op, MIR_new_uint_op (ctx, mask2));
    }
    if (!signed_integer_type_p (var.decl->decl_spec.type)) {
      emit2 (ctx, MIR_MOV, temp_op1.mir_op, val->mir_op);
      *val = temp_op3;
    } else {
      emit3 (ctx, MIR_LSH, temp_op1.mir_op, val->mir_op, MIR_new_int_op (ctx, 64 - width));
      emit3 (ctx, MIR_RSH, temp_op1.mir_op, temp_op1.mir_op, MIR_new_int_op (ctx, 64 - width));
      *val = temp_op1;
    }
    emit3 (ctx, MIR_AND, temp_op3.mir_op, temp_op1.mir_op, MIR_new_uint_op (ctx, mask));
    temp_op4 = get_new_temp (ctx, MIR_T_I64);
    if (var.decl->bit_offset == 0) {
      temp_op4 = temp_op3;
    } else {
      emit3 (ctx, MIR_LSH, temp_op4.mir_op, temp_op3.mir_op,
             MIR_new_int_op (ctx, var.decl->bit_offset));
    }
    if (!ignore_others_p) {
      emit3 (ctx, MIR_OR, temp_op4.mir_op, temp_op4.mir_op, temp_op2.mir_op);
    }
    emit2 (ctx, MIR_MOV, var.mir_op, temp_op4.mir_op);
  }
}

static void add_bit_field (uint64_t *u, uint64_t v, decl_t member_decl) {
  uint64_t mask, mask2;
  int bit_offset = member_decl->bit_offset, width = member_decl->width;

  mask = 0xffffffffffffffff >> (64 - width);
  mask2 = ~(mask << bit_offset);
  *u &= mask2;
  v &= mask;
  if (signed_integer_type_p (member_decl->decl_spec.type)) {
    v <<= (64 - width);
    v = (int64_t) v >> (64 - width);
  }
  v <<= bit_offset;
  *u |= v;
}

static void gen_initializer (MIR_context_t ctx, size_t init_start, op_t var,
                             const char *global_name, mir_size_t size, int local_p) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);
  op_t val;
  size_t str_len;
  mir_size_t data_size, offset = 0, rel_offset = 0;
  init_el_t init_el, next_init_el;
  MIR_reg_t base;
  MIR_type_t t;
  MIR_item_t data;
  MIR_module_t module;
  struct expr *e;

  if (var.mir_op.mode == MIR_OP_REG) { /* scalar initialization: */
    assert (local_p && offset == 0 && VARR_LENGTH (init_el_t, init_els) - init_start == 1);
    init_el = VARR_GET (init_el_t, init_els, init_start);
    val = gen (ctx, init_el.init, NULL, NULL, TRUE, NULL);
    t = get_op_type (ctx, var);
    val = cast (ctx, val, get_mir_type (ctx, init_el.el_type), FALSE);
    emit_scalar_assign (ctx, var, &val, t, FALSE);
  } else if (local_p) { /* local variable initialization: */
    assert (var.mir_op.mode == MIR_OP_MEM && var.mir_op.u.mem.index == 0);
    offset = var.mir_op.u.mem.disp;
    base = var.mir_op.u.mem.base;
    for (size_t i = init_start; i < VARR_LENGTH (init_el_t, init_els); i++) {
      init_el = VARR_GET (init_el_t, init_els, i);
      t = get_mir_type (ctx, init_el.el_type);
      if (rel_offset < init_el.offset) { /* fill the gap: */
        gen_memset (ctx, offset + rel_offset, base, init_el.offset - rel_offset);
        rel_offset = init_el.offset;
      }
      if (t == MIR_T_UNDEF)
        val = new_op (NULL, MIR_new_mem_op (ctx, t, offset + rel_offset, base, 0, 1));
      val = gen (ctx, init_el.init, NULL, NULL, t != MIR_T_UNDEF, t != MIR_T_UNDEF ? NULL : &val);
      if (!scalar_type_p (init_el.el_type)) {
        mir_size_t s = init_el.init->code == N_STR ? init_el.init->u.s.len
                                                   : raw_type_size (c2m_ctx, init_el.el_type);

        gen_memcpy (ctx, offset + rel_offset, base, val, s);
        rel_offset = init_el.offset + s;
      } else {
        val = cast (ctx, val, get_mir_type (ctx, init_el.el_type), FALSE);
        emit_scalar_assign (ctx,
                            new_op (init_el.member_decl,
                                    MIR_new_mem_op (ctx, t, offset + init_el.offset, base, 0, 1)),
                            &val, t, i == init_start || rel_offset == init_el.offset);
        rel_offset = init_el.offset + _MIR_type_size (ctx, t);
      }
    }
    if (rel_offset < size) /* fill the tail: */
      gen_memset (ctx, offset + rel_offset, base, size - rel_offset);
  } else {
    assert (var.mir_op.mode == MIR_OP_REF);
    for (size_t i = init_start; i < VARR_LENGTH (init_el_t, init_els); i++) {
      init_el = VARR_GET (init_el_t, init_els, i);
      if (i != init_start && init_el.offset == VARR_GET (init_el_t, init_els, i - 1).offset)
        continue;
      e = init_el.init->attr;
      if (!e->const_addr_p) {
        if (e->const_p) {
          convert_value (e, init_el.el_type);
          e->type = init_el.el_type; /* to get the right value in the subsequent gen call */
        }
        val = gen (ctx, init_el.init, NULL, NULL, TRUE, NULL);
        assert (val.mir_op.mode == MIR_OP_INT || val.mir_op.mode == MIR_OP_UINT
                || val.mir_op.mode == MIR_OP_FLOAT || val.mir_op.mode == MIR_OP_DOUBLE
                || val.mir_op.mode == MIR_OP_LDOUBLE || val.mir_op.mode == MIR_OP_STR
                || val.mir_op.mode == MIR_OP_REF);
      }
      if (rel_offset < init_el.offset) { /* fill the gap: */
        data = MIR_new_bss (ctx, global_name, init_el.offset - rel_offset);
        if (global_name != NULL) var.decl->item = data;
        global_name = NULL;
      }
      t = get_mir_type (ctx, init_el.el_type);
      if (e->const_addr_p) {
        node_t def;

        if ((def = e->def_node) == NULL) { /* constant address */
          mir_size_t s = e->u.i_val;

          data = MIR_new_data (ctx, global_name, MIR_T_P, 1, &s);
          data_size = _MIR_type_size (ctx, MIR_T_P);
        } else {
          if (def->code != N_STR) {
            data = ((decl_t) def->attr)->item;
          } else {
            module = DLIST_TAIL (MIR_module_t, *MIR_get_module_list (ctx));
            data = MIR_new_string_data (ctx, _MIR_get_temp_item_name (ctx, module),
                                        (MIR_str_t){def->u.s.len, def->u.s.s});
            move_item_to_module_start (module, data);
          }
          data = MIR_new_ref_data (ctx, global_name, data, e->u.i_val);
          data_size = _MIR_type_size (ctx, t);
        }
      } else if (val.mir_op.mode == MIR_OP_REF) {
        data = MIR_new_ref_data (ctx, global_name, val.mir_op.u.ref, 0);
        data_size = _MIR_type_size (ctx, t);
      } else if (val.mir_op.mode != MIR_OP_STR) {
        union {
          int8_t i8;
          uint8_t u8;
          int16_t i16;
          uint16_t u16;
          int32_t i32;
          uint32_t u32;
          int64_t i64;
          uint64_t u64;
          float f;
          double d;
          long double ld;
        } u;
        if (init_el.member_decl != NULL && init_el.member_decl->bit_offset >= 0) {
          uint64_t u = 0;

          assert (val.mir_op.mode == MIR_OP_INT || val.mir_op.mode == MIR_OP_UINT);
          add_bit_field (&u, val.mir_op.u.u, init_el.member_decl);
          for (; i + 1 < VARR_LENGTH (init_el_t, init_els); i++, init_el = next_init_el) {
            next_init_el = VARR_GET (init_el_t, init_els, i + 1);
            if (next_init_el.offset != init_el.offset) break;
            if (next_init_el.member_decl->bit_offset == init_el.member_decl->bit_offset) continue;
            val = gen (ctx, next_init_el.init, NULL, NULL, TRUE, NULL);
            assert (val.mir_op.mode == MIR_OP_INT || val.mir_op.mode == MIR_OP_UINT);
            add_bit_field (&u, val.mir_op.u.u, next_init_el.member_decl);
          }
          val.mir_op.u.u = u;
        }
        switch (t) {
        case MIR_T_I8: u.i8 = val.mir_op.u.i; break;
        case MIR_T_U8: u.u8 = val.mir_op.u.u; break;
        case MIR_T_I16: u.i16 = val.mir_op.u.i; break;
        case MIR_T_U16: u.u16 = val.mir_op.u.u; break;
        case MIR_T_I32: u.i32 = val.mir_op.u.i; break;
        case MIR_T_U32: u.u32 = val.mir_op.u.u; break;
        case MIR_T_I64: u.i64 = val.mir_op.u.i; break;
        case MIR_T_U64: u.u64 = val.mir_op.u.u; break;
        case MIR_T_F: u.f = val.mir_op.u.f; break;
        case MIR_T_D: u.d = val.mir_op.u.d; break;
        case MIR_T_LD: u.ld = val.mir_op.u.ld; break;
        default: assert (FALSE);
        }
        data = MIR_new_data (ctx, global_name, t, 1, &u);
        data_size = _MIR_type_size (ctx, t);
      } else if (init_el.el_type->mode == TM_ARR) {
        data_size = raw_type_size (c2m_ctx, init_el.el_type);
        str_len = val.mir_op.u.str.len;
        if (data_size < str_len) {
          data = MIR_new_data (ctx, global_name, MIR_T_U8, data_size, val.mir_op.u.str.s);
        } else {
          data = MIR_new_string_data (ctx, global_name, val.mir_op.u.str);
          if (data_size > str_len) MIR_new_bss (ctx, NULL, data_size - str_len);
        }
      } else {
        module = DLIST_TAIL (MIR_module_t, *MIR_get_module_list (ctx));
        data = MIR_new_string_data (ctx, _MIR_get_temp_item_name (ctx, module), val.mir_op.u.str);
        move_item_to_module_start (module, data);
        data = MIR_new_ref_data (ctx, global_name, data, 0);
        data_size = _MIR_type_size (ctx, t);
      }
      if (global_name != NULL) var.decl->item = data;
      global_name = NULL;
      rel_offset = init_el.offset + data_size;
    }
    if (rel_offset < size) { /* fill the tail: */
      data = MIR_new_bss (ctx, global_name, size - rel_offset);
      if (global_name != NULL) var.decl->item = data;
    }
  }
}

static MIR_item_t get_ref_item (MIR_context_t ctx, node_t def, const char *name) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);
  struct decl *decl = def->attr;

  if (def->code == N_FUNC_DEF
      || (def->code == N_SPEC_DECL && NL_EL (def->ops, 1)->code == N_DECL
          && decl->scope == top_scope && decl->decl_spec.type->mode != TM_FUNC
          && !decl->decl_spec.typedef_p && !decl->decl_spec.extern_p))
    return (decl->decl_spec.linkage == N_EXTERN ? MIR_new_export (ctx, name)
                                                : MIR_new_forward (ctx, name));
  return NULL;
}

static void emit_bin_op (MIR_context_t ctx, node_t r, struct type *type, op_t res, op_t op1,
                         op_t op2) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);
  op_t temp;

  if (type->mode == TM_PTR) { /* ptr +/- int */
    assert (r->code == N_ADD || r->code == N_SUB || r->code == N_ADD_ASSIGN
            || r->code == N_SUB_ASSIGN);
    if (((struct expr *) NL_HEAD (r->ops)->attr)->type->mode != TM_PTR) /* int + ptr */
      SWAP (op1, op2, temp);
    if (op2.mir_op.mode == MIR_OP_INT || op2.mir_op.mode == MIR_OP_UINT) {
      op2 = new_op (NULL,
                    MIR_new_int_op (ctx, op2.mir_op.u.i * type_size (c2m_ctx, type->u.ptr_type)));
    } else {
      temp = get_new_temp (ctx, get_mir_type (ctx, type));
      emit3 (ctx, sizeof (mir_size_t) == 8 ? MIR_MUL : MIR_MULS, temp.mir_op, op2.mir_op,
             MIR_new_int_op (ctx, type_size (c2m_ctx, type->u.ptr_type)));
      op2 = temp;
    }
  }
  emit3 (ctx, get_mir_type_insn_code (ctx, type, r), res.mir_op, op1.mir_op, op2.mir_op);
  if (type->mode != TM_PTR
      && (type = ((struct expr *) NL_HEAD (r->ops)->attr)->type)->mode == TM_PTR) { /* ptr - ptr */
    assert (r->code == N_SUB || r->code == N_SUB_ASSIGN);
    emit3 (ctx, sizeof (mir_size_t) == 8 ? MIR_DIV : MIR_DIVS, res.mir_op, res.mir_op,
           MIR_new_int_op (ctx, type_size (c2m_ctx, type->u.ptr_type)));
  }
}

static int signed_case_compare (const void *v1, const void *v2) {
  case_t c1 = *(const case_t *) v1, c2 = *(const case_t *) v2;
  struct expr *e1 = NL_HEAD (c1->case_node->ops)->attr;
  struct expr *e2 = NL_HEAD (c2->case_node->ops)->attr;

  assert (e1->u.i_val != e2->u.i_val);
  return e1->u.i_val < e2->u.i_val ? -1 : 1;
}

static int unsigned_case_compare (const void *v1, const void *v2) {
  case_t c1 = *(const case_t *) v1, c2 = *(const case_t *) v2;
  struct expr *e1 = NL_HEAD (c1->case_node->ops)->attr;
  struct expr *e2 = NL_HEAD (c2->case_node->ops)->attr;

  assert (e1->u.u_val != e2->u.u_val);
  return e1->u.u_val < e2->u.u_val ? -1 : 1;
}

static op_t gen (MIR_context_t ctx, node_t r, MIR_label_t true_label, MIR_label_t false_label,
                 int val_p, op_t *desirable_dest) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);
  op_t res, op1, op2, var, val;
  MIR_type_t t;
  MIR_insn_code_t insn_code;
  MIR_type_t mir_type;
  struct expr *e;
  struct type *type;
  decl_t decl;
  long double ld;
  long long ll;
  unsigned long long ull;
  int expr_attr_p, stmt_p;

  classify_node (r, &expr_attr_p, &stmt_p);
  assert ((true_label == NULL && false_label == NULL)
          || (true_label != NULL && false_label != NULL));
  assert (!val_p || desirable_dest == NULL);
  if (r->code != N_ANDAND && r->code != N_OROR && expr_attr_p && push_const_val (ctx, r, &res))
    goto finish;
  switch (r->code) {
  case N_LIST:
    for (node_t n = NL_HEAD (r->ops); n != NULL; n = NL_NEXT (n))
      gen (ctx, n, true_label, false_label, val_p, NULL);
    break;
  case N_IGNORE: break; /* do nothing */
  case N_I:
  case N_L: ll = r->u.l; goto int_val;
  case N_LL:
    ll = r->u.ll;
  int_val:
    res = new_op (NULL, MIR_new_int_op (ctx, ll));
    break;
  case N_U:
  case N_UL: ull = r->u.ul; goto uint_val;
  case N_ULL:
    ull = r->u.ull;
  uint_val:
    res = new_op (NULL, MIR_new_uint_op (ctx, ull));
    break;
  case N_F: ld = r->u.f; goto float_val;
  case N_D: ld = r->u.d; goto float_val;
  case N_LD:
    ld = r->u.ld;
  float_val:
    mir_type = get_mir_type (ctx, ((struct expr *) r->attr)->type);
    res = new_op (NULL, (mir_type == MIR_T_F ? MIR_new_float_op (ctx, ld)
                                             : mir_type == MIR_T_D ? MIR_new_double_op (ctx, ld)
                                                                   : MIR_new_ldouble_op (ctx, ld)));
    break;
  case N_CH: ll = r->u.ch; goto int_val;
  case N_STR:
    res
      = new_op (NULL,
                MIR_new_str_op (ctx, (MIR_str_t){r->u.s.len, r->u.s.s}));  //???what to do with decl
                                                                           // and str in initializer
    break;
  case N_COMMA:
    gen (ctx, NL_HEAD (r->ops), NULL, NULL, FALSE, NULL);
    res = gen (ctx, NL_EL (r->ops, 1), true_label, false_label, TRUE, NULL);
    break;
  case N_ANDAND:
  case N_OROR:
    if (!push_const_val (ctx, r, &res)) {
      MIR_label_t temp_label = MIR_new_label (ctx), t_label = true_label, f_label = false_label;
      int make_val_p = t_label == NULL;

      if (make_val_p) {
        t_label = MIR_new_label (ctx);
        f_label = MIR_new_label (ctx);
      }
      assert (t_label != NULL && f_label != NULL);
      gen (ctx, NL_HEAD (r->ops), r->code == N_ANDAND ? temp_label : t_label,
           r->code == N_ANDAND ? f_label : temp_label, FALSE, NULL);
      emit_insn (ctx, temp_label);
      gen (ctx, NL_EL (r->ops, 1), t_label, f_label, FALSE, NULL);
      if (make_val_p) {
        MIR_label_t end_label = MIR_new_label (ctx);

        type = ((struct expr *) r->attr)->type;
        res = get_new_temp (ctx, get_mir_type (ctx, type));
        emit_insn (ctx, t_label);
        emit2 (ctx, MIR_MOV, res.mir_op, one_op.mir_op);
        emit1 (ctx, MIR_JMP, MIR_new_label_op (ctx, end_label));
        emit_insn (ctx, f_label);
        emit2 (ctx, MIR_MOV, res.mir_op, zero_op.mir_op);
        emit_insn (ctx, end_label);
      }
      true_label = false_label = NULL;
    } else if (true_label != NULL) {
      int true_p;

      assert (res.mir_op.mode == MIR_OP_INT || res.mir_op.mode == MIR_OP_UINT
              || res.mir_op.mode == MIR_OP_FLOAT || res.mir_op.mode == MIR_OP_DOUBLE);
      true_p = ((res.mir_op.mode == MIR_OP_INT && res.mir_op.u.i != 0)
                || (res.mir_op.mode == MIR_OP_UINT && res.mir_op.u.u != 0)
                || (res.mir_op.mode == MIR_OP_FLOAT && res.mir_op.u.f != 0.0f)
                || (res.mir_op.mode == MIR_OP_DOUBLE && res.mir_op.u.d != 0.0));
      emit1 (ctx, MIR_JMP, MIR_new_label_op (ctx, true_p ? true_label : false_label));
      true_label = false_label = NULL;
    }
    break;
  case N_BITWISE_NOT:
    gen_unary_op (ctx, r, &op1, &res);
    t = get_mir_type (ctx, ((struct expr *) r->attr)->type);
    emit3 (ctx, t == MIR_T_I64 || t == MIR_T_U64 ? MIR_XOR : MIR_XORS, res.mir_op, op1.mir_op,
           minus_one_op.mir_op);
    break;
  case N_NOT:
    if (true_label != NULL) {
      gen (ctx, NL_HEAD (r->ops), false_label, true_label, FALSE, NULL);
      true_label = false_label = NULL;
    } else {
      MIR_label_t end_label = MIR_new_label (ctx);
      MIR_label_t t_label = MIR_new_label (ctx), f_label = MIR_new_label (ctx);

      res = get_new_temp (ctx, MIR_T_I64);
      gen (ctx, NL_HEAD (r->ops), t_label, f_label, FALSE, NULL);
      emit_insn (ctx, t_label);
      emit2 (ctx, MIR_MOV, res.mir_op, zero_op.mir_op);
      emit1 (ctx, MIR_JMP, MIR_new_label_op (ctx, end_label));
      emit_insn (ctx, f_label);
      emit2 (ctx, MIR_MOV, res.mir_op, one_op.mir_op);
      emit_insn (ctx, end_label);
    }
    break;
  case N_ADD:
  case N_SUB:
    if (NL_NEXT (NL_HEAD (r->ops)) == NULL) { /* unary */
      MIR_insn_code_t ic = get_mir_insn_code (ctx, r);

      gen_unary_op (ctx, r, &op1, &res);
      if (r->code == N_ADD) {
        ic = (ic == MIR_FADD ? MIR_FMOV
                             : ic == MIR_DADD ? MIR_DMOV : ic == MIR_LDADD ? MIR_LDMOV : MIR_MOV);
        emit2 (ctx, ic, res.mir_op, op1.mir_op);
      } else {
        ic
          = (ic == MIR_FSUB
               ? MIR_FNEG
               : ic == MIR_DSUB ? MIR_DNEG
                                : ic == MIR_LDSUB ? MIR_LDNEG : ic == MIR_SUB ? MIR_NEG : MIR_NEGS);
        emit2 (ctx, ic, res.mir_op, op1.mir_op);
      }
      break;
    }
  /* Fall through: */
  case N_AND:
  case N_OR:
  case N_XOR:
  case N_LSH:
  case N_RSH:
  case N_MUL:
  case N_DIV:
  case N_MOD:
    gen_bin_op (ctx, r, &op1, &op2, &res);
    emit_bin_op (ctx, r, ((struct expr *) r->attr)->type, res, op1, op2);
    break;
  case N_EQ:
  case N_NE:
  case N_LT:
  case N_LE:
  case N_GT:
  case N_GE: {
    struct type *type1 = ((struct expr *) NL_HEAD (r->ops)->attr)->type;
    struct type *type2 = ((struct expr *) NL_EL (r->ops, 1)->attr)->type;
    struct type type, ptr_type = get_ptr_int_type (FALSE);

    type = arithmetic_conversion (type1->mode == TM_PTR ? &ptr_type : type1,
                                  type2->mode == TM_PTR ? &ptr_type : type2);
    set_type_layout (c2m_ctx, &type);
    gen_cmp_op (ctx, r, &type, &op1, &op2, &res);
    insn_code = get_mir_type_insn_code (ctx, &type, r);
    if (true_label == NULL) {
      emit3 (ctx, insn_code, res.mir_op, op1.mir_op, op2.mir_op);
    } else {
      insn_code = get_compare_branch_code (insn_code);
      emit3 (ctx, insn_code, MIR_new_label_op (ctx, true_label), op1.mir_op, op2.mir_op);
      emit1 (ctx, MIR_JMP, MIR_new_label_op (ctx, false_label));
      true_label = false_label = NULL;
    }
    break;
  }
  case N_POST_INC:
  case N_POST_DEC: {
    struct type *type = ((struct expr *) r->attr)->type2;

    t = get_mir_type (ctx, type);
    var = gen (ctx, NL_HEAD (r->ops), NULL, NULL, FALSE, NULL);
    op1 = force_val (ctx, var, FALSE);
    res = get_new_temp (ctx, t);
    emit2 (ctx, tp_mov (t), res.mir_op, op1.mir_op);
    val = promote (ctx, op1, t, TRUE);
    op2 = promote (ctx,
                   type->mode != TM_PTR
                     ? one_op
                     : new_op (NULL, MIR_new_int_op (ctx, type_size (c2m_ctx, type->u.ptr_type))),
                   t, FALSE);
    emit3 (ctx, get_mir_insn_code (ctx, r), val.mir_op, val.mir_op, op2.mir_op);
    t = promote_mir_int_type (t);
    goto assign;
  }
  case N_INC:
  case N_DEC: {
    struct type *type = ((struct expr *) r->attr)->type2;

    t = get_mir_type (ctx, type);
    var = gen (ctx, NL_HEAD (r->ops), NULL, NULL, FALSE, NULL);
    val = promote (ctx, force_val (ctx, var, FALSE), t, TRUE);
    op2 = promote (ctx,
                   type->mode != TM_PTR
                     ? one_op
                     : new_op (NULL, MIR_new_int_op (ctx, type_size (c2m_ctx, type->u.ptr_type))),
                   t, FALSE);
    t = promote_mir_int_type (t);
    res = get_new_temp (ctx, t);
    emit3 (ctx, get_mir_insn_code (ctx, r), val.mir_op, val.mir_op, op2.mir_op);
    goto assign;
  }
  case N_AND_ASSIGN:
  case N_OR_ASSIGN:
  case N_XOR_ASSIGN:
  case N_LSH_ASSIGN:
  case N_RSH_ASSIGN:
  case N_ADD_ASSIGN:
  case N_SUB_ASSIGN:
  case N_MUL_ASSIGN:
  case N_DIV_ASSIGN:
  case N_MOD_ASSIGN:
    gen_assign_bin_op (ctx, r, ((struct expr *) r->attr)->type2, &val, &op2, &var);
    emit_bin_op (ctx, r, ((struct expr *) r->attr)->type2, val, val, op2);
    t = get_op_type (ctx, var);
    t = promote_mir_int_type (t);
    res = get_new_temp (ctx, t);
    goto assign;
    break;
  case N_ASSIGN:
    var = gen (ctx, NL_HEAD (r->ops), NULL, NULL, FALSE, NULL);
    t = get_op_type (ctx, var);
    op2
      = gen (ctx, NL_EL (r->ops, 1), NULL, NULL, t != MIR_T_UNDEF, t != MIR_T_UNDEF ? NULL : &var);
    if (t == MIR_T_UNDEF) {
      res = var;
      val = op2;
    } else {
      t = promote_mir_int_type (t);
      val = promote (ctx, op2, t, TRUE);
      res = get_new_temp (ctx, t);
    }
  assign: /* t/val is promoted type/new value of assign expression */
    if (scalar_type_p (((struct expr *) r->attr)->type)) {
      assert (t != MIR_T_UNDEF);
      val = cast (ctx, val, get_mir_type (ctx, ((struct expr *) r->attr)->type), FALSE);
      emit_scalar_assign (ctx, var, &val, t, FALSE);
      if (r->code != N_POST_INC && r->code != N_POST_DEC)
        emit2_noopt (ctx, tp_mov (t), res.mir_op, val.mir_op);
    } else { /* block move */
      mir_size_t size = type_size (c2m_ctx, ((struct expr *) r->attr)->type);

      assert (r->code == N_ASSIGN);
      block_move (ctx, var, val, size);
    }
    break;
  case N_ID: {
    e = r->attr;
    assert (!e->const_p);
    if (e->lvalue_node == NULL) {
      res = new_op (NULL, MIR_new_ref_op (ctx, ((decl_t) e->def_node->attr)->item));
    } else if ((decl = e->lvalue_node->attr)->scope == top_scope || decl->decl_spec.static_p
               || decl->decl_spec.linkage != N_IGNORE) {
      t = get_mir_type (ctx, e->type);
      res = get_new_temp (ctx, MIR_T_I64);
      emit2 (ctx, MIR_MOV, res.mir_op, MIR_new_ref_op (ctx, decl->item));
      res = new_op (decl, MIR_new_mem_op (ctx, t, 0, res.mir_op.u.reg, 0, 1));
    } else if (!decl->reg_p) {
      t = get_mir_type (ctx, e->type);
      res = new_op (decl, MIR_new_mem_op (ctx, t, decl->offset,
                                          MIR_reg (ctx, FP_NAME, curr_func->u.func), 0, 1));
    } else {
      const char *name;
      reg_var_t reg_var;

      t = get_mir_type (ctx, e->type);
      assert (t != MIR_T_UNDEF);
      t = promote_mir_int_type (t);
      name = get_reg_var_name (ctx, t, r->u.s.s,
                               ((struct node_scope *) decl->scope->attr)->func_scope_num);
      reg_var = get_reg_var (ctx, t, name);
      res = new_op (decl, MIR_new_reg_op (ctx, reg_var.reg));
    }
    break;
  }
  case N_IND: {
    MIR_type_t ind_t;
    node_t arr = NL_HEAD (r->ops);
    mir_size_t size = type_size (c2m_ctx, ((struct expr *) r->attr)->type);

    t = get_mir_type (ctx, ((struct expr *) r->attr)->type);
    op1 = gen (ctx, arr, NULL, NULL, TRUE, NULL);
    op2 = gen (ctx, NL_EL (r->ops, 1), NULL, NULL, TRUE, NULL);
    ind_t = get_mir_type (ctx, ((struct expr *) NL_EL (r->ops, 1)->attr)->type);
    op2 = force_reg (ctx, op2, ind_t);
    if (((struct expr *) arr->attr)->type->arr_type != NULL) { /* it was an array */
      size = type_size (c2m_ctx, ((struct expr *) arr->attr)->type->arr_type->u.arr_type->el_type);
      op1 = force_reg_or_mem (ctx, op1, MIR_T_I64);
      assert (op1.mir_op.mode == MIR_OP_REG || op1.mir_op.mode == MIR_OP_MEM);
    } else {
      op1 = force_reg (ctx, op1, MIR_T_I64);
      assert (op1.mir_op.mode == MIR_OP_REG);
    }
    res = op1;
    res.decl = NULL;
    if (res.mir_op.mode == MIR_OP_REG)
      res.mir_op = MIR_new_mem_op (ctx, t, 0, res.mir_op.u.reg, 0, 1);
    if (res.mir_op.u.mem.base == 0 && size == 1) {
      res.mir_op.u.mem.base = op2.mir_op.u.reg;
    } else if (res.mir_op.u.mem.index == 0 && size <= MIR_MAX_SCALE) {
      res.mir_op.u.mem.index = op2.mir_op.u.reg;
      res.mir_op.u.mem.scale = size;
    } else {
      op_t temp_op;

      temp_op = get_new_temp (ctx, MIR_T_I64);
      emit3 (ctx, MIR_MUL, temp_op.mir_op, op2.mir_op, MIR_new_int_op (ctx, size));
      if (res.mir_op.u.mem.base != 0)
        emit3 (ctx, MIR_ADD, temp_op.mir_op, temp_op.mir_op,
               MIR_new_reg_op (ctx, res.mir_op.u.mem.base));
      res.mir_op.u.mem.base = temp_op.mir_op.u.reg;
    }
    res.mir_op.u.mem.type = t;
    break;
  }
  case N_ADDR: {
    int add_p = FALSE;

    op1 = gen (ctx, NL_HEAD (r->ops), NULL, NULL, FALSE, NULL);
    if (op1.mir_op.mode == MIR_OP_REG || op1.mir_op.mode == MIR_OP_REF
        || op1.mir_op.mode == MIR_OP_STR) { /* array or func */
      res = op1;
      res.decl = NULL;
      break;
    }
    assert (op1.mir_op.mode == MIR_OP_MEM);
    t = get_mir_type (ctx, ((struct expr *) r->attr)->type);
    res = get_new_temp (ctx, t);
    if (op1.mir_op.u.mem.index != 0) {
      emit3 (ctx, MIR_MUL, res.mir_op, MIR_new_reg_op (ctx, op1.mir_op.u.mem.index),
             MIR_new_int_op (ctx, op1.mir_op.u.mem.scale));
      add_p = TRUE;
    }
    if (op1.mir_op.u.mem.disp != 0) {
      if (add_p)
        emit3 (ctx, MIR_ADD, res.mir_op, res.mir_op, MIR_new_int_op (ctx, op1.mir_op.u.mem.disp));
      else
        emit2 (ctx, MIR_MOV, res.mir_op, MIR_new_int_op (ctx, op1.mir_op.u.mem.disp));
      add_p = TRUE;
    }
    if (op1.mir_op.u.mem.base != 0) {
      if (add_p)
        emit3 (ctx, MIR_ADD, res.mir_op, res.mir_op, MIR_new_reg_op (ctx, op1.mir_op.u.mem.base));
      else
        emit2 (ctx, MIR_MOV, res.mir_op, MIR_new_reg_op (ctx, op1.mir_op.u.mem.base));
    }
    break;
  }
  case N_DEREF:
    op1 = gen (ctx, NL_HEAD (r->ops), NULL, NULL, TRUE, NULL);
    op1 = force_reg (ctx, op1, MIR_T_I64);
    assert (op1.mir_op.mode == MIR_OP_REG);
    if ((type = ((struct expr *) r->attr)->type)->mode == TM_PTR
        && type->u.ptr_type->mode == TM_FUNC) {
      res = op1;
    } else {
      t = get_mir_type (ctx, type);
      op1.mir_op = MIR_new_mem_op (ctx, t, 0, op1.mir_op.u.reg, 0, 1);
      res = new_op (NULL, op1.mir_op);
    }
    break;
  case N_FIELD:
  case N_DEREF_FIELD: {
    node_t def_node;

    e = r->attr;
    def_node = e->lvalue_node;
    assert (def_node != NULL && def_node->code == N_MEMBER);
    decl = def_node->attr;
    op1 = gen (ctx, NL_HEAD (r->ops), NULL, NULL, r->code == N_DEREF_FIELD, NULL);
    t = get_mir_type (ctx, decl->decl_spec.type);
    if (r->code == N_FIELD) {
      assert (op1.mir_op.mode == MIR_OP_MEM);
      op1.mir_op
        = MIR_new_mem_op (ctx, t, op1.mir_op.u.mem.disp + decl->offset, op1.mir_op.u.mem.base,
                          op1.mir_op.u.mem.index, op1.mir_op.u.mem.scale);
    } else {
      op1 = force_reg (ctx, op1, MIR_T_I64);
      assert (op1.mir_op.mode == MIR_OP_REG);
      op1.mir_op = MIR_new_mem_op (ctx, t, decl->offset, op1.mir_op.u.reg, 0, 1);
    }
    res = new_op (decl, op1.mir_op);
    break;
  }
  case N_COND: {
    node_t cond = NL_HEAD (r->ops);
    node_t true_expr = NL_NEXT (cond);
    node_t false_expr = NL_NEXT (true_expr);
    MIR_label_t true_label = MIR_new_label (ctx), false_label = MIR_new_label (ctx);
    MIR_label_t end_label = MIR_new_label (ctx);
    struct type *type = ((struct expr *) r->attr)->type;
    op_t addr;
    int void_p = void_type_p (type);
    mir_size_t size = type_size (c2m_ctx, ((struct expr *) r->attr)->type);

    if (!void_p) t = get_mir_type (ctx, type);
    gen (ctx, cond, true_label, false_label, FALSE, NULL);
    emit_insn (ctx, true_label);
    op1 = gen (ctx, true_expr, NULL, NULL, !void_p && t != MIR_T_UNDEF, NULL);
    if (!void_p) {
      if (t != MIR_T_UNDEF) {
        res = get_new_temp (ctx, t);
        emit2 (ctx, tp_mov (t), res.mir_op, op1.mir_op);
      } else if (desirable_dest == NULL) {
        res = get_new_temp (ctx, MIR_T_I64);
        addr = mem_to_address (ctx, op1);
        emit2 (ctx, MIR_MOV, res.mir_op, addr.mir_op);
      } else {
        block_move (ctx, *desirable_dest, op1, size);
        res = *desirable_dest;
      }
    }
    emit1 (ctx, MIR_JMP, MIR_new_label_op (ctx, end_label));
    emit_insn (ctx, false_label);
    op1 = gen (ctx, false_expr, NULL, NULL, !void_p && t != MIR_T_UNDEF, NULL);
    if (!void_p) {
      if (t != MIR_T_UNDEF) {
        emit2 (ctx, tp_mov (t), res.mir_op, op1.mir_op);
      } else if (desirable_dest == NULL) {
        addr = mem_to_address (ctx, op1);
        emit2 (ctx, MIR_MOV, res.mir_op, addr.mir_op);
        res = new_op (NULL, MIR_new_mem_op (ctx, MIR_T_I8, 0, res.mir_op.u.reg, 0, 1));
      } else {
        block_move (ctx, res, op1, size);
      }
    }
    emit_insn (ctx, end_label);
    break;
  }
  case N_ALIGNOF:
  case N_SIZEOF:
  case N_EXPR_SIZEOF: assert (FALSE); break;
  case N_CAST:
    assert (!((struct expr *) r->attr)->const_p);
    op1 = gen (ctx, NL_EL (r->ops, 1), NULL, NULL, TRUE, NULL);
    type = ((struct expr *) r->attr)->type;
    if (void_type_p (type)) {
      res = op1;
      res.decl = NULL;
      res.mir_op.mode = MIR_OP_UNDEF;
    } else {
      t = get_mir_type (ctx, type);
      res = cast (ctx, op1, t, TRUE);
    }
    break;
  case N_COMPOUND_LITERAL: {
    const char *global_name = NULL;
    node_t type_name = NL_HEAD (r->ops);
    decl_t decl = type_name->attr;
    struct expr *expr = r->attr;
    MIR_module_t module = DLIST_TAIL (MIR_module_t, *MIR_get_module_list (ctx));
    size_t init_start;

    if (decl->scope == top_scope) {
      assert (decl->item == NULL);
      global_name = _MIR_get_temp_item_name (ctx, module);
    }
    init_start = VARR_LENGTH (init_el_t, init_els);
    collect_init_els (c2m_ctx, NULL, &decl->decl_spec.type, NL_EL (r->ops, 1),
                      decl->scope == top_scope || decl->decl_spec.linkage == N_STATIC
                        || decl->decl_spec.linkage == N_EXTERN || decl->decl_spec.static_p
                        || decl->decl_spec.thread_local_p,
                      TRUE);
    if (decl->scope == top_scope)
      qsort (VARR_ADDR (init_el_t, init_els) + init_start,
             VARR_LENGTH (init_el_t, init_els) - init_start, sizeof (init_el_t), cmp_init_el);
    if (decl->scope == top_scope || decl->decl_spec.static_p) {
      var = new_op (decl, MIR_new_ref_op (ctx, NULL));
    } else {
      t = get_mir_type (ctx, expr->type);
      var = new_op (decl, MIR_new_mem_op (ctx, t, decl->offset,
                                          MIR_reg (ctx, FP_NAME, curr_func->u.func), 0, 1));
    }
    gen_initializer (ctx, init_start, var, global_name,
                     raw_type_size (c2m_ctx, decl->decl_spec.type),
                     decl->scope != top_scope && !decl->decl_spec.static_p);
    VARR_TRUNC (init_el_t, init_els, init_start);
    if (var.mir_op.mode == MIR_OP_REF) var.mir_op.u.ref = var.decl->item;
    res = var;
    break;
  }
  case N_CALL: {
    node_t func = NL_HEAD (r->ops), param_list, param, args = NL_EL (r->ops, 1);
    struct decl_spec *decl_spec;
    size_t ops_start;
    struct expr *call_expr = r->attr, *func_expr;
    struct type *func_type, *type = call_expr->type;
    MIR_item_t proto_item;
    mir_size_t saved_call_arg_area_offset_before_args;
    int va_arg_p = call_expr->builtin_call_p && strcmp (func->u.s.s, BUILTIN_VA_ARG) == 0;
    int va_start_p = call_expr->builtin_call_p && strcmp (func->u.s.s, BUILTIN_VA_START) == 0;
    int alloca_p = call_expr->builtin_call_p && strcmp (func->u.s.s, ALLOCA) == 0;
    int builtin_call_p = alloca_p || va_arg_p || va_start_p, inline_p = FALSE;
    int struct_p;

    ops_start = VARR_LENGTH (MIR_op_t, call_ops);
    if (!builtin_call_p) {
      func_expr = func->attr;
      func_type = func_expr->type;
      assert (func_type->mode == TM_PTR && func_type->u.ptr_type->mode == TM_FUNC);
      func_type = func_type->u.ptr_type;
      proto_item = func_type->u.func_type->proto_item;  // ???
      VARR_PUSH (MIR_op_t, call_ops, MIR_new_ref_op (ctx, proto_item));
      op1 = gen (ctx, func, NULL, NULL, TRUE, NULL);
      if (op1.mir_op.mode == MIR_OP_REF && func->code == N_ID
          && ((decl_t) func_expr->def_node->attr)->decl_spec.inline_p)
        inline_p = TRUE;
      VARR_PUSH (MIR_op_t, call_ops, op1.mir_op);
    }
    if (scalar_type_p (type)) {
      t = get_mir_type (ctx, type);
      t = promote_mir_int_type (t);
      res = get_new_temp (ctx, t);
      VARR_PUSH (MIR_op_t, call_ops, res.mir_op);
    } else if (type->mode == TM_STRUCT || type->mode == TM_UNION) {
      node_t block = NL_EL (curr_func_def->ops, 3);
      struct node_scope *ns = block->attr;

      res = get_new_temp (ctx, MIR_T_I64);
      emit3 (ctx, MIR_ADD, res.mir_op,
             MIR_new_reg_op (ctx, MIR_reg (ctx, FP_NAME, curr_func->u.func)),
             MIR_new_int_op (ctx, curr_call_arg_area_offset + ns->size - ns->call_arg_area_size));
      if (!builtin_call_p) update_call_arg_area_offset (c2m_ctx, type, FALSE);
      VARR_PUSH (MIR_op_t, call_ops, res.mir_op);
      res.mir_op = MIR_new_mem_op (ctx, MIR_T_UNDEF, 0, res.mir_op.u.reg, 0, 1);
      t = MIR_T_I64;
    }
    saved_call_arg_area_offset_before_args = curr_call_arg_area_offset;
    if (va_arg_p) {
      op1 = get_new_temp (ctx, MIR_T_I64);
      op2 = gen (ctx, NL_HEAD (args->ops), NULL, NULL, TRUE, NULL);
      MIR_append_insn (ctx, curr_func,
                       MIR_new_insn (ctx, MIR_VA_ARG, op1.mir_op, op2.mir_op,
                                     MIR_new_mem_op (ctx, t, 0, 0, 0, 1)));
      op2 = get_new_temp (ctx, t);
      MIR_append_insn (ctx, curr_func,
                       MIR_new_insn (ctx, tp_mov (t), op2.mir_op,
                                     MIR_new_mem_op (ctx, t, 0, op1.mir_op.u.reg, 0, 1)));
      if (res.mir_op.mode == MIR_OP_REG) {
        res = op2;
      } else {
        assert (res.mir_op.mode == MIR_OP_MEM);
        res.mir_op.u.mem.base = op2.mir_op.u.reg;
      }
    } else if (va_start_p) {
      op1 = gen (ctx, NL_HEAD (args->ops), NULL, NULL, TRUE, NULL);
      MIR_append_insn (ctx, curr_func, MIR_new_insn (ctx, MIR_VA_START, op1.mir_op));
    } else if (alloca_p) {
      res = get_new_temp (ctx, t);
      op1 = gen (ctx, NL_HEAD (args->ops), NULL, NULL, TRUE, NULL);
      MIR_append_insn (ctx, curr_func, MIR_new_insn (ctx, MIR_ALLOCA, res.mir_op, op1.mir_op));
    } else {
      param_list = func_type->u.func_type->param_list;
      param = NL_HEAD (param_list->ops);
      for (node_t arg = NL_HEAD (args->ops); arg != NULL; arg = NL_NEXT (arg)) {
        e = arg->attr;
        struct_p = e->type->mode == TM_STRUCT || e->type->mode == TM_UNION;
        op2 = gen (ctx, arg, NULL, NULL, !struct_p, NULL);
        assert (param != NULL || NL_HEAD (param_list->ops) == NULL
                || func_type->u.func_type->dots_p);
        if (struct_p) { /* pass an adress of struct/union: */
          assert (op2.mir_op.mode == MIR_OP_MEM);
          op2 = mem_to_address (ctx, op2);
        } else if (param != NULL) {
          assert (param->code == N_SPEC_DECL || param->code == N_TYPE);
          decl_spec = get_param_decl_spec (param);
          t = get_mir_type (ctx, decl_spec->type);
          t = promote_mir_int_type (t);
          op2 = promote (ctx, op2, t, FALSE);
        } else {
          t = get_mir_type (ctx, e->type);
          t = promote_mir_int_type (t);
          op2 = promote (ctx, op2, t == MIR_T_F ? MIR_T_D : t, FALSE);
        }
        VARR_PUSH (MIR_op_t, call_ops, op2.mir_op);
        if (param != NULL) param = NL_NEXT (param);
      }
      MIR_append_insn (ctx, curr_func,
                       MIR_new_insn_arr (ctx, (inline_p ? MIR_INLINE : MIR_CALL),
                                         VARR_LENGTH (MIR_op_t, call_ops) - ops_start,
                                         VARR_ADDR (MIR_op_t, call_ops) + ops_start));
    }
    curr_call_arg_area_offset = saved_call_arg_area_offset_before_args;
    VARR_TRUNC (MIR_op_t, call_ops, ops_start);
    break;
  }
  case N_GENERIC: {
    node_t list = NL_EL (r->ops, 1);
    node_t ga_case = NL_HEAD (list->ops);

    /* first element is now a compatible generic association case */
    op1 = gen (ctx, NL_EL (ga_case->ops, 1), NULL, NULL, TRUE, NULL);
    t = get_mir_type (ctx, ((struct expr *) r->attr)->type);
    res = promote (ctx, op1, t, TRUE);
    break;
  }
  case N_SPEC_DECL: {  // ??? export and defintion with external declaration
    node_t specs = NL_HEAD (r->ops);
    node_t declarator = NL_NEXT (specs);
    node_t initializer = NL_NEXT (declarator);
    node_t id, curr_node;
    symbol_t sym;
    decl_t curr_decl;
    size_t i, init_start;
    const char *name;

    decl = (decl_t) r->attr;
    if (declarator != NULL && declarator->code != N_IGNORE && decl->item == NULL) {
      id = NL_HEAD (declarator->ops);
      name = (decl->scope != top_scope && decl->decl_spec.static_p
                ? get_func_static_var_name (ctx, id->u.s.s, decl)
                : id->u.s.s);
      if (decl->used_p && decl->scope != top_scope && decl->decl_spec.linkage == N_STATIC) {
        decl->item = MIR_new_forward (ctx, name);
        move_item_forward (ctx, decl->item);
      } else if (decl->used_p && decl->decl_spec.linkage != N_IGNORE) {
        if (symbol_find (c2m_ctx, S_REGULAR, id,
                         decl->decl_spec.linkage == N_EXTERN ? top_scope : decl->scope, &sym)
            && (decl->item = get_ref_item (ctx, sym.def_node, name)) == NULL) {
          for (i = 0; i < VARR_LENGTH (node_t, sym.defs); i++)
            if ((decl->item = get_ref_item (ctx, VARR_GET (node_t, sym.defs, i), name)) != NULL)
              break;
        }
        if (decl->item == NULL) decl->item = MIR_new_import (ctx, name);
        if (decl->scope != top_scope) move_item_forward (ctx, decl->item);
      }
      if (declarator->code == N_DECL && decl->decl_spec.type->mode != TM_FUNC
          && !decl->decl_spec.typedef_p && !decl->decl_spec.extern_p) {
        if (initializer->code == N_IGNORE) {
          if (decl->scope != top_scope && decl->decl_spec.static_p) {
            decl->item = MIR_new_bss (ctx, name, raw_type_size (c2m_ctx, decl->decl_spec.type));
          } else if (decl->scope == top_scope
                     && symbol_find (c2m_ctx, S_REGULAR, id, top_scope, &sym)
                     && ((curr_decl = sym.def_node->attr)->item == NULL
                         || curr_decl->item->item_type != MIR_bss_item)) {
            for (i = 0; i < VARR_LENGTH (node_t, sym.defs); i++) {
              curr_node = VARR_GET (node_t, sym.defs, i);
              curr_decl = curr_node->attr;
              if ((curr_decl->item != NULL && curr_decl->item->item_type == MIR_bss_item)
                  || NL_EL (curr_node->ops, 2)->code != N_IGNORE)
                break;
            }
            if (i >= VARR_LENGTH (node_t, sym.defs)) /* No item yet or no decl with intializer: */
              decl->item = MIR_new_bss (ctx, name, raw_type_size (c2m_ctx, decl->decl_spec.type));
          }
        } else if (initializer->code != N_IGNORE) {  // ??? general code
          init_start = VARR_LENGTH (init_el_t, init_els);
          collect_init_els (c2m_ctx, NULL, &decl->decl_spec.type, initializer,
                            decl->decl_spec.linkage == N_STATIC
                              || decl->decl_spec.linkage == N_EXTERN || decl->decl_spec.static_p
                              || decl->decl_spec.thread_local_p,
                            TRUE);
          if (decl->scope == top_scope)
            qsort (VARR_ADDR (init_el_t, init_els) + init_start,
                   VARR_LENGTH (init_el_t, init_els) - init_start, sizeof (init_el_t), cmp_init_el);
          if (id->attr == NULL) {
            node_t saved_scope = curr_scope;

            curr_scope = decl->scope;
            check (c2m_ctx, id, NULL);
            curr_scope = saved_scope;
          }
          if (decl->scope == top_scope || decl->decl_spec.static_p) {
            var = new_op (decl, MIR_new_ref_op (ctx, NULL));
          } else {
            var = gen (ctx, id, NULL, NULL, FALSE, NULL);
            assert (var.decl != NULL
                    && (var.mir_op.mode == MIR_OP_REG
                        || (var.mir_op.mode == MIR_OP_MEM && var.mir_op.u.mem.index == 0)));
          }
          gen_initializer (ctx, init_start, var, name,
                           raw_type_size (c2m_ctx, decl->decl_spec.type),
                           decl->scope != top_scope && !decl->decl_spec.static_p);
          VARR_TRUNC (init_el_t, init_els, init_start);
        }
        if (decl->item != NULL && decl->scope == top_scope && !decl->decl_spec.static_p) {
          MIR_new_export (ctx, name);
        } else if (decl->item != NULL && decl->scope != top_scope && decl->decl_spec.static_p) {
          MIR_item_t item = MIR_new_forward (ctx, name);

          move_item_forward (ctx, item);
        }
      }
    }
    break;
  }
  case N_ST_ASSERT: /* do nothing */ break;
  case N_INIT: break;  // ???
  case N_FUNC_DEF: {
    node_t decl_specs = NL_HEAD (r->ops);
    node_t declarator = NL_NEXT (decl_specs);
    node_t decls = NL_NEXT (declarator);
    node_t stmt = NL_NEXT (decls);
    struct node_scope *ns = stmt->attr;
    decl_t param_decl, decl = r->attr;
    struct type *decl_type = decl->decl_spec.type;
    node_t param, param_declarator, param_id;
    struct type *param_type;
    MIR_insn_t insn;
    MIR_type_t res_type, param_mir_type;
    MIR_reg_t fp_reg, param_reg;
    const char *name;

    assert (declarator != NULL && declarator->code == N_DECL
            && NL_HEAD (declarator->ops)->code == N_ID);
    assert (decl_type->mode == TM_FUNC);
    reg_free_mark = 0;
    curr_func_def = r;
    curr_call_arg_area_offset = 0;
    collect_args_and_func_types (ctx, decl_type->u.func_type, &res_type);
    curr_func
      = ((decl_type->u.func_type->dots_p
            ? MIR_new_vararg_func_arr
            : MIR_new_func_arr) (ctx, NL_HEAD (declarator->ops)->u.s.s,
                                 res_type == MIR_T_UNDEF ? 0 : 1, &res_type,
                                 VARR_LENGTH (MIR_var_t, vars), VARR_ADDR (MIR_var_t, vars)));
    decl->item = curr_func;
    if (ns->stack_var_p /* we can have empty struct only with size 0 and still need a frame: */
        || ns->size > 0) {
      fp_reg = MIR_new_func_reg (ctx, curr_func->u.func, MIR_T_I64, FP_NAME);
      MIR_append_insn (ctx, curr_func,
                       MIR_new_insn (ctx, MIR_ALLOCA, MIR_new_reg_op (ctx, fp_reg),
                                     MIR_new_int_op (ctx, ns->size)));
    }
    for (size_t i = 0; i < VARR_LENGTH (MIR_var_t, vars); i++)
      get_reg_var (ctx, MIR_T_UNDEF, VARR_GET (MIR_var_t, vars, i).name);
    for (size_t i = 0; i < VARR_LENGTH (node_t, mem_params); i++) {
      param = VARR_GET (node_t, mem_params, i);
      param_declarator = NL_EL (param->ops, 1);
      param_decl = param->attr;
      assert (param_declarator != NULL && param_declarator->code == N_DECL);
      param_id = NL_HEAD (param_declarator->ops);
      param_type = param_decl->decl_spec.type;
      name = get_param_name (ctx, &param_mir_type, param_type, param_id->u.s.s);
      if (param_type->mode == TM_STRUCT || param_type->mode == TM_UNION) {
        param_reg = get_reg_var (ctx, MIR_POINTER_TYPE, name).reg;
        val = new_op (NULL, MIR_new_mem_op (ctx, MIR_T_UNDEF, 0, param_reg, 0, 1));
        var = new_op (param_decl, MIR_new_mem_op (ctx, MIR_T_UNDEF, param_decl->offset,
                                                  MIR_reg (ctx, FP_NAME, curr_func->u.func), 0, 1));
        block_move (ctx, var, val, type_size (c2m_ctx, param_type));
      } else {
        assert (!param_decl->reg_p);
        emit2 (ctx, tp_mov (param_mir_type),
               MIR_new_mem_op (ctx, param_mir_type, param_decl->offset,
                               MIR_reg (ctx, FP_NAME, curr_func->u.func), 0, 1),
               MIR_new_reg_op (ctx, get_reg_var (ctx, MIR_T_UNDEF, name).reg));
      }
    }
    gen (ctx, stmt, NULL, NULL, FALSE, NULL);
    if ((insn = DLIST_TAIL (MIR_insn_t, curr_func->u.func->insns)) == NULL
        || (insn->code != MIR_RET && insn->code != MIR_JMP)) {
      if (res_type == MIR_T_UNDEF)
        emit_insn (ctx, MIR_new_ret_insn (ctx, 0));
      else if (res_type == MIR_T_D)
        emit_insn (ctx, MIR_new_ret_insn (ctx, 1, MIR_new_double_op (ctx, 0.0)));
      else if (res_type == MIR_T_LD)
        emit_insn (ctx, MIR_new_ret_insn (ctx, 1, MIR_new_ldouble_op (ctx, 0.0)));
      else if (res_type == MIR_T_F)
        emit_insn (ctx, MIR_new_ret_insn (ctx, 1, MIR_new_float_op (ctx, 0.0)));
      else if (scalar_type_p (adjust_type (c2m_ctx, decl->decl_spec.type->u.func_type->ret_type)))
        emit_insn (ctx, MIR_new_ret_insn (ctx, 1, MIR_new_int_op (ctx, 0)));
      else
        assert (FALSE); /* ??? not implemented */
    }
    MIR_finish_func (ctx);
    if (decl->decl_spec.linkage == N_EXTERN) MIR_new_export (ctx, NL_HEAD (declarator->ops)->u.s.s);
    finish_curr_func_reg_vars (ctx);
    break;
  }
  case N_BLOCK:
    emit_label (ctx, r);
    gen (ctx, NL_EL (r->ops, 1), NULL, NULL, FALSE, NULL);
    break;
  case N_MODULE: gen (ctx, NL_HEAD (r->ops), NULL, NULL, FALSE, NULL); break;  // ???
  case N_IF: {
    node_t expr = NL_EL (r->ops, 1);
    node_t if_stmt = NL_NEXT (expr);
    node_t else_stmt = NL_NEXT (if_stmt);
    MIR_label_t if_label = MIR_new_label (ctx), else_label = MIR_new_label (ctx);
    MIR_label_t end_label = MIR_new_label (ctx);

    assert (false_label == NULL && true_label == NULL);
    emit_label (ctx, r);
    top_gen (ctx, expr, if_label, else_label);
    emit_insn (ctx, if_label);
    gen (ctx, if_stmt, NULL, NULL, FALSE, NULL);
    emit1 (ctx, MIR_JMP, MIR_new_label_op (ctx, end_label));
    emit_insn (ctx, else_label);
    gen (ctx, else_stmt, NULL, NULL, FALSE, NULL);
    emit_insn (ctx, end_label);
    break;
  }
  case N_SWITCH: {
    node_t expr = NL_EL (r->ops, 1);
    node_t stmt = NL_NEXT (expr);
    struct switch_attr *switch_attr = r->attr;
    op_t case_reg_op;
    struct expr *e2;
    case_t c;
    MIR_label_t saved_break_label = break_label;
    int signed_p, short_p;
    size_t len;
    mir_ullong range = 0;

    assert (false_label == NULL && true_label == NULL);
    emit_label (ctx, r);
    break_label = MIR_new_label (ctx);
    case_reg_op = gen (ctx, expr, NULL, NULL, TRUE, NULL);
    type = ((struct expr *) expr->attr)->type;
    signed_p = signed_integer_type_p (type);
    mir_type = get_mir_type (ctx, type);
    short_p = mir_type != MIR_T_I64 && mir_type != MIR_T_U64;
    case_reg_op = force_reg (ctx, case_reg_op, mir_type);
    if (switch_attr->min_val_case != NULL) {
      e = NL_HEAD (switch_attr->min_val_case->case_node->ops)->attr;
      e2 = NL_HEAD (switch_attr->max_val_case->case_node->ops)->attr;
      range = signed_p ? e2->u.i_val - e->u.i_val : e2->u.u_val - e->u.u_val;
    }
    len = DLIST_LENGTH (case_t, switch_attr->case_labels);
    if (!switch_attr->ranges_p && len > 4 && range != 0 && range / len < 3) { /* use MIR_SWITCH */
      mir_ullong curr_val, prev_val, n;
      op_t index = get_new_temp (ctx, MIR_T_I64);
      MIR_label_t label = break_label;

      c = DLIST_TAIL (case_t, switch_attr->case_labels);
      if (c->case_node->code == N_DEFAULT) {
        assert (DLIST_NEXT (case_t, c) == NULL);
        label = get_label (ctx, c->case_target_node);
      }
      emit3 (ctx, short_p ? MIR_SUBS : MIR_SUB, index.mir_op, case_reg_op.mir_op,
             signed_p ? MIR_new_int_op (ctx, e->u.i_val) : MIR_new_uint_op (ctx, e->u.u_val));
      emit3 (ctx, short_p ? MIR_UBGTS : MIR_UBGT, MIR_new_label_op (ctx, label), index.mir_op,
             MIR_new_uint_op (ctx, range));
      VARR_TRUNC (case_t, switch_cases, 0);
      for (c = DLIST_HEAD (case_t, switch_attr->case_labels);
           c != NULL && c->case_node->code != N_DEFAULT; c = DLIST_NEXT (case_t, c))
        VARR_PUSH (case_t, switch_cases, c);
      qsort (VARR_ADDR (case_t, switch_cases), VARR_LENGTH (case_t, switch_cases), sizeof (case_t),
             signed_p ? signed_case_compare : unsigned_case_compare);
      VARR_TRUNC (MIR_op_t, switch_ops, 0);
      VARR_PUSH (MIR_op_t, switch_ops, index.mir_op);
      for (size_t i = 0; i < VARR_LENGTH (case_t, switch_cases); i++) {
        c = VARR_GET (case_t, switch_cases, i);
        e2 = NL_HEAD (c->case_node->ops)->attr;
        curr_val = signed_p ? e2->u.i_val - e->u.i_val : e2->u.u_val - e->u.u_val;
        if (i != 0) {
          for (n = prev_val + 1; n < curr_val; n++)
            VARR_PUSH (MIR_op_t, switch_ops, MIR_new_label_op (ctx, label));
        }
        VARR_PUSH (MIR_op_t, switch_ops,
                   MIR_new_label_op (ctx, get_label (ctx, c->case_target_node)));
        prev_val = curr_val;
      }
      emit_insn (ctx, MIR_new_insn_arr (ctx, MIR_SWITCH, VARR_LENGTH (MIR_op_t, switch_ops),
                                        VARR_ADDR (MIR_op_t, switch_ops)));
    } else {
      for (c = DLIST_HEAD (case_t, switch_attr->case_labels); c != NULL;
           c = DLIST_NEXT (case_t, c)) {
        MIR_label_t cont_label, label = get_label (ctx, c->case_target_node);
        node_t case_expr, case_expr2;

        if (c->case_node->code == N_DEFAULT) {
          assert (DLIST_NEXT (case_t, c) == NULL);
          emit1 (ctx, MIR_JMP, MIR_new_label_op (ctx, label));
          break;
        }
        case_expr = NL_HEAD (c->case_node->ops);
        case_expr2 = NL_NEXT (case_expr);
        e = case_expr->attr;
        assert (e->const_p && integer_type_p (e->type));
        if (case_expr2 == NULL) {
          emit3 (ctx, short_p ? MIR_BEQS : MIR_BEQ, MIR_new_label_op (ctx, label),
                 case_reg_op.mir_op, MIR_new_int_op (ctx, e->u.i_val));
        } else {
          e2 = case_expr2->attr;
          assert (e2->const_p && integer_type_p (e2->type));
          cont_label = MIR_new_label (ctx);
          if (signed_p) {
            emit3 (ctx, short_p ? MIR_BLTS : MIR_BLT, MIR_new_label_op (ctx, cont_label),
                   case_reg_op.mir_op, MIR_new_int_op (ctx, e->u.i_val));
            emit3 (ctx, short_p ? MIR_BLES : MIR_BLE, MIR_new_label_op (ctx, label),
                   case_reg_op.mir_op, MIR_new_int_op (ctx, e2->u.i_val));
          } else {
            emit3 (ctx, short_p ? MIR_UBLTS : MIR_UBLT, MIR_new_label_op (ctx, cont_label),
                   case_reg_op.mir_op, MIR_new_int_op (ctx, e->u.i_val));
            emit3 (ctx, short_p ? MIR_UBLES : MIR_UBLE, MIR_new_label_op (ctx, label),
                   case_reg_op.mir_op, MIR_new_int_op (ctx, e2->u.i_val));
          }
          emit_insn (ctx, cont_label);
        }
      }
      if (c == NULL) /* no default: */
        emit1 (ctx, MIR_JMP, MIR_new_label_op (ctx, break_label));
    }
    top_gen (ctx, stmt, NULL, NULL);
    emit_insn (ctx, break_label);
    break_label = saved_break_label;
    break;
  }
  case N_DO: {
    node_t expr = NL_EL (r->ops, 1);
    node_t stmt = NL_NEXT (expr);
    MIR_label_t saved_continue_label = continue_label, saved_break_label = break_label;
    MIR_label_t start_label = MIR_new_label (ctx);

    assert (false_label == NULL && true_label == NULL);
    continue_label = MIR_new_label (ctx);
    break_label = MIR_new_label (ctx);
    emit_label (ctx, r);
    emit_insn (ctx, start_label);
    gen (ctx, stmt, NULL, NULL, FALSE, NULL);
    emit_insn (ctx, continue_label);
    top_gen (ctx, expr, start_label, break_label);
    emit_insn (ctx, break_label);
    continue_label = saved_continue_label;
    break_label = saved_break_label;
    break;
  }
  case N_WHILE: {
    node_t expr = NL_EL (r->ops, 1);
    node_t stmt = NL_NEXT (expr);
    MIR_label_t stmt_label = MIR_new_label (ctx);
    MIR_label_t saved_continue_label = continue_label, saved_break_label = break_label;

    assert (false_label == NULL && true_label == NULL);
    continue_label = MIR_new_label (ctx);
    break_label = MIR_new_label (ctx);
    emit_label (ctx, r);
    emit_insn (ctx, continue_label);
    top_gen (ctx, expr, stmt_label, break_label);
    emit_insn (ctx, stmt_label);
    gen (ctx, stmt, NULL, NULL, FALSE, NULL);
    emit1 (ctx, MIR_JMP, MIR_new_label_op (ctx, continue_label));
    emit_insn (ctx, break_label);
    continue_label = saved_continue_label;
    break_label = saved_break_label;
    break;
  }
  case N_FOR: {
    node_t init = NL_EL (r->ops, 1);
    node_t cond = NL_NEXT (init);
    node_t iter = NL_NEXT (cond);
    node_t stmt = NL_NEXT (iter);
    MIR_label_t start_label = MIR_new_label (ctx), stmt_label = MIR_new_label (ctx);
    MIR_label_t saved_continue_label = continue_label, saved_break_label = break_label;

    assert (false_label == NULL && true_label == NULL);
    continue_label = MIR_new_label (ctx);
    break_label = MIR_new_label (ctx);
    emit_label (ctx, r);
    top_gen (ctx, init, NULL, NULL);
    emit_insn (ctx, start_label);
    if (cond->code != N_IGNORE) /* non-empty condition: */
      top_gen (ctx, cond, stmt_label, break_label);
    emit_insn (ctx, stmt_label);
    gen (ctx, stmt, NULL, NULL, FALSE, NULL);
    emit_insn (ctx, continue_label);
    top_gen (ctx, iter, NULL, NULL);
    emit1 (ctx, MIR_JMP, MIR_new_label_op (ctx, start_label));
    emit_insn (ctx, break_label);
    continue_label = saved_continue_label;
    break_label = saved_break_label;
    break;
  }
  case N_GOTO: {
    node_t target = r->attr;

    assert (false_label == NULL && true_label == NULL);
    emit_label (ctx, r);
    emit1 (ctx, MIR_JMP, MIR_new_label_op (ctx, get_label (ctx, target)));
    break;
  }
  case N_CONTINUE:
    assert (false_label == NULL && true_label == NULL);
    emit_label (ctx, r);
    emit1 (ctx, MIR_JMP, MIR_new_label_op (ctx, continue_label));
    break;
  case N_BREAK:
    assert (false_label == NULL && true_label == NULL);
    emit_label (ctx, r);
    emit1 (ctx, MIR_JMP, MIR_new_label_op (ctx, break_label));
    break;
  case N_RETURN: {
    decl_t func_decl = curr_func_def->attr;
    struct type *func_type = func_decl->decl_spec.type;
    struct type *ret_type = func_type->u.func_type->ret_type;
    int scalar_p = scalar_type_p (ret_type);
    mir_size_t size = type_size (c2m_ctx, ret_type);
    MIR_reg_t ret_addr_reg;

    assert (false_label == NULL && true_label == NULL);
    emit_label (ctx, r);
    if (NL_EL (r->ops, 1)->code == N_IGNORE) {
      emit_insn (ctx, MIR_new_ret_insn (ctx, 0));
      break;
    }
    if (!scalar_p) {
      MIR_reg_t ret_addr_reg = MIR_reg (ctx, RET_ADDR_NAME, curr_func->u.func);

      var = new_op (NULL, MIR_new_mem_op (ctx, MIR_T_I8, 0, ret_addr_reg, 0, 1));
    }
    val = gen (ctx, NL_EL (r->ops, 1), NULL, NULL, scalar_p, scalar_p ? NULL : &var);
    if (scalar_p) {
      t = get_mir_type (ctx, ret_type);
      t = promote_mir_int_type (t);
      val = promote (ctx, val, t, FALSE);
      emit_insn (ctx, MIR_new_ret_insn (ctx, 1, val.mir_op));
    } else { /* block return */
      block_move (ctx, var, val, size);
      emit_insn (ctx, MIR_new_ret_insn (ctx, 0));
    }
    break;
  }
  case N_EXPR:
    assert (false_label == NULL && true_label == NULL);
    emit_label (ctx, r);
    top_gen (ctx, NL_EL (r->ops, 1), NULL, NULL);
    break;
  default: abort ();
  }
finish:
  if (true_label != NULL) {
    MIR_op_t lab_op = MIR_new_label_op (ctx, true_label);

    type = ((struct expr *) r->attr)->type;
    if (!floating_type_p (type)) {
      res = promote (ctx, force_val (ctx, res, type->arr_type != NULL), MIR_T_I64, FALSE);
      emit2 (ctx, MIR_BT, lab_op, res.mir_op);
    } else if (type->u.basic_type == TP_FLOAT) {
      emit3 (ctx, MIR_FBNE, lab_op, res.mir_op, MIR_new_float_op (ctx, 0.0));
    } else if (type->u.basic_type == TP_DOUBLE) {
      emit3 (ctx, MIR_DBNE, lab_op, res.mir_op, MIR_new_double_op (ctx, 0.0));
    } else {
      assert (type->u.basic_type == TP_LDOUBLE);
      emit3 (ctx, MIR_LDBNE, lab_op, res.mir_op, MIR_new_ldouble_op (ctx, 0.0));
    }
    emit1 (ctx, MIR_JMP, MIR_new_label_op (ctx, false_label));
  } else if (val_p) {
    res = force_val (ctx, res, ((struct expr *) r->attr)->type->arr_type != NULL);
  }
  if (stmt_p) curr_call_arg_area_offset = 0;
  return res;
}

DEF_HTAB (MIR_item_t);
static HTAB (MIR_item_t) * proto_tab;

static htab_hash_t proto_hash (MIR_item_t pi) {
  MIR_proto_t p = pi->u.proto;
  MIR_var_t *args = VARR_ADDR (MIR_var_t, p->args);
  uint64_t h = mir_hash_init (42);

  h = mir_hash_step (h, p->nres);
  h = mir_hash_step (h, p->vararg_p);
  for (uint32_t i = 0; i < p->nres; i++) h = mir_hash_step (h, p->res_types[i]);
  for (size_t i = 0; i < VARR_LENGTH (MIR_var_t, p->args); i++) {
    h = mir_hash_step (h, args[i].type);
    h = mir_hash_step (h, mir_hash (args[i].name, strlen (args[i].name), 24));
  }
  return mir_hash_finish (h);
}

static int proto_eq (MIR_item_t pi1, MIR_item_t pi2) {
  MIR_proto_t p1 = pi1->u.proto, p2 = pi2->u.proto;

  if (p1->nres != p2->nres || p1->vararg_p != p2->vararg_p
      || VARR_LENGTH (MIR_var_t, p1->args) != VARR_LENGTH (MIR_var_t, p2->args))
    return FALSE;
  for (uint32_t i = 0; i < p1->nres; i++)
    if (p1->res_types[i] != p1->res_types[i]) return FALSE;

  MIR_var_t *args1 = VARR_ADDR (MIR_var_t, p1->args), *args2 = VARR_ADDR (MIR_var_t, p2->args);

  for (size_t i = 0; i < VARR_LENGTH (MIR_var_t, p1->args); i++)
    if (args1[i].type != args2[i].type || strcmp (args1[i].name, args2[i].name) != 0) return FALSE;
  return TRUE;
}

static MIR_item_t get_mir_proto (MIR_context_t ctx, int vararg_p, MIR_type_t ret_type,
                                 VARR (MIR_var_t) * vars) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);
  struct MIR_item pi, *el;
  struct MIR_proto p;
  char buf[30];

  pi.u.proto = &p;
  p.vararg_p = vararg_p;
  p.nres = ret_type == MIR_T_UNDEF ? 0 : 1;
  p.res_types = &ret_type;
  p.args = vars;
  if (HTAB_DO (MIR_item_t, proto_tab, &pi, HTAB_FIND, el)) return el;
  sprintf (buf, "proto%d", curr_mir_proto_num++);
  el = (vararg_p ? MIR_new_vararg_proto_arr : MIR_new_proto_arr) (ctx, buf, p.nres, &ret_type,
                                                                  VARR_LENGTH (MIR_var_t, vars),
                                                                  VARR_ADDR (MIR_var_t, vars));
  HTAB_DO (MIR_item_t, proto_tab, el, HTAB_INSERT, el);
  return el;
}

static void gen_mir_protos (MIR_context_t ctx) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);
  node_t call, func;
  struct type *type;
  struct func_type *func_type;
  MIR_type_t ret_type;

  curr_mir_proto_num = 0;
  HTAB_CREATE (MIR_item_t, proto_tab, 512, proto_hash, proto_eq);
  for (size_t i = 0; i < VARR_LENGTH (node_t, call_nodes); i++) {
    call = VARR_GET (node_t, call_nodes, i);
    assert (call->code == N_CALL);
    func = NL_HEAD (call->ops);
    type = ((struct expr *) func->attr)->type;
    assert (type->mode == TM_PTR && type->u.ptr_type->mode == TM_FUNC);
    set_type_layout (c2m_ctx, type);
    func_type = type->u.ptr_type->u.func_type;
    assert (func_type->param_list->code == N_LIST);
    collect_args_and_func_types (ctx, func_type, &ret_type);
    func_type->proto_item
      = get_mir_proto (ctx, func_type->dots_p || NL_HEAD (func_type->param_list->ops) == NULL,
                       ret_type, vars);
  }
  HTAB_DESTROY (MIR_item_t, proto_tab);
}

static void gen_finish (MIR_context_t ctx) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);

  if (c2m_ctx == NULL || c2m_ctx->gen_ctx == NULL) return;
  finish_reg_vars (ctx);
  if (vars != NULL) VARR_DESTROY (MIR_var_t, vars);
  if (mem_params != NULL) VARR_DESTROY (node_t, mem_params);
  if (call_ops != NULL) VARR_DESTROY (MIR_op_t, call_ops);
  if (switch_ops != NULL) VARR_DESTROY (MIR_op_t, switch_ops);
  if (switch_cases != NULL) VARR_DESTROY (case_t, switch_cases);
  if (init_els != NULL) VARR_DESTROY (init_el_t, init_els);
  free (c2m_ctx->gen_ctx);
}

static void gen_mir (MIR_context_t ctx, node_t r) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);

  c2m_ctx->gen_ctx = c2mir_calloc (ctx, sizeof (struct gen_ctx));
  zero_op = new_op (NULL, MIR_new_int_op (ctx, 0));
  one_op = new_op (NULL, MIR_new_int_op (ctx, 1));
  minus_one_op = new_op (NULL, MIR_new_int_op (ctx, -1));
  init_reg_vars (ctx);
  VARR_CREATE (MIR_var_t, vars, 32);
  VARR_CREATE (node_t, mem_params, 16);
  gen_mir_protos (ctx);
  VARR_CREATE (MIR_op_t, call_ops, 32);
  VARR_CREATE (MIR_op_t, switch_ops, 128);
  VARR_CREATE (case_t, switch_cases, 64);
  VARR_CREATE (init_el_t, init_els, 128);
  memset_proto = memset_item = memcpy_proto = memcpy_item = NULL;
  top_gen (ctx, r, NULL, NULL);
  gen_finish (ctx);
}

/* ------------------------- MIR generator finish ----------------------------- */

/* New Page */

static const char *get_node_name (node_code_t code) {
#define REP_SEP ;
#define C(n) \
  case N_##n: return #n
  switch (code) {
    C (IGNORE);
    REP8 (C, I, L, LL, U, UL, ULL, F, D);
    REP8 (C, LD, CH, STR, ID, COMMA, ANDAND, OROR, EQ);
    REP8 (C, NE, LT, LE, GT, GE, ASSIGN, BITWISE_NOT, NOT);
    REP8 (C, AND, AND_ASSIGN, OR, OR_ASSIGN, XOR, XOR_ASSIGN, LSH, LSH_ASSIGN);
    REP8 (C, RSH, RSH_ASSIGN, ADD, ADD_ASSIGN, SUB, SUB_ASSIGN, MUL, MUL_ASSIGN);
    REP8 (C, DIV, DIV_ASSIGN, MOD, MOD_ASSIGN, IND, FIELD, ADDR, DEREF);
    REP8 (C, DEREF_FIELD, COND, INC, DEC, POST_INC, POST_DEC, ALIGNOF, SIZEOF);
    REP8 (C, EXPR_SIZEOF, CAST, COMPOUND_LITERAL, CALL, GENERIC, GENERIC_ASSOC, IF, SWITCH);
    REP8 (C, WHILE, DO, FOR, GOTO, CONTINUE, BREAK, RETURN, EXPR);
    REP8 (C, BLOCK, CASE, DEFAULT, LABEL, LIST, SPEC_DECL, SHARE, TYPEDEF);
    REP8 (C, EXTERN, STATIC, AUTO, REGISTER, THREAD_LOCAL, DECL, VOID, CHAR);
    REP8 (C, SHORT, INT, LONG, FLOAT, DOUBLE, SIGNED, UNSIGNED, BOOL);
    REP8 (C, STRUCT, UNION, ENUM, ENUM_CONST, MEMBER, CONST, RESTRICT, VOLATILE);
    REP8 (C, ATOMIC, INLINE, NO_RETURN, ALIGNAS, FUNC, STAR, POINTER, DOTS);
    REP7 (C, ARR, INIT, FIELD_ID, TYPE, ST_ASSERT, FUNC_DEF, MODULE);
  default: abort ();
  }
#undef C
#undef REP_SEP
}

static void print_char (FILE *f, int ch) {
  assert (ch >= 0);
  if (ch == '"' || ch == '\"' || ch == '\\') fprintf (f, "\\");
  if (isprint (ch))
    fprintf (f, "%c", ch);
  else
    fprintf (f, "\\%o", ch);
}

static void print_chars (FILE *f, const char *str, size_t len) {
  for (size_t i = 0; i < len; i++) print_char (f, str[i]);
}

static void print_node (MIR_context_t ctx, FILE *f, node_t n, int indent, int attr_p);

void debug_node (MIR_context_t ctx, node_t n) { print_node (ctx, stderr, n, 0, TRUE); }

static void print_ops (MIR_context_t ctx, FILE *f, node_t n, int indent, int attr_p) {
  int i;
  node_t op;

  for (i = 0; (op = get_op (n, i)) != NULL; i++) print_node (ctx, f, op, indent + 2, attr_p);
}

static void print_qual (FILE *f, struct type_qual type_qual) {
  if (type_qual.const_p) fprintf (f, ", const");
  if (type_qual.restrict_p) fprintf (f, ", restrict");
  if (type_qual.volatile_p) fprintf (f, ", volatile");
  if (type_qual.atomic_p) fprintf (f, ", atomic");
}

static void print_type (MIR_context_t ctx, FILE *f, struct type *type) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);

  switch (type->mode) {
  case TM_UNDEF: fprintf (f, "undef type mode"); break;
  case TM_BASIC:
    switch (type->u.basic_type) {
    case TP_UNDEF: fprintf (f, "undef type"); break;
    case TP_VOID: fprintf (f, "void"); break;
    case TP_BOOL: fprintf (f, "bool"); break;
    case TP_CHAR: fprintf (f, "char"); break;
    case TP_SCHAR: fprintf (f, "signed char"); break;
    case TP_UCHAR: fprintf (f, "unsigned char"); break;
    case TP_SHORT: fprintf (f, "short"); break;
    case TP_USHORT: fprintf (f, "unsigned short"); break;
    case TP_INT: fprintf (f, "int"); break;
    case TP_UINT: fprintf (f, "unsigned int"); break;
    case TP_LONG: fprintf (f, "long"); break;
    case TP_ULONG: fprintf (f, "unsigned long"); break;
    case TP_LLONG: fprintf (f, "long long"); break;
    case TP_ULLONG: fprintf (f, "unsigned long long"); break;
    case TP_FLOAT: fprintf (f, "float"); break;
    case TP_DOUBLE: fprintf (f, "double"); break;
    case TP_LDOUBLE: fprintf (f, "long double"); break;
    default: assert (FALSE);
    }
    break;
  case TM_ENUM: fprintf (f, "enum node %lu", type->u.tag_type->uid); break;
  case TM_PTR:
    fprintf (f, "ptr (");
    print_type (ctx, f, type->u.ptr_type);
    fprintf (f, ")");
    break;
  case TM_STRUCT: fprintf (f, "struct node %lu", type->u.tag_type->uid); break;
  case TM_UNION: fprintf (f, "union node %lu", type->u.tag_type->uid); break;
  case TM_ARR:
    fprintf (f, "array [%s", type->u.arr_type->static_p ? "static " : "");
    print_qual (f, type->u.arr_type->ind_type_qual);
    fprintf (f, "size node %lu] (", type->u.arr_type->size->uid);
    print_type (ctx, f, type->u.arr_type->el_type);
    fprintf (f, ")");
    break;
  case TM_FUNC:
    fprintf (f, "func ");
    print_type (ctx, f, type->u.func_type->ret_type);
    fprintf (f, "(params node %lu", type->u.func_type->param_list->uid);
    fprintf (f, type->u.func_type->dots_p ? ", ...)" : ")");
    break;
  default: assert (FALSE);
  }
  print_qual (f, type->type_qual);
  if (incomplete_type_p (c2m_ctx, type)) fprintf (f, ", incomplete");
  if (type->raw_size != MIR_SIZE_MAX)
    fprintf (f, ", raw size = %llu", (unsigned long long) type->raw_size);
  if (type->align >= 0) fprintf (f, ", align = %d", type->align);
  fprintf (f, " ");
}

static void print_decl_spec (MIR_context_t ctx, FILE *f, struct decl_spec *decl_spec) {
  if (decl_spec->typedef_p) fprintf (f, " typedef, ");
  if (decl_spec->extern_p) fprintf (f, " extern, ");
  if (decl_spec->static_p) fprintf (f, " static, ");
  if (decl_spec->auto_p) fprintf (f, " auto, ");
  if (decl_spec->register_p) fprintf (f, " register, ");
  if (decl_spec->thread_local_p) fprintf (f, " thread local, ");
  if (decl_spec->inline_p) fprintf (f, " inline, ");
  if (decl_spec->no_return_p) fprintf (f, " no return, ");
  if (decl_spec->align >= 0) fprintf (f, " align = %d, ", decl_spec->align);
  if (decl_spec->align_node != NULL)
    fprintf (f, " strictest align node %lu, ", decl_spec->align_node->uid);
  if (decl_spec->linkage != N_IGNORE)
    fprintf (f, " %s linkage, ", decl_spec->linkage == N_STATIC ? "static" : "extern");
  print_type (ctx, f, decl_spec->type);
}

static void print_decl (MIR_context_t ctx, FILE *f, decl_t decl) {
  if (decl == NULL) return;
  fprintf (f, ": ");
  if (decl->scope != NULL) fprintf (f, "scope node = %lu, ", decl->scope->uid);
  print_decl_spec (ctx, f, &decl->decl_spec);
  if (decl->addr_p) fprintf (f, ", addressable");
  if (decl->used_p) fprintf (f, ", used");
  if (decl->reg_p)
    fprintf (f, ", reg");
  else {
    fprintf (f, ", offset = %llu", (unsigned long long) decl->offset);
    if (decl->bit_offset >= 0) fprintf (f, ", bit offset = %d", decl->bit_offset);
  }
}

static void print_expr (MIR_context_t ctx, FILE *f, struct expr *e) {
  if (e == NULL) return; /* e.g. N_ID which is not an expr */
  fprintf (f, ": ");
  if (e->lvalue_node) fprintf (f, "lvalue, ");
  print_type (ctx, f, e->type);
  if (e->const_p) {
    fprintf (f, ", const = ");
    if (!integer_type_p (e->type)) {
      fprintf (f, " %.*Lg\n", LDBL_DECIMAL_DIG, (long double) e->u.d_val);
    } else if (signed_integer_type_p (e->type)) {
      fprintf (f, "%lld", (long long) e->u.i_val);
    } else {
      fprintf (f, "%llu", (unsigned long long) e->u.u_val);
    }
  }
}

static void print_node (MIR_context_t ctx, FILE *f, node_t n, int indent, int attr_p) {
  int i;

  fprintf (f, "%6lu: ", n->uid);
  for (i = 0; i < indent; i++) fprintf (f, " ");
  if (n == err_node) {
    fprintf (f, "<error>\n");
    return;
  }
  fprintf (f, "%s (", get_node_name (n->code));
  print_pos (f, n->pos, FALSE);
  fprintf (f, ")");
  switch (n->code) {
  case N_IGNORE: fprintf (f, "\n"); break;
  case N_I: fprintf (f, " %lld", (long long) n->u.l); goto expr;
  case N_L: fprintf (f, " %lldl", (long long) n->u.l); goto expr;
  case N_LL: fprintf (f, " %lldll", (long long) n->u.ll); goto expr;
  case N_U: fprintf (f, " %lluu", (unsigned long long) n->u.ul); goto expr;
  case N_UL: fprintf (f, " %lluul", (unsigned long long) n->u.ul); goto expr;
  case N_ULL: fprintf (f, " %lluull", (unsigned long long) n->u.ull); goto expr;
  case N_F: fprintf (f, " %.*g", FLT_DECIMAL_DIG, (double) n->u.f); goto expr;
  case N_D: fprintf (f, " %.*g", DBL_DECIMAL_DIG, (double) n->u.d); goto expr;
  case N_LD: fprintf (f, " %.*Lg", LDBL_DECIMAL_DIG, (long double) n->u.ld); goto expr;
  case N_CH:
    fprintf (f, " '");
    print_char (f, n->u.ch);
    fprintf (f, "'");
    goto expr;
  case N_STR:
    fprintf (f, " \"");
    print_chars (f, n->u.s.s, n->u.s.len);
    fprintf (f, "\"");
    goto expr;
  case N_ID:
    fprintf (f, " %s", n->u.s.s);
  expr:
    if (attr_p && n->attr != NULL) print_expr (ctx, f, n->attr);
    fprintf (f, "\n");
    break;
  case N_COMMA:
  case N_ANDAND:
  case N_OROR:
  case N_EQ:
  case N_NE:
  case N_LT:
  case N_LE:
  case N_GT:
  case N_GE:
  case N_ASSIGN:
  case N_BITWISE_NOT:
  case N_NOT:
  case N_AND:
  case N_AND_ASSIGN:
  case N_OR:
  case N_OR_ASSIGN:
  case N_XOR:
  case N_XOR_ASSIGN:
  case N_LSH:
  case N_LSH_ASSIGN:
  case N_RSH:
  case N_RSH_ASSIGN:
  case N_ADD:
  case N_ADD_ASSIGN:
  case N_SUB:
  case N_SUB_ASSIGN:
  case N_MUL:
  case N_MUL_ASSIGN:
  case N_DIV:
  case N_DIV_ASSIGN:
  case N_MOD:
  case N_MOD_ASSIGN:
  case N_IND:
  case N_FIELD:
  case N_ADDR:
  case N_DEREF:
  case N_DEREF_FIELD:
  case N_COND:
  case N_INC:
  case N_DEC:
  case N_POST_INC:
  case N_POST_DEC:
  case N_ALIGNOF:
  case N_SIZEOF:
  case N_EXPR_SIZEOF:
  case N_CAST:
  case N_COMPOUND_LITERAL:
  case N_CALL:
  case N_GENERIC:
    if (attr_p && n->attr != NULL) print_expr (ctx, f, n->attr);
    fprintf (f, "\n");
    print_ops (ctx, f, n, indent, attr_p);
    break;
  case N_GENERIC_ASSOC:
  case N_IF:
  case N_WHILE:
  case N_DO:
  case N_CONTINUE:
  case N_BREAK:
  case N_RETURN:
  case N_EXPR:
  case N_CASE:
  case N_DEFAULT:
  case N_LABEL:
  case N_SHARE:
  case N_TYPEDEF:
  case N_EXTERN:
  case N_STATIC:
  case N_AUTO:
  case N_REGISTER:
  case N_THREAD_LOCAL:
  case N_DECL:
  case N_VOID:
  case N_CHAR:
  case N_SHORT:
  case N_INT:
  case N_LONG:
  case N_FLOAT:
  case N_DOUBLE:
  case N_SIGNED:
  case N_UNSIGNED:
  case N_BOOL:
  case N_ENUM:
  case N_CONST:
  case N_RESTRICT:
  case N_VOLATILE:
  case N_ATOMIC:
  case N_INLINE:
  case N_NO_RETURN:
  case N_ALIGNAS:
  case N_STAR:
  case N_POINTER:
  case N_DOTS:
  case N_ARR:
  case N_INIT:
  case N_FIELD_ID:
  case N_TYPE:
  case N_ST_ASSERT:
    fprintf (f, "\n");
    print_ops (ctx, f, n, indent, attr_p);
    break;
  case N_LIST:
    if (attr_p && n->attr != NULL) {
      fprintf (f, ": ");
      print_decl_spec (ctx, f, (struct decl_spec *) n->attr);
    }
    fprintf (f, "\n");
    print_ops (ctx, f, n, indent, attr_p);
    break;
  case N_SPEC_DECL:
  case N_MEMBER:
  case N_FUNC_DEF:
    if (attr_p && n->attr != NULL) print_decl (ctx, f, (decl_t) n->attr);
    fprintf (f, "\n");
    print_ops (ctx, f, n, indent, attr_p);
    break;
  case N_FUNC:
    if (!attr_p || n->attr == NULL) {
      fprintf (f, "\n");
      print_ops (ctx, f, n, indent, attr_p);
      break;
    }
    /* fall through: */
  case N_STRUCT:
  case N_UNION:
  case N_MODULE:
  case N_BLOCK:
  case N_FOR:
    if (!attr_p
        || ((n->code == N_STRUCT || n->code == N_UNION)
            && (NL_EL (n->ops, 1) == NULL || NL_EL (n->ops, 1)->code == N_IGNORE)))
      fprintf (f, "\n");
    else if (n->code == N_MODULE)
      fprintf (f, ": the top scope");
    else if (n->attr != NULL)
      fprintf (f, ": higher scope node %lu", ((struct node_scope *) n->attr)->scope->uid);
    if (n->code == N_STRUCT || n->code == N_UNION)
      fprintf (f, "\n");
    else if (attr_p && n->attr != NULL)
      fprintf (f, ", size = %llu, offset = %llu\n",
               (unsigned long long) ((struct node_scope *) n->attr)->size,
               (unsigned long long) ((struct node_scope *) n->attr)->offset);
    print_ops (ctx, f, n, indent, attr_p);
    break;
  case N_SWITCH:
    if (attr_p && n->attr != NULL) {
      fprintf (f, ": ");
      print_type (ctx, f, &((struct switch_attr *) n->attr)->type);
    }
    fprintf (f, "\n");
    print_ops (ctx, f, n, indent, attr_p);
    break;
  case N_GOTO:
    if (attr_p && n->attr != NULL) fprintf (f, ": target node %lu\n", ((node_t) n->attr)->uid);
    print_ops (ctx, f, n, indent, attr_p);
    break;
  case N_ENUM_CONST:
    if (attr_p && n->attr != NULL)
      fprintf (f, ": val = %lld\n", (long long) ((struct enum_value *) n->attr)->val);
    print_ops (ctx, f, n, indent, attr_p);
    break;
  default: abort ();
  }
}

static void init_include_dirs (MIR_context_t ctx) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);
  const char *str;

  VARR_CREATE (char_ptr_t, headers, 0);
  VARR_CREATE (char_ptr_t, system_headers, 0);
  for (size_t i = 0; i < options->include_dirs_num; i++) {
    VARR_PUSH (char_ptr_t, headers, options->include_dirs[i]);
    VARR_PUSH (char_ptr_t, system_headers, options->include_dirs[i]);
  }
  VARR_PUSH (char_ptr_t, headers, NULL);
  for (size_t i = 0; i < sizeof (standard_include_dirs) / sizeof (char *); i++) {
    VARR_TRUNC (char, temp_string, 0);
    add_to_temp_string (c2m_ctx, SOURCEDIR);
    add_to_temp_string (c2m_ctx, standard_include_dirs[i]);
    str = uniq_cstr (c2m_ctx, VARR_ADDR (char, temp_string)).s;
    VARR_PUSH (char_ptr_t, system_headers, str);
    VARR_TRUNC (char, temp_string, 0);
    add_to_temp_string (c2m_ctx, INSTALLDIR);
    add_to_temp_string (c2m_ctx, "../");
    add_to_temp_string (c2m_ctx, standard_include_dirs[i]);
    str = uniq_cstr (c2m_ctx, VARR_ADDR (char, temp_string)).s;
    VARR_PUSH (char_ptr_t, system_headers, str);
  }
#ifdef __linux__
  VARR_PUSH (char_ptr_t, system_headers, "/usr/include");
#endif
  VARR_PUSH (char_ptr_t, system_headers, NULL);
  header_dirs = (const char **) VARR_ADDR (char_ptr_t, headers);
  system_header_dirs = (const char **) VARR_ADDR (char_ptr_t, system_headers);
}

static int check_id_p (c2m_ctx_t c2m_ctx, const char *str) {
  int ok_p;

  if ((ok_p = isalpha (str[0]) || str[0] == '_')) {
    for (size_t i = 1; str[i] != '\0'; i++)
      if (!isalnum (str[i]) && str[i] != '_') {
        ok_p = FALSE;
        break;
      }
  }
  if (!ok_p && options->message_file != NULL)
    fprintf (options->message_file, "macro name %s is not an identifier\n", str);
  return ok_p;
}

static void define_cmd_macro (c2m_ctx_t c2m_ctx, const char *name, const char *def) {
  pos_t pos;
  token_t t, id;
  struct macro macro;
  macro_t tab_m;
  VARR (token_t) * repl;

  pos.fname = COMMAND_LINE_SOURCE_NAME;
  pos.lno = 1;
  pos.ln_pos = 0;
  VARR_CREATE (token_t, repl, 16);
  id = new_id_token (c2m_ctx, pos, name);
  VARR_TRUNC (char, temp_string, 0);
  for (; *def != '\0'; def++) VARR_PUSH (char, temp_string, *def);
  VARR_PUSH (char, temp_string, '\0');
  reverse (temp_string);
  set_string_stream (c2m_ctx, VARR_ADDR (char, temp_string), pos, NULL);
  while ((t = get_next_pptoken (c2m_ctx))->code != T_EOFILE && t->code != T_EOU)
    VARR_PUSH (token_t, repl, t);
  if (check_id_p (c2m_ctx, id->repr)) {
    macro.id = id;
    if (HTAB_DO (macro_t, macro_tab, &macro, HTAB_FIND, tab_m)) {
      if (!replacement_eq_p (tab_m->replacement, repl) && options->message_file != NULL)
        fprintf (options->message_file, "warning -- redefinition of macro %s on the command line\n",
                 id->repr);
      HTAB_DO (macro_t, macro_tab, &macro, HTAB_DELETE, tab_m);
    }
    new_macro (c2m_ctx, macro.id, NULL, repl);
  }
}

static void undefine_cmd_macro (c2m_ctx_t c2m_ctx, const char *name) {
  pos_t pos;
  token_t id;
  struct macro macro;
  macro_t tab_m;

  pos.fname = COMMAND_LINE_SOURCE_NAME;
  pos.lno = 1;
  pos.ln_pos = 0;
  id = new_id_token (c2m_ctx, pos, name);
  if (check_id_p (c2m_ctx, id->repr)) {
    macro.id = id;
    HTAB_DO (macro_t, macro_tab, &macro, HTAB_DELETE, tab_m);
  }
}

static void process_macro_commands (MIR_context_t ctx) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);

  for (size_t i = 0; i < options->macro_commands_num; i++)
    if (options->macro_commands[i].def)
      define_cmd_macro (c2m_ctx, options->macro_commands[i].name, options->macro_commands[i].def);
    else
      undefine_cmd_macro (c2m_ctx, options->macro_commands[i].name);
}

static void compile_init (MIR_context_t ctx, struct c2mir_options *ops, int (*getc_func) (void)) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);

  options = ops;
  n_errors = n_warnings = 0;
  c_getc = getc_func;
  VARR_CREATE (char, symbol_text, 128);
  VARR_CREATE (char, temp_string, 128);
  parse_init (ctx);
  curr_scope = NULL;
  context_init (ctx);
  init_include_dirs (ctx);
  process_macro_commands (ctx);
  VARR_CREATE (node_t, call_nodes, 128); /* used in context and gen */
  VARR_CREATE (node_t, containing_anon_members, 8);
  VARR_CREATE (init_object_t, init_object_path, 8);
}

static void compile_finish (MIR_context_t ctx) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);

  if (symbol_text != NULL) VARR_DESTROY (char, symbol_text);
  if (temp_string != NULL) VARR_DESTROY (char, temp_string);
  parse_finish (c2m_ctx);
  context_finish (ctx);
  if (headers != NULL) VARR_DESTROY (char_ptr_t, headers);
  if (system_headers != NULL) VARR_DESTROY (char_ptr_t, system_headers);
  if (call_nodes != NULL) VARR_DESTROY (node_t, call_nodes);
  if (containing_anon_members != NULL) VARR_DESTROY (node_t, containing_anon_members);
  if (init_object_path != NULL) VARR_DESTROY (init_object_t, init_object_path);
}

#include <sys/time.h>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

static double real_usec_time (void) {
  struct timeval tv;

  gettimeofday (&tv, NULL);
  return tv.tv_usec + tv.tv_sec * 1000000.0;
}

static const char *get_module_name (MIR_context_t ctx) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);
  static char str[50];

  sprintf (str, "M%d", options->module_num);
  return str;
}

static int top_level_getc (c2m_ctx_t c2m_ctx) { return c_getc (); }

int c2mir_compile (MIR_context_t ctx, struct c2mir_options *ops, int (*getc_func) (void),
                   const char *source_name, FILE *output_file) {
  c2m_ctx_t c2m_ctx = *c2m_ctx_loc (ctx);
  double start_time = real_usec_time ();
  node_t r;
  unsigned n_error_before;
  MIR_module_t m;
  const char *base_name;

  if (c2m_ctx == NULL) return 0;
  if (setjmp (c2m_ctx->env)) {
    compile_finish (ctx);
    return 0;
  }
  compile_init (ctx, ops, getc_func);
  if (options->verbose_p && options->message_file != NULL)
    fprintf (options->message_file, "C2MIR init end           -- %.0f usec\n",
             real_usec_time () - start_time);
  add_stream (c2m_ctx, NULL, source_name, top_level_getc);
  if (!options->no_prepro_p) add_standard_includes (c2m_ctx);
  pre (c2m_ctx, source_name);
  if (options->verbose_p && options->message_file != NULL)
    fprintf (options->message_file, "  C2MIR preprocessor end    -- %.0f usec\n",
             real_usec_time () - start_time);
  if (!options->prepro_only_p) {
    r = parse (c2m_ctx);
    if (options->verbose_p && options->message_file != NULL)
      fprintf (options->message_file, "  C2MIR parser end          -- %.0f usec\n",
               real_usec_time () - start_time);
    if (options->verbose_p && options->message_file != NULL && n_errors)
      fprintf (options->message_file, "parser - FAIL\n");
    if (!options->syntax_only_p) {
      n_error_before = n_errors;
      do_context (c2m_ctx, r);
      if (n_errors > n_error_before) {
        if (options->debug_p) print_node (ctx, options->message_file, r, 0, FALSE);
        if (options->verbose_p && options->message_file != NULL)
          fprintf (options->message_file, "C2MIR context checker - FAIL\n");
      } else {
        if (options->debug_p) print_node (ctx, options->message_file, r, 0, TRUE);
        if (options->verbose_p && options->message_file != NULL)
          fprintf (options->message_file, "  C2MIR context checker end -- %.0f usec\n",
                   real_usec_time () - start_time);
        m = MIR_new_module (ctx, get_module_name (ctx));
        gen_mir (ctx, r);
        if ((options->asm_p || options->object_p) && n_errors == 0) {
          if (strcmp (source_name, COMMAND_LINE_SOURCE_NAME) == 0) {
            MIR_output_module (ctx, options->message_file, m);
          } else if (output_file != NULL) {
            (options->asm_p ? MIR_output_module : MIR_write_module) (ctx, output_file, m);
            if (ferror (output_file) || fclose (output_file)) {
              fprintf (options->message_file, "C2MIR error in writing file %s\n", base_name);
              n_errors++;
            }
          }
        }
        MIR_finish_module (ctx);
        if (options->verbose_p && options->message_file != NULL)
          fprintf (options->message_file, "  C2MIR generator end       -- %.0f usec\n",
                   real_usec_time () - start_time);
      }
    }
  }
  compile_finish (ctx);
  if (options->verbose_p && options->message_file != NULL)
    fprintf (options->message_file, "C2MIR compiler end                -- %.0f usec\n",
             real_usec_time () - start_time);
  return n_errors == 0;
}

/* Local Variables:                */
/* mode: c                         */
/* page-delimiter: "/\\* New Page" */
/* End:                            */
