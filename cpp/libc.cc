// libc.cc: Replacement for pyext/libc.c

#include "cpp/libc.h"

#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <glob.h>
#include <limits.h>
#include <locale.h>
#include <regex.h>
#include <signal.h>  // NSIG
#include <stdint.h>
#include <stdlib.h>
#include <string.h>  // strsignal(), strstr()
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>    // nanosleep()
#include <unistd.h>  // gethostname()
#include <wchar.h>

#include <limits>
#include <vector>

namespace libc {

BigStr* gethostname() {
  // Note: Fixed issue #1656 - OS X and FreeBSD don't have HOST_NAME_MAX
  // https://reviews.freebsd.org/D30062
  BigStr* result = OverAllocatedStr(_POSIX_HOST_NAME_MAX);
  int status = ::gethostname(result->data_, _POSIX_HOST_NAME_MAX);
  if (status != 0) {
    throw Alloc<OSError>(errno);
  }
  // Important: set the length of the string!
  result->MaybeShrink(strlen(result->data_));
  return result;
}

BigStr* realpath(BigStr* path) {
  BigStr* result = OverAllocatedStr(PATH_MAX);
  char* p = ::realpath(path->data_, result->data_);
  if (p == nullptr) {
    throw Alloc<OSError>(errno);
  }
  result->MaybeShrink(strlen(result->data_));
  return result;
}

int fnmatch(BigStr* pat, BigStr* str, int flags) {
#ifdef FNM_EXTMATCH
  flags |= FNM_EXTMATCH;
#else
  // Detected by ./configure
#endif

  int result = ::fnmatch(pat->data_, str->data_, flags);
  switch (result) {
  case 0:
    return 1;
  case FNM_NOMATCH:
    return 0;
  default:
    // Other error
    return -1;
  }
}

List<BigStr*>* glob(BigStr* pat, int flags) {
  glob_t results;
  // Hm, it's weird that the first one can't be called with GLOB_APPEND.  You
  // get a segfault.
  // int flags = GLOB_APPEND;
  // flags |= GLOB_NOMAGIC;
  int ret = glob(pat->data_, flags, NULL, &results);

  const char* err_str = NULL;
  switch (ret) {
  case 0:  // no error
    break;
  case GLOB_ABORTED:
    err_str = "GLOB_ABORTED";
    break;
  case GLOB_NOMATCH:
    // No error, because not matching isn't necessarily a problem.
    // NOTE: This can be turned on to log overaggressive calls to glob().
    // err_str = "nothing matched";
    break;
  case GLOB_NOSPACE:
    err_str = "GLOB_NOSPACE";
    break;
  default:
    err_str = "<unknown>";
    break;
  }
  if (err_str) {
    throw Alloc<RuntimeError>(StrFromC(err_str));
  }

  // http://stackoverflow.com/questions/3512414/does-this-pylist-appendlist-py-buildvalue-leak
  size_t n = results.gl_pathc;
  auto matches = NewList<BigStr*>();

  // Print array of results
  size_t i;
  for (i = 0; i < n; i++) {
    const char* m = results.gl_pathv[i];
    matches->append(StrFromC(m));
  }
  globfree(&results);

  return matches;
}

// Raises RuntimeError if the pattern is invalid.  TODO: Use a different
// exception?
List<int>* regex_search(BigStr* pattern, int cflags, BigStr* str, int eflags,
                        int pos) {
  cflags |= REG_EXTENDED;
  regex_t pat;
  int status = regcomp(&pat, pattern->data_, cflags);
  if (status != 0) {
    char error_desc[50];
    regerror(status, &pat, error_desc, 50);

    char error_message[80];
    snprintf(error_message, 80, "Invalid regex %s (%s)", pattern->data_,
             error_desc);

    throw Alloc<ValueError>(StrFromC(error_message));
  }
  // log("pat = %d, str = %d", len(pattern), len(str));

  int num_groups = pat.re_nsub + 1;  // number of captures

  List<int>* indices = NewList<int>();
  indices->reserve(num_groups * 2);

  const char* s = str->data_;
  regmatch_t* pmatch =
      static_cast<regmatch_t*>(malloc(sizeof(regmatch_t) * num_groups));
  bool match = regexec(&pat, s + pos, num_groups, pmatch, eflags) == 0;
  if (match) {
    int i;
    for (i = 0; i < num_groups; i++) {
      int start = pmatch[i].rm_so;
      if (start != -1) {
        start += pos;
      }
      indices->append(start);

      int end = pmatch[i].rm_eo;
      if (end != -1) {
        end += pos;
      }
      indices->append(end);
    }
  }

  free(pmatch);
  regfree(&pat);

  if (!match) {
    return nullptr;
  }

  return indices;
}

// For ${//}, the number of groups is always 1, so we want 2 match position
// results -- the whole regex (which we ignore), and then first group.
//
// For [[ =~ ]], do we need to count how many matches the user gave?

const int NMATCH = 2;

// Odd: This a Tuple2* not Tuple2 because it's Optional[Tuple2]!
Tuple2<int, int>* regex_first_group_match(BigStr* pattern, BigStr* str,
                                          int pos) {
  regex_t pat;
  regmatch_t m[NMATCH];

  // Could have been checked by regex_parse for [[ =~ ]], but not for glob
  // patterns like ${foo/x*/y}.

  if (regcomp(&pat, pattern->data_, REG_EXTENDED) != 0) {
    throw Alloc<RuntimeError>(
        StrFromC("Invalid regex syntax (func_regex_first_group_match)"));
  }

  // Match at offset 'pos'
  int result = regexec(&pat, str->data_ + pos, NMATCH, m, 0 /*flags*/);
  regfree(&pat);

  if (result != 0) {
    return nullptr;
  }

  // Assume there is a match
  regoff_t start = m[1].rm_so;
  regoff_t end = m[1].rm_eo;
  Tuple2<int, int>* tup = Alloc<Tuple2<int, int>>(pos + start, pos + end);

  return tup;
}

int wcswidth(BigStr* s) {
  // Behavior of mbstowcs() depends on LC_CTYPE

  // Calculate length first
  int num_wide_chars = ::mbstowcs(NULL, s->data_, 0);
  if (num_wide_chars == -1) {
    throw Alloc<UnicodeError>(StrFromC("mbstowcs() 1"));
  }

  // Allocate buffer
  int buf_size = (num_wide_chars + 1) * sizeof(wchar_t);
  wchar_t* wide_chars = static_cast<wchar_t*>(malloc(buf_size));
  DCHECK(wide_chars != nullptr);

  // Convert to wide chars
  num_wide_chars = ::mbstowcs(wide_chars, s->data_, num_wide_chars);
  if (num_wide_chars == -1) {
    free(wide_chars);  // cleanup

    throw Alloc<UnicodeError>(StrFromC("mbstowcs() 2"));
  }

  // Find number of columns
  int width = ::wcswidth(wide_chars, num_wide_chars);
  if (width == -1) {
    free(wide_chars);  // cleanup

    // unprintable chars
    throw Alloc<UnicodeError>(StrFromC("wcswidth()"));
  }

  free(wide_chars);
  return width;
}

int get_terminal_width() {
  struct winsize w;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == -1) {
    throw Alloc<IOError>(errno);
  }
  return w.ws_col;
}

