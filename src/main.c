// SPDX-License-Identifier: Apache-2.0
#include "llvm/llvm.h"
#include "compiler.h"
#include "path.h"
#include "clang/Basic/Version.inc" // CLANG_VERSION_STRING

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h> // sleep
#include <err.h>
#include <libgen.h>

#include "../lib/musl/src/internal/version.h"
#define MUSL_VERSION_STR VERSION

typedef bool(*linkerfn_t)(int argc, char*const* argv, bool can_exit_early);

const char* coprogname;
const char* coexefile;
const char* coroot;
const char* cocachedir;
bool coverbose = false;
u32 comaxproc = 1;

// externally-implemented tools
int main_build(int argc, char*const* argv); // build.c
int cc_main(int argc, char* argv[]); // cc.c
int llvm_ar_main(int argc, char **argv); // llvm/llvm-ar.cc

static linkerfn_t nullable ld_impl(sys_t);
static const char* ld_impl_name(linkerfn_t nullable f);


static int usage(FILE* f) {
  // ld usage text
  linkerfn_t ldf = ld_impl(target_default()->sys);
  char host_ld[128];
  if (ldf) {
    snprintf(host_ld, sizeof(host_ld),
      "  ld        %s linker (host)\n", ld_impl_name(ldf));
  } else {
    host_ld[0] = 0;
  }

  fprintf(f,
    "Usage: %s <command> [args ...]\n"
    "Commands:\n"
    "  build     Build a project\n"
    "\n"
    "  ar        Archiver\n"
    "  cc        C compiler (clang)\n"
    "  ranlib    Archive index generator\n"
    "\n"
    "%s" // ld for host, if any
    "  ld.lld    ELF linker\n"
    "  ld64.lld  Mach-O linker\n"
    "  lld-link  COFF linker\n"
    "  wasm-ld   WebAssembly linker\n"
    "\n"
    "  help      Print help on stdout and exit\n"
    "  targets   List supported targets\n"
    "  version   Print version on stdout and exit\n"
    "\n"
    "For help with a specific command:\n"
    "  %s <command> --help\n"
    "",
    coprogname,
    host_ld,
    coprogname);
  return 0;
}


void print_co_version() {
  printf("compis " CO_VERSION_STR);
  #ifdef CO_VERSION_GIT_STR
    printf(" (" CO_VERSION_GIT_STR ")");
  #endif
  const target_t* host = target_default();
  printf(
    " %s-%s"
    ", llvm " CLANG_VERSION_STRING
    ", musl " MUSL_VERSION_STR
    "\n",
    arch_name(host->arch), sys_name(host->sys));
}


static const char* ld_impl_name(linkerfn_t nullable f) {
  if (f == LLDLinkMachO) return "Mach-O";
  if (f == LLDLinkELF)   return "ELF";
  if (f == LLDLinkWasm)  return "WebAssembly";
  if (f == LLDLinkCOFF)  return "COFF";
  return "?";
}


static linkerfn_t nullable ld_impl(sys_t sys) {
  switch ((enum target_sys)sys) {
    case SYS_macos:
      return LLDLinkMachO;
    case SYS_linux:
      return LLDLinkELF;
    // case SYS_windows:
    //   return LLDLinkCOFF;
    // case SYS_wasi:
    // case SYS_wasm:
    //   return LLDLinkWasm;
    case SYS_none:
    case SYS_COUNT:
      return NULL;
  }
}


static int ld_main(int argc, char* argv[]) {
  linkerfn_t impl = safechecknotnull( ld_impl(target_default()->sys) );
  return !impl(argc, argv, true);
}


static void coroot_init(memalloc_t ma) {
  const char* envvar = getenv("COROOT");
  if (envvar && *envvar) {
    coroot = path_abs(ma, envvar);
  } else {
    coroot = path_dir_m(ma, coexefile);
    #if DEBUG
      if (str_endswith(coroot, "/out/debug"))
        coroot = path_join_m(ma, coroot, "../../lib");
    #elif !defined(CO_DISTRIBUTION)
      if (str_endswith(coroot, "/out/opt") || str_endswith(coroot, "/out/debug"))
        coroot = path_join_m(ma, coroot, "../../lib");
    #endif
  }
  safecheck(coroot != NULL);
  char* probe = path_join_alloca(coroot, "co/coprelude.h");
  if (!fs_isfile(probe))
    warnx("warning: invalid COROOT '%s' (compiling may not work)", coroot);
}


static void cocachedir_init(memalloc_t ma) {
  const char* envvar = getenv("COCACHE");
  if (envvar && *envvar) {
    cocachedir = path_abs(ma, envvar);
  } else {
    cocachedir = path_join_m(ma, sys_homedir(), ".cache/compis/" CO_VERSION_STR);
  }
  safecheck(cocachedir != NULL);
}


int main(int argc, char* argv[]) {
  coprogname = strrchr(argv[0], PATH_SEPARATOR);
  coprogname = coprogname ? coprogname + 1 : argv[0];
  coexefile = safechecknotnull(LLVMGetMainExecutable(argv[0]));

  const char* exe_basename = strrchr(coexefile, PATH_SEPARATOR);
  exe_basename = exe_basename ? exe_basename + 1 : coexefile;

  bool is_multicall = (
    strcmp(coprogname, exe_basename) != 0 &&
    strcmp(coprogname, "compis") != 0 &&
    strcmp(coprogname, "co") != 0 );
  const char* cmd = is_multicall ? coprogname : argv[1] ? argv[1] : "";

  if (*cmd == 0) {
    usage(stdout);
    log("%s: missing command; try `%s help`", coprogname, coprogname);
    return 1;
  }

  #define IS(...)  __VARG_DISP(IS,__VA_ARGS__)
  #define IS1(name)  (streq(cmd, (name)))
  #define IS2(a,b)   (streq(cmd, (a)) || streq(cmd, (b)))
  #define IS3(a,b,c) (streq(cmd, (a)) || streq(cmd, (b)) || streq(cmd, (c)))

  // clang "cc" may spawn itself in a new process
  if IS("-cc1", "-cc1as")
    return clang_main(argc, argv);

  // shave away "prog" from argv when not a multicall
  if (!is_multicall) {
    argc--;
    argv++;
  }

  // commands that do not touch any compis code (no need for compis init)
  if IS("ld.lld")       return LLDLinkELF(argc, argv, true) ? 0 : 1;
  if IS("ld64.lld")     return LLDLinkMachO(argc, argv, true) ? 0 : 1;
  if IS("lld-link")     return LLDLinkCOFF(argc, argv, true) ? 0 : 1;
  if IS("wasm-ld")      return LLDLinkWasm(argc, argv, true) ? 0 : 1;
  if IS("ar", "ranlib") return llvm_ar_main(argc, argv);

  // initialize global state
  memalloc_t ma = memalloc_ctx();
  comaxproc = sys_ncpu();
  relpath_init();
  tmpbuf_init(ma);
  sym_init(ma);
  coroot_init(ma);
  cocachedir_init(ma);
  err_t err = llvm_init();
  if (err) errx(1, "llvm_init: %s", err_str(err));

  // command dispatch
  if IS("build")                return main_build(argc, argv);
  if IS("cc")                   return cc_main(argc, argv);
  if IS("ld")                   return ld_main(argc, argv);
  if IS("targets")              return print_supported_targets(), 0;
  if IS("version", "--version") return print_co_version(), 0;
  if IS("help", "--help", "-h") return usage(stdout);

  log("%s: unknown command \"%s\"", coprogname, cmd);
  return 1;
}
