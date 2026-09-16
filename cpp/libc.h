// libc.h: Replacement for native/libc.c

#ifndef LIBC_H
#define LIBC_H

#include <langinfo.h>  // using CODESET
#include <locale.h>    // using LC_CTYPE
#include <stdlib.h>

#include "mycpp/runtime.h"

namespace libc {

// TODO: SHARE with pyext
inline void print_time(double real, double user, double sys) {
  fprintf(stderr, "real\t%.3f\n", real);
  fprintf(stderr, "user\t%.3f\n", user);
  fprintf(stderr, "sys\t%.3f\n", sys);
}

BigStr* realpath(BigStr* path);

BigStr* gethostname();

int fnmatch(BigStr* pat, BigStr* str, int flags = 0);

List<BigStr*>* glob(BigStr* pat, int flags = 0);

Tuple2<int, int>* regex_first_group_match(BigStr* pattern, BigStr* str,
                                          int pos);

List<int>* regex_search(BigStr* pattern, int cflags, BigStr* str, int eflags,
                        int pos = 0);

int wcswidth(BigStr* str);
int get_terminal_width();
int sleep_until_error(double seconds);

BigStr* strsignal(int sig_num);

// Grease's first native Linux/libc vocabulary. These wrappers return errno
// rather than exposing libc failure sentinels to Grease. Mapping and file
// descriptor handles are opaque registry indices, never raw pointers or kernel
// file descriptor numbers.
Tuple2<int, int>* grease_mmap(BigStr* length, int protection,
                              int mapping_flags, int file_handle,
                              BigStr* offset);
int grease_munmap(int mapping_handle);
int grease_mprotect(int mapping_handle, int protection);
int grease_msync(int mapping_handle, int sync_flags);

Tuple2<int, BigStr*>* grease_mapping_read(int mapping_handle, BigStr* offset,
                                          BigStr* length);
int grease_mapping_write(int mapping_handle, BigStr* offset, BigStr* data);

Tuple2<int, int>* grease_openat(int directory_handle, BigStr* path,
                                int open_flags, int mode);
int grease_close(int file_handle);
int grease_linkat(int old_directory_handle, BigStr* old_path,
                   int new_directory_handle, BigStr* new_path,
                   bool follow_symlink);
int grease_symlinkat(BigStr* target, int directory_handle, BigStr* path);
int grease_unlinkat(int directory_handle, BigStr* path,
                     bool remove_directory);

BigStr* grease_errno_name(int errno_num);
BigStr* grease_errno_message(int errno_num);

}  // namespace libc

// pylib/locale_.py
namespace pylocale {

constexpr int codeset = CODESET;
constexpr int lc_all = LC_ALL;
constexpr int lc_collate = LC_COLLATE;
constexpr int lc_ctype = LC_CTYPE;
#undef CODESET
#undef LC_ALL
#undef LC_COLLATE
#undef LC_CTYPE
constexpr int CODESET = codeset;
constexpr int LC_ALL = lc_all;
constexpr int LC_COLLATE = lc_collate;
constexpr int LC_CTYPE = lc_ctype;

class Error {
 public:
  static constexpr ObjHeader obj_header() {
    return ObjHeader::ClassFixed(kZeroMask, sizeof(Error));
  }
};
BigStr* setlocale(int category, BigStr* locale);
BigStr* nl_langinfo(int item);

}  // namespace pylocale

#endif  // LIBC_H