int sleep_until_error(double seconds) {
  struct timespec req;
  req.tv_sec = static_cast<time_t>(seconds);
  req.tv_nsec = static_cast<time_t>((seconds - req.tv_sec) * 1e9);

  // Return 0 or errno
  int result = 0;
  if (nanosleep(&req, NULL) < 0) {
    result = errno;
  }
  return result;
}

BigStr* strsignal(int sig_num) {
  // Validate signal number range
  if (sig_num < 1 || sig_num >= NSIG) {
    throw Alloc<ValueError>(StrFromC("signal number out of range"));
  }

  char* res = ::strsignal(sig_num);

  // Return nullptr only if strsignal() fails (returns NULL)
  if (res == nullptr) {
    return nullptr;
  }

  return StrFromC(res);
}

// Stable Grease flag bits. They are intentionally unrelated to platform
// header values, which stay below this boundary.
const int kProtRead = 1;
const int kProtWrite = 2;
const int kProtExecute = 4;

const int kMapPrivate = 1;
const int kMapShared = 2;
const int kMapAnonymous = 4;

const int kSyncSync = 1;
const int kSyncAsync = 2;
const int kSyncInvalidate = 4;

const int kOpenReadOnly = 1;
const int kOpenWriteOnly = 2;
const int kOpenReadWrite = 4;
const int kOpenCreate = 8;
const int kOpenExclusive = 16;
const int kOpenTruncate = 32;
const int kOpenAppend = 64;
const int kOpenDirectory = 128;
const int kOpenNoFollow = 256;
const int kOpenCloseOnExec = 512;

