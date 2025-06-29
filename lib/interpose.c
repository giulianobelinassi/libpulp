/*
 *  libpulp - User-space Livepatching Library
 *
 *  Copyright (C) 2021 SUSE Software Solutions GmbH
 *
 *  This file is part of libpulp.
 *
 *  libpulp is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  libpulp is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with libpulp.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <err.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <gnu/libc-version.h>
#include <grp.h>
#include <limits.h>
#include <link.h>
#include <pthread.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/param.h>

#include "config.h"
#include "ld_rtld.h"
#include "msg_queue.h"
#include "ulp.h"
#include "ulp_common.h"
#include "arch_common.h"

/* This header should be included last, as it poisons some symbols.  */
#include "error.h"

/* Declare internal glibc functions which are exposed. We have to use them
   while __ulp_asunsafe_begin have not run yet.  */
extern void __libc_free(void *);
extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void *__libc_valloc(size_t);
extern void *__libc_pvalloc(size_t);
extern void *__libc_memalign(size_t, size_t);
/* aligned_alloc doesn't have an alternate public symbol.  */
/* posix_memalign doesn't have an alternate public symbol.  */
extern const char *__progname;

static int flag = 0;

/* Memory allocation functions. */
static void (*real_free)(void *) = NULL;
static void *(*real_malloc)(size_t) = NULL;
static void *(*real_calloc)(size_t, size_t) = NULL;
static void *(*real_realloc)(void *, size_t) = NULL;
static void *(*real_valloc)(size_t) = NULL;
static void *(*real_pvalloc)(size_t) = NULL;
static void *(*real_memalign)(size_t, size_t) = NULL;
static void *(*real_aligned_alloc)(size_t, size_t) = NULL;
static int (*real_posix_memalign)(void **, size_t, size_t) = NULL;

/* Dynamic loader functions. */
static void *(*real_dlopen)(const char *, int) = NULL;
static void *(*real_dlmopen)(Lmid_t, const char *, int) = NULL;
static int (*real_dlclose)(void *) = NULL;
static int (*real_dladdr)(const void *, Dl_info *) = NULL;
static int (*real_dladdr1)(const void *, Dl_info *, void **, int) = NULL;
static int (*real_dlinfo)(void *, int, void *) = NULL;

/* Linker structures.  */
static pthread_mutex_t *dl_load_lock = NULL;
static pthread_mutex_t *dl_load_write_lock = NULL;

/** @brief Get symbol by its name
 *
 * Example: calling this function with name = 'printf' will return
 * the ELF symbol referring to the printf function.
 *
 * @param dynsym: The symbol table.
 * @param dynstr: The symbol string table.
 * @param len: The length of dynsym
 * @param name: Name of the symbol to search.
 *
 * @return a pointer to the wanted symbol in the symbol table.
 */
static Elf64_Sym *
get_symbol_by_name(Elf64_Sym dynsym[], const char *dynstr, int len,
                   const char *name)
{
  int i;
  for (i = 0; i < len; i++) {
    /* st_name contains the offset of the symbol's name on the dynstr table. */
    if (!strcmp(dynstr + dynsym[i].st_name, name))
      return &dynsym[i];
  }

  /* Symbol not found.  */
  return NULL;
}

/** struct containing the parameters that will be passed to dl_find_symbol.  */
struct dl_iterate_arg
{
  /* Input.  */

  /** Name of the .so file to find the symbol. */
  const char *library;

  /** The name of the wanted symbol. */
  const char *symbol;

  /* Output.  */

  /** The address of the symbol in the program. */
  void *symbol_addr;

  /** The address bias where the library was loaded.  */
  uintptr_t bias_addr;

  /** The TLS module index of the library.  */
  int tls_index;
};

