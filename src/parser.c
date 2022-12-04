// Parser
// SPDX-License-Identifier: Apache-2.0
#include "c0lib.h"
#include "compiler.h"
#include "abuf.h"

#include <stdlib.h>

// #define LOG_PRATT(fmt, args...) log("parse> " fmt, ##args)
#if !defined(LOG_PRATT) || !defined(DEBUG)
  #undef LOG_PRATT
  #define LOG_PRATT(args...) ((void)0)
#else
  #define LOG_PRATT_ENABLED
#endif

typedef enum {
  PREC_COMMA,         // ,
  PREC_ASSIGN,        // =  +=  -=  |=  (et al ...)
  PREC_LOGICAL_OR,    // ||
  PREC_LOGICAL_AND,   // &&
  PREC_BITWISE_OR,    // |
  PREC_BITWISE_XOR,   // ^
  PREC_BITWISE_AND,   // &
  PREC_EQUAL,         // ==  !=
  PREC_COMPARE,       // <  <=  >  >=
  PREC_SHIFT,         // <<  >>
  PREC_ADD,           // +  -
  PREC_MUL,           // *  /  %
  PREC_UNARY_PREFIX,  // ++  --  +  -  !  ~  *  &  ?
  PREC_UNARY_POSTFIX, // ++  --  ()  []
  PREC_MEMBER,        // .

  PREC_LOWEST = PREC_COMMA,
} prec_t;


//#define PPARAMS parser_t* p, prec_t prec
#define PARGS   p, prec

typedef stmt_t*(*prefix_stmt_parselet_t)(parser_t* p);
typedef stmt_t*(*infix_stmt_parselet_t)(parser_t* p, prec_t prec, stmt_t* left);

typedef expr_t*(*prefix_expr_parselet_t)(parser_t*, exprflag_t);
typedef expr_t*(*infix_expr_parselet_t)(parser_t*, prec_t, expr_t*, exprflag_t);

typedef type_t*(*prefix_type_parselet_t)(parser_t* p);
typedef type_t*(*infix_type_parselet_t)(parser_t* p, prec_t prec, type_t* left);

typedef struct {
  prefix_stmt_parselet_t nullable prefix;
  infix_stmt_parselet_t  nullable infix;
  prec_t                    prec;
} stmt_parselet_t;

typedef struct {
  prefix_expr_parselet_t nullable prefix;
  infix_expr_parselet_t  nullable infix;
  prec_t                    prec;
} expr_parselet_t;

typedef struct {
  prefix_type_parselet_t nullable prefix;
  infix_type_parselet_t  nullable infix;
  prec_t                    prec;
} type_parselet_t;

// parselet table (defined towards end of file)
static const stmt_parselet_t stmt_parsetab[TOK_COUNT];
static const expr_parselet_t expr_parsetab[TOK_COUNT];
static const type_parselet_t type_parsetab[TOK_COUNT];

// last_resort_node is returned by mknode when memory allocation fails
static struct { node_t; u8 opaque[64]; } _last_resort_node = { .kind=NODE_BAD };
node_t* last_resort_node = (node_t*)&_last_resort_node;


static u32 u64log10(u64 u) {
  // U64_MAX 18446744073709551615
  u32 w = 20;
  u64 x = 10000000000000000000llu;
  while (w > 1) {
    if (u >= x)
      break;
    x /= 10;
    w--;
  }
  return w;
}


inline static scanstate_t save_scanstate(parser_t* p) {
  return *(scanstate_t*)&p->scanner;
}

inline static void restore_scanstate(parser_t* p, scanstate_t state) {
  *(scanstate_t*)&p->scanner = state;
}


inline static tok_t currtok(parser_t* p) {
  return p->scanner.tok.t;
}

inline static srcloc_t currloc(parser_t* p) {
  return p->scanner.tok.loc;
}


static void next(parser_t* p) {
  scanner_next(&p->scanner);
}


// static tok_t lookahead(parser_t* p, u32 distance) {
//   scanstate_t scanstate = save_scanstate(p);
//   while (distance--)
//     next(p);
//   tok_t tok = currtok(p);
//   restore_scanstate(p, scanstate);
//   return tok;
// }


static bool lookahead_issym(parser_t* p, sym_t sym) {
  scanstate_t scanstate = save_scanstate(p);
  next(p);
  bool ok = currtok(p) == TID && p->scanner.sym == sym;
  restore_scanstate(p, scanstate);
  return ok;
}


#ifdef LOG_PRATT_ENABLED
  static void log_pratt(parser_t* p, const char* msg) {
    log("parse> %s:%u:%u\t%-12s %s",
      p->scanner.tok.loc.input->name,
      p->scanner.tok.loc.line,
      p->scanner.tok.loc.col,
      tok_name(currtok(p)),
      msg);
  }
  static void log_pratt_infix(
    parser_t* p, const char* class,
    const void* nullable parselet_infix, prec_t parselet_prec,
    prec_t ctx_prec)
  {
    char buf[128];
    abuf_t a = abuf_make(buf, sizeof(buf));
    abuf_fmt(&a, "infix %s ", class);
    if (parselet_infix && parselet_prec >= ctx_prec) {
      abuf_str(&a, "match");
    } else if (parselet_infix) {
      abuf_fmt(&a, "(skip; prec(%d) < ctx_prec(%d))", parselet_prec, ctx_prec);
    } else {
      abuf_str(&a, "(no match)");
    }
    abuf_terminate(&a);
    return log_pratt(p, buf);
  }
#else
  #define log_pratt_infix(...) ((void)0)
  #define log_pratt(...) ((void)0)
#endif


// fastforward advances the scanner until one of the tokens in stoplist is encountered.
// The stoplist token encountered is consumed.
// stoplist should be NULL-terminated.
static void fastforward(parser_t* p, const tok_t* stoplist) {
  while (currtok(p) != TEOF) {
    const tok_t* tp = stoplist;
    while (*tp) {
      if (*tp++ == currtok(p))
        return;
    }
    next(p);
  }
}

static void fastforward_semi(parser_t* p) {
  fastforward(p, (const tok_t[]){ TSEMI, 0 });
}


srcrange_t node_srcrange(const node_t* n) {
  srcrange_t r = { .start = n->loc, .focus = n->loc };
  switch (n->kind) {
    case EXPR_INTLIT:
      r.end.line = r.focus.line;
      r.end.col = r.focus.col + u64log10( ((intlit_t*)n)->intval);
      break;
    case EXPR_ID:
      r.end.line = r.focus.line;
      r.end.col = r.focus.col + strlen(((idexpr_t*)n)->name);
  }
  return r;
}


ATTR_FORMAT(printf,3,4)
static void error1(parser_t* p, srcrange_t srcrange, const char* fmt, ...) {
  if (p->scanner.inp == p->scanner.inend && p->scanner.tok.t == TEOF)
    return;
  va_list ap;
  va_start(ap, fmt);
  report_diagv(p->scanner.compiler, srcrange, DIAG_ERR, fmt, ap);
  va_end(ap);
}


ATTR_FORMAT(printf,3,4)
static void error(parser_t* p, const node_t* nullable n, const char* fmt, ...) {
  if (p->scanner.inp == p->scanner.inend && p->scanner.tok.t == TEOF)
    return;
  srcrange_t srcrange = n ? node_srcrange(n) : (srcrange_t){ .focus = currloc(p), };
  va_list ap;
  va_start(ap, fmt);
  report_diagv(p->scanner.compiler, srcrange, DIAG_ERR, fmt, ap);
  va_end(ap);
}


ATTR_FORMAT(printf,3,4)
static void warning(parser_t* p, const node_t* nullable n, const char* fmt, ...) {
  srcrange_t srcrange = n ? node_srcrange(n) : (srcrange_t){ .focus = currloc(p), };
  va_list ap;
  va_start(ap, fmt);
  report_diagv(p->scanner.compiler, srcrange, DIAG_WARN, fmt, ap);
  va_end(ap);
}


static void out_of_mem(parser_t* p) {
  error(p, NULL, "out of memory");
  // end scanner, making sure we don't keep going
  p->scanner.inp = p->scanner.inend;
}


static const char* fmttok(parser_t* p, usize bufindex, tok_t tok, slice_t lit) {
  buf_t* buf = &p->tmpbuf[bufindex];
  buf_clear(buf);
  buf_reserve(buf, 64);
  tok_descr(buf->p, buf->cap, tok, lit);
  return buf->chars;
}


static const char* fmtnode(parser_t* p, u32 bufindex, const void* n, u32 depth) {
  buf_t* buf = &p->tmpbuf[bufindex];
  buf_clear(buf);
  node_fmt(buf, n, depth);
  return buf->chars;
}


static void unexpected(parser_t* p, const char* errmsg) {
  const char* tokstr = fmttok(p, 0, currtok(p), scanner_lit(&p->scanner));
  int msglen = (int)strlen(errmsg);
  if (msglen && *errmsg != ',' && *errmsg != ';')
    msglen++;
  error(p, NULL, "unexpected %s%*s", tokstr, msglen, errmsg);
}