struct MappingSlot {
  void* address;
  size_t length;
  int protection;
  bool active;
};

struct FileSlot {
  int fd;
  bool active;
};

static std::vector<MappingSlot> g_mappings;
static std::vector<FileSlot> g_files;

static int ParseUnsigned(BigStr* text, uint64_t* out) {
  if (len(text) == 0 || text->data_[0] == '-') {
    return EINVAL;
  }

  errno = 0;
  char* end = nullptr;
  unsigned long long number = strtoull(text->data_, &end, 10);
  if (errno != 0) {
    return errno;
  }
  if (end == text->data_ || *end != '\0') {
    return EINVAL;
  }
  *out = static_cast<uint64_t>(number);
  return 0;
}

static int ParseSize(BigStr* text, size_t* out) {
  uint64_t number = 0;
  int error_num = ParseUnsigned(text, &number);
  if (error_num != 0) {
    return error_num;
  }
  if (number > std::numeric_limits<size_t>::max()) {
    return EOVERFLOW;
  }
  *out = static_cast<size_t>(number);
  return 0;
}

static int ParseOffset(BigStr* text, off_t* out) {
  uint64_t number = 0;
  int error_num = ParseUnsigned(text, &number);
  if (error_num != 0) {
    return error_num;
  }
  uint64_t max_offset =
      static_cast<uint64_t>(std::numeric_limits<off_t>::max());
  if (number > max_offset) {
    return EOVERFLOW;
  }
  *out = static_cast<off_t>(number);
  return 0;
}

static int StoreMapping(void* address, size_t length, int protection) {
  for (size_t i = 0; i < g_mappings.size(); ++i) {
    if (!g_mappings[i].active) {
      g_mappings[i].address = address;
      g_mappings[i].length = length;
      g_mappings[i].protection = protection;
      g_mappings[i].active = true;
      return static_cast<int>(i + 1);
    }
  }
  if (g_mappings.size() >= static_cast<size_t>(INT_MAX - 1)) {
    return 0;
  }
  g_mappings.push_back({address, length, protection, true});
  return static_cast<int>(g_mappings.size());
}

static MappingSlot* GetMapping(int handle) {
  if (handle <= 0 || static_cast<size_t>(handle) > g_mappings.size()) {
    return nullptr;
  }
  MappingSlot* slot = &g_mappings[handle - 1];
  return slot->active ? slot : nullptr;
}

static int StoreFile(int fd) {
  for (size_t i = 0; i < g_files.size(); ++i) {
    if (!g_files[i].active) {
      g_files[i].fd = fd;
      g_files[i].active = true;
      return static_cast<int>(i + 1);
    }
  }
  if (g_files.size() >= static_cast<size_t>(INT_MAX - 1)) {
    return 0;
  }
  g_files.push_back({fd, true});
  return static_cast<int>(g_files.size());
}

static FileSlot* GetFile(int handle) {
  if (handle <= 0 || static_cast<size_t>(handle) > g_files.size()) {
    return nullptr;
  }
  FileSlot* slot = &g_files[handle - 1];
  return slot->active ? slot : nullptr;
}

static int NativeDirectoryFd(int handle, int* fd) {
  if (handle == 0) {
    *fd = AT_FDCWD;
    return 0;
  }
  FileSlot* slot = GetFile(handle);
  if (slot == nullptr) {
    return EBADF;
  }
  *fd = slot->fd;
  return 0;
}

static int NativeFileFd(int handle, int* fd) {
  FileSlot* slot = GetFile(handle);
  if (slot == nullptr) {
    return EBADF;
  }
  *fd = slot->fd;
  return 0;
}

