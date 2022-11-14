#include "c0lib.h"
#include "compiler.h"
#include "abuf.h"


// #define DEBUG_SCANNING


static const struct { const char* s; tok_t t; } keywordtab[] = {
  #define _(NAME, ...)
  #define KEYWORD(str, NAME) {str, NAME},
  #include "tokens.h"
  #undef _
  #undef KEYWORD
};


static void scan0(scanner_t* s);


bool scanner_init(scanner_t* s, compiler_t* c) {
  memset(s, 0, sizeof(*s));
  s->compiler = c;
  buf_init(&s->litbuf, c->ma);

  // keywordtab must be sorted
  #if DEBUG
    for (usize i = 1; i < countof(keywordtab); i++)
      assertf(strcmp(keywordtab[i-1].s, keywordtab[i].s) < 0,
        "keywordtab out of order (%s)", keywordtab[i].s);
  #endif

  return true;
}


void scanner_dispose(scanner_t* s) {
  buf_dispose(&s->litbuf);
}


void scanner_set_input(scanner_t* s, input_t* input) {
  s->input = input;
  s->inp = input->data.p;
  s->inend = input->data.p + input->data.size;
  s->linestart = input->data.p;
  s->tok.loc.line = 1;
  s->tok.loc.col = 1;
  s->tok.loc.input = input;
  s->lineno = 1;
}


static void stop_scanning(scanner_t* s) {
  // move cursor to end of source causes scanner_next to return TEOF
  s->inp = s->inend;
  s->tok.t = TEOF;
}


slice_t scanner_lit(const scanner_t* s) {
  assert((uintptr)s->inp >= (uintptr)s->tokstart);
  usize len = (usize)(uintptr)(s->inp - s->tokstart) - s->litlenoffs;
  return (slice_t){
    .bytes = s->tokstart,
    .len = len,
  };
}


ATTR_FORMAT(printf,2,3)
static void error(scanner_t* s, const char* fmt, ...) {
  srcrange_t srcrange = { .focus = s->tok.loc };
  va_list ap;
  va_start(ap, fmt);
  report_diagv(s->compiler, srcrange, DIAG_ERR, fmt, ap);
  va_end(ap);
  stop_scanning(s);
}


static void newline(scanner_t* s) {
  assert(*s->inp == '\n');
  s->lineno++;
  s->linestart = s->inp + 1;
}


static void floatnumber(scanner_t* s, int base) {
  s->tok.t = TFLOATLIT;
  bool allowsign = false;
  buf_clear(&s->litbuf);
  if UNLIKELY(!buf_reserve(&s->litbuf, 128))
    return error(s, "out of memory");
  int ok = 1;
  if (base == 16)
    ok = buf_print(&s->litbuf, "0x");

  for (; s->inp != s->inend; ++s->inp) {
    switch (*s->inp) {
    case 'E':
    case 'e':
      allowsign = true;
      break;
    case 'P':
    case 'p':
      if (base < 16)
        goto end;
      allowsign = true;
      break;
    case '+':
    case '-':
      if (!allowsign)
        goto end;
      break;
    case '_':
      continue;
    case '.':
      allowsign = false;
      break;
    default:
      if (!isalnum(*s->inp))
        goto end;
      allowsign = false;
    }
    ok &= buf_push(&s->litbuf, *s->inp);
  }

end:
  ok &= buf_nullterm(&s->litbuf);
  if UNLIKELY(!ok)
    return error(s, "out of memory");
}


static void number(scanner_t* s, int base) {
  s->tok.t = TINTLIT;
  s->insertsemi = true;
  s->litint = 0;
  const u8* start_inp = s->inp;

  u64 cutoff = 0xFFFFFFFFFFFFFFFFllu; // u64
  u64 acc = 0;
  u64 cutlim = cutoff % base;
  cutoff /= (u64)base;
  int any = 0;
  u8 c = *s->inp;

  for (; s->inp != s->inend; c = *++s->inp) {
    switch (c) {
      case '0' ... '9': c -= '0';      break;
      case 'A' ... 'Z': c -= 'A' - 10; break;
      case 'a' ... 'z': c -= 'a' - 10; break;
      case '_': continue; // ignore
      case '.':
        if (base == 10 || base == 16) {
          s->inp = start_inp; // rewind
          return floatnumber(s, base);
        }
        c = base; // trigger error branch below
        break;
      default:
        goto end;
    }
    if UNLIKELY(c >= base) {
      error(s, "invalid base-%d integer literal", base);
      return;
    }
    if (any < 0 || acc > cutoff || (acc == cutoff && (u64)c > cutlim)) {
      any = -1;
    } else {
      any = 1;
      acc *= base;
      acc += c;
    }
  }
end:
  s->litint = acc;
  if UNLIKELY(any < 0)
    error(s, "integer literal too large");
  if UNLIKELY(c == '_')
    error(s, "trailing \"_\" after integer literal");
}