static void expect_fail(parser_t* p, tok_t expecttok, const char* errmsg) {
  const char* want = fmttok(p, 0, expecttok, (slice_t){0});
  const char* got = fmttok(p, 1, currtok(p), scanner_lit(&p->scanner));
  int msglen = (int)strlen(errmsg);
  if (msglen && *errmsg != ',' && *errmsg != ';')
    msglen++;
  error(p, NULL, "expected %s%*s, got %s", want, msglen, errmsg, got);
}


static bool expect_token(parser_t* p, tok_t expecttok, const char* errmsg) {
  bool ok = currtok(p) == expecttok;
  if UNLIKELY(!ok)
    expect_fail(p, expecttok, errmsg);
  return ok;
}


static bool expect(parser_t* p, tok_t expecttok, const char* errmsg) {
  bool ok = expect_token(p, expecttok, errmsg);
  next(p);
  return ok;
}


static bool expect2(parser_t* p, tok_t tok, const char* errmsg) {
  if LIKELY(currtok(p) == tok) {
    next(p);
    return true;
  }
  unexpected(p, errmsg);
  fastforward(p, (const tok_t[]){ tok, TSEMI, 0 });
  if (currtok(p) == tok)
    next(p);
  return false;
}


#define mknode(p, TYPE, kind)      ( (TYPE*)_mknode((p), sizeof(TYPE), (kind)) )
#define mkexpr(p, TYPE, kind, fl)  ( (TYPE*)_mkexpr((p), sizeof(TYPE), (kind), (fl)) )


node_t* _mknode(parser_t* p, usize size, nodekind_t kind) {
  mem_t m = mem_alloc_zeroed(p->ast_ma, size);
  if UNLIKELY(m.p == NULL)
    return out_of_mem(p), last_resort_node;
  node_t* n = m.p;
  n->kind = kind;
  n->loc = currloc(p);
  return n;
}


static expr_t* _mkexpr(parser_t* p, usize size, nodekind_t kind, exprflag_t fl) {
  assertf(nodekind_isexpr(kind), "%s", nodekind_name(kind));
  expr_t* n = (expr_t*)_mknode(p, size, kind);
  n->flags = fl;
  n->type = type_void;
  return n;
}


static void* mkbad(parser_t* p) {
  expr_t* n = (expr_t*)mknode(p, __typeof__(_last_resort_node), NODE_BAD);
  n->type = type_void;
  return n;
}


static reftype_t* mkreftype(parser_t* p, bool ismut) {
  reftype_t* t = mknode(p, reftype_t, TYPE_REF);
  t->size = p->scanner.compiler->ptrsize;
  t->align = t->size;
  t->ismut = ismut;
  return t;
}


node_t* clone_node(parser_t* p, const node_t* n) {
  switch (n->kind) {
  case EXPR_FIELD:
  case EXPR_PARAM:
  case EXPR_LET:
  case EXPR_VAR:
    return (node_t*)CLONE_NODE(p, (local_t*)n);
  default:
    panic("TODO %s %s", __FUNCTION__, nodekind_name(n->kind));
  }
}


// —————————————————————————————————————————————————————————————————————————————————————


static void enter_scope(parser_t* p) {
  if (!scope_push(&p->scope, p->ma))
    out_of_mem(p);
}


static void leave_scope(parser_t* p) {
  scope_pop(&p->scope);
}


static node_t* nullable lookup(parser_t* p, sym_t name) {
  node_t* n = scope_lookup(&p->scope, name, U32_MAX);
  if (!n) {
    // look in package scope and its parent universe scope
    void** vp = map_lookup(&p->pkgdefs, name, strlen(name));
    if (!vp)
      return NULL;
    n = *vp;
  }
  // increase reference count
  if (node_isexpr(n)) {
    ((expr_t*)n)->nrefs++;
  } else if (node_isusertype(n)) {
    ((usertype_t*)n)->nrefs++;
  }
  return n;
}


static void define_replace(parser_t* p, sym_t name, node_t* n) {
  //dlog("define_replace %s %s", name, nodekind_name(n->kind));
  assert(n->kind != EXPR_ID);
  assert(name != sym__);
  if UNLIKELY(!scope_define(&p->scope, p->ma, name, n))
    out_of_mem(p);
  if (scope_istoplevel(&p->scope)) {
    void** vp = map_assign(&p->pkgdefs, p->ma, name, strlen(name));
    if UNLIKELY(!vp)
      return out_of_mem(p);
    *vp = n;
  }
}


static void define(parser_t* p, sym_t name, node_t* n) {
  if (name == sym__)
    return;
  //dlog("define %s %s", name, nodekind_name(n->kind));

  node_t* existing = scope_lookup(&p->scope, name, 0);
  if (existing)
    goto err_duplicate;

  if (!scope_define(&p->scope, p->ma, name, n))
    out_of_mem(p);

  // top-level definitions also goes into package scope
  if (scope_istoplevel(&p->scope)) {
    void** vp = map_assign(&p->pkgdefs, p->ma, name, strlen(name));
    if (!vp)
      return out_of_mem(p);
    if (*vp)
      goto err_duplicate;
    *vp = n;
  }
  return;
err_duplicate:
  error(p, n, "redefinition of \"%s\"", name);
}


// —————————————————————————————————————————————————————————————————————————————————————


static void push(parser_t* p, ptrarray_t* children, void* child) {
  if UNLIKELY(!ptrarray_push(children, p->ast_ma, child))
    out_of_mem(p);
}


static void typectx_push(parser_t* p, type_t* t) {
  if UNLIKELY(!ptrarray_push(&p->typectxstack, p->ma, p->typectx))
    out_of_mem(p);
  p->typectx = t;
}

static void typectx_pop(parser_t* p) {
  assert(p->typectxstack.len > 0);
  p->typectx = ptrarray_pop(&p->typectxstack);
}


static void dotctx_push(parser_t* p, expr_t* nullable n) {
  if UNLIKELY(!ptrarray_push(&p->dotctxstack, p->ma, p->dotctx))
    out_of_mem(p);
  p->dotctx = n;
}

static void dotctx_pop(parser_t* p) {
  assert(p->dotctxstack.len > 0);
  p->dotctx = ptrarray_pop(&p->dotctxstack);
}


static bool check_types_compat(
  parser_t* p,
  const type_t* nullable x,
  const type_t* nullable y,
  const node_t* nullable origin)
{
  if UNLIKELY(!!x * !!y && !types_iscompat(x, y)) { // "!!x * !!y": ignore NULL
    const char* xs = fmtnode(p, 0, x, 1);
    const char* ys = fmtnode(p, 1, y, 1);
    error(p, origin, "incompatible types, %s and %s", xs, ys);
    return false;
  }
  return true;
}


static stmt_t* stmt(parser_t* p, prec_t prec) {
  tok_t tok = currtok(p);
  const stmt_parselet_t* parselet = &stmt_parsetab[tok];
  log_pratt(p, "prefix stmt");
  if UNLIKELY(!parselet->prefix) {
    unexpected(p, "where a statement is expected");
    fastforward_semi(p);
    return mkbad(p);
  }
  stmt_t* n = parselet->prefix(p);
  for (;;) {
    tok = currtok(p);
    parselet = &stmt_parsetab[tok];
    log_pratt_infix(p, "stmt", parselet->infix, parselet->prec, prec);
    if (parselet->infix == NULL || parselet->prec < prec)
      return n;
    n = parselet->infix(PARGS, n);
  }
}


static expr_t* expr(parser_t* p, prec_t prec, exprflag_t fl) {
  tok_t tok = currtok(p);
  const expr_parselet_t* parselet = &expr_parsetab[tok];
  log_pratt(p, "prefix expr");
  if UNLIKELY(!parselet->prefix) {
    unexpected(p, "where an expression is expected");
    fastforward_semi(p);
    return mkbad(p);
  }
  expr_t* n = parselet->prefix(p, fl);
  for (;;) {
    tok = currtok(p);
    parselet = &expr_parsetab[tok];
    log_pratt_infix(p, "expr", parselet->infix, parselet->prec, prec);
    if (parselet->infix == NULL || parselet->prec < prec)
      return n;
    n = parselet->infix(PARGS, n, fl);
  }
}


static type_t* type(parser_t* p, prec_t prec) {
  tok_t tok = currtok(p);
  const type_parselet_t* parselet = &type_parsetab[tok];
  log_pratt(p, "prefix type");
  if UNLIKELY(!parselet->prefix) {
    unexpected(p, "where type is expected");
    fastforward_semi(p);
    return type_void;
  }
  type_t* t = parselet->prefix(p);
  for (;;) {
    tok = currtok(p);
    parselet = &type_parsetab[tok];
    log_pratt_infix(p, "type", parselet->infix, parselet->prec, prec);
    if (parselet->infix == NULL || parselet->prec < prec)
      return t;
    t = parselet->infix(PARGS, t);
  }
}