static int NativeProtection(int grease_flags) {
  int result = PROT_NONE;
  if (grease_flags & kProtRead) {
    result |= PROT_READ;
  }
  if (grease_flags & kProtWrite) {
    result |= PROT_WRITE;
  }
  if (grease_flags & kProtExecute) {
    result |= PROT_EXEC;
  }
  return result;
}

static int NativeMappingFlags(int grease_flags, int* result) {
  int native_flags = 0;
  bool private_mapping = grease_flags & kMapPrivate;
  bool shared_mapping = grease_flags & kMapShared;
  if (private_mapping == shared_mapping) {
    return EINVAL;
  }
  native_flags |= private_mapping ? MAP_PRIVATE : MAP_SHARED;
  if (grease_flags & kMapAnonymous) {
#ifdef MAP_ANONYMOUS
    native_flags |= MAP_ANONYMOUS;
#else
    return ENOTSUP;
#endif
  }
  *result = native_flags;
  return 0;
}

static int NativeSyncFlags(int grease_flags, int* result) {
  bool sync = grease_flags & kSyncSync;
  bool async = grease_flags & kSyncAsync;
  if (sync == async) {
    return EINVAL;
  }
  int native_flags = sync ? MS_SYNC : MS_ASYNC;
  if (grease_flags & kSyncInvalidate) {
    native_flags |= MS_INVALIDATE;
  }
  *result = native_flags;
  return 0;
}

static int NativeOpenFlags(int grease_flags, int* result) {
  int access_count = 0;
  int native_flags = 0;

  if (grease_flags & kOpenReadOnly) {
    native_flags |= O_RDONLY;
    ++access_count;
  }
  if (grease_flags & kOpenWriteOnly) {
    native_flags |= O_WRONLY;
    ++access_count;
  }
  if (grease_flags & kOpenReadWrite) {
    native_flags |= O_RDWR;
    ++access_count;
  }
  if (access_count != 1) {
    return EINVAL;
  }

  if (grease_flags & kOpenCreate) {
    native_flags |= O_CREAT;
  }
  if (grease_flags & kOpenExclusive) {
    native_flags |= O_EXCL;
  }
  if (grease_flags & kOpenTruncate) {
    native_flags |= O_TRUNC;
  }
  if (grease_flags & kOpenAppend) {
    native_flags |= O_APPEND;
  }
#ifdef O_DIRECTORY
  if (grease_flags & kOpenDirectory) {
    native_flags |= O_DIRECTORY;
  }
#else
  if (grease_flags & kOpenDirectory) {
    return ENOTSUP;
  }
#endif
#ifdef O_NOFOLLOW
  if (grease_flags & kOpenNoFollow) {
    native_flags |= O_NOFOLLOW;
  }
#else
  if (grease_flags & kOpenNoFollow) {
    return ENOTSUP;
  }
#endif
#ifdef O_CLOEXEC
  if (grease_flags & kOpenCloseOnExec) {
    native_flags |= O_CLOEXEC;
  }
#else
  if (grease_flags & kOpenCloseOnExec) {
    return ENOTSUP;
  }
#endif

  *result = native_flags;
  return 0;
}

Tuple2<int, int>* grease_mmap(BigStr* length_text, int protection,
                              int mapping_flags, int file_handle,
                              BigStr* offset_text) {
  size_t length = 0;
  int error_num = ParseSize(length_text, &length);
  if (error_num != 0) {
    return Alloc<Tuple2<int, int>>(error_num, 0);
  }

  off_t offset = 0;
  error_num = ParseOffset(offset_text, &offset);
  if (error_num != 0) {
    return Alloc<Tuple2<int, int>>(error_num, 0);
  }

  int native_flags = 0;
  error_num = NativeMappingFlags(mapping_flags, &native_flags);
  if (error_num != 0) {
    return Alloc<Tuple2<int, int>>(error_num, 0);
  }

  int fd = -1;
  if (!(mapping_flags & kMapAnonymous)) {
    error_num = NativeFileFd(file_handle, &fd);
    if (error_num != 0) {
      return Alloc<Tuple2<int, int>>(error_num, 0);
    }
  }

  errno = 0;
  void* address =
      ::mmap(nullptr, length, NativeProtection(protection), native_flags, fd,
             offset);
  if (address == MAP_FAILED) {
    return Alloc<Tuple2<int, int>>(errno, 0);
  }

  int handle = StoreMapping(address, length, protection);
  if (handle == 0) {
    ::munmap(address, length);
    return Alloc<Tuple2<int, int>>(ENOMEM, 0);
  }
  return Alloc<Tuple2<int, int>>(0, handle);
}