static void zeronumber(scanner_t* s) {
  int base = 10;
  if (s->inp < s->inend) switch (*s->inp) {
    case 'X':
    case 'x':
      base = 16;
      s->inp++;
      break;
    case 'B':
    case 'b':
      base = 2;
      s->inp++;
      break;
    case 'O':
    case 'o':
      base = 8;
      s->inp++;
      break;
  }
  return number(s, base);
}


static bool utf8seq(scanner_t* s) {
  // TODO: improve this to be better and fully & truly verify UTF8
  u8 a = (u8)*s->inp++;
  if ((a & 0xc0) != 0xc0 || ((u8)*s->inp & 0xc0) != 0x80)
    return false;
  if (*s->inp++ == 0)   return false; // len<2
  if ((a >> 5) == 0x6)  return true;  // 2 bytes
  if (*s->inp++ == 0)   return false; // len<3
  if ((a >> 4) == 0xE)  return true;  // 3 bytes
  if (*s->inp++ == 0)   return false; // len<4
  if ((a >> 3) == 0x1E) return true;  // 4 bytes
  return false;
}


static void intern_identifier(scanner_t* s) {
  slice_t lit = scanner_lit(s);
  s->sym = sym_intern(lit.chars, lit.len);
}


static void identifier_utf8(scanner_t* s) {
  while (s->inp < s->inend) {
    if ((u8)*s->inp >= UTF8_SELF) {
      if UNLIKELY(!utf8seq(s))
        return error(s, "invalid UTF8 sequence");
    } else if (isalnum(*s->inp) || *s->inp == '_') {
      s->inp++;
    } else {
      break;
    }
  }
  s->tok.t = TID;
  intern_identifier(s);
}


static void maybe_keyword(scanner_t* s) {
  // binary search for matching keyword & convert currtok to keyword
  usize low = 0, high = countof(keywordtab), mid;
  int cmp;
  slice_t lit = scanner_lit(s);

  while (low < high) {
    mid = (low + high) / 2;
    cmp = strncmp(lit.chars, keywordtab[mid].s, lit.len);
    //dlog("maybe_keyword %.*s <> %s = %d",
    //  (int)lit.len, lit.chars, keywordtab[mid].s, cmp);
    if (cmp == 0) {
      s->tok.t = keywordtab[mid].t;
      break;
    }
    if (cmp < 0) {
      high = mid;
    } else {
      low = mid + 1;
    }
  }
}


static void identifier(scanner_t* s) {
  while (s->inp < s->inend && ( isalnum(*s->inp) || *s->inp == '_' ))
    s->inp++;
  if (s->inp < s->inend && (u8)*s->inp >= UTF8_SELF)
    return identifier_utf8(s);
  s->tok.t = TID;
  s->insertsemi = true;
  intern_identifier(s);
  maybe_keyword(s);
}


static void skip_comment(scanner_t* s) {
  assert(s->inp+1 < s->inend);
  u8 c = s->inp[1];

  if (c == '/') {
    // line comment "// ... <LF>"
    s->inp += 2;
    while (s->inp < s->inend && *s->inp != '\n')
      s->inp++;
    return;
  }

  // block comment "/* ... */"
  s->inp += 2;
  const u8* startstar = s->inp - 1; // make sure "/*/" != "/**/"
  while (s->inp < s->inend) {
    if (*s->inp == '\n') {
      newline(s);
    } else if (*s->inp == '/' && *(s->inp - 1) == '*' && s->inp - 1 != startstar) {
      s->inp++; // consume '*'
      break;
    }
    s->inp++;
  }
}