static type_t* named_type(parser_t* p, sym_t name, const node_t* nullable origin) {
  const node_t* ref = lookup(p, name);
  if UNLIKELY(!ref) {
    error(p, origin, "unknown type \"%s\"", name);
  } else if UNLIKELY(!node_istype(ref)) {
    error(p, origin, "%s is not a type", name);
  } else {
    return (type_t*)ref;
  }
  return type_void;
}


static type_t* type_id(parser_t* p) {
  type_t* t = named_type(p, p->scanner.sym, NULL);
  next(p);
  return t;
}


fun_t* nullable lookup_method(parser_t* p, type_t* recv, sym_t name) {
  // find method map for recv
  void** mmp = map_lookup_ptr(&p->methodmap, recv);
  if (!mmp)
    return NULL; // no methods on recv
  map_t* mm = assertnotnull(*mmp);

  // find method of name
  void** mp = map_lookup_ptr(mm, name);
  return mp ? assertnotnull(*mp) : NULL;
}


local_t* nullable lookup_struct_field(structtype_t* st, sym_t name) {
  ptrarray_t fields = st->fields;
  for (u32 i = 0; i < fields.len; i++) {
    local_t* f = (local_t*)fields.v[i];
    if (f->name == name)
      return f;
  }
  return NULL;
}


expr_t* nullable lookup_member(parser_t* p, type_t* recv, sym_t name) {
  if (recv->kind == TYPE_STRUCT) {
    local_t* field = lookup_struct_field((structtype_t*)recv, name);
    if (field)
      return (expr_t*)field;
  }
  return (expr_t*)lookup_method(p, recv, name);
}


// field = id ("," id)* type ("=" expr ("," expr))
static bool struct_fieldset(parser_t* p, structtype_t* st) {
  u32 fields_start = st->fields.len;
  for (;;) {
    local_t* f = mknode(p, local_t, EXPR_FIELD);
    f->name = p->scanner.sym;
    expect(p, TID, "");

    if UNLIKELY(lookup_struct_field(st, f->name)) {
      error(p, NULL, "duplicate field %s", f->name);
    } else if UNLIKELY(lookup_method(p, (type_t*)st, f->name)) {
      error(p, NULL, "field %s conflicts with method of same name", f->name);
    }

    push(p, &st->fields, f);
    if (currtok(p) != TCOMMA)
      break;
    next(p);
  }

  type_t* t = type(p, PREC_MEMBER);
  for (u32 i = fields_start; i < st->fields.len; i++)
    ((local_t*)st->fields.v[i])->type = t;

  if (currtok(p) != TASSIGN)
    return false;

  next(p);
  u32 i = fields_start;
  for (;;) {
    if (i == st->fields.len) {
      error(p, NULL, "excess field initializer");
      expr(p, PREC_COMMA, EX_RVALUE);
      break;
    }
    local_t* f = st->fields.v[i++];
    typectx_push(p, f->type);
    f->init = expr(p, PREC_COMMA, EX_RVALUE);
    typectx_pop(p);
    if (currtok(p) != TCOMMA)
      break;
    next(p);
    if UNLIKELY(!types_iscompat(f->type, f->init->type)) {
      const char* got = fmtnode(p, 0, f->init->type, 1);
      const char* expect = fmtnode(p, 1, f->type, 1);
      error(p, (node_t*)f->init,
        "field initializer of type %s where type %s is expected", got, expect);
    }
  }
  if (i < st->fields.len)
    error(p, NULL, "missing field initializer");
  return true;
}


static fun_t* fun(parser_t*, exprflag_t, type_t* nullable methodof, bool requirename);


static type_t* type_struct(parser_t* p) {
  structtype_t* st = mknode(p, structtype_t, TYPE_STRUCT);
  next(p);
  while (currtok(p) != TRBRACE) {
    if (currtok(p) == TFUN) {
      fun_t* f = fun(p, 0, (type_t*)st, /*requirename*/true);
      push(p, &st->methods, f);
    } else {
      st->hasinit |= struct_fieldset(p, st);
    }
    if (currtok(p) != TSEMI)
      break;
    next(p);
  }
  expect(p, TRBRACE, "to end struct");
  for (u32 i = 0; i < st->fields.len; i++) {
    local_t* f = st->fields.v[i];
    type_t* ft = assertnotnull(f->type);
    st->align = MAX(st->align, ft->align);
    st->size += ft->size;
  }
  st->size = ALIGN2(st->size, st->align);
  return (type_t*)st;
}


// ptr_type = "*" type
static type_t* type_ptr(parser_t* p) {
  ptrtype_t* t = mknode(p, ptrtype_t, TYPE_PTR);
  next(p);
  t->size = p->scanner.compiler->ptrsize;
  t->align = t->size;
  t->elem = type(p, PREC_UNARY_PREFIX);
  return (type_t*)t;
}


static type_t* type_ref1(parser_t* p, bool ismut) {
  reftype_t* t = mkreftype(p, ismut);
  next(p);
  t->elem = type(p, PREC_UNARY_PREFIX);
  return (type_t*)t;
}


// ref_type = "&" type
static type_t* type_ref(parser_t* p) {
  return type_ref1(p, /*ismut*/false);
}


// mut_type = "mut" ref_type
static type_t* type_mut(parser_t* p) {
  next(p);
  if UNLIKELY(currtok(p) != TAND) {
    unexpected(p, "expecting '&'");
    return mkbad(p);
  }
  return type_ref1(p, /*ismut*/true);
}


// optional_type = "?" type
static type_t* type_optional(parser_t* p) {
  opttype_t* t = mknode(p, opttype_t, TYPE_OPTIONAL);
  next(p);
  t->elem = type(p, PREC_UNARY_PREFIX);
  return (type_t*)t;
}


// typedef = "type" id type
static stmt_t* stmt_typedef(parser_t* p) {
  typedef_t* n = mknode(p, typedef_t, STMT_TYPEDEF);
  next(p);
  n->name = p->scanner.sym;
  bool nameok = expect(p, TID, "");
  if (nameok)
    define(p, n->name, (node_t*)n);
  n->type = type(p, PREC_COMMA);
  if (nameok && !scope_define(&p->scope, p->ma, n->name, n->type))
    out_of_mem(p);
  if (n->type->kind == TYPE_STRUCT)
    ((structtype_t*)n->type)->name = n->name;
  return (stmt_t*)n;
}


static bool resolve_id(parser_t* p, idexpr_t* n) {
  n->ref = lookup(p, n->name);
  if UNLIKELY(!n->ref) {
    error(p, (node_t*)n, "undeclared identifier \"%s\"", n->name);
    return false;
  } else if (node_isexpr(n->ref)) {
    n->type = ((expr_t*)n->ref)->type;
  } else if (nodekind_istype(n->ref->kind)) {
    n->type = (type_t*)n->ref;
  } else {
    error(p, (node_t*)n, "cannot use %s \"%s\" as an expression",
      nodekind_fmt(n->ref->kind), n->name);
    return false;
  }
  return true;
}


static expr_t* expr_id(parser_t* p, exprflag_t fl) {
  idexpr_t* n = mkexpr(p, idexpr_t, EXPR_ID, fl);
  n->name = p->scanner.sym;
  next(p);
  resolve_id(p, n);
  return (expr_t*)n;
}


static expr_t* expr_var(parser_t* p, exprflag_t fl) {
  local_t* n = mkexpr(p, local_t, currtok(p) == TLET ? EXPR_LET : EXPR_VAR, fl);
  next(p);
  if (currtok(p) != TID) {
    unexpected(p, "expecting identifier");
    return mkbad(p);
  } else {
    n->name = p->scanner.sym;
    next(p);
  }
  bool ok = true;
  if (currtok(p) == TASSIGN) {
    next(p);
    typectx_push(p, type_void);
    n->init = expr(p, PREC_ASSIGN, fl | EX_RVALUE);
    typectx_pop(p);
    n->type = n->init->type;
  } else {
    n->type = type(p, PREC_LOWEST);
    if (currtok(p) == TASSIGN) {
      next(p);
      typectx_push(p, n->type);
      n->init = expr(p, PREC_ASSIGN, fl | EX_RVALUE);
      typectx_pop(p);
      ok = check_types_compat(p, n->type, n->init->type, (node_t*)n->init);
    }
  }

  define(p, n->name, (node_t*)n);

  // check for required initializer expression
  if (!n->init && ok) {
    if UNLIKELY(n->kind == EXPR_LET) {
      error(p, NULL, "missing value for let binding, expecting '='");
      ok = false;
    } else if UNLIKELY(n->type->kind == TYPE_REF) {
      error(p, NULL, "missing initial value for reference variable, expecting '='");
      ok = false;
    }
  }

  return (expr_t*)n;
}