int grease_munmap(int mapping_handle) {
  MappingSlot* mapping = GetMapping(mapping_handle);
  if (mapping == nullptr) {
    return EINVAL;
  }
  if (::munmap(mapping->address, mapping->length) == -1) {
    return errno;
  }
  mapping->active = false;
  mapping->address = nullptr;
  mapping->length = 0;
  mapping->protection = 0;
  return 0;
}

int grease_mprotect(int mapping_handle, int protection) {
  MappingSlot* mapping = GetMapping(mapping_handle);
  if (mapping == nullptr) {
    return EINVAL;
  }
  if (::mprotect(mapping->address, mapping->length,
                 NativeProtection(protection)) == -1) {
    return errno;
  }
  mapping->protection = protection;
  return 0;
}

int grease_msync(int mapping_handle, int sync_flags) {
  MappingSlot* mapping = GetMapping(mapping_handle);
  if (mapping == nullptr) {
    return EINVAL;
  }
  int native_flags = 0;
  int error_num = NativeSyncFlags(sync_flags, &native_flags);
  if (error_num != 0) {
    return error_num;
  }
  if (::msync(mapping->address, mapping->length, native_flags) == -1) {
    return errno;
  }
  return 0;
}

Tuple2<int, BigStr*>* grease_mapping_read(int mapping_handle,
                                          BigStr* offset_text,
                                          BigStr* length_text) {
  MappingSlot* mapping = GetMapping(mapping_handle);
  if (mapping == nullptr) {
    return Alloc<Tuple2<int, BigStr*>>(EINVAL, kEmptyString);
  }
  if (!(mapping->protection & kProtRead)) {
    return Alloc<Tuple2<int, BigStr*>>(EACCES, kEmptyString);
  }

  size_t offset = 0;
  int error_num = ParseSize(offset_text, &offset);
  if (error_num != 0) {
    return Alloc<Tuple2<int, BigStr*>>(error_num, kEmptyString);
  }

  size_t length = 0;
  error_num = ParseSize(length_text, &length);
  if (error_num != 0) {
    return Alloc<Tuple2<int, BigStr*>>(error_num, kEmptyString);
  }

  if (offset > mapping->length || length > mapping->length - offset) {
    return Alloc<Tuple2<int, BigStr*>>(EINVAL, kEmptyString);
  }
  if (length > static_cast<size_t>(INT_MAX)) {
    return Alloc<Tuple2<int, BigStr*>>(EOVERFLOW, kEmptyString);
  }

  BigStr* result = OverAllocatedStr(static_cast<int>(length));
  memcpy(result->data_, static_cast<char*>(mapping->address) + offset, length);
  result->MaybeShrink(static_cast<int>(length));
  return Alloc<Tuple2<int, BigStr*>>(0, result);
}

int grease_mapping_write(int mapping_handle, BigStr* offset_text,
                          BigStr* data) {
  MappingSlot* mapping = GetMapping(mapping_handle);
  if (mapping == nullptr) {
    return EINVAL;
  }
  if (!(mapping->protection & kProtWrite)) {
    return EACCES;
  }

  size_t offset = 0;
  int error_num = ParseSize(offset_text, &offset);
  if (error_num != 0) {
    return error_num;
  }

  size_t length = static_cast<size_t>(len(data));
  if (offset > mapping->length || length > mapping->length - offset) {
    return EINVAL;
  }

  memcpy(static_cast<char*>(mapping->address) + offset, data->data_, length);
  return 0;
}

