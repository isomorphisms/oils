#include "cpp/libc.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>  // setlocale()
#include <regex.h>   // regcomp()
#include <stdio.h>
#include <stdlib.h>  // mkdtemp()
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>  // gethostname()

#include "mycpp/runtime.h"
#include "vendor/greatest.h"

TEST hostname_test() {
  BigStr* s0 = libc::gethostname();
  ASSERT(s0 != nullptr);

  char buf[1024];
  ASSERT(gethostname(buf, HOST_NAME_MAX) == 0);
  ASSERT(str_equals(s0, StrFromC(buf)));

  PASS();
}

TEST realpath_test() {
  BigStr* result = libc::realpath(StrFromC("/"));
  ASSERT(str_equals(StrFromC("/"), result));

  bool caught = false;
  try {
    libc::realpath(StrFromC("/nonexistent_ZZZ"));
  } catch (IOError_OSError* e) {
    caught = true;
  }
  ASSERT(caught);

  PASS();
}

TEST libc_test() {
  log("sizeof(wchar_t) = %d", sizeof(wchar_t));

  int width = 0;

  // TODO: enable this test.  Is it not picking LC_CTYPE?
  // Do we have to do some initialization like libc.cpython_reset_locale() ?
#if 0
  try {
    // mu character \u{03bc} in utf-8
    width = libc::wcswidth(StrFromC("\xce\xbc"));
  } catch (UnicodeError* e) {
    log("UnicodeError %s", e->message->data_);
  }
  ASSERT_EQ_FMT(2, width, "%d");
#endif

  BigStr* h = libc::gethostname();
  log("gethostname() = %s %d", h->data_, len(h));

  width = libc::wcswidth(StrFromC("foo"));
  ASSERT_EQ(3, width);

  libc::print_time(0.1, 0.2, 0.3);

  PASS();
}

static List<BigStr*>* Groups(BigStr* s, List<int>* indices) {
  List<BigStr*>* groups = NewList<BigStr*>();
  int n = len(indices) / 2;
  for (int i = 0; i < n; ++i) {
    int start = indices->at(2 * i);
    int end = indices->at(2 * i + 1);
    if (start == -1) {
      groups->append(nullptr);
    } else {
      groups->append(s->slice(start, end));
    }
  }
  return groups;
}

TEST regex_wrapper_test() {
  BigStr* s1 = StrFromC("-abaacaaa");
  List<int>* indices = libc::regex_search(StrFromC("(a+).(a+)"), 0, s1, 0);
  List<BigStr*>* results = Groups(s1, indices);
  ASSERT_EQ_FMT(3, len(results), "%d");
  ASSERT(str_equals(StrFromC("abaa"), results->at(0)));  // whole match
  ASSERT(str_equals(StrFromC("a"), results->at(1)));
  ASSERT(str_equals(StrFromC("aa"), results->at(2)));

  indices = libc::regex_search(StrFromC("z+"), 0, StrFromC("abaacaaa"), 0);
  ASSERT_EQ(nullptr, indices);

  // Alternation gives unmatched group
  BigStr* s2 = StrFromC("b");
  indices = libc::regex_search(StrFromC("(a)|(b)"), 0, s2, 0);
  results = Groups(s2, indices);
  ASSERT_EQ_FMT(3, len(results), "%d");
  ASSERT(str_equals(StrFromC("b"), results->at(0)));  // whole match
  ASSERT_EQ(nullptr, results->at(1));
  ASSERT(str_equals(StrFromC("b"), results->at(2)));

  // Like Unicode test below
  indices = libc::regex_search(StrFromC("_._"), 0, StrFromC("_x_"), 0);
  ASSERT(indices != nullptr);
  ASSERT_EQ_FMT(2, len(indices), "%d");
  ASSERT_EQ_FMT(0, indices->at(0), "%d");
  ASSERT_EQ_FMT(3, indices->at(1), "%d");

  // TODO(unicode)
#if 0
  //indices = libc::regex_search(StrFromC("_._"), 0, StrFromC("_\u03bc_"), 0);
  indices = libc::regex_search(StrFromC("_._"), 0, StrFromC("_μ_"), 0);
  ASSERT(indices != nullptr);
  ASSERT_EQ_FMT(2, len(indices), "%d");
  ASSERT_EQ_FMT(0, indices->at(0), "%d");
  ASSERT_EQ_FMT(0, indices->at(0), "%d");
#endif

  Tuple2<int, int>* result;
  BigStr* s = StrFromC("oXooXoooXoX");
  result = libc::regex_first_group_match(StrFromC("(X.)"), s, 0);
  ASSERT_EQ_FMT(1, result->at0(), "%d");
  ASSERT_EQ_FMT(3, result->at1(), "%d");

  result = libc::regex_first_group_match(StrFromC("(X.)"), s, 3);
  ASSERT_EQ_FMT(4, result->at0(), "%d");
  ASSERT_EQ_FMT(6, result->at1(), "%d");

  result = libc::regex_first_group_match(StrFromC("(X.)"), s, 6);
  ASSERT_EQ_FMT(8, result->at0(), "%d");
  ASSERT_EQ_FMT(10, result->at1(), "%d");

  PASS();
}