/** @brief dl_iterate_phdr callback.
 *
 * This function do the hard work into gathering the necessary informations
 * about the symbols in the process. It works by being a callback to
 * dl_iterate_phdr (read its manpage), which pass into "info" the ELF program
 * headers (phdr) of:
 *  1. The current binary.
 *  2. Each dynamic library (.so) loaded with the program.
 *
 * Then it parses the structures there to find the .dynsym, .dynstr, and .hash
 * sections containing respectively:
 *
 *  1. The dynamic symbol table.
 *  2. The symbol string table with the name of each symbol.
 *  3. The hash table (only used to find the number of symbol in .dynsym).
 *
 * The library name which we want to find its symbol, and the wanted symbol
 * is passed on struct dl_iterate_arg, which is passed on the "data" argument.
 * If library name is NULL, this function will find the first occurence of
 * "symbol" in the entire program. The output of this function is also written
 * on the struct pointed by "data", and it is a pointer to the symbol in the
 * program.
 *
 * Good references to understeand how to parse the ELF program headers are:
 *  1. The elf.h header.
 *  2. 'dl_iterate_phdr' manpage.
 *  3. 'Learing Linux Binary Analysis' (Elfmaster, 2016).
 *  4. 'Linkers and Loaders' (Levine, 1999).
 *
 * @param info: Program header infos (see dl_iterate_phdr).
 * @param size: sizeof(dl_phdr_info).
 * @param data: Data to this function. Also used as return value.
 *
 * @return 1 when done; 0 to request next library.
 */
static int
dl_find_symbol(struct dl_phdr_info *info, size_t size, void *data)
{
  /* We call the symbol table as dynsym because that is most likely to be the
   * section in DT_SYMTAB.  However, this is not necessary true in all cases.
   */
  Elf64_Sym *dynsym = NULL;
  const char *dynstr = NULL;
  int *hash_addr;

  int i;
  int num_symbols = 0;
  struct dl_iterate_arg *args = (struct dl_iterate_arg *)data;

  /* Sanity check if size matches the size of the struct.  */
  if (size != sizeof(*info)) {
    libpulp_errx(EXIT_FAILURE, "dl_phdr_info size is unexpected");
    return 0;
  }

  /* Initialize output value as being NULL (symbol not found).  */
  args->symbol_addr = NULL;

  /* Initialize TLS index with invalid value.  */
  args->tls_index = -1;

  /* Check if the current info is the library we want to find the symbols.  */
  if (args->library && !strstr(info->dlpi_name, args->library))
    return 0;

  /* Pointers to linux-vdso.so are invalid, so skip this library.  */
  if (!strcmp(info->dlpi_name, "linux-vdso.so.1"))
    return 0;

  /* Iterate each program headers to find the information we need. */
  for (i = 0; i < info->dlpi_phnum; i++) {
    const Elf64_Phdr *phdr_addr = &info->dlpi_phdr[i];

    /* We are interested in symbols, so look for the dynamic symbols in the
     * PT_DYNAMIC tag. */
    if (phdr_addr->p_type == PT_DYNAMIC) {

      /* The address in p_paddr is relative to the .so header, so we need to
       * add the base address where the .so was mapped in the process. In case
       * it is the binary itself, dlpi_addr is zero.  */
      Elf64_Dyn *dyn = (Elf64_Dyn *)(info->dlpi_addr + phdr_addr->p_paddr);

      /* Iterate over each tag in this section.  */
      for (; dyn->d_tag != DT_NULL; dyn++) {
        switch (dyn->d_tag) {
          case DT_SYMTAB:
            dynsym = (Elf64_Sym *)dyn->d_un.d_ptr;
            break;

          case DT_STRTAB:
            dynstr = (const char *)dyn->d_un.d_ptr;
            break;

          case DT_SYMENT:
            /* This section stores the size of a symbol entry. So compare it
             * with the size of Elf64_Sym as a sanity check.  */
            if (dyn->d_un.d_val != sizeof(Elf64_Sym)) {
              libpulp_errx(EXIT_FAILURE, "DT_SYMENT value of %s is unexpected",
                           info->dlpi_name);
              return 0;
            }
            break;

          case DT_HASH:
            /* Look at the hash section for the number of the symbols in the
             * symbol table.  This section structure in memory is:
             *
             * hash_t nbuckets;
             * hash_t nchains;
             * hash_t buckets[nbuckets];
             * hash_t chain[nchains];
             *
             * hash_t is either int32_t or int64_t according to the arch.
             * On x86_64 it is 32-bits.
             * */
            hash_addr = (int *)dyn->d_un.d_ptr;
            num_symbols = hash_addr[1]; /* Get nchains.  */
            break;
        }
      }
    }
  }

  /* With the symbol table identified, find the wanted symbol.  */
  if (dynstr && dynsym) {
    Elf64_Sym *sym;
    args->tls_index = info->dlpi_tls_modid;

    sym = get_symbol_by_name(dynsym, dynstr, num_symbols, args->symbol);
    if (sym)
      args->symbol_addr = (void *)(sym->st_value);

    args->bias_addr = info->dlpi_addr;
    /* Alert dl_iterate_phdr that we are finished.  */
    return 1;
  }
  return 0;
}