static void clear_rvalue(parser_t* p, expr_t* n) {
  n->flags &= ~EX_RVALUE;
  switch (n->kind) {
    case EXPR_IF:
      clear_rvalue(p, (expr_t*)((ifexpr_t*)n)->thenb);
      if (((ifexpr_t*)n)->elseb)
        clear_rvalue(p, (expr_t*)((ifexpr_t*)n)->elseb);
      break;
    case EXPR_BLOCK: {
      block_t* b = (block_t*)n;
      for (u32 i = 0; i < b->children.len; i++)
        clear_rvalue(p, b->children.v[i]);
      break;
    }
  }
}


static block_t* block(parser_t* p, exprflag_t fl) {
  block_t* n = mkexpr(p, block_t, EXPR_BLOCK, fl);
  next(p);

  bool isrvalue = fl & EX_RVALUE;
  fl &= ~EX_RVALUE;
  u32 exit_expr_index = 0;
  bool reported_unreachable = false;

  if (currtok(p) != TRBRACE && currtok(p) != TEOF) {
    for (;;) {
      expr_t* cn = expr(p, PREC_LOWEST, fl);
      if UNLIKELY(!ptrarray_push(&n->children, p->ast_ma, cn)) {
        out_of_mem(p);
        break;
      }

      if (n->flags & EX_EXITS) {
        if (!reported_unreachable) {
          reported_unreachable = true;
          warning(p, (node_t*)cn, "unreachable code");
        }
      } else if (cn->kind == EXPR_RETURN) {
        exit_expr_index = n->children.len - 1;
        n->flags |= EX_EXITS;
      }
      // } else {
      //   // treat _all_ block-level expressions as rvalues, with some exceptions
      //   switch (cn->kind) {
      //     case EXPR_RETURN:
      //       exit_expr_index = n->children.len - 1;
      //       n->flags |= EX_EXITS;
      //       break;
      //     case EXPR_FUN:
      //     case EXPR_BLOCK:
      //     case EXPR_CALL:
      //     case EXPR_VAR:
      //     case EXPR_LET:
      //     case EXPR_IF:
      //     case EXPR_FOR:
      //     case EXPR_BOOLLIT:
      //     case EXPR_INTLIT:
      //     case EXPR_FLOATLIT:
      //       break;
      //     default:
      //       // e.g. "z" in "{ z; 3 }"
      //       check_rvalue(p, cn);
      //   }
      // }

      if (currtok(p) != TSEMI)
        break;
      next(p); // consume ";"

      if (currtok(p) == TRBRACE || currtok(p) == TEOF)
        break;

      clear_rvalue(p, cn);
    }
  }

  expect2(p, TRBRACE, ", expected '}' or ';'");

  // if (isrvalue) {
  //   check_rvalue(p, (expr_t*)n);
  // } else if (n->children.len > 0) {
  if (!isrvalue && n->children.len > 0) {
    u32 index = n->children.len-1;
    if (n->flags & EX_EXITS)
      index = exit_expr_index;
    clear_rvalue(p, n->children.v[index]);
  }

  return n;
}


static block_t* any_as_block(parser_t* p, exprflag_t fl) {
  if (currtok(p) == TLBRACE)
    return block(p, fl);
  block_t* n = mkexpr(p, block_t, EXPR_BLOCK, fl);
  expr_t* cn = expr(p, PREC_COMMA, fl);
  if UNLIKELY(!ptrarray_push(&n->children, p->ast_ma, cn))
    out_of_mem(p);
  return n;
}


static expr_t* expr_block(parser_t* p, exprflag_t fl) {
  enter_scope(p);
  block_t* n = block(p, fl);
  leave_scope(p);
  return (expr_t*)n;
}


static expr_t* nullable check_if_cond(parser_t* p, expr_t* cond) {
  if (cond->type->kind == TYPE_BOOL)
    return NULL;

  if (!type_isopt(cond->type)) {
    error(p, (node_t*)cond, "conditional is not a boolean");
    return NULL;
  }

  opttype_t* opt_type = (opttype_t*)cond->type;

  // redefine as non-optional
  switch (cond->kind) {
    case EXPR_ID: {
      // e.g. "if x { ... }"
      idexpr_t* id = (idexpr_t*)cond;
      if (!node_isexpr(id->ref)) {
        error(p, (node_t*)cond, "conditional is not an expression");
        return NULL;
      }

      idexpr_t* id2 = CLONE_NODE(p, id);
      id2->type = opt_type->elem;
      // id2->flags &= ~EX_RVALUE_CHECKED;

      expr_t* ref2 = (expr_t*)clone_node(p, id->ref);
      ref2->flags |= EX_SHADOWS_OPTIONAL;
      // ref2->flags &= ~EX_RVALUE_CHECKED;
      ref2->type = opt_type->elem;
      define_replace(p, id->name, (node_t*)ref2);

      return (expr_t*)id2;
    }
    case EXPR_LET:
    case EXPR_VAR: {
      // e.g. "if let x = expr { ... }"
      ((local_t*)cond)->type = opt_type->elem;
      cond->flags |= EX_OPTIONAL;
      break;
    }
  }

  return NULL;
}


static expr_t* expr_if(parser_t* p, exprflag_t fl) {
  ifexpr_t* n = mkexpr(p, ifexpr_t, EXPR_IF, fl);
  next(p);

  // enter "cond" scope
  enter_scope(p);

  n->cond = expr(p, PREC_COMMA, fl | EX_RVALUE);
  expr_t* type_narrowed_binding = check_if_cond(p, n->cond);

  // "then"
  enter_scope(p);
  n->thenb = any_as_block(p, fl);
  leave_scope(p);

  // "else"
  if (currtok(p) == TELSE) {
    next(p);
    enter_scope(p);
    n->elseb = any_as_block(p, fl);
    leave_scope(p);
  }

  // leave "cond" scope
  leave_scope(p);

  if (type_narrowed_binding) {
    expr_t* dst = n->cond;
    while (dst->kind == EXPR_ID && node_isexpr(((idexpr_t*)dst)->ref))
      dst = (expr_t*)((idexpr_t*)dst)->ref;
    dst->nrefs += type_narrowed_binding->nrefs;
  }

  return (expr_t*)n;
}


// for       = "for" ( for_head | for_phead ) expr
// for_head  = ( expr | expr? ";" expr ";" expr? )
// for_phead = "(" for_head ")"
static expr_t* expr_for(parser_t* p, exprflag_t fl) {
  forexpr_t* n = mkexpr(p, forexpr_t, EXPR_FOR, fl);
  next(p);
  bool paren = currtok(p) == TLPAREN;
  if (paren)
    next(p);
  if (currtok(p) == TSEMI) {
    // "for ; i < 4; i++"
    next(p);
    n->cond = expr(p, PREC_COMMA, fl);
    expect(p, TSEMI, "");
    next(p);
    n->end = expr(p, PREC_COMMA, fl);
  } else {
    // "for i < 4"
    n->cond = expr(p, PREC_COMMA, fl);
    if (currtok(p) == TSEMI) {
      // "for i = 0; i < 4; i++"
      next(p);
      n->start = n->cond;
      n->cond = expr(p, PREC_COMMA, fl);
      expect(p, TSEMI, "");
      n->end = expr(p, PREC_COMMA, fl);
    }
  }
  if (paren)
    expect(p, TRPAREN, "");
  n->body = expr(p, PREC_COMMA, fl);
  return (expr_t*)n;
}


// return = "return" (expr ("," expr)*)?
static expr_t* expr_return(parser_t* p, exprflag_t fl) {
  retexpr_t* n = mkexpr(p, retexpr_t, EXPR_RETURN, fl | EX_RVALUE_CHECKED);
  next(p);
  if (currtok(p) == TSEMI)
    return (expr_t*)n;
  n->value = expr(p, PREC_COMMA, fl | EX_RVALUE);
  n->type = n->value->type;
  return (expr_t*)n;
}


static type_t* select_int_type(parser_t* p, const intlit_t* n, u64 isneg) {
  type_t* type = p->typectx;
  u64 maxval = 0;
  u64 uintval = n->intval;
  if (isneg)
    uintval &= ~0x1000000000000000; // clear negative bit

  bool u = type->isunsigned;

  switch (type->kind) {
  case TYPE_I8:  maxval = u ? 0xffllu               : 0x7fllu+isneg; break;
  case TYPE_I16: maxval = u ? 0xffffllu             : 0x7fffllu+isneg; break;
  case TYPE_I32: maxval = u ? 0xffffffffllu         : 0x7fffffffllu+isneg; break;
  case TYPE_I64: maxval = u ? 0xffffffffffffffffllu : 0x7fffffffffffffffllu+isneg; break;
  default: // all other type contexts results in TYPE_INT
    if (isneg) {
      if (uintval <= 0x80000000llu)         return type_int;
      if (uintval <= 0x8000000000000000llu) return type_i64;
      // trigger error report
      maxval = 0x8000000000000000llu;
      type = type_i64;
    } else {
      if (n->intval <= 0x7fffffffllu)         return type_int;
      if (n->intval <= 0x7fffffffffffffffllu) return type_i64;
      maxval = 0xffffffffffffffffllu;
      type = type_u64;
    }
  }

  if UNLIKELY(uintval > maxval) {
    const char* ts = fmtnode(p, 0, type, 1);
    slice_t lit = scanner_lit(&p->scanner);
    error(p, (node_t*)n, "integer constant %s%.*s overflows %s",
      isneg ? "-" : "", (int)lit.len, lit.chars, ts);
  }
  return type;
}