TEST glob_test() {
  // This depends on the file system
  auto files = libc::glob(StrFromC("*.testdata"));
  // 3 files are made by the shell wrapper
  ASSERT_EQ_FMT(3, len(files), "%d");

  print(files->at(0));

  auto files2 = libc::glob(StrFromC("*.pyzzz"));
  ASSERT_EQ_FMT(0, len(files2), "%d");

  PASS();
}

TEST fnmatch_test() {
  BigStr* s1 = (StrFromC("foo.py "))->strip();
  ASSERT(libc::fnmatch(StrFromC("*.py"), s1));
  ASSERT(!libc::fnmatch(StrFromC("*.py"), StrFromC("foo.p")));

  // Unicode - ? is byte or code point?
  ASSERT(libc::fnmatch(StrFromC("_?_"), StrFromC("_x_")));

  // TODO(unicode)
  // ASSERT(libc::fnmatch(StrFromC("_?_"), StrFromC("_\u03bc_")));
  // ASSERT(libc::fnmatch(StrFromC("_?_"), StrFromC("_μ_")));

  // extended glob
  ASSERT(libc::fnmatch(StrFromC("*(foo|bar).py"), StrFromC("foo.py")));
  ASSERT(!libc::fnmatch(StrFromC("*(foo|bar).py"), StrFromC("foo.p")));

  PASS();
}

// These are stable Grease bridge bits, not native header constants.  The
// wrapper translates them to the host/Bionic constants below the language
// boundary.
const int kGreaseProtRead = 1;
const int kGreaseProtWrite = 2;
const int kGreaseMapPrivate = 1;
const int kGreaseMapShared = 2;
const int kGreaseMapAnonymous = 4;
const int kGreaseSyncSync = 1;
const int kGreaseOpenReadOnly = 1;
const int kGreaseOpenReadWrite = 4;
const int kGreaseOpenDirectory = 128;

TEST grease_native_memory_test() {
  Tuple2<int, int>* failed =
      libc::grease_mmap(StrFromC("0"), kGreaseProtRead,
                        kGreaseMapPrivate | kGreaseMapAnonymous, -1,
                        StrFromC("0"));
  ASSERT_EQ_FMT(EINVAL, failed->at0(), "%d");
  ASSERT_EQ_FMT(0, failed->at1(), "%d");
  ASSERT(str_equals(StrFromC("EINVAL"), libc::grease_errno_name(EINVAL)));

  Tuple2<int, int>* mapped =
      libc::grease_mmap(StrFromC("4096"),
                        kGreaseProtRead | kGreaseProtWrite,
                        kGreaseMapPrivate | kGreaseMapAnonymous, -1,
                        StrFromC("0"));
  ASSERT_EQ_FMT(0, mapped->at0(), "%d");
  int handle = mapped->at1();
  ASSERT(handle > 0);

  ASSERT_EQ_FMT(0,
                libc::grease_mapping_write(handle, StrFromC("17"),
                                            StrFromC("pensieve")),
                "%d");
  Tuple2<int, BigStr*>* read =
      libc::grease_mapping_read(handle, StrFromC("17"), StrFromC("8"));
  ASSERT_EQ_FMT(0, read->at0(), "%d");
  ASSERT(str_equals(StrFromC("pensieve"), read->at1()));

  Tuple2<int, BigStr*>* past_end =
      libc::grease_mapping_read(handle, StrFromC("4094"), StrFromC("8"));
  ASSERT_EQ_FMT(EINVAL, past_end->at0(), "%d");

  ASSERT_EQ_FMT(0, libc::grease_mprotect(handle, kGreaseProtRead), "%d");
  ASSERT_EQ_FMT(0,
                libc::grease_mprotect(handle,
                                      kGreaseProtRead | kGreaseProtWrite),
                "%d");

  ASSERT_EQ_FMT(0, libc::grease_munmap(handle), "%d");
  ASSERT_EQ_FMT(EINVAL, libc::grease_munmap(handle), "%d");

  PASS();
}

static bool TempPath(char* out, size_t out_size, const char* directory,
                     const char* leaf) {
  int n = snprintf(out, out_size, "%s/%s", directory, leaf);
  return n > 0 && static_cast<size_t>(n) < out_size;
}