Tuple2<int, int>* grease_openat(int directory_handle, BigStr* path,
                                int open_flags, int mode) {
  int directory_fd = AT_FDCWD;
  int error_num = NativeDirectoryFd(directory_handle, &directory_fd);
  if (error_num != 0) {
    return Alloc<Tuple2<int, int>>(error_num, 0);
  }

  int native_flags = 0;
  error_num = NativeOpenFlags(open_flags, &native_flags);
  if (error_num != 0) {
    return Alloc<Tuple2<int, int>>(error_num, 0);
  }

  errno = 0;
  int fd = ::openat(directory_fd, path->data_, native_flags,
                    static_cast<mode_t>(mode));
  if (fd == -1) {
    return Alloc<Tuple2<int, int>>(errno, 0);
  }

  int handle = StoreFile(fd);
  if (handle == 0) {
    ::close(fd);
    return Alloc<Tuple2<int, int>>(ENOMEM, 0);
  }
  return Alloc<Tuple2<int, int>>(0, handle);
}

int grease_close(int file_handle) {
  FileSlot* file = GetFile(file_handle);
  if (file == nullptr) {
    return EBADF;
  }
  if (::close(file->fd) == -1) {
    return errno;
  }
  file->active = false;
  file->fd = -1;
  return 0;
}

int grease_linkat(int old_directory_handle, BigStr* old_path,
                   int new_directory_handle, BigStr* new_path,
                   bool follow_symlink) {
  int old_fd = AT_FDCWD;
  int error_num = NativeDirectoryFd(old_directory_handle, &old_fd);
  if (error_num != 0) {
    return error_num;
  }
  int new_fd = AT_FDCWD;
  error_num = NativeDirectoryFd(new_directory_handle, &new_fd);
  if (error_num != 0) {
    return error_num;
  }

  int flags = follow_symlink ? AT_SYMLINK_FOLLOW : 0;
  if (::linkat(old_fd, old_path->data_, new_fd, new_path->data_, flags) == -1) {
    return errno;
  }
  return 0;
}

int grease_symlinkat(BigStr* target, int directory_handle, BigStr* path) {
  int directory_fd = AT_FDCWD;
  int error_num = NativeDirectoryFd(directory_handle, &directory_fd);
  if (error_num != 0) {
    return error_num;
  }
  if (::symlinkat(target->data_, directory_fd, path->data_) == -1) {
    return errno;
  }
  return 0;
}

int grease_unlinkat(int directory_handle, BigStr* path,
                     bool remove_directory) {
  int directory_fd = AT_FDCWD;
  int error_num = NativeDirectoryFd(directory_handle, &directory_fd);
  if (error_num != 0) {
    return error_num;
  }
  int flags = remove_directory ? AT_REMOVEDIR : 0;
  if (::unlinkat(directory_fd, path->data_, flags) == -1) {
    return errno;
  }
  return 0;
}

BigStr* grease_errno_name(int errno_num) {
  switch (errno_num) {
  case 0:
    return StrFromC("OK");
  case EACCES:
    return StrFromC("EACCES");
  case EBADF:
    return StrFromC("EBADF");
  case EEXIST:
    return StrFromC("EEXIST");
  case EINVAL:
    return StrFromC("EINVAL");
  case EIO:
    return StrFromC("EIO");
  case EISDIR:
    return StrFromC("EISDIR");
  case ELOOP:
    return StrFromC("ELOOP");
  case ENAMETOOLONG:
    return StrFromC("ENAMETOOLONG");
  case ENOENT:
    return StrFromC("ENOENT");
  case ENOMEM:
    return StrFromC("ENOMEM");
  case ENOSPC:
    return StrFromC("ENOSPC");
  case ENOTDIR:
    return StrFromC("ENOTDIR");
  case ENOTSUP:
    return StrFromC("ENOTSUP");
  case EPERM:
    return StrFromC("EPERM");
  case EOVERFLOW:
    return StrFromC("EOVERFLOW");
  case EROFS:
    return StrFromC("EROFS");
  default:
    return StrFromC("ERRNO");
  }
}

BigStr* grease_errno_message(int errno_num) {
  const char* message = strerror(errno_num);
  return message == nullptr ? StrFromC("unknown native error")
                            : StrFromC(message);
}

}  // namespace libc

namespace pylocale {
BigStr* setlocale(int category, BigStr* locale) {
  char* locale_name = ::setlocale(category, locale->data_);
  if (locale_name == nullptr) {
    throw Alloc<Error>();
  }
  return StrFromC(locale_name);
}
BigStr* nl_langinfo(int item) {
  return StrFromC(::nl_langinfo(item));
}
}  // namespace pylocale