static expr_t* intlit(parser_t* p, exprflag_t fl, bool isneg) {
  intlit_t* n = mkexpr(p, intlit_t, EXPR_INTLIT, fl | EX_RVALUE_CHECKED | EX_ANALYZED);
  n->intval = p->scanner.litint;
  n->type = select_int_type(p, n, (u64)isneg);
  next(p);
  return (expr_t*)n;
}


static expr_t* floatlit(parser_t* p, exprflag_t fl, bool isneg) {
  floatlit_t* n = mkexpr(p, floatlit_t, EXPR_FLOATLIT,
    fl | EX_RVALUE_CHECKED | EX_ANALYZED);
  char* endptr = NULL;

  // note: scanner always starts float litbuf with '+'
  if (isneg)
    p->scanner.litbuf.chars[0] = '-';

  if (p->typectx == type_f32) {
    n->type = type_f32;
    n->f32val = strtof(p->scanner.litbuf.chars, &endptr);
    if (endptr != p->scanner.litbuf.chars + p->scanner.litbuf.len) {
      error(p, (node_t*)n, "invalid floating-point constant");
    } else if (n->f32val == HUGE_VALF) {
      error(p, (node_t*)n, "32-bit floating-point constant too large");
    }
  } else {
    n->type = type_f64;
    n->f64val = strtod(p->scanner.litbuf.chars, &endptr);
    if (endptr != p->scanner.litbuf.chars + p->scanner.litbuf.len) {
      error(p, (node_t*)n, "invalid floating-point constant");
    } else if (n->f64val == HUGE_VAL) {
      // e.g. 1.e999
      error(p, (node_t*)n, "64-bit floating-point constant too large");
    }
  }

  next(p);
  return (expr_t*)n;
}


static expr_t* expr_intlit(parser_t* p, exprflag_t fl) {
  return intlit(p, fl, /*isneg*/false);
}


static expr_t* expr_floatlit(parser_t* p, exprflag_t fl) {
  return floatlit(p, fl, /*isneg*/false);
}


static expr_t* expr_prefix_op(parser_t* p, exprflag_t fl) {
  unaryop_t* n = mkexpr(p, unaryop_t, EXPR_PREFIXOP, fl);
  n->op = currtok(p);
  next(p);
  fl |= EX_RVALUE;
  switch (currtok(p)) {
    // special case for negative number constants
    case TINTLIT:   n->expr = intlit(p, /*isneg*/n->op == TMINUS, fl); break;
    case TFLOATLIT: n->expr = floatlit(p, /*isneg*/n->op == TMINUS, fl); break;
    default:        n->expr = expr(p, PREC_UNARY_PREFIX, fl);
  }
  n->type = n->expr->type;
  return (expr_t*)n;
}


static expr_t* expr_infix_op(parser_t* p, prec_t prec, expr_t* left, exprflag_t fl) {
  binop_t* n = mkexpr(p, binop_t, EXPR_BINOP, fl);
  n->op = currtok(p);
  next(p);

  left->flags |= EX_RVALUE;
  n->left = left;

  typectx_push(p, left->type);
  n->right = expr(p, prec, fl | EX_RVALUE);
  typectx_pop(p);

  n->type = left->type;
  return (expr_t*)n;
}


static expr_t* expr_cmp_op(parser_t* p, prec_t prec, expr_t* left, exprflag_t fl) {
  expr_t* n = expr_infix_op(p, prec, left, fl);
  n->type = type_bool;
  return n;
}


static bool expr_isstorage(const expr_t* n) {
  switch (n->kind) {
  case EXPR_ID: {
    const idexpr_t* id = (const idexpr_t*)n;
    return id->ref && nodekind_isexpr(id->ref->kind) && expr_isstorage((expr_t*)id->ref);
  }
  case EXPR_MEMBER:
  case EXPR_PARAM:
  case EXPR_LET:
  case EXPR_VAR:
  case EXPR_FUN:
  case EXPR_DEREF:
    return true;
  default:
    return false;
  }
}


// expr_ismut returns true if n is something that can be mutated
static bool expr_ismut(const expr_t* n) {
  assert(expr_isstorage(n));
  switch (n->kind) {
  case EXPR_ID: {
    const idexpr_t* id = (const idexpr_t*)n;
    return id->ref && nodekind_isexpr(id->ref->kind) && expr_ismut((expr_t*)id->ref);
  }
  case EXPR_MEMBER: {
    const member_t* m = (const member_t*)n;
    return expr_ismut(m->target) && expr_ismut(m->recv);
  }
  case EXPR_PARAM:
  case EXPR_VAR:
    return true;
  default:
    return false;
  }
  return true;
}


// postfix_op = expr ("++" | "--")
static expr_t* postfix_op(parser_t* p, prec_t prec, expr_t* left, exprflag_t fl) {
  unaryop_t* n = mkexpr(p, unaryop_t, EXPR_POSTFIXOP, fl);
  n->op = currtok(p);
  next(p);
  n->expr = left;
  n->type = left->type;
  return (expr_t*)n;
}


// deref_expr = "*" expr
static expr_t* expr_deref(parser_t* p, exprflag_t fl) {
  unaryop_t* n = mkexpr(p, unaryop_t, EXPR_DEREF, fl);
  n->op = currtok(p);
  next(p);
  n->expr = expr(p, PREC_UNARY_PREFIX, fl);
  reftype_t* t = (reftype_t*)n->expr->type;

  if UNLIKELY(t->kind != TYPE_REF) {
    const char* ts = fmtnode(p, 0, t, 1);
    error(p, (node_t*)n, "dereferencing non-reference value of type %s", ts);
  } else {
    n->type = t->elem;
  }

  return (expr_t*)n;
}


// ref_expr = "&" location
static expr_t* expr_ref1(parser_t* p, bool ismut, exprflag_t fl) {
  unaryop_t* n = mkexpr(p, unaryop_t, EXPR_PREFIXOP, fl);
  n->op = currtok(p);
  next(p);
  n->expr = expr(p, PREC_UNARY_PREFIX, fl | EX_RVALUE);

  if UNLIKELY(n->expr->type->kind == TYPE_REF) {
    const char* ts = fmtnode(p, 0, n->expr->type, 1);
    error(p, (node_t*)n, "referencing reference type %s", ts);
  } else if UNLIKELY(!expr_isstorage(n->expr)) {
    const char* ts = fmtnode(p, 0, n->expr->type, 1);
    error(p, (node_t*)n, "referencing ephemeral value of type %s", ts);
  } else if UNLIKELY(ismut && !expr_ismut(n->expr)) {
    const char* s = fmtnode(p, 0, n->expr, 1);
    nodekind_t k = n->expr->kind;
    if (k == EXPR_ID)
      k = ((idexpr_t*)n->expr)->ref->kind;
    error(p, (node_t*)n, "mutable reference to immutable %s %s", nodekind_fmt(k), s);
  }

  reftype_t* t = mkreftype(p, ismut);
  t->elem = n->expr->type;
  n->type = (type_t*)t;
  return (expr_t*)n;
}

static expr_t* expr_ref(parser_t* p, exprflag_t fl) {
  return expr_ref1(p, /*ismut*/false, fl);
}


// mut_expr = "mut" ref_expr
static expr_t* expr_mut(parser_t* p, exprflag_t fl) {
  next(p);
  if UNLIKELY(currtok(p) != TAND) {
    unexpected(p, "expecting '&'");
    return mkbad(p);
  }
  return expr_ref1(p, /*ismut*/true, fl);
}


// group = "(" expr ")"
static expr_t* expr_group(parser_t* p, exprflag_t fl) {
  next(p);
  expr_t* n = expr(p, PREC_COMMA, fl);
  expect(p, TRPAREN, "");
  return n;
}


// named_param_or_id = id ":" expr | id
static expr_t* named_param_or_id(parser_t* p, exprflag_t fl) {
  assert(currtok(p) == TID);
  idexpr_t* n = (idexpr_t*)_mkexpr(
    p, MAX(sizeof(idexpr_t),sizeof(local_t)), EXPR_ID, fl);
  n->name = p->scanner.sym;
  next(p);
  if (currtok(p) == TCOLON) {
    next(p);
    sym_t name = n->name;
    local_t* local = (local_t*)n;
    local->kind = EXPR_PARAM;
    local->name = name;
    local->init = expr(p, PREC_COMMA, fl);
    local->type = local->init->type;
  } else {
    resolve_id(p, n);
  }
  return (expr_t*)n;
}