TEST grease_native_at_filesystem_test() {
  char directory_template[] = "/tmp/grease-native-XXXXXX";
  char* directory = mkdtemp(directory_template);
  ASSERT(directory != nullptr);

  char index_path[PATH_MAX];
  ASSERT(TempPath(index_path, sizeof(index_path), directory, "index.bin"));

  int setup_fd = open(index_path, O_CREAT | O_RDWR | O_TRUNC, 0600);
  ASSERT(setup_fd >= 0);
  ASSERT(ftruncate(setup_fd, 4096) == 0);
  ASSERT_EQ_FMT(8, static_cast<int>(pwrite(setup_fd, "fragment", 8, 0)), "%d");
  ASSERT_EQ_FMT(0, close(setup_fd), "%d");

  Tuple2<int, int>* opened_dir =
      libc::grease_openat(0, StrFromC(directory),
                           kGreaseOpenReadOnly | kGreaseOpenDirectory, 0);
  ASSERT_EQ_FMT(0, opened_dir->at0(), "%d");
  int directory_handle = opened_dir->at1();
  ASSERT(directory_handle > 0);

  Tuple2<int, int>* missing =
      libc::grease_openat(directory_handle, StrFromC("missing"),
                           kGreaseOpenReadOnly, 0);
  ASSERT_EQ_FMT(ENOENT, missing->at0(), "%d");
  ASSERT_EQ_FMT(0, missing->at1(), "%d");

  Tuple2<int, int>* opened_file =
      libc::grease_openat(directory_handle, StrFromC("index.bin"),
                           kGreaseOpenReadWrite, 0);
  ASSERT_EQ_FMT(0, opened_file->at0(), "%d");
  int file_handle = opened_file->at1();
  ASSERT(file_handle > 0);

  Tuple2<int, int>* mapped =
      libc::grease_mmap(StrFromC("4096"),
                        kGreaseProtRead | kGreaseProtWrite,
                        kGreaseMapShared, file_handle, StrFromC("0"));
  ASSERT_EQ_FMT(0, mapped->at0(), "%d");
  int mapping_handle = mapped->at1();

  Tuple2<int, BigStr*>* original = libc::grease_mapping_read(
      mapping_handle, StrFromC("0"), StrFromC("8"));
  ASSERT_EQ_FMT(0, original->at0(), "%d");
  ASSERT(str_equals(StrFromC("fragment"), original->at1()));

  ASSERT_EQ_FMT(0,
                libc::grease_mapping_write(mapping_handle, StrFromC("0"),
                                            StrFromC("pensieve")),
                "%d");
  ASSERT_EQ_FMT(0, libc::grease_msync(mapping_handle, kGreaseSyncSync), "%d");
  ASSERT_EQ_FMT(0, libc::grease_munmap(mapping_handle), "%d");
  ASSERT_EQ_FMT(0, libc::grease_close(file_handle), "%d");

  int verify_fd = open(index_path, O_RDONLY);
  ASSERT(verify_fd >= 0);
  char verify[9];
  memset(verify, 0, sizeof(verify));
  ASSERT_EQ_FMT(8, static_cast<int>(read(verify_fd, verify, 8)), "%d");
  ASSERT_EQ_FMT(0, close(verify_fd), "%d");
  ASSERT(strcmp(verify, "pensieve") == 0);

  ASSERT_EQ_FMT(0,
                libc::grease_linkat(directory_handle, StrFromC("index.bin"),
                                    directory_handle, StrFromC("index-hard"),
                                    false),
                "%d");
  ASSERT_EQ_FMT(0,
                libc::grease_symlinkat(StrFromC("index.bin"), directory_handle,
                                       StrFromC("index-link")),
                "%d");

  char hard_path[PATH_MAX];
  char link_path[PATH_MAX];
  ASSERT(TempPath(hard_path, sizeof(hard_path), directory, "index-hard"));
  ASSERT(TempPath(link_path, sizeof(link_path), directory, "index-link"));

  struct stat original_stat;
  struct stat hard_stat;
  struct stat link_stat;
  ASSERT_EQ_FMT(0, stat(index_path, &original_stat), "%d");
  ASSERT_EQ_FMT(0, stat(hard_path, &hard_stat), "%d");
  ASSERT(original_stat.st_ino == hard_stat.st_ino);
  ASSERT_EQ_FMT(0, lstat(link_path, &link_stat), "%d");
  ASSERT(S_ISLNK(link_stat.st_mode));

  ASSERT_EQ_FMT(0,
                libc::grease_unlinkat(directory_handle, StrFromC("index-hard"),
                                      false),
                "%d");
  ASSERT_EQ_FMT(0,
                libc::grease_unlinkat(directory_handle, StrFromC("index-link"),
                                      false),
                "%d");
  ASSERT_EQ_FMT(ENOENT,
                libc::grease_unlinkat(directory_handle, StrFromC("index-link"),
                                      false),
                "%d");
  ASSERT_EQ_FMT(0,
                libc::grease_unlinkat(directory_handle, StrFromC("index.bin"),
                                      false),
                "%d");
  ASSERT_EQ_FMT(0, libc::grease_close(directory_handle), "%d");
  ASSERT_EQ_FMT(0, rmdir(directory), "%d");

  PASS();
}

TEST for_test_coverage() {
  // Sometimes we're not connected to a terminal
  try {
    libc::get_terminal_width();
  } catch (IOError_OSError* e) {
  }

  PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
  gHeap.Init();

  GREATEST_MAIN_BEGIN();

  RUN_TEST(hostname_test);
  RUN_TEST(realpath_test);
  RUN_TEST(libc_test);
  RUN_TEST(regex_wrapper_test);
  RUN_TEST(glob_test);
  RUN_TEST(fnmatch_test);
  RUN_TEST(grease_native_memory_test);
  RUN_TEST(grease_native_at_filesystem_test);
  RUN_TEST(for_test_coverage);

  gHeap.CleanProcessExit();

  GREATEST_MAIN_END();
  return 0;
}