static void scan1(scanner_t* s) {
  s->tokstart = s->inp;
  s->tok.loc.line = s->lineno;
  s->tok.loc.col = (u32)(uintptr)(s->tokstart - s->linestart) + 1;

  bool insertsemi = s->insertsemi;
  s->insertsemi = false;

  u8 c = *s->inp++; // load current char and advance input pointer

  switch (c) {
  case '(': s->tok.t = TLPAREN; return;
  case ')': s->insertsemi = true; s->tok.t = TRPAREN; return;
  case '{': s->tok.t = TLBRACE; return;
  case '}': s->insertsemi = true; s->tok.t = TRBRACE; return;
  case '[': s->tok.t = TLBRACK; return;
  case ']': s->insertsemi = true; s->tok.t = TRBRACK; return;
  case ';': s->tok.t = TSEMI; return;
  case ',': s->tok.t = TCOMMA; return;
  case '+': s->tok.t = TPLUS; return;
  case '*': s->tok.t = TSTAR; return;
  case '%': s->tok.t = TPERCENT; return;
  case '&': s->tok.t = TAND; return;
  case '|': s->tok.t = TOR; return;
  case '^': s->tok.t = TXOR; return;
  case '~': s->tok.t = TTILDE; return;
  case '#': s->tok.t = THASH; return;
  case '<': s->tok.t = TLT; return;
  case '>': s->tok.t = TGT; return;
  case '=': s->tok.t = TASSIGN;
    if (s->inp < s->inend && *s->inp == '=')
      s->inp++, s->tok.t = TEQ;
    return;

  case '0': return zeronumber(s);

  case '.':
    if (s->inp < s->inend) switch (*s->inp) {
    case '0' ... '9':
      s->inp--;
      return floatnumber(s, 10);
    case '.':
      s->tok.t = TDOTDOT;
      s->inp++;
      if (s->inp < s->inend && *s->inp == '.') {
        s->inp++;
        s->tok.t = TDOTDOTDOT;
      }
      return;
    }
    s->tok.t = TDOT;
    return;

  case '/':
    if (s->inp < s->inend && (*s->inp == '/' || *s->inp == '*')) {
      s->inp--;
      s->insertsemi = insertsemi;
      skip_comment(s);
      MUSTTAIL return scan0(s);
    }
    s->tok.t = TSLASH;
    return;

  default:
    if (isdigit(c)) {
      s->inp--;
      return number(s, 10);
    }
    if (c >= UTF8_SELF) {
      s->inp--; // identifier_utf8 needs to read c
      return identifier_utf8(s);
    }
    if (isalpha(c) || c == '_')
      return identifier(s);
    error(s, "unexpected input byte 0x%02X '%C'", c, c);
    stop_scanning(s);
  }
}


static void scan0(scanner_t* s) {
  s->litlenoffs = 0;

  // save for TSEMI
  u32 prev_lineno = s->lineno;
  const u8* prev_linestart = s->linestart;

  // skip whitespace
  while (s->inp < s->inend && isspace(*s->inp)) {
    if (*s->inp == '\n')
      newline(s);
    s->inp++;
  }

  // should we insert an implicit semicolon?
  if (prev_linestart != s->linestart && s->insertsemi) {
    s->insertsemi = false;
    s->tokstart = prev_linestart;
    s->tok.t = TSEMI;
    s->tok.loc.line = prev_lineno;
    s->tok.loc.col = (usize)(uintptr)(s->tokend - prev_linestart) + 1;
    return;
  }

  // EOF?
  if UNLIKELY(s->inp >= s->inend) {
    s->tokstart = s->inend;
    s->tok.t = TEOF;
    s->tok.loc.line = s->lineno;
    s->tok.loc.col = (u32)(uintptr)(s->tokstart - s->linestart) + 1;
    if (s->insertsemi) {
      s->tok.t = TSEMI;
      s->insertsemi = false;
    }
    return;
  }

  MUSTTAIL return scan1(s);
}


void scanner_next(scanner_t* s) {
  s->tokend = s->inp;
  scan0(s);
  #ifdef DEBUG_SCANNING
    u32 line = s->tok.loc.line;
    u32 col = s->tok.loc.col;
    const char* srcfile = s->tok.loc.input->name;
    const char* name = tok_name(s->tok.t);
    slice_t lit = scanner_lit(s);
    log("scan>  %s:%u:%u\t%-12s \"%.*s\"\t%llu\t0x%llx",
      srcfile, line, col, name, (int)lit.len, lit.chars, s->litint, s->litint);
  #endif
}