// args = arg (("," | ";") arg) ("," | ";")?
// arg  = expr | id ":" expr
static void args(parser_t* p, ptrarray_t* args, type_t* recvtype, exprflag_t fl) {
  local_t param0 = { {{EXPR_PARAM}}, .type = recvtype };
  local_t** paramv = (local_t*[]){ &param0 };
  u32 paramc = 1;

  if (recvtype->kind == TYPE_FUN) {
    funtype_t* ft = (funtype_t*)recvtype;
    paramv = (local_t**)ft->params.v;
    paramc = ft->params.len;
    if (paramc > 0 && paramv[0]->isthis) {
      paramv++;
      paramc--;
    }
  } else if (recvtype->kind == TYPE_STRUCT) {
    structtype_t* st = (structtype_t*)recvtype;
    paramv = (local_t**)st->fields.v;
    paramc = st->fields.len;
  }

  typectx_push(p, type_void);

  for (u32 paramidx = 0; ;paramidx++) {
    type_t* t = (paramidx < paramc) ? paramv[paramidx]->type : type_void;
    typectx_push(p, t);

    expr_t* arg;
    if (currtok(p) == TID) {
      arg = named_param_or_id(p, fl);
    } else {
      arg = expr(p, PREC_COMMA, fl);
    }

    typectx_pop(p);

    push(p, args, arg);

    if (currtok(p) != TSEMI && currtok(p) != TCOMMA)
      return;
    next(p);
  }

  typectx_pop(p);
}


// call = expr "(" args? ")"
static expr_t* expr_postfix_call(parser_t* p, prec_t prec, expr_t* left, exprflag_t fl) {
  call_t* n = mkexpr(p, call_t, EXPR_CALL, fl);
  next(p);
  type_t* recvtype = left->type;
  left->flags |= EX_RVALUE;
  if (left->type && left->type->kind == TYPE_FUN) {
    funtype_t* ft = (funtype_t*)left->type;
    n->type = ft->result;
  } else if (left->type && nodekind_istype(left->type->kind)) {
    n->type = left->type;
    recvtype = left->type;
  } else {
    error(p, (node_t*)n, "calling %s; expected function or type",
      left->type ? nodekind_fmt(left->type->kind) : nodekind_fmt(left->kind));
  }
  n->recv = left;
  if (currtok(p) != TRPAREN)
    args(p, &n->args, recvtype ? recvtype : type_void, fl);
  expect(p, TRPAREN, "to end function call");

  // eliminate type casts of same type, e.g. "(TYPE i8 (INTLIT 3)) => (INTLIT 3)"
  if (recvtype->kind != TYPE_FUN &&
      n->args.len == 1 &&
      types_iscompat(((expr_t*)n->args.v[0])->type, n->type))
  {
    return n->args.v[0];
  }

  return (expr_t*)n;
}


// subscript = expr "[" expr "]"
static expr_t* expr_postfix_subscript(
  parser_t* p, prec_t prec, expr_t* left, exprflag_t fl)
{
  unaryop_t* n = mkexpr(p, unaryop_t, EXPR_POSTFIXOP, fl);
  next(p);
  panic("TODO");
  return (expr_t*)n;
}


// member = expr "." id
static expr_t* expr_postfix_member(
  parser_t* p, prec_t prec, expr_t* left, exprflag_t fl)
{
  member_t* n = mkexpr(p, member_t, EXPR_MEMBER, fl);
  next(p);
  left->flags |= EX_RVALUE;
  n->recv = left;
  n->name = p->scanner.sym;
  expect(p, TID, "");
  return (expr_t*)n;
}


// dotmember = "." id
static expr_t* expr_dotmember(parser_t* p, exprflag_t fl) {
  if UNLIKELY(!p->dotctx) {
    error(p, NULL, "\".\" shorthand outside of context");
    expr_t* n = mkbad(p);
    fastforward_semi(p);
    return n;
  }
  return expr_postfix_member(p, PREC_MEMBER, p->dotctx, fl);
}


static type_t* this_param_type(parser_t* p, type_t* recvt, bool ismut) {
  if (!ismut) {
    // pass certain types as value instead of pointer when access is read-only
    if (nodekind_isprimtype(recvt->kind)) // e.g. int, i32
      return recvt;
    if (recvt->kind == TYPE_STRUCT) {
      // small structs
      structtype_t* st = (structtype_t*)recvt;
      usize ptrsize = p->scanner.compiler->ptrsize;
      if (st->align <= ptrsize && st->size <= ptrsize*2)
        return recvt;
    }
  }
  // pointer type
  reftype_t* t = mkreftype(p, ismut);
  t->elem = recvt;
  return (type_t*)t;
}


static void this_param(parser_t* p, fun_t* fun, local_t* param, bool ismut) {
  if UNLIKELY(!fun->methodof) {
    param->type = type_void;
    param->nrefs = 1; // prevent "unused parameter" warning
    error(p, (node_t*)param, "\"this\" parameter of non-method function");
    return;
  }
  param->isthis = true;
  param->type = this_param_type(p, fun->methodof, ismut);
}


static bool fun_params(parser_t* p, fun_t* fun) {
  // params = "(" param (sep param)* sep? ")"
  // param  = Id Type? | Type
  // sep    = "," | ";"
  //
  // e.g.  (T)  (x T)  (x, y T)  (T1, T2, T3)


  bool isnametype = false; // when at least one param has type; e.g. "x T"

  // typeq: temporary storage for params to support "typed groups" of parameters,
  // e.g. "x, y int" -- "x" does not have a type until we parsed "y" and "int", so when
  // we parse "x" we put it in typeq. Also, "x" might be just a type and not a name in
  // the case all args are just types e.g. "T1, T2, T3".
  ptrarray_t typeq = {0}; // local_t*[]

  while (currtok(p) != TEOF) {
    local_t* param = mkexpr(p, local_t, EXPR_PARAM, 0);
    if UNLIKELY(param == NULL)
      goto oom;
    param->type = NULL; // clear type_void set by mkexpr for later check

    if (!ptrarray_push(&fun->params, p->ast_ma, param))
      goto oom;

    bool this_ismut = false;
    if (currtok(p) == TMUT && fun->params.len == 1 && lookahead_issym(p, sym_this)) {
      this_ismut = true;
      next(p);
    }

    if (currtok(p) == TID) {
      // name, eg "x"; could be parameter name or type. Assume name for now.
      param->name = p->scanner.sym;
      param->loc = currloc(p);
      next(p);

      // check for "this" as first argument
      if (param->name == sym_this && fun->params.len == 1) {
        isnametype = true;
        this_param(p, fun, param, this_ismut);
        goto loopend;
      }

      switch (currtok(p)) {
        case TRPAREN:
        case TCOMMA:
        case TSEMI: // just a name, eg "x" in "(x, y)"
          if (!ptrarray_push(&typeq, p->ast_ma, param))
            goto oom;
          break;
        default: // type follows name, eg "int" in "x int"
          param->type = type(p, PREC_LOWEST);
          isnametype = true;
          // cascade type to predecessors
          for (u32 i = 0; i < typeq.len; i++) {
            local_t* prev_param = typeq.v[i];
            prev_param->type = param->type;
          }
          typeq.len = 0;
      }
    } else {
      // definitely a type
      param->name = sym__;
      if (!param->name)
        goto oom;
      param->type = type(p, PREC_LOWEST);
    }

  loopend:
    switch (currtok(p)) {
      case TCOMMA:
      case TSEMI:
        next(p); // consume "," or ";"
        if (currtok(p) == TRPAREN)
          goto finish; // trailing "," or ";"
        break; // continue reading more
      case TRPAREN:
        goto finish;
      default:
        unexpected(p, "expecting ',' ';' or ')'");
        fastforward(p, (const tok_t[]){ TRPAREN, TSEMI, 0 });
        goto finish;
    }
  }
finish:
  if (isnametype) {
    // name-and-type form; e.g. "(x, y T, z Y)".
    // Error if at least one param has type, but last one doesn't, e.g. "(x, y int, z)"
    if UNLIKELY(typeq.len > 0) {
      error(p, NULL, "expecting type");
      for (u32 i = 0; i < fun->params.len; i++) {
        local_t* param = (local_t*)fun->params.v[i];
        if (!param->type)
          param->type = type_void;
      }
    }
  } else {
    // type-only form, e.g. "(T, T, Y)"
    for (u32 i = 0; i < fun->params.len; i++) {
      local_t* param = (local_t*)fun->params.v[i];
      if (param->type)
        continue;
      // make type from id
      param->type = named_type(p, param->name, (node_t*)param);
      param->name = sym__;
    }
  }
  ptrarray_dispose(&typeq, p->ast_ma);
  return isnametype;
oom:
  out_of_mem(p);
  return false;
}