/** @brief Get the address of a loaded symbol from library.
 *
 * This function will return the address where the symbol with the name
 * "symbol" from the library "library" was allocated in memory.
 *
 * Example: calling this function with symbol = "printf" will return
 * the address where the printf function is.
 *
 * @param library: name of the library where the symbol is from.
 * @param symbol: name of the wanted symbol
 * @param old_faddr: Offset of symbol, as found during packing.
 *
 * @return the address where the symbol was allocated nn the program.
 */
void *
get_loaded_symbol_addr(const char *library, const char *symbol,
                       void *old_faddr)
{
  /* Check if the current info is the program's binary itself.  In that case
   * we must handle things somewhat differently.  */
  if (library == NULL || !strcmp(library, __progname)) {
    library = "";
  }

  struct dl_iterate_arg arg = { .library = library, .symbol = symbol };
  dl_iterate_phdr(dl_find_symbol, &arg);

  if (old_faddr != NULL && arg.symbol_addr != old_faddr) {
    WARN("Symbol requested not found in .dynsym. Using address from .ulp");
    return arg.bias_addr + old_faddr;
  }

  if (arg.symbol_addr == NULL) {
    /* Symbol not found.  */
    return NULL;
  }

  return arg.bias_addr + arg.symbol_addr;
}

static int
dl_find_base_addr(struct dl_phdr_info *info, size_t size, void *data)
{
  struct dl_iterate_arg *args = (struct dl_iterate_arg *)data;

  /* Highly improvable that any library will have this base address.  */
  args->bias_addr = 0xFF;

  /* Initialize TLS index with an incorrect value.  */
  args->tls_index = -1;

  /* Sanity check if size matches the size of the struct.  */
  if (size != sizeof(*info)) {
    libpulp_errx(EXIT_FAILURE, "dl_phdr_info size is unexpected");
    return 0;
  }

  /* Pointers to linux-vdso.so are invalid, so skip this library.  */
  if (!strcmp(info->dlpi_name, "linux-vdso.so.1"))
    return 0;

  /* Check if the current info is the library we want to find the symbols.  */
  if (args->library && !strstr(info->dlpi_name, args->library))
    return 0;

  /* Found.  Set symbol_addr as the base address of library.  */
  args->bias_addr = info->dlpi_addr;
  args->tls_index = info->dlpi_tls_modid;

  /* Alert dl_iterate that we are finished.  */
  return 1;
}

void *
get_loaded_library_base_addr(const char *library)
{

  /* Check if the current info is the program's binary itself.  In that case
   * we must handle things somewhat differently.  */
  if (library == NULL || !strcmp(library, __progname)) {
    library = "";
  }

  struct dl_iterate_arg arg = { .library = library };
  dl_iterate_phdr(dl_find_base_addr, &arg);

  return (void *)arg.bias_addr;
}

int
get_loaded_library_tls_index(const char *library)
{
  /* Check if the current info is the program's binary itself.  In that case
   * we must handle things somewhat differently.  */
  if (library == NULL || !strcmp(library, __progname)) {
    library = "";
  }

  struct dl_iterate_arg arg = { .library = library };
  dl_iterate_phdr(dl_find_base_addr, &arg);

  return arg.tls_index;
}

/** @brief Find address of dl functions lock.
 *
 * This function finds the locks which are held when dlsym and dlmsym functions
 * are called. This is necessary because of:
 *   * dlsym behaviour changes depending of where it is being called.
 * Proof of this can be found in glibc's dlfcn/dlsym.c. Notice that
 * RETURN_ADDRESS(0) is passed to internal functions, which gets the previous
 * frame address. The practical behaviour is that if we interpose dlsym, then
 * software that interposes functions from glibc will find libpulp functions
 * instead, and libpulp will find the software interposed functions, creating
 * an infinite recursion.
 *
 * Therefore, we can not interpose dlsym and use a custom lock, as we do for
 * the other functions, and we instead check if the dlsym lock is held.
 */
static void
get_ld_global_locks()
{
  char libc_ver[32];
  const char *tok;
  int major, minor;

  void *rtld_global = get_loaded_symbol_addr(LD_LINUX, "_rtld_global", NULL);
  if (!rtld_global) {
    libpulp_crash("symbol _rtld_global not found in ld-linux-x86_64.so\n");
  }

  strcpy(libc_ver, gnu_get_libc_version());

  tok = strtok(libc_ver, ".");
  major = atoi(tok);
  tok = strtok(NULL, ".");
  minor = atoi(tok);

  if (major == 2) {
    if (31 <= minor && minor < 35) {
      struct rtld_global__2_31 *rtld = rtld_global;
      dl_load_lock = &rtld->_dl_load_lock.mutex;
      dl_load_write_lock = &rtld->_dl_load_write_lock.mutex;
    }
    else if (35 <= minor) {
      struct rtld_global__2_35 *rtld = rtld_global;
      dl_load_lock = &rtld->_dl_load_lock.mutex;
      dl_load_write_lock = &rtld->_dl_load_write_lock.mutex;
    }
    else {
      libpulp_crash("glibc version %d.%d is unsupported\n", major, minor);
    }
  }
}

// @brief Disable livepatching based on LIBPULP_DISABLE_ON_PATH variable.
//
// This function will scan the LIBPULP_DISABLE_ON_PATH variable for wildcards
// which would match the path to the program's binary.
//
// Example: LIBPULP_DISABLE_ON_PATH=/home/*:/tmp/*
//
// Would block livepatching any program launched from the /home or /tmp folder.
static void
maybe_disable_livepatching_on_path(void)
{
  const char *disabled_names = getenv("LIBPULP_DISABLE_ON_PATH");
  if (disabled_names == NULL) {
    return;
  }

  size_t len = strlen(disabled_names);

  char *names = malloc(len + 1);
  if (names == NULL) {
    set_libpulp_error_state(errno);
    return;
  }
  memcpy(names, disabled_names, len + 1);

  char process_path[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", process_path, sizeof(process_path));
  if ((size_t)n >= sizeof(process_path)) {
    WARN("Unable to get path to executable. Livepatching is disabled.");
    set_libpulp_error_state(EINITFAIL);
  }

  /* readlink do not append the '\0' character.  Do it now.  */
  process_path[n] = '\0';

  const char *wildcard;
  for (wildcard = strtok(names, ":"); wildcard != NULL;
       wildcard = strtok(NULL, ":")) {
    if (fnmatch(wildcard, process_path, FNM_EXTMATCH) == 0) {
      /* Match.  */
      set_libpulp_error_state(EUSRBLOCKED);
      WARN("Matched path pattern %s: livepatching disabled by user request.",
           wildcard);
      break;
    }
  }

  free(names);
}

/** @brief Disable livepatching based on LIBPULP_DISABLE_ON_USERS variable.
 *
 * This function will scan the LIBPULP_DISABLE_ON_USERS variable for wildcards
 * which would match the path to the user name or uid.
 *
 * Example: LIBPULP_DISABLE_ON_USERS=1000:root
 *
 * Would block livepatching any program launched from user 'root' or user with
 * uid = 1000.
 */
static void
maybe_disable_livepatching_on_user(void)
{
  const char *disabled_names = getenv("LIBPULP_DISABLE_ON_USERS");
  if (disabled_names == NULL) {
    return;
  }

  size_t len = strlen(disabled_names);

  char *names = malloc(len + 1);
  if (names == NULL) {
    set_libpulp_error_state(errno);
    return;
  }
  memcpy(names, disabled_names, len + 1);

  uid_t uid = getuid();
  struct passwd *pws = getpwuid(uid);
  const char *uname = pws ? pws->pw_name : NULL;

  const char *wildcard;
  for (wildcard = strtok(names, ":"); wildcard != NULL;
       wildcard = strtok(NULL, ":")) {
    if (isnumber(wildcard) && strtoul(wildcard, NULL, 10) == uid) {
      set_libpulp_error_state(EUSRBLOCKED);
      WARN("Matched uid %s: livepatching disabled by user request.", wildcard);
      break;
    }
    else if (uname != NULL) {
      if (fnmatch(wildcard, uname, FNM_EXTMATCH) == 0) {
        /* Match.  */
        set_libpulp_error_state(EUSRBLOCKED);
        WARN("Matched user pattern %s: livepatching disabled by user request.",
             wildcard);
        break;
      }
    }
  }

  free(names);
}

/** @brief Disable livepatching based on LIBPULP_DISABLE_ON_GROUPS variable.
 *
 * This function will scan the LIBPULP_DISABLE_ON_GROUPS variable for wildcards
 * which would match the path to the group name or group id.
 *
 * Example: LIBPULP_DISABLE_ON_USERS=1000:root
 *
 * Would block livepatching any program launched from user 'root' or user with
 * uid = 1000.
 */
static void
maybe_disable_livepatching_on_group(void)
{
  const char *disabled_names = getenv("LIBPULP_DISABLE_ON_GROUPS");
  if (disabled_names == NULL) {
    return;
  }

  size_t len = strlen(disabled_names);

  char *names = malloc(len + 1);
  if (names == NULL) {
    set_libpulp_error_state(errno);
    return;
  }
  memcpy(names, disabled_names, len + 1);

  gid_t gid = getgid();
  struct group *g = getgrgid(gid);
  const char *gname = g ? g->gr_name : NULL;

  const char *wildcard;
  for (wildcard = strtok(names, ":"); wildcard != NULL;
       wildcard = strtok(NULL, ":")) {
    if (isnumber(wildcard) && strtoul(wildcard, NULL, 10) == gid) {
      set_libpulp_error_state(EUSRBLOCKED);
      WARN("Matched gid %s: livepatching disabled by user request.", wildcard);
      break;
    }
    else if (gname != NULL) {
      if (fnmatch(wildcard, gname, FNM_EXTMATCH) == 0) {
        /* Match.  */
        set_libpulp_error_state(EUSRBLOCKED);
        WARN(
            "Matched group pattern %s: livepatching disabled by user request.",
            wildcard);
        break;
      }
    }
  }

  free(names);
}

__attribute__((constructor)) void
__ulp_asunsafe_begin(void)
{
  /*
   * If the address of dlsym is know (real_malloc not NULL) this function
   * has already been executed successfully and learned the real
   * addresses of all interposed function, so do not run it again.
   */
  if (real_malloc)
    return;

  bool ok = true;

  real_dlopen = dlsym(RTLD_NEXT, "dlopen");
  real_dlmopen = dlsym(RTLD_NEXT, "dlmopen");
  real_dlclose = dlsym(RTLD_NEXT, "dlclose");
  real_dladdr = dlsym(RTLD_NEXT, "dladdr");
  real_dladdr1 = dlsym(RTLD_NEXT, "dladdr1");
  real_dlinfo = dlsym(RTLD_NEXT, "dlinfo");

  /* Check if we got the symbols we need from libdl.  */
  if (!real_dlopen) {
    set_libpulp_error_state_with_reason(ENOLIBDL, "unable to find function `dlopen`.");
    ok = false;
  }

  if (!real_dlmopen) {
    set_libpulp_error_state_with_reason(ENOLIBDL, "unable to find function `dlmopen`.");
    ok = false;
  }

  if (!real_dlclose) {
    set_libpulp_error_state_with_reason(ENOLIBDL, "unable to find function `dlclose`.");
    ok = false;
  }

  if (!real_dladdr) {
    set_libpulp_error_state_with_reason(ENOLIBDL, "unable to find function `dladdr`.");
    ok = false;
  }

  if (!real_dladdr1) {
    set_libpulp_error_state_with_reason(ENOLIBDL, "unable to find function `dladdr1`.");
    ok = false;
  }

  if (!real_dlinfo) {
    set_libpulp_error_state_with_reason(ENOLIBDL, "unable to find function `dlinfo`.");
    ok = false;
  }

  real_free = dlsym(RTLD_NEXT, "free");
  real_malloc = dlsym(RTLD_NEXT, "malloc");
  real_calloc = dlsym(RTLD_NEXT, "calloc");
  real_realloc = dlsym(RTLD_NEXT, "realloc");
  real_valloc = dlsym(RTLD_NEXT, "valloc");
  real_pvalloc = dlsym(RTLD_NEXT, "pvalloc");
  real_memalign = dlsym(RTLD_NEXT, "memalign");
  real_aligned_alloc = dlsym(RTLD_NEXT, "aligned_alloc");
  real_posix_memalign = dlsym(RTLD_NEXT, "posix_memalign");

  /* Check if we got the symbols we need from glibc.  */
  if (!real_free) {
    set_libpulp_error_state_with_reason(ENOLIBC, "unable to find function `free`.");
    ok = false;
  }

  if (!real_malloc) {
    set_libpulp_error_state_with_reason(ENOLIBC, "unable to find function `malloc`.");
    ok = false;
  }

  if (!real_calloc) {
    set_libpulp_error_state_with_reason(ENOLIBC, "unable to find function `calloc`.");
    ok = false;
  }

  if (!real_realloc) {
    set_libpulp_error_state_with_reason(ENOLIBC, "unable to find function `realloc`.");
    ok = false;
  }

  if (!real_pvalloc) {
    set_libpulp_error_state_with_reason(ENOLIBC, "unable to find function `pvalloc`.");
    ok = false;
  }

  if (!real_memalign) {
    set_libpulp_error_state_with_reason(ENOLIBC, "unable to find function `memalign`.");
    ok = false;
  }

  if (!real_aligned_alloc) {
    set_libpulp_error_state_with_reason(ENOLIBC, "unable to find function `aligned_alloc`.");
    ok = false;
  }

  if (!real_posix_memalign) {
    set_libpulp_error_state_with_reason(ENOLIBC, "unable to find function `posix_memalign`");
    ok = false;
  }

  if (ok == false)
    return;

  get_ld_global_locks();

  maybe_disable_livepatching_on_path();
  maybe_disable_livepatching_on_user();
  maybe_disable_livepatching_on_group();
}

static bool
dl_locks_held(void)
{

  libpulp_assert(0 <= dl_load_lock->__data.__lock &&
                 dl_load_lock->__data.__lock <= 1);
  libpulp_assert(0 <= dl_load_write_lock->__data.__lock &&
                 dl_load_write_lock->__data.__lock <= 1);

  return (dl_load_lock->__data.__lock || dl_load_write_lock->__data.__lock);
}

/** @brief Lock the `flag` lock to indicate that this process in being patched.
 *
 * This function will lock the `flag` lock, which indicates that this process
 * is being analyzed by libpulp tools.  This runs when the thread was hijacked.
 *
 * @return   0 if lock was not held, 1 if held.
 */
int
__ulp_asunsafe_trylock(void)
{
  int local;
  if (dl_locks_held())
    return 1;

  local = __sync_val_compare_and_swap(&flag, 0, 1);
  if (local)
    return 1;
  return 0;
}

/** @brief Unlock the `flag` lock.  */
int
__ulp_asunsafe_unlock(void)
{
  __sync_fetch_and_and(&flag, 0);
  return 0;
}

/* Interposed functions.  */

void
free(void *ptr)
{
  if (real_free == NULL) {
    __libc_free(ptr);
    return;
  }

  __sync_fetch_and_add(&flag, 1);
  real_free(ptr);
  __sync_fetch_and_sub(&flag, 1);
}

void *
malloc(size_t size)
{
  void *result;

  if (real_malloc == NULL) {
    result = __libc_malloc(size);
    return result;
  }

  __sync_fetch_and_add(&flag, 1);
  result = real_malloc(size);
  __sync_fetch_and_sub(&flag, 1);

  return result;
}

void *
calloc(size_t nmemb, size_t size)
{
  void *result;

  if (real_calloc == NULL) {
    result = __libc_calloc(nmemb, size);
    return result;
  }

  __sync_fetch_and_add(&flag, 1);
  result = real_calloc(nmemb, size);
  __sync_fetch_and_sub(&flag, 1);

  return result;
}

void *
realloc(void *ptr, size_t size)
{
  void *result;

  if (real_realloc == NULL) {
    return __libc_realloc(ptr, size);
  }

  __sync_fetch_and_add(&flag, 1);
  result = real_realloc(ptr, size);
  __sync_fetch_and_sub(&flag, 1);

  return result;
}

void *
valloc(size_t size)
{
  void *result;

  if (real_valloc == NULL) {
    return __libc_valloc(size);
  }

  __sync_fetch_and_add(&flag, 1);
  result = real_valloc(size);
  __sync_fetch_and_sub(&flag, 1);

  return result;
}

void *
pvalloc(size_t size)
{
  void *result;

  if (real_pvalloc == NULL) {
    return __libc_pvalloc(size);
  }

  __sync_fetch_and_add(&flag, 1);
  result = real_pvalloc(size);
  __sync_fetch_and_sub(&flag, 1);

  return result;
}

void *
memalign(size_t alignment, size_t size)
{
  void *result;

  if (real_memalign == NULL) {
    return __libc_memalign(alignment, size);
  }

  __sync_fetch_and_add(&flag, 1);
  result = real_memalign(alignment, size);
  __sync_fetch_and_sub(&flag, 1);

  return result;
}

void *
aligned_alloc(size_t alignment, size_t size)
{
  void *result;

  if (real_aligned_alloc == NULL) {
    /* We have to emulate this function using memalign.  */
    if ((alignment & (alignment - 1)) || (size & (alignment - 1))) {
      errno = EINVAL;
      return NULL;
    }

    return  __libc_memalign(alignment, size);
  }

  __sync_fetch_and_add(&flag, 1);
  result = real_aligned_alloc(alignment, size);
  __sync_fetch_and_sub(&flag, 1);

  return result;
}

int
posix_memalign(void **memptr, size_t alignment, size_t size)
{
  int result;

  if (real_posix_memalign == NULL) {
    void *mem;

    /* Implement posix_memalign using memalign.  */
    if (alignment % sizeof (void *) != 0
        || !powerof2 (alignment / sizeof (void *))
        || alignment == 0)
      return EINVAL;

    mem = __libc_memalign(alignment, size);

    if (mem != NULL) {
      *memptr = mem;
      return 0;
    }

    return ENOMEM;
  }

  __sync_fetch_and_add(&flag, 1);
  result = real_posix_memalign(memptr, alignment, size);
  __sync_fetch_and_sub(&flag, 1);

  return result;
}

void *
dlopen(const char *filename, int flags)
{
  void *result;

  if (real_dlopen == NULL) {
    __ulp_asunsafe_begin();
  }

  __sync_fetch_and_add(&flag, 1);
  result = real_dlopen(filename, flags);
  __sync_fetch_and_sub(&flag, 1);

  return result;
}

void *
dlmopen(Lmid_t nsid, const char *file, int mode)
{
  void *result;

  if (real_dlmopen == NULL) {
    __ulp_asunsafe_begin();
  }

  __sync_fetch_and_add(&flag, 1);
  result = real_dlmopen(nsid, file, mode);
  __sync_fetch_and_sub(&flag, 1);

  return result;
}

int
dlclose(void *handle)
{
  int result;

  if (real_dlclose == NULL) {
    __ulp_asunsafe_begin();
  }

  __sync_fetch_and_add(&flag, 1);
  result = real_dlclose(handle);
  __sync_fetch_and_sub(&flag, 1);

  return result;
}

int
dladdr(const void *address, Dl_info *info)
{
  int result;

  if (real_dladdr == NULL) {
    __ulp_asunsafe_begin();
  }

  __sync_fetch_and_add(&flag, 1);
  result = real_dladdr(address, info);
  __sync_fetch_and_sub(&flag, 1);

  return result;
}

int
dladdr1(const void *address, Dl_info *info, void **extra_info, int flags)
{
  int result;

  if (real_dladdr1 == NULL) {
    __ulp_asunsafe_begin();
  }

  __sync_fetch_and_add(&flag, 1);
  result = real_dladdr1(address, info, extra_info, flags);
  __sync_fetch_and_sub(&flag, 1);

  return result;
}

int
dlinfo(void *handle, int request, void *arg)
{
  int result;

  if (real_dlinfo == NULL) {
    __ulp_asunsafe_begin();
  }

  __sync_fetch_and_add(&flag, 1);
  result = real_dlinfo(handle, request, arg);
  __sync_fetch_and_sub(&flag, 1);

  return result;
}