// T** typeidmap_assign(parser_t*, sym_t tid, T, nodekind_t)
#define typeidmap_assign(p, tid, T, kind) \
  (T**)_typeidmap_assign((p), (tid), (kind))
static type_t** _typeidmap_assign(parser_t* p, sym_t tid, nodekind_t kind) {
  compiler_t* c = p->scanner.compiler;
  type_t** tp = (type_t**)map_assign_ptr(&c->typeidmap, c->ma, tid);
  if UNLIKELY(!tp) {
    out_of_mem(p);
    return (type_t**)&last_resort_node;
  }
  if (*tp)
    assert((*tp)->kind == kind);
  return tp;
}


static sym_t typeid_fun(parser_t* p, const ptrarray_t* params, type_t* result) {
  buf_t* buf = &p->tmpbuf[0];
  buf_clear(buf);
  buf_push(buf, TYPEID_PREFIX(TYPE_FUN));
  if UNLIKELY(!buf_print_leb128_u32(buf, params->len))
    goto fail;
  for (u32 i = 0; i < params->len; i++) {
    local_t* param = params->v[i];
    assert(param->kind == EXPR_PARAM);
    if UNLIKELY(!typeid_append(buf, assertnotnull(param->type)))
      goto fail;
  }
  if UNLIKELY(!typeid_append(buf, result))
    goto fail;
  return sym_intern(buf->p, buf->len);
fail:
  out_of_mem(p);
  return sym__;
}


static funtype_t* funtype(parser_t* p, ptrarray_t* params, type_t* result) {
  // build typeid
  sym_t tid = typeid_fun(p, params, result);

  // find existing function type
  funtype_t** typeidmap_slot = typeidmap_assign(p, tid, funtype_t, TYPE_FUN);
  if (*typeidmap_slot)
    return *typeidmap_slot;

  // build function type
  funtype_t* ft = mknode(p, funtype_t, TYPE_FUN);
  ft->size = p->scanner.compiler->ptrsize;
  ft->align = ft->size;
  ft->isunsigned = true;
  ft->result = result;
  if UNLIKELY(!ptrarray_reserve(&ft->params, p->ast_ma, params->len)) {
    out_of_mem(p);
  } else {
    ft->params.len = params->len;
    for (u32 i = 0; i < params->len; i++) {
      local_t* param = params->v[i];
      assert(param->kind == EXPR_PARAM);
      ft->params.v[i] = param;
    }
  }
  *typeidmap_slot = ft;
  return ft;
}


static map_t* nullable get_or_create_methodmap(parser_t* p, const type_t* t) {
  // get or create method map for type
  void** mmp = map_assign_ptr(&p->methodmap, p->ma, t);
  if UNLIKELY(!mmp)
    return out_of_mem(p), NULL;
  if (!*mmp) {
    if (!(*mmp = mem_alloct(p->ma, map_t)) || !map_init(*mmp, p->ma, 8))
      return out_of_mem(p), NULL;
  }
  return *mmp;
}


static void add_method(parser_t* p, fun_t* fun, srcloc_t name_loc) {
  assertnotnull(fun->methodof);
  assertnotnull(fun->name);
  assert(fun->name != sym__);
  map_t* mm = get_or_create_methodmap(p, fun->methodof);
  if UNLIKELY(!mm)
    return;
  void** mp = map_assign_ptr(mm, p->ma, fun->name);
  if UNLIKELY(!mp)
    return out_of_mem(p);

  expr_t* existing = *mp;
  if (!existing && fun->methodof->kind == TYPE_STRUCT)
    existing = (expr_t*)lookup_struct_field((structtype_t*)fun->methodof, fun->name);

  if UNLIKELY(existing) {
    const char* s = fmtnode(p, 0, fun->methodof, 0);
    srcrange_t srcrange = { .focus = name_loc };
    if (existing->kind == EXPR_FUN) {
      error1(p, srcrange, "duplicate method \"%s\" for type %s", fun->name, s);
    } else {
      error1(p, srcrange, "duplicate member \"%s\" for type %s, conflicts with %s",
        fun->name, s, nodekind_fmt(existing->kind));
    }
    if (existing->loc.line)
      warning(p, (node_t*)existing, "previously defined here");
    return;
  }

  *mp = fun;
}


static void fun_name(parser_t* p, fun_t* fun, type_t* nullable recv) {
  fun->name = p->scanner.sym;
  srcloc_t name_loc = currloc(p);
  next(p);

  if (recv) {
    // function defined inside type context, e.g. "type Foo { fun bar(){} }"
    fun->methodof = recv;
  } else {
    if (currtok(p) != TDOT) {
      // plain function name, e.g. "foo"
      return;
    }
    // method function name, e.g. "Foo.bar"
    next(p);

    // resolve receiver, e.g. "Foo" in "Foo.bar"
    recv = (type_t*)lookup(p, fun->name);
    if UNLIKELY(!recv) {
      error1(p, (srcrange_t){ .focus = name_loc },
        "undeclared identifier \"%s\"", fun->name);
      return;
    }
    if UNLIKELY(!nodekind_istype(recv->kind)) {
      const char* s = fmtnode(p, 0, recv, 1);
      error1(p, (srcrange_t){ .focus = name_loc }, "%s is not a type", s);
      return;
    }
    fun->methodof = recv;

    // method name, e.g. "bar" in "Foo.bar"
    fun->name = p->scanner.sym;
    name_loc = currloc(p);
    if (!expect(p, TID, "after '.'"))
      return;
  }

  // add name => fun to recv's method map
  add_method(p, fun, name_loc);
}


static bool fun_prototype(
  parser_t* p, fun_t* n, type_t* nullable methodof, bool requirename)
{
  if (currtok(p) == TID) {
    fun_name(p, n, methodof);
  } else if (requirename) {
    error(p, NULL, "missing function name");
  }

  // parameters
  bool has_named_params = false;
  if UNLIKELY(!expect(p, TLPAREN, "for parameters")) {
    fastforward(p, (const tok_t[]){ TLBRACE, TSEMI, 0 });
    n->type = mkbad(p);
    return has_named_params;
  }
  if (currtok(p) != TRPAREN)
    has_named_params = fun_params(p, n);
  expect(p, TRPAREN, "to end parameters");

  // result type
  type_t* result = type_void;
  // check for "{}", e.g. "fun foo() {}" => "fun foo() void {}"
  if (currtok(p) != TLBRACE)
    result = type(p, PREC_MEMBER);

  n->type = (type_t*)funtype(p, &n->params, result);

  return has_named_params;
}


static type_t* type_fun(parser_t* p) {
  fun_t f = { .kind = EXPR_FUN, .loc = currloc(p) };
  next(p);
  fun_prototype(p, &f, NULL, /*requirename*/false);
  return (type_t*)f.type;
}


static void fun_body(parser_t* p, fun_t* n, exprflag_t fl) {
  bool hasthis = n->params.len && ((local_t*)n->params.v[0])->isthis;
  if (hasthis) {
    assertnotnull(n->methodof);
    dotctx_push(p, n->params.v[0]);
  }

  fun_t* outer_fun = p->fun;
  p->fun = n;

  funtype_t* ft = (funtype_t*)n->type;

  fl |= EX_RVALUE;
  if (ft->result == type_void)
    fl &= ~EX_RVALUE;

  typectx_push(p, ft->result);
  enter_scope(p);

  n->body = any_as_block(p, fl);

  // even though it may have implicit return, in practice a function body
  // block is never an expression itself.
  n->body->flags &= ~EX_RVALUE;

  leave_scope(p);
  typectx_pop(p);

  p->fun = outer_fun;

  if (hasthis)
    dotctx_pop(p);
}


// fundef = "fun" name "(" params? ")" result ( ";" | "{" body "}")
// result = params
// body   = (stmt ";")*
static fun_t* fun(
  parser_t* p, exprflag_t fl, type_t* nullable methodof, bool requirename)
{
  fun_t* n = mkexpr(p, fun_t, EXPR_FUN, fl);
  next(p);
  bool has_named_params = fun_prototype(p, n, methodof, requirename);

  // define named function
  if (n->name && n->type->kind != NODE_BAD && !n->methodof)
    define(p, n->name, (node_t*)n);

  // define named parameters
  if (has_named_params) {
    enter_scope(p);
    for (u32 i = 0; i < n->params.len; i++)
      define(p, ((local_t*)n->params.v[i])->name, n->params.v[i]);
  }

  // body?
  if (currtok(p) != TSEMI) {
    if UNLIKELY(!has_named_params && n->params.len > 0)
      error(p, NULL, "function without named arguments can't have a body");
    fun_body(p, n, fl);
  }

  if (has_named_params)
    leave_scope(p);

  return n;
}

static expr_t* expr_fun(parser_t* p, exprflag_t fl) {
  return (expr_t*)fun(p, fl, NULL, /*requirename*/false);
}

static stmt_t* stmt_fun(parser_t* p) {
  return (stmt_t*)fun(p, 0, NULL, /*requirename*/true);
}


unit_t* parser_parse(parser_t* p, memalloc_t ast_ma, input_t* input) {
  p->ast_ma = ast_ma;
  scope_clear(&p->scope);
  scanner_set_input(&p->scanner, input);
  unit_t* unit = mknode(p, unit_t, NODE_UNIT);
  next(p);

  enter_scope(p);

  while (currtok(p) != TEOF) {
    stmt_t* n = stmt(p, PREC_LOWEST);
    push(p, &unit->children, n);
    if (!expect_token(p, TSEMI, "")) {
      fastforward_semi(p);
    } else {
      next(p);
    }
  }

  leave_scope(p);

  return unit;
}


static const map_t* universe() {
  static map_t m = {0};
  _Atomic(usize) init = 0;
  if (init++)
    return &m;

  const struct {
    const char* key;
    const void* node;
  } entries[] = {
    // types
    {"void", type_void},
    {"bool", type_bool},
    {"int",  type_int},
    {"uint", type_uint},
    {"i8",   type_i8},
    {"i16",  type_i16},
    {"i32",  type_i32},
    {"i64",  type_i64},
    {"u8",   type_u8},
    {"u16",  type_u16},
    {"u32",  type_u32},
    {"u64",  type_u64},
    {"f32",  type_f32},
    {"f64",  type_f64},
    // constants
    {"true",  const_true},
    {"false", const_false},
  };
  static void* storage[
    (MEMALLOC_BUMP_OVERHEAD + MAP_STORAGE_X(countof(entries))) / sizeof(void*)] = {0};
  memalloc_t ma = memalloc_bump(storage, sizeof(storage), MEMALLOC_STORAGE_ZEROED);
  safecheckx(map_init(&m, ma, countof(entries)));
  for (usize i = 0; i < countof(entries); i++) {
    void** valp = map_assign(&m, ma, entries[i].key, strlen(entries[i].key));
    assertnotnull(valp);
    *valp = (void*)entries[i].node;
  }
  return &m;
}


bool parser_init(parser_t* p, compiler_t* c) {
  memset(p, 0, sizeof(*p));

  if (!scanner_init(&p->scanner, c))
    return false;

  if (!map_init(&p->pkgdefs, c->ma, 32))
    goto err1;
  p->pkgdefs.parent = universe();
  if (!map_init(&p->tmpmap, c->ma, 32))
    goto err2;
  if (!map_init(&p->methodmap, c->ma, 32))
    goto err3;

  for (usize i = 0; i < countof(p->tmpbuf); i++)
    buf_init(&p->tmpbuf[i], c->ma);

  p->ma = p->scanner.compiler->ma;

  // note: p->typectxstack & dotctxstack are valid when zero initialized
  p->typectx = type_void;
  p->dotctx = NULL;

  return true;
err3:
  map_dispose(&p->tmpmap, c->ma);
err2:
  map_dispose(&p->pkgdefs, c->ma);
err1:
  scanner_dispose(&p->scanner);
  return false;
}


void parser_dispose(parser_t* p) {
  for (usize i = 0; i < countof(p->tmpbuf); i++)
    buf_dispose(&p->tmpbuf[i]);
  map_dispose(&p->pkgdefs, p->ma);
  map_dispose(&p->tmpmap, p->ma);
  map_dispose(&p->methodmap, p->ma);
  ptrarray_dispose(&p->typectxstack, p->ma);
  ptrarray_dispose(&p->dotctxstack, p->ma);
  scanner_dispose(&p->scanner);
}


// parselet tables


static const expr_parselet_t expr_parsetab[TOK_COUNT] = {
  // infix ops (in order of precedence from weakest to strongest)
  [TASSIGN]    = {NULL, expr_infix_op, PREC_ASSIGN}, // =
  [TMULASSIGN] = {NULL, expr_infix_op, PREC_ASSIGN}, // *=
  [TDIVASSIGN] = {NULL, expr_infix_op, PREC_ASSIGN}, // /=
  [TMODASSIGN] = {NULL, expr_infix_op, PREC_ASSIGN}, // %=
  [TADDASSIGN] = {NULL, expr_infix_op, PREC_ASSIGN}, // +=
  [TSUBASSIGN] = {NULL, expr_infix_op, PREC_ASSIGN}, // -=
  [TSHLASSIGN] = {NULL, expr_infix_op, PREC_ASSIGN}, // <<=
  [TSHRASSIGN] = {NULL, expr_infix_op, PREC_ASSIGN}, // >>=
  [TANDASSIGN] = {NULL, expr_infix_op, PREC_ASSIGN}, // &=
  [TXORASSIGN] = {NULL, expr_infix_op, PREC_ASSIGN}, // ^=
  [TORASSIGN]  = {NULL, expr_infix_op, PREC_ASSIGN}, // |=
  [TOROR]      = {NULL, expr_cmp_op, PREC_LOGICAL_OR}, // ||
  [TANDAND]    = {NULL, expr_cmp_op, PREC_LOGICAL_AND}, // &&
  [TOR]        = {NULL, expr_infix_op, PREC_BITWISE_OR}, // |
  [TXOR]       = {NULL, expr_infix_op, PREC_BITWISE_XOR}, // ^
  [TAND]       = {expr_ref, expr_infix_op, PREC_BITWISE_AND}, // &
  [TEQ]        = {NULL, expr_cmp_op, PREC_EQUAL}, // ==
  [TNEQ]       = {NULL, expr_cmp_op, PREC_EQUAL}, // !=
  [TLT]        = {NULL, expr_cmp_op, PREC_COMPARE},   // <
  [TGT]        = {NULL, expr_cmp_op, PREC_COMPARE},   // >
  [TLTEQ]      = {NULL, expr_cmp_op, PREC_COMPARE}, // <=
  [TGTEQ]      = {NULL, expr_cmp_op, PREC_COMPARE}, // >=
  [TSHL]       = {NULL, expr_infix_op, PREC_SHIFT}, // >>
  [TSHR]       = {NULL, expr_infix_op, PREC_SHIFT}, // <<
  [TPLUS]      = {expr_prefix_op, expr_infix_op, PREC_ADD}, // +
  [TMINUS]     = {expr_prefix_op, expr_infix_op, PREC_ADD}, // -
  [TSTAR]      = {expr_deref, expr_infix_op, PREC_MUL}, // *
  [TSLASH]     = {NULL, expr_infix_op, PREC_MUL}, // /
  [TPERCENT]   = {NULL, expr_infix_op, PREC_MUL}, // %

  // prefix and postfix ops (in addition to the ones above)
  [TPLUSPLUS]   = {expr_prefix_op, postfix_op, PREC_UNARY_PREFIX}, // ++
  [TMINUSMINUS] = {expr_prefix_op, postfix_op, PREC_UNARY_PREFIX}, // --
  [TNOT]        = {expr_prefix_op, NULL, PREC_UNARY_PREFIX}, // !
  [TTILDE]      = {expr_prefix_op, NULL, PREC_UNARY_PREFIX}, // ~
  [TMUT]        = {expr_mut, NULL, PREC_UNARY_PREFIX},
  [TLPAREN]     = {expr_group, expr_postfix_call, PREC_UNARY_POSTFIX}, // (

  // postfix ops
  [TLBRACK] = {NULL, expr_postfix_subscript, PREC_UNARY_POSTFIX}, // [

  // member ops
  [TDOT] = {expr_dotmember, expr_postfix_member, PREC_MEMBER}, // .

  // keywords & identifiers
  [TID]  = {expr_id, NULL, 0},
  [TFUN] = {expr_fun, NULL, 0},
  [TLET] = {expr_var, NULL, 0},
  [TVAR] = {expr_var, NULL, 0},
  [TIF]  = {expr_if, NULL, 0},
  [TFOR] = {expr_for, NULL, 0},
  [TRETURN] = {expr_return, NULL, 0},

  // constant literals
  [TINTLIT]   = {expr_intlit, NULL, 0},
  [TFLOATLIT] = {expr_floatlit, NULL, 0},

  // block
  [TLBRACE] = {expr_block, NULL, 0},
};


// type
static const type_parselet_t type_parsetab[TOK_COUNT] = {
  [TID]       = {type_id, NULL, 0},
  [TLBRACE]   = {type_struct, NULL, 0},
  [TFUN]      = {type_fun, NULL, 0},
  [TSTAR]     = {type_ptr, NULL, 0},
  [TAND]      = {type_ref, NULL, 0},
  [TMUT]      = {type_mut, NULL, 0},
  [TQUESTION] = {type_optional, NULL, 0},
};


// statement
static const stmt_parselet_t stmt_parsetab[TOK_COUNT] = {
  [TFUN]  = {stmt_fun, NULL, 0},
  [TTYPE] = {stmt_typedef, NULL, 0},
};
