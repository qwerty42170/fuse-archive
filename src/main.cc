// Copyright 2021 The Fuse-Archive Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// ----------------

// fuse-archive read-only mounts an archive or compressed file (e.g. foo.tar,
// foo.tar.gz, foo.xz, foo.zip) as a FUSE file system
// (https://en.wikipedia.org/wiki/Filesystem_in_Userspace).
//
// To build:
//   g++ -O3 main.cc `pkg-config libarchive fuse --cflags --libs` -o example
//
// To use:
//   ./example ../test/data/archive.zip the/path/to/the/mountpoint
//   ls -l                              the/path/to/the/mountpoint
//   fusermount -u                      the/path/to/the/mountpoint
//
// Pass the "-f" flag to "./example" for foreground operation.

// ---- Preprocessor

#define FUSE_USE_VERSION 26

#include <archive.h>
#include <archive_entry.h>
#include <fcntl.h>
#include <fuse.h>
#include <langinfo.h>
#include <locale.h>
#include <sys/stat.h>
#include <syslog.h>
#include <termios.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#define TRY(operation)               \
  do {                               \
    int try_status_code = operation; \
    if (try_status_code) {           \
      return try_status_code;        \
    }                                \
  } while (false)

// TRY_EXIT_CODE is like TRY but is used in the main function where Linux exit
// codes range from 0 to 255. In contrast, TRY is used for other functions
// where e.g. it's valid to return a negative value like -ENOENT.
#define TRY_EXIT_CODE(operation)        \
  do {                                  \
    int try_status_code = operation;    \
    if (try_status_code < 0) {          \
      return EXIT_CODE_GENERIC_FAILURE; \
    } else if (try_status_code > 0) {   \
      return try_status_code;           \
    }                                   \
  } while (false)

// ---- Exit Codes

// These are values passed to the exit function, or returned by main. These are
// (Linux or Linux-like) application exit codes, not library error codes.
//
// Note that, unless the -f command line option was passed for foreground
// operation, the parent process may very well ignore the exit code value after
// daemonization succeeds.

enum {
  EXIT_CODE_GENERIC_FAILURE = 1,
  EXIT_CODE_CANNOT_OPEN_ARCHIVE = 11,
  EXIT_CODE_PASSPHRASE_REQUIRED = 20,
  EXIT_CODE_PASSPHRASE_INCORRECT = 21,
  EXIT_CODE_PASSPHRASE_NOT_SUPPORTED = 22,
  EXIT_CODE_INVALID_RAW_ARCHIVE = 30,
  EXIT_CODE_INVALID_ARCHIVE_HEADER = 31,
  EXIT_CODE_INVALID_ARCHIVE_CONTENTS = 32,
};

// ---- Compile-time Configuration

#define PROGRAM_NAME "fuse-archive"

#ifndef FUSE_ARCHIVE_VERSION
#define FUSE_ARCHIVE_VERSION "0.1.14"
#endif

#ifndef BLOCK_SIZE
#define BLOCK_SIZE 16384
#endif

#ifndef NUM_SAVED_READERS
#define NUM_SAVED_READERS 8
#endif

#ifndef NUM_SIDE_BUFFERS
#define NUM_SIDE_BUFFERS 8
#elif NUM_SIDE_BUFFERS <= 0
#error "invalid NUM_SIDE_BUFFERS"
#endif

// This defaults to 128 KiB (0x20000 bytes) because, on a vanilla x86_64 Debian
// Linux, that seems to be the largest buffer size passed to my_read.
#ifndef SIDE_BUFFER_SIZE
#define SIDE_BUFFER_SIZE 131072
#elif SIDE_BUFFER_SIZE <= 0
#error "invalid SIDE_BUFFER_SIZE"
#endif

// ---- Platform specifics

#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__APPLE__)
#define lseek64 lseek
#endif

// ---- Globals

static struct {
  int arg_count = 0;
  bool help = false;
  bool version = false;
  bool quiet = false;
  bool redact = false;
} g_options;

enum {
  KEY_HELP,
  KEY_VERSION,
  KEY_QUIET,
  KEY_VERBOSE,
  KEY_REDACT,
};

static struct fuse_opt g_fuse_opts[] = {
    FUSE_OPT_KEY("-h", KEY_HELP),            //
    FUSE_OPT_KEY("--help", KEY_HELP),        //
    FUSE_OPT_KEY("-V", KEY_VERSION),         //
    FUSE_OPT_KEY("--version", KEY_VERSION),  //
    FUSE_OPT_KEY("--quiet", KEY_QUIET),      //
    FUSE_OPT_KEY("-q", KEY_QUIET),           //
    FUSE_OPT_KEY("--verbose", KEY_VERBOSE),  //
    FUSE_OPT_KEY("-v", KEY_VERBOSE),         //
    FUSE_OPT_KEY("--redact", KEY_REDACT),    //
    FUSE_OPT_KEY("redact", KEY_REDACT),      //
    // The remaining options are listed for e.g. "-o formatraw" command line
    // compatibility with the https://github.com/cybernoid/archivemount program
    // but are otherwise ignored. For example, this program detects 'raw'
    // archives automatically and only supports read-only, not read-write.
    FUSE_OPT_KEY("--passphrase", FUSE_OPT_KEY_DISCARD),  //
    FUSE_OPT_KEY("passphrase", FUSE_OPT_KEY_DISCARD),    //
    FUSE_OPT_KEY("formatraw", FUSE_OPT_KEY_DISCARD),     //
    FUSE_OPT_KEY("nobackup", FUSE_OPT_KEY_DISCARD),      //
    FUSE_OPT_KEY("nosave", FUSE_OPT_KEY_DISCARD),        //
    FUSE_OPT_KEY("readonly", FUSE_OPT_KEY_DISCARD),      //
    FUSE_OPT_END,
};

// g_archive_filename is the command line argument naming the archive file.
static const char* g_archive_filename = nullptr;

// g_archive_innername is the base name of g_archive_filename, minus the file
// extension suffix. For example, if g_archive_filename is "/foo/bar.ext0.ext1"
// then g_archive_innername is "bar.ext0".
static const char* g_archive_innername = nullptr;

static std::string g_mount_point;

// g_archive_fd is the file descriptor returned by opening g_archive_filename.
static int g_archive_fd = -1;

// g_archive_file_size is the size of the g_archive_filename file.
static int64_t g_archive_file_size = 0;

// g_archive_fd_position_current is the read position of g_archive_fd.
//
// etc_hwm is the etc_current high water mark (the largest value seen). When
// compared to g_archive_file_size, it proxies what proportion of the archive
// has been processed. This matters for 'raw' archives that need a complete
// decompression pass (as they do not have a table of contents within to
// explicitly record the decompressed file size).
static int64_t g_archive_fd_position_current = 0;
static int64_t g_archive_fd_position_hwm = 0;

// g_archive_realpath holds the canonicalised absolute path of the archive
// file. The command line argument may give a relative filename (one that
// doesn't start with a slash) and the fuse_main function may change the
// current working directory, so subsequent archive_read_open_filename calls
// use this absolute filepath instead. g_archive_filename is still used for
// logging. g_archive_realpath is allocated in pre_initialize and never freed.
static const char* g_archive_realpath = nullptr;

// Decryption password.
static std::string password;

// Number of times the decryption password has been requested.
static int password_count = 0;

// g_archive_is_raw is whether the archive file is 'cooked' or 'raw'.
//
// We support 'cooked' archive files (e.g. foo.tar.gz or foo.zip) but also what
// libarchive calls 'raw' files (e.g. foo.gz), which are compressed but not
// explicitly an archive (a collection of files). libarchive can still present
// it as an implicit archive containing 1 file.
static bool g_archive_is_raw = false;

// g_uid and g_gid are the user/group IDs for the files we serve. They're the
// same as the current uid/gid.
//
// libfuse will override my_getattr's use of these variables if the "-o uid=N"
// or "-o gid=N" command line options are set.
static uid_t g_uid = 0;
static gid_t g_gid = 0;

// We serve ls and stat requests from an in-memory directory tree of nodes.
// Building that tree is one of the first things that we do.
static struct archive* g_initialize_archive = nullptr;
static struct archive_entry* g_initialize_archive_entry = nullptr;
static int64_t g_initialize_index_within_archive = -1;

// g_displayed_progress is whether we have printed a progress message.
static bool g_displayed_progress = false;

// These global variables are the in-memory directory tree of nodes.
//
// Building the directory tree can take minutes, for archive file formats like
// .tar.gz that are compressed but also do not contain an explicit on-disk
// directory of archive entries.
struct Node;
static std::unordered_map<std::string, Node*> g_nodes_by_name;
static std::vector<Node*> g_nodes_by_index;

// g_root_node being non-nullptr means that initialization is complete .
static Node* g_root_node = nullptr;
static constexpr blksize_t block_size = 512;
static blkcnt_t g_block_count = 1;

// g_saved_readers is a cache of warm readers. libarchive is designed for
// streaming access, not random access, and generally does not support seeking
// backwards. For example, if some other program reads "/foo", "/bar" and then
// "/baz" sequentially from an archive (via this program) and those correspond
// to the 60th, 40th and 50th archive entries in that archive, then:
//
//  - A naive implementation (calling archive_read_free when each FUSE file is
//    closed) would have to start iterating from the first archive entry each
//    time a FUSE file is opened, for 150 iterations (60 + 40 + 50) in total.
//  - Saving readers in an LRU (Least Recently Used) cache (calling
//    release_reader when each FUSE file is closed) allows just 110 iterations
//    (60 + 40 + 10) in total. The Reader for "/bar" can be re-used for "/baz".
//
// Re-use eligibility is based on the archive entries' sequential numerical
// indexes within the archive, not on their string pathnames.
//
// When copying all of the files out of an archive (e.g. "cp -r" from the
// command line) and the files are accessed in the natural order, caching
// readers means that the overall time can be linear instead of quadratic.
//
// Each array element is a pair. The first half of the pair is a unique_ptr for
// the Reader. The second half of the pair is a uint64_t LRU priority value.
// Higher/lower values are more/less recently used and the release_reader
// function evicts the array element with the lowest LRU priority value.
struct Reader;
static std::pair<std::unique_ptr<Reader>, uint64_t>
    g_saved_readers[NUM_SAVED_READERS] = {};

// g_side_buffer_data and g_side_buffer_metadata combine to hold side buffers:
// statically allocated buffers used as a destination for decompressed bytes
// when Reader::advance_offset isn't a no-op. These buffers are roughly
// equivalent to Unix's /dev/null or Go's io.Discard as a first approximation.
// However, since we are already producing valid decompressed bytes, by saving
// them (and their metadata), we may be able to serve some subsequent my_read
// requests cheaply, without having to spin up another libarchive decompressor
// to walk forward from the start of the archive entry.
//
// In particular (https://crbug.com/1245925#c18), even when libfuse is single-
// threaded, we have seen kernel readahead causing the offset arguments in a
// sequence of my_read calls to sometimes arrive out-of-order, where
// conceptually consecutive reads are swapped. With side buffers, we can serve
// the second-to-arrive request by a cheap memcpy instead of an expensive
// "re-do decompression from the start". That side-buffer was filled by a
// Reader::advance_offset side-effect from serving the first-to-arrive request.
static uint8_t g_side_buffer_data[NUM_SIDE_BUFFERS][SIDE_BUFFER_SIZE] = {};
static struct side_buffer_metadata {
  int64_t index_within_archive;
  int64_t offset_within_entry;
  int64_t length;
  uint64_t lru_priority;

  static uint64_t next_lru_priority;

  bool contains(int64_t index_within_archive,
                int64_t offset_within_entry,
                uint64_t length) {
    if (this->index_within_archive >= 0 &&
        this->index_within_archive == index_within_archive &&
        this->offset_within_entry <= offset_within_entry) {
      const int64_t o = offset_within_entry - this->offset_within_entry;
      return this->length >= o && (this->length - o) >= length;
    }
    return false;
  }
} g_side_buffer_metadata[NUM_SIDE_BUFFERS] = {};
uint64_t side_buffer_metadata::next_lru_priority = 0;

// The side buffers are also repurposed as source (compressed) and destination
// (decompressed) buffers during the initial pass over the archive file.
#define SIDE_BUFFER_INDEX_COMPRESSED 0
#define SIDE_BUFFER_INDEX_DECOMPRESSED 1
#if NUM_SIDE_BUFFERS <= 1
#error "invalid NUM_SIDE_BUFFERS"
#endif

// ---- Libarchive Error Codes

// determine_passphrase_exit_code converts libarchive errors to fuse-archive
// exit codes. libarchive doesn't have designated passphrase-related error
// numbers. As for whether a particular archive file's encryption is supported,
// libarchive isn't consistent in archive_read_has_encrypted_entries returning
// ARCHIVE_READ_FORMAT_ENCRYPTION_UNSUPPORTED. Instead, we do a string
// comparison on the various possible error messages.
static int determine_passphrase_exit_code(const std::string_view e) {
  if (e.starts_with("Incorrect passphrase")) {
    return EXIT_CODE_PASSPHRASE_INCORRECT;
  }

  if (e.starts_with("Passphrase required")) {
    return EXIT_CODE_PASSPHRASE_REQUIRED;
  }

  static const std::string_view not_supported_prefixes[] = {
      "Crypto codec not supported",
      "Decryption is unsupported",
      "Encrypted file is unsupported",
      "Encryption is not supported",
      "RAR encryption support unavailable",
      "The archive header is encrypted, but currently not supported",
      "The file content is encrypted, but currently not supported",
      "Unsupported encryption format",
  };

  for (const std::string_view prefix : not_supported_prefixes) {
    if (e.starts_with(prefix)) {
      return EXIT_CODE_PASSPHRASE_NOT_SUPPORTED;
    }
  }

  return EXIT_CODE_INVALID_ARCHIVE_CONTENTS;
}

// ---- Logging

// redact replaces s with a placeholder string when the "--redact" command line
// option was given. This may prevent Personally Identifiable Information (PII)
// such as archive filenames or archive entry pathnames from being logged.
static const char* redact(const char* s) {
  return g_options.redact ? "(redacted)" : s;
}

static const char* redact(const std::string& s) {
  return redact(s.c_str());
}

// Temporarily suppresses the echo on the terminal.
// Used when waiting for password to be typed.
class SuppressEcho {
 public:
  explicit SuppressEcho() {
    if (tcgetattr(STDIN_FILENO, &tattr_) < 0) {
      return;
    }

    struct termios tattr = tattr_;
    tattr.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &tattr);
    reset_ = true;
  }

  ~SuppressEcho() {
    if (reset_) {
      tcsetattr(STDIN_FILENO, TCSAFLUSH, &tattr_);
    }
  }

  explicit operator bool() const { return reset_; }

 private:
  struct termios tattr_;
  bool reset_ = false;
};

const char* read_password_from_stdin(struct archive*, void* /*data*/) {
  if (password_count++) {
    return nullptr;
  }

  const SuppressEcho guard;
  if (guard) {
    std::cout << "Password > " << std::flush;
  }

  // Read password from standard input.
  if (!std::getline(std::cin, password)) {
    password.clear();
  }

  if (guard) {
    std::cout << "Got it!" << std::endl;
  }

  // Remove newline at the end of password.
  while (password.ends_with('\n')) {
    password.pop_back();
  }

  if (password.empty()) {
    syslog(LOG_DEBUG, "Got an empty password");
    return nullptr;
  }

  syslog(LOG_DEBUG, "Got a password of %zd bytes", password.size());
  return password.c_str();
}

// ---- Libarchive Read Callbacks

static uint32_t initialization_progress_out_of_1000000() {
  const int64_t m = g_archive_fd_position_hwm;
  const int64_t n = g_archive_file_size;

  if (m <= 0 || n <= 0) {
    return 0;
  }

  if (m >= n) {
    return 1000000;
  }

  return 1'000'000.0 * m / n;
}

static void update_g_archive_fd_position_hwm() {
  int64_t h = g_archive_fd_position_hwm;
  if (h < g_archive_fd_position_current) {
    g_archive_fd_position_hwm = g_archive_fd_position_current;
  }

  const auto period = std::chrono::seconds(1);
  static auto next = std::chrono::steady_clock::now() + period;
  const auto now = std::chrono::steady_clock::now();
  if (!g_options.quiet && (now >= next)) {
    next = now + period;
    const int percent = initialization_progress_out_of_1000000() / 10000;
    if (isatty(STDERR_FILENO)) {
      if (g_displayed_progress) {
        fprintf(stderr, "\e[F\e[K");
      }
      fprintf(stderr, "Loading %d%%\n", percent);
      fflush(stderr);
    } else {
      syslog(LOG_INFO, "Loading %d%%", percent);
    }
    g_displayed_progress = true;
  }
}

// The callbacks below are only used during start-up, for the initial pass
// through the archive to build the node tree, based on the g_archive_fd file
// descriptor that stays open for the lifetime of the process. They are like
// libarchive's built-in "read from a file" callbacks but also update
// g_archive_fd_position_etc. The callback_data arguments are ignored in favor
// of global variables.

static int my_file_close(struct archive* a, void* callback_data) {
  return ARCHIVE_OK;
}

static int my_file_open(struct archive* a, void* callback_data) {
  g_archive_fd_position_current = 0;
  g_archive_fd_position_hwm = 0;
  return ARCHIVE_OK;
}

static ssize_t my_file_read(struct archive* a,
                            void* callback_data,
                            const void** out_dst_ptr) {
  if (g_archive_fd < 0) {
    archive_set_error(a, EIO, "invalid g_archive_fd");
    return ARCHIVE_FATAL;
  }
  uint8_t* dst_ptr = &g_side_buffer_data[SIDE_BUFFER_INDEX_COMPRESSED][0];
  while (true) {
    const ssize_t n = read(g_archive_fd, dst_ptr, SIDE_BUFFER_SIZE);
    if (n >= 0) {
      g_archive_fd_position_current += n;
      update_g_archive_fd_position_hwm();
      *out_dst_ptr = dst_ptr;
      return n;
    }

    if (errno == EINTR) {
      continue;
    }

    archive_set_error(a, errno, "could not read archive file: %s",
                      strerror(errno));
    break;
  }
  return ARCHIVE_FATAL;
}

static int64_t my_file_seek(struct archive* a,
                            void* callback_data,
                            int64_t offset,
                            int whence) {
  if (g_archive_fd < 0) {
    archive_set_error(a, EIO, "invalid g_archive_fd");
    return ARCHIVE_FATAL;
  }

  int64_t o = lseek64(g_archive_fd, offset, whence);
  if (o >= 0) {
    g_archive_fd_position_current = o;
    update_g_archive_fd_position_hwm();
    return o;
  }

  archive_set_error(a, errno, "could not seek in archive file: %s",
                    strerror(errno));
  return ARCHIVE_FATAL;
}

static int64_t my_file_skip(struct archive* a,
                            void* callback_data,
                            int64_t delta) {
  if (g_archive_fd < 0) {
    archive_set_error(a, EIO, "invalid g_archive_fd");
    return ARCHIVE_FATAL;
  }

  const int64_t o0 = lseek64(g_archive_fd, 0, SEEK_CUR);
  const int64_t o1 = lseek64(g_archive_fd, delta, SEEK_CUR);
  if (o1 >= 0 && o0 >= 0) {
    g_archive_fd_position_current = o1;
    update_g_archive_fd_position_hwm();
    return o1 - o0;
  }

  archive_set_error(a, errno, "could not seek in archive file: %s",
                    strerror(errno));
  return ARCHIVE_FATAL;
}

static int my_file_switch(struct archive* a,
                          void* callback_data0,
                          void* callback_data1) {
  return ARCHIVE_OK;
}

static int my_archive_read_open(struct archive* a) {
  TRY(archive_read_set_callback_data(a, nullptr));
  TRY(archive_read_set_close_callback(a, my_file_close));
  TRY(archive_read_set_open_callback(a, my_file_open));
  TRY(archive_read_set_read_callback(a, my_file_read));
  TRY(archive_read_set_seek_callback(a, my_file_seek));
  TRY(archive_read_set_skip_callback(a, my_file_skip));
  TRY(archive_read_set_switch_callback(a, my_file_switch));
  return archive_read_open1(a);
}

// ---- Side Buffer

// acquire_side_buffer returns the index of the least recently used side
// buffer. This indexes g_side_buffer_data and g_side_buffer_metadata.
static int acquire_side_buffer() {
  // The preprocessor already checks "#elif NUM_SIDE_BUFFERS <= 0".
  int oldest_i = 0;
  uint64_t oldest_lru_priority = g_side_buffer_metadata[0].lru_priority;
  for (int i = 1; i < NUM_SIDE_BUFFERS; i++) {
    if (oldest_lru_priority > g_side_buffer_metadata[i].lru_priority) {
      oldest_lru_priority = g_side_buffer_metadata[i].lru_priority;
      oldest_i = i;
    }
  }
  g_side_buffer_metadata[oldest_i].index_within_archive = -1;
  g_side_buffer_metadata[oldest_i].offset_within_entry = -1;
  g_side_buffer_metadata[oldest_i].length = -1;
  g_side_buffer_metadata[oldest_i].lru_priority = UINT64_MAX;
  return oldest_i;
}

static bool read_from_side_buffer(int64_t index_within_archive,
                                  char* dst_ptr,
                                  size_t dst_len,
                                  int64_t offset_within_entry) {
  // Find the longest side buffer that contains (index_within_archive,
  // offset_within_entry, dst_len).
  int best_i = -1;
  int64_t best_length = -1;
  for (int i = 0; i < NUM_SIDE_BUFFERS; i++) {
    struct side_buffer_metadata* meta = &g_side_buffer_metadata[i];
    if (meta->length > best_length &&
        meta->contains(index_within_archive, offset_within_entry, dst_len)) {
      best_i = i;
      best_length = meta->length;
    }
  }

  if (best_i >= 0) {
    struct side_buffer_metadata* meta = &g_side_buffer_metadata[best_i];
    meta->lru_priority = ++side_buffer_metadata::next_lru_priority;
    int64_t o = offset_within_entry - meta->offset_within_entry;
    memcpy(dst_ptr, g_side_buffer_data[best_i] + o, dst_len);
    return true;
  }
  return false;
}

// ---- Reader

// Reader bundles libarchive concepts (an archive and an archive entry) and
// other state to point to a particular offset (in decompressed space) of a
// particular archive entry (identified by its index) in an archive.
//
// A Reader is backed by its own archive_read_open_filename call, managed by
// libarchive, so each can be positioned independently.
struct Reader {
  struct archive* archive;
  struct archive_entry* archive_entry;
  int64_t index_within_archive;
  int64_t offset_within_entry;

  Reader(struct archive* _archive)
      : archive(_archive),
        archive_entry(nullptr),
        index_within_archive(-1),
        offset_within_entry(0) {}

  ~Reader() {
    if (this->archive) {
      archive_read_free(this->archive);
    }
  }

  // advance_index walks forward until positioned at the want'th index. An
  // index identifies an archive entry. If this Reader wasn't already
  // positioned at that index, it also resets the Reader's offset to zero.
  //
  // It returns success (true) or failure (false).
  bool advance_index(int64_t want) {
    if (!this->archive) {
      return false;
    }

    while (this->index_within_archive < want) {
      const int status =
          archive_read_next_header(this->archive, &this->archive_entry);

      if (status == ARCHIVE_EOF) {
        syslog(LOG_ERR, "inconsistent archive %s", redact(g_archive_filename));
        return false;
      }

      if (status != ARCHIVE_OK && status != ARCHIVE_WARN) {
        syslog(LOG_ERR, "invalid archive %s: %s", redact(g_archive_filename),
               archive_error_string(this->archive));
        return false;
      }

      this->index_within_archive++;
      this->offset_within_entry = 0;
    }

    return true;
  }

  // advance_offset walks forward until positioned at the want'th offset. An
  // offset identifies a byte position relative to the start of an archive
  // entry's decompressed contents.
  //
  // The pathname is used for log messages.
  //
  // It returns success (true) or failure (false).
  bool advance_offset(int64_t want, const char* pathname) {
    if (!this->archive || !this->archive_entry) {
      return false;
    }

    if (want < this->offset_within_entry) {
      // We can't walk backwards.
      return false;
    }

    if (want == this->offset_within_entry) {
      // We are exactly where we want to be.
      return true;
    }

    // We are behind where we want to be. Advance (decompressing from the
    // archive entry into a side buffer) until we get there.
    const int sb = acquire_side_buffer();
    if (sb < 0 || NUM_SIDE_BUFFERS <= sb) {
      return false;
    }
    uint8_t* dst_ptr = g_side_buffer_data[sb];
    struct side_buffer_metadata* meta = &g_side_buffer_metadata[sb];
    while (want > this->offset_within_entry) {
      const int64_t original_owe = this->offset_within_entry;
      int64_t dst_len = want - original_owe;
      // If the amount we need to advance is greater than the SIDE_BUFFER_SIZE,
      // we need multiple this->read calls, but the total advance might not be
      // an exact multiple of SIDE_BUFFER_SIZE. Read that remainder amount
      // first, not last. For example, if advancing 260KiB with a 128KiB
      // SIDE_BUFFER_SIZE then read 4+128+128 instead of 128+128+4. This leaves
      // a full side buffer when we've finished advancing, maximizing later
      // requests' chances of side-buffer-as-cache hits.
      if (dst_len > SIDE_BUFFER_SIZE) {
        dst_len %= SIDE_BUFFER_SIZE;
        if (dst_len == 0) {
          dst_len = SIDE_BUFFER_SIZE;
        }
      }

      const ssize_t n = this->read(dst_ptr, dst_len, pathname);
      if (n < 0) {
        meta->index_within_archive = -1;
        meta->offset_within_entry = -1;
        meta->length = -1;
        meta->lru_priority = 0;
        return false;
      }

      meta->index_within_archive = this->index_within_archive;
      meta->offset_within_entry = original_owe;
      meta->length = n;
      meta->lru_priority = ++side_buffer_metadata::next_lru_priority;
    }

    return true;
  }

  // read copies from the archive entry's decompressed contents to the
  // destination buffer. It also advances the Reader's offset_within_entry.
  //
  // The pathname is used for log messages.
  ssize_t read(void* dst_ptr, size_t dst_len, const char* pathname) {
    const ssize_t n = archive_read_data(this->archive, dst_ptr, dst_len);
    if (n < 0) {
      syslog(LOG_ERR, "could not serve %s from %s: %s", redact(pathname),
             redact(g_archive_filename), archive_error_string(this->archive));
      return -EIO;
    }

    if (n > dst_len) {
      syslog(LOG_ERR, "too much data serving %s from %s", redact(pathname),
             redact(g_archive_filename));
      // Something has gone wrong, possibly a buffer overflow, so abort.
      abort();
    }

    this->offset_within_entry += n;
    return n;
  }
};

// swap swaps fields of two readers.
static void swap(Reader& a, Reader& b) {
  std::swap(a.archive, b.archive);
  std::swap(a.archive_entry, b.archive_entry);
  std::swap(a.index_within_archive, b.index_within_archive);
  std::swap(a.offset_within_entry, b.offset_within_entry);
}

// acquire_reader returns a Reader positioned at the start (offset == 0) of the
// given index'th entry of the archive.
static std::unique_ptr<Reader> acquire_reader(
    int64_t want_index_within_archive) {
  if (want_index_within_archive < 0) {
    syslog(LOG_ERR, "negative index_within_archive");
    return nullptr;
  }

  int best_i = -1;
  int64_t best_index_within_archive = -1;
  int64_t best_offset_within_entry = -1;
  for (int i = 0; i < NUM_SAVED_READERS; i++) {
    const Reader* const sri = g_saved_readers[i].first.get();
    if (sri &&
        std::pair(best_index_within_archive, best_offset_within_entry) <
            std::pair(sri->index_within_archive, sri->offset_within_entry) &&
        std::pair(sri->index_within_archive, sri->offset_within_entry) <=
            std::pair(want_index_within_archive, int64_t(0))) {
      best_i = i;
      best_index_within_archive = sri->index_within_archive;
      best_offset_within_entry = sri->offset_within_entry;
    }
  }

  std::unique_ptr<Reader> r;
  if (best_i >= 0) {
    r = std::move(g_saved_readers[best_i].first);
    g_saved_readers[best_i].second = 0;
  } else {
    struct archive* const a = archive_read_new();
    if (!a) {
      syslog(LOG_ERR, "out of memory");
      return nullptr;
    }

    if (!password.empty()) {
      archive_read_add_passphrase(a, password.c_str());
    }

    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    archive_read_support_format_raw(a);
    if (archive_read_open_filename(a, g_archive_realpath, BLOCK_SIZE) !=
        ARCHIVE_OK) {
      syslog(LOG_ERR, "could not read %s: %s", redact(g_archive_filename),
             archive_error_string(a));
      archive_read_free(a);
      return nullptr;
    }
    r = std::make_unique<Reader>(a);
  }

  if (!r->advance_index(want_index_within_archive)) {
    return nullptr;
  }

  return r;
}

// release_reader returns r to the reader cache.
static void release_reader(std::unique_ptr<Reader> r) {
  if (NUM_SAVED_READERS <= 0) {
    return;
  }
  int oldest_i = 0;
  uint64_t oldest_lru_priority = g_saved_readers[0].second;
  for (int i = 1; i < NUM_SAVED_READERS; i++) {
    if (oldest_lru_priority > g_saved_readers[i].second) {
      oldest_lru_priority = g_saved_readers[i].second;
      oldest_i = i;
    }
  }
  static uint64_t next_lru_priority = 0;
  g_saved_readers[oldest_i].first = std::move(r);
  g_saved_readers[oldest_i].second = ++next_lru_priority;
}

// ---- In-Memory Directory Tree

struct Node {
  std::string rel_name;  // Relative (not absolute) pathname.
  std::string symlink;
  int64_t index_within_archive;
  int64_t size;
  time_t mtime;
  mode_t mode;

  Node* parent = nullptr;
  Node* last_child = nullptr;
  Node* first_child = nullptr;
  Node* next_sibling = nullptr;

  Node(std::string&& _rel_name,
       std::string&& _symlink,
       int64_t _index_within_archive,
       int64_t _size,
       time_t _mtime,
       mode_t _mode)
      : rel_name(std::move(_rel_name)),
        symlink(std::move(_symlink)),
        index_within_archive(_index_within_archive),
        size(_size),
        mtime(_mtime),
        mode(_mode) {}

  Node(const Node&) = delete;

  bool is_dir() const { return S_ISDIR(mode); }

  void add_child(Node* n) {
    assert(n);
    assert(!n->parent);
    assert(is_dir());
    // Count one "block" for each directory entry.
    size += block_size;
    n->parent = this;
    if (last_child == nullptr) {
      last_child = n;
      first_child = n;
    } else {
      last_child->next_sibling = n;
      last_child = n;
    }
  }

  int64_t get_block_count() const {
    return (size + (block_size - 1)) / block_size;
  }

  struct stat get_stat() const {
    struct stat z = {};
    z.st_mode = mode;
    z.st_nlink = 1;
    z.st_uid = g_uid;
    z.st_gid = g_gid;
    z.st_size = size;
    z.st_mtime = mtime;
    z.st_blksize = block_size;
    z.st_blocks = get_block_count();
    return z;
  }
};

// valid_pathname returns whether the C string p is neither "", "./" or "/"
// and, when splitting on '/' into pathname fragments, no fragment is "", "."
// or ".." other than a possibly leading "" or "." fragment when p starts with
// "/" or "./".
//
// If allow_slashes is false then p must not contain "/".
//
// When iterating over fragments, the p pointer does not move but the q and r
// pointers bracket each fragment:
//   "/an/example/pathname"
//    pq-r|      ||       |
//    p   q------r|       |
//    p           q-------r
static bool valid_pathname(const char* const p, bool allow_slashes) {
  if (!p) {
    return false;
  }
  const char* q = p;
  if (q[0] == '.' && q[1] == '/') {
    if (!allow_slashes) {
      return false;
    }
    q += 2;
  } else if (*q == '/') {
    if (!allow_slashes) {
      return false;
    }
    q++;
  }
  if (*q == 0) {
    return false;
  }
  while (true) {
    const char* r = q;
    while (*r != 0) {
      if (*r != '/') {
        r++;
      } else if (!allow_slashes) {
        return false;
      } else {
        break;
      }
    }
    size_t len = r - q;
    if (len == 0) {
      return false;
    } else if (len == 1 && q[0] == '.') {
      return false;
    } else if (len == 2 && q[0] == '.' && q[1] == '.') {
      return false;
    }
    if (*r == 0) {
      break;
    }
    q = r + 1;
  }
  return true;
}

// normalize_pathname validates and returns e's pathname, prepending a leading
// "/" if it didn't already have one.
static std::string normalize_pathname(struct archive_entry* e) {
  const char* s = archive_entry_pathname_utf8(e);
  if (!s) {
    s = archive_entry_pathname(e);
    if (!s) {
      syslog(LOG_ERR, "archive entry in %s has empty pathname",
             redact(g_archive_filename));
      return "";
    }
  }

  // For 'raw' archives, libarchive defaults to "data" when the compression
  // file format doesn't contain the original file's name. For fuse-archive, we
  // use the archive filename's innername instead. Given an archive filename of
  // "/foo/bar.txt.bz2", the sole file within will be served as "bar.txt".
  if (g_archive_is_raw && g_archive_innername &&
      (g_archive_innername[0] != '\x00') && (strcmp(s, "data") == 0)) {
    s = g_archive_innername;
  }

  if (!valid_pathname(s, true)) {
    syslog(LOG_ERR, "archive entry in %s has invalid pathname: %s",
           redact(g_archive_filename), redact(s));
    return "";
  }
  if (s[0] == '.' && s[1] == '/') {
    return std::string(s + 1);
  } else if (*s == '/') {
    return std::string(s);
  }
  return std::string("/") + std::string(s);
}

static int insert_leaf_node(std::string&& pathname,
                            std::string&& symlink,
                            int64_t index_within_archive,
                            int64_t size,
                            time_t mtime,
                            mode_t mode) {
  if (index_within_archive < 0) {
    syslog(LOG_ERR, "negative index_within_archive in %s: %s",
           redact(g_archive_filename), redact(pathname));
    return -EIO;
  }

  Node* parent = g_root_node;

  const mode_t rx_bits = mode & 0555;
  const mode_t r_bits = rx_bits & 0444;
  const mode_t branch_mode = rx_bits | (r_bits >> 2) | S_IFDIR;
  const mode_t leaf_mode = rx_bits | (symlink.empty() ? S_IFREG : S_IFLNK);

  // p, q and r point to pathname fragments per the valid_pathname comment.
  const char* p = pathname.c_str();
  if (*p == 0 || *p != '/') {
    return 0;
  }

  const char* q = p + 1;
  while (true) {
    // A directory's mtime is the oldest of its leaves' mtimes.
    if (parent->mtime < mtime) {
      parent->mtime = mtime;
    }
    parent->mode |= branch_mode;

    const char* r = q;
    while (*r != 0 && *r != '/') {
      r++;
    }

    std::string abs_pathname(p, r - p);
    std::string rel_pathname(q, r - q);
    if (*r == 0) {
      // Insert an explicit leaf node (a regular file).
      Node* const n = new Node(std::move(rel_pathname), std::move(symlink),
                               index_within_archive, size, mtime, leaf_mode);

      // Add to g_nodes_by_name.
      const auto [_, ok] =
          g_nodes_by_name.try_emplace(std::move(abs_pathname), n);
      if (!ok) {
        syslog(LOG_WARNING, "name collision: %s", redact(abs_pathname));
        delete n;
        return 0;
      }

      parent->add_child(n);
      g_block_count += n->get_block_count();
      g_block_count += 1;

      // Add to g_nodes_by_index.
      assert(g_nodes_by_index.size() <= index_within_archive);
      g_nodes_by_index.resize(index_within_archive);
      g_nodes_by_index.push_back(n);
      break;
    }
    q = r + 1;

    // Insert an implicit branch node (a directory).
    Node*& n = g_nodes_by_name[abs_pathname];
    if (n) {
      if (!n->is_dir()) {
        syslog(LOG_WARNING, "name collision: %s", redact(abs_pathname));
        return 0;
      }
    } else {
      n = new Node(std::move(rel_pathname), "", -1, 0, mtime, branch_mode);
      parent->add_child(n);
      g_block_count += 1;
    }

    parent = n;
  }

  return 0;
}

static int insert_leaf(struct archive* a,
                       struct archive_entry* e,
                       int64_t index_within_archive) {
  std::string pathname = normalize_pathname(e);
  if (pathname.empty()) {
    // normalize_pathname already printed a log message.
    return 0;
  }

  std::string symlink;
  const mode_t mode = archive_entry_mode(e);

  if (S_ISBLK(mode) || S_ISCHR(mode) || S_ISFIFO(mode) || S_ISSOCK(mode)) {
    syslog(LOG_ERR, "irregular file type in %s: %s", redact(g_archive_filename),
           redact(pathname));
    return 0;
  }

  if (S_ISLNK(mode)) {
    const char* s = archive_entry_symlink_utf8(e);
    if (!s) {
      s = archive_entry_symlink(e);
    }
    if (s) {
      symlink = std::string(s);
    }
    if (symlink.empty()) {
      syslog(LOG_ERR, "empty link in %s: %s", redact(g_archive_filename),
             redact(pathname));
      return 0;
    }
  }

  int64_t size = archive_entry_size(e);
  // 'Raw' archives don't always explicitly record the decompressed size. We'll
  // have to decompress it to find out. Some 'cooked' archives also don't
  // explicitly record this (at the time archive_read_next_header returns). See
  // https://github.com/libarchive/libarchive/issues/1764
  if (!archive_entry_size_is_set(e)) {
    while (true) {
      const ssize_t n = archive_read_data(
          a, g_side_buffer_data[SIDE_BUFFER_INDEX_DECOMPRESSED],
          SIDE_BUFFER_SIZE);
      if (n == 0) {
        break;
      }

      if (n < 0) {
        syslog(LOG_ERR, "could not decompress %s: %s",
               redact(g_archive_filename), archive_error_string(a));
        return -EIO;
      }

      if (n > SIDE_BUFFER_SIZE) {
        syslog(LOG_ERR, "too much data decompressing %s",
               redact(g_archive_filename));
        // Something has gone wrong, possibly a buffer overflow, so abort.
        abort();
      }

      size += n;
    }
  }

  return insert_leaf_node(std::move(pathname), std::move(symlink),
                          index_within_archive, size, archive_entry_mtime(e),
                          mode);
}

static int build_tree() {
  if (g_initialize_index_within_archive < 0) {
    return -EIO;
  }
  bool first = true;
  while (true) {
    if (first) {
      // The entry was already read by pre_initialize.
      first = false;
    } else {
      int status = archive_read_next_header(g_initialize_archive,
                                            &g_initialize_archive_entry);
      g_initialize_index_within_archive++;
      if (status == ARCHIVE_EOF) {
        break;
      }

      if (status == ARCHIVE_WARN) {
        syslog(LOG_ERR, "libarchive warning for %s: %s",
               redact(g_archive_filename),
               archive_error_string(g_initialize_archive));
      } else if (status != ARCHIVE_OK) {
        syslog(LOG_ERR, "invalid archive %s: %s", redact(g_archive_filename),
               archive_error_string(g_initialize_archive));
        return -EIO;
      }
    }

    if (S_ISDIR(archive_entry_mode(g_initialize_archive_entry))) {
      continue;
    }

    TRY(insert_leaf(g_initialize_archive, g_initialize_archive_entry,
                    g_initialize_index_within_archive));
  }
  return 0;
}

// ---- Lazy Initialization

// This section (pre_initialize and post_initialize_etc) are the "two parts"
// described in the "Building is split into two parts" comment above.

static void insert_root_node() {
  g_root_node = new Node("", "", -1, 0, 0, S_IFDIR);
  g_nodes_by_name["/"] = g_root_node;
}

static int pre_initialize() {
  if (!g_archive_filename) {
    syslog(LOG_ERR, "missing archive_filename argument");
    return EXIT_CODE_GENERIC_FAILURE;
  }

  g_archive_realpath = realpath(g_archive_filename, nullptr);
  if (!g_archive_realpath) {
    syslog(LOG_ERR, "could not get absolute path of %s: %m",
           redact(g_archive_filename));
    return EXIT_CODE_CANNOT_OPEN_ARCHIVE;
  }

  g_archive_fd = open(g_archive_realpath, O_RDONLY);
  if (g_archive_fd < 0) {
    syslog(LOG_ERR, "could not open %s: %m", redact(g_archive_filename));
    return EXIT_CODE_CANNOT_OPEN_ARCHIVE;
  }

  struct stat z;
  if (fstat(g_archive_fd, &z) != 0) {
    syslog(LOG_ERR, "could not stat %s", redact(g_archive_filename));
    return EXIT_CODE_GENERIC_FAILURE;
  }
  g_archive_file_size = z.st_size;

  g_initialize_archive = archive_read_new();
  if (!g_initialize_archive) {
    syslog(LOG_ERR, "out of memory");
    return EXIT_CODE_GENERIC_FAILURE;
  }

  archive_read_set_passphrase_callback(g_initialize_archive, nullptr,
                                       &read_password_from_stdin);
  archive_read_support_filter_all(g_initialize_archive);
  archive_read_support_format_all(g_initialize_archive);
  archive_read_support_format_raw(g_initialize_archive);
  if (my_archive_read_open(g_initialize_archive) != ARCHIVE_OK) {
    syslog(LOG_ERR, "could not open %s: %s", redact(g_archive_filename),
           archive_error_string(g_initialize_archive));
    archive_read_free(g_initialize_archive);
    g_initialize_archive = nullptr;
    g_initialize_archive_entry = nullptr;
    g_initialize_index_within_archive = -1;
    return EXIT_CODE_GENERIC_FAILURE;
  }

  while (true) {
    int status = archive_read_next_header(g_initialize_archive,
                                          &g_initialize_archive_entry);
    g_initialize_index_within_archive++;
    if (status == ARCHIVE_WARN) {
      syslog(LOG_ERR, "libarchive warning for %s: %s",
             redact(g_archive_filename),
             archive_error_string(g_initialize_archive));
    } else if (status != ARCHIVE_OK) {
      if (status != ARCHIVE_EOF) {
        syslog(LOG_ERR, "invalid archive %s: %s", redact(g_archive_filename),
               archive_error_string(g_initialize_archive));
      }
      archive_read_free(g_initialize_archive);
      g_initialize_archive = nullptr;
      g_initialize_archive_entry = nullptr;
      g_initialize_index_within_archive = -1;
      if (status != ARCHIVE_EOF) {
        return EXIT_CODE_INVALID_ARCHIVE_HEADER;
      }
      // Building the tree for an empty archive is trivial.
      insert_root_node();
      return 0;
    }

    if (S_ISDIR(archive_entry_mode(g_initialize_archive_entry))) {
      continue;
    }
    break;
  }

  // For 'raw' archives, check that at least one of the compression filters
  // (e.g. bzip2, gzip) actually triggered. We don't want to mount arbitrary
  // data (e.g. foo.jpeg).
  if (archive_format(g_initialize_archive) == ARCHIVE_FORMAT_RAW) {
    g_archive_is_raw = true;
    const int n = archive_filter_count(g_initialize_archive);
    for (int i = 0; true; i++) {
      if (i == n) {
        archive_read_free(g_initialize_archive);
        g_initialize_archive = nullptr;
        g_initialize_archive_entry = nullptr;
        g_initialize_index_within_archive = -1;
        syslog(LOG_ERR, "invalid raw archive: %s", redact(g_archive_filename));
        return EXIT_CODE_INVALID_RAW_ARCHIVE;
      }

      if (archive_filter_code(g_initialize_archive, i) != ARCHIVE_FILTER_NONE) {
        break;
      }
    }
  } else {
    // Otherwise, reading the first byte of the first non-directory entry will
    // reveal whether we also need a passphrase.
    ssize_t n = archive_read_data(
        g_initialize_archive,
        g_side_buffer_data[SIDE_BUFFER_INDEX_DECOMPRESSED], 1);
    if (n < 0) {
      const char* const e = archive_error_string(g_initialize_archive);
      syslog(LOG_ERR, "%s: %s", redact(g_archive_filename), e);
      const int ret = determine_passphrase_exit_code(e);
      archive_read_free(g_initialize_archive);
      g_initialize_archive = nullptr;
      g_initialize_archive_entry = nullptr;
      g_initialize_index_within_archive = -1;
      return ret;
    }
  }

  return 0;
}

static int post_initialize_sync() {
  if (g_root_node) {
    return 0;
  }

  insert_root_node();
  const int error = build_tree();
  archive_read_free(g_initialize_archive);
  g_initialize_archive = nullptr;
  g_initialize_archive_entry = nullptr;
  g_initialize_index_within_archive = -1;
  if (g_archive_fd >= 0) {
    close(g_archive_fd);
    g_archive_fd = -1;
  }

  if (g_displayed_progress && error == 0) {
    if (isatty(STDERR_FILENO)) {
      fprintf(stderr, "\e[F\e[K");
      fflush(stderr);
    } else {
      syslog(LOG_INFO, "Loaded 100%%");
    }
  }

  return error;
}

// ---- FUSE Callbacks

static int my_getattr(const char* pathname, struct stat* z) {
  const auto it = g_nodes_by_name.find(pathname);
  if (it == g_nodes_by_name.end()) {
    return -ENOENT;
  }

  *z = it->second->get_stat();
  return 0;
}

static int my_readlink(const char* pathname, char* dst_ptr, size_t dst_len) {
  const auto it = g_nodes_by_name.find(pathname);
  if (it == g_nodes_by_name.end()) {
    return -ENOENT;
  }

  const Node* const n = it->second;
  assert(n);
  if (n->symlink.empty() || dst_len == 0) {
    return -ENOLINK;
  }

  snprintf(dst_ptr, dst_len, "%s", n->symlink.c_str());
  return 0;
}

static int my_open(const char* pathname, struct fuse_file_info* ffi) {
  const auto it = g_nodes_by_name.find(pathname);
  if (it == g_nodes_by_name.end()) {
    return -ENOENT;
  }

  const Node* const n = it->second;
  assert(n);
  if (S_ISDIR(n->mode)) {
    return -EISDIR;
  }

  if (n->index_within_archive < 0 || !ffi) {
    return -EIO;
  }

  if ((ffi->flags & O_ACCMODE) != O_RDONLY) {
    return -EACCES;
  }

  std::unique_ptr<Reader> ur = acquire_reader(n->index_within_archive);
  if (!ur) {
    return -EIO;
  }

  ffi->keep_cache = 1;

  static_assert(sizeof(ffi->fh) >= sizeof(Reader*));
  ffi->fh = reinterpret_cast<uintptr_t>(ur.release());
  return 0;
}

static int my_read(const char* pathname,
                   char* dst_ptr,
                   size_t dst_len,
                   off_t offset,
                   struct fuse_file_info* ffi) {
  if (offset < 0 || dst_len > INT_MAX) {
    return -EINVAL;
  }

  Reader* const r = reinterpret_cast<Reader*>(ffi->fh);
  if (!r || !r->archive || !r->archive_entry) {
    return -EIO;
  }

  const uint64_t i = r->index_within_archive;
  if (i >= g_nodes_by_index.size()) {
    return -EIO;
  }

  const Node* const n = g_nodes_by_index[i];
  if (!n) {
    return -EIO;
  }

  const int64_t size = n->size;
  if (size < 0) {
    return -EIO;
  }

  if (size <= offset) {
    return 0;
  }

  const uint64_t remaining = size - offset;
  if (dst_len > remaining) {
    dst_len = remaining;
  }

  if (dst_len == 0) {
    return 0;
  }

  if (read_from_side_buffer(r->index_within_archive, dst_ptr, dst_len,
                            offset)) {
    return dst_len;
  }

  // libarchive is designed for streaming access, not random access. If we
  // need to seek backwards, there's more work to do.
  if (offset < r->offset_within_entry) {
    // Acquire a new Reader, swap it with r and release the new Reader. We
    // swap (modify r in-place) instead of updating ffi->fh to point to the
    // new Reader, because libfuse ignores any changes to the ffi->fh value
    // after this function returns (this function is not an 'open' callback).
    std::unique_ptr<Reader> ur = acquire_reader(r->index_within_archive);
    if (!ur || !ur->archive || !ur->archive_entry) {
      return -EIO;
    }
    swap(*r, *ur);
    release_reader(std::move(ur));
  }

  if (!r->advance_offset(offset, pathname)) {
    return -EIO;
  }

  return r->read(dst_ptr, dst_len, pathname);
}

static int my_release(const char* pathname, struct fuse_file_info* ffi) {
  Reader* const r = reinterpret_cast<Reader*>(ffi->fh);
  if (!r) {
    return -EIO;
  }
  release_reader(std::unique_ptr<Reader>(r));
  return 0;
}

static int my_readdir(const char* pathname,
                      void* buf,
                      fuse_fill_dir_t filler,
                      off_t offset,
                      struct fuse_file_info* ffi) {
  const auto iter = g_nodes_by_name.find(pathname);
  if (iter == g_nodes_by_name.end()) {
    return -ENOENT;
  }

  Node* const n = iter->second;
  if (!S_ISDIR(n->mode)) {
    return -ENOTDIR;
  }

  if (filler(buf, ".", nullptr, 0) || filler(buf, "..", nullptr, 0)) {
    return -ENOMEM;
  }

  for (Node* p = n->first_child; p; p = p->next_sibling) {
    const struct stat z = p->get_stat();
    if (filler(buf, p->rel_name.c_str(), &z, 0)) {
      return -ENOMEM;
    }
  }

  return 0;
}

static int my_statfs([[maybe_unused]] const char* const path,
                     struct statvfs* const st) {
  st->f_bsize = block_size;
  st->f_frsize = block_size;
  st->f_blocks = g_block_count;
  st->f_bfree = 0;
  st->f_bavail = 0;
  st->f_files = g_nodes_by_name.size();
  st->f_ffree = 0;
  st->f_favail = 0;
  st->f_flag = ST_RDONLY;
  st->f_namemax = NAME_MAX;
  return 0;
}

static void* my_init(struct fuse_conn_info* conn) {
  return nullptr;
}

static void my_destroy(void* arg) {
  assert(!arg);
}

static const struct fuse_operations my_operations = {
    .getattr = my_getattr,
    .readlink = my_readlink,
    .open = my_open,
    .read = my_read,
    .statfs = my_statfs,
    .release = my_release,
    .readdir = my_readdir,
    .init = my_init,
    .destroy = my_destroy,
};

// ---- Main

// innername returns the "bar.ext0" from "/foo/bar.ext0.ext1".
const char* innername(const char* filename) {
  if (!filename) {
    return nullptr;
  }
  const char* last_slash = strrchr(filename, '/');
  if (last_slash) {
    filename = last_slash + 1;
  }
  const char* last_dot = strrchr(filename, '.');
  if (last_dot) {
    return strndup(filename, last_dot - filename);
  }
  return strdup(filename);
}

static int my_opt_proc(void* /*private_data*/,
                       const char* arg,
                       int key,
                       struct fuse_args* /*out_args*/) {
  constexpr int KEEP = 1;
  constexpr int DISCARD = 0;
  constexpr int ERROR = -1;

  switch (key) {
    case FUSE_OPT_KEY_NONOPT:
      switch (++g_options.arg_count) {
        case 1:
          g_archive_filename = arg;
          g_archive_innername = innername(arg);
          return DISCARD;

        case 2:
          g_mount_point = arg;
          return KEEP;

        default:
          fprintf(stderr,
                  "%s: only two arguments allowed: filename and mountpoint\n",
                  PROGRAM_NAME);
          return ERROR;
      }

    case KEY_HELP:
      g_options.help = true;
      return DISCARD;

    case KEY_VERSION:
      g_options.version = true;
      return DISCARD;

    case KEY_QUIET:
      setlogmask(LOG_UPTO(LOG_ERR));
      g_options.quiet = true;
      return DISCARD;

    case KEY_VERBOSE:
      setlogmask(LOG_UPTO(LOG_DEBUG));
      return DISCARD;

    case KEY_REDACT:
      g_options.redact = true;
      return DISCARD;
  }

  return KEEP;
}

static int ensure_utf_8_encoding() {
  // libarchive (especially for reading 7z) has locale-dependent behavior.
  // Non-ASCII pathnames can trigger "Pathname cannot be converted from
  // UTF-16LE to current locale" warnings from archive_read_next_header and
  // archive_entry_pathname_utf8 subsequently returning nullptr.
  //
  // Calling setlocale to enforce a UTF-8 encoding can avoid that. Try various
  // arguments and pick the first one that is supported and produces UTF-8.
  static const char* const locales[] = {
      // As of 2021, many systems (including Debian) support "C.UTF-8".
      "C.UTF-8",
      // However, "C.UTF-8" is not a POSIX standard and glibc 2.34 (released
      // 2021-08-01) does not support it. It may come to glibc 2.35 (see the
      // sourceware.org commit link below), but until then and on older
      // systems, try the popular "en_US.UTF-8".
      //
      // https://sourceware.org/git/?p=glibc.git;a=commit;h=466f2be6c08070e9113ae2fdc7acd5d8828cba50
      "en_US.UTF-8",
      // As a final fallback, an empty string means to use the relevant
      // environment variables (LANG, LC_ALL, etc).
      "",
  };

  for (const char* const locale : locales) {
    if (setlocale(LC_ALL, locale) &&
        (strcmp("UTF-8", nl_langinfo(CODESET)) == 0)) {
      return 0;
    }
  }

  syslog(LOG_ERR, "could not ensure UTF-8 encoding");
  return EXIT_CODE_GENERIC_FAILURE;
}

// Removes directory `mount_point` in destructor.
struct Cleanup {
  const int dirfd = open(".", O_DIRECTORY | O_PATH);
  fuse_args* args = nullptr;
  std::string mount_point;

  ~Cleanup() {
    if (!mount_point.empty()) {
      if (unlinkat(dirfd, mount_point.c_str(), AT_REMOVEDIR) == 0) {
        syslog(LOG_DEBUG, "Removed mount point %s", redact(mount_point));
      } else {
        syslog(LOG_ERR, "Cannot remove mount point %s: %s", redact(mount_point),
               strerror(errno));
      }
    }

    if (args) {
      fuse_opt_free_args(args);
    }

    if (close(dirfd) < 0) {
      syslog(LOG_ERR, "Cannot close file descriptor: %s", strerror(errno));
    }
  }
};

int main(int argc, char** argv) {
  openlog(PROGRAM_NAME, LOG_PERROR, LOG_USER);
  setlogmask(LOG_UPTO(LOG_INFO));

  // Initialize side buffers as invalid.
  for (int i = 0; i < NUM_SIDE_BUFFERS; i++) {
    g_side_buffer_metadata[i].index_within_archive = -1;
    g_side_buffer_metadata[i].offset_within_entry = -1;
    g_side_buffer_metadata[i].length = -1;
    g_side_buffer_metadata[i].lru_priority = 0;
  }

  TRY_EXIT_CODE(ensure_utf_8_encoding());

  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
  Cleanup cleanup{.args = &args};

  if (argc <= 0 || !argv) {
    syslog(LOG_ERR, "missing command line arguments");
    return EXIT_CODE_GENERIC_FAILURE;
  }

  if (fuse_opt_parse(&args, &g_options, g_fuse_opts, &my_opt_proc) < 0) {
    syslog(LOG_ERR, "could not parse command line arguments");
    return EXIT_CODE_GENERIC_FAILURE;
  }

  // Force single-threading. It's simpler.
  //
  // For example, there may be complications about acquiring an unused side
  // buffer if NUM_SIDE_BUFFERS is less than the number of threads.
  fuse_opt_add_arg(&args, "-s");

  // Mount read-only.
  fuse_opt_add_arg(&args, "-o");
  fuse_opt_add_arg(&args, "ro");

  if (g_options.help) {
    fprintf(stderr,
            R"(usage: %s [options] <archive_file> [mount_point]

general options:
    -o opt,[opt...]        mount options
    -h   --help            print help
    -V   --version         print version

%s options:
    -q   --quiet           do not print progress messages
    -v   --verbose         print more log messages
         --redact          redact pathnames from log messages
         -o redact         ditto

)",
            PROGRAM_NAME, PROGRAM_NAME);
    fuse_opt_add_arg(&args, "-ho");  // I think ho means "help output".
    fuse_main(args.argc, args.argv, &my_operations, nullptr);
    return EXIT_SUCCESS;
  }

  if (g_options.version) {
    fprintf(stderr, PROGRAM_NAME " version: %s\n", FUSE_ARCHIVE_VERSION);
    fuse_opt_add_arg(&args, "--version");
    fuse_main(args.argc, args.argv, &my_operations, nullptr);
    return EXIT_SUCCESS;
  }

  g_uid = getuid();
  g_gid = getgid();
  TRY_EXIT_CODE(pre_initialize());

  if (!g_mount_point.empty()) {
    // Try to create the mount point directory if it doesn't exist.
    if (mkdirat(cleanup.dirfd, g_mount_point.c_str(), 0777) == 0) {
      syslog(LOG_DEBUG, "Created mount point %s", redact(g_mount_point));
      cleanup.mount_point = g_mount_point;
    } else if (errno == EEXIST) {
      syslog(LOG_DEBUG, "Mount point %s already exists", redact(g_mount_point));
    } else {
      syslog(LOG_ERR, "Cannot create mount point %s: %s", redact(g_mount_point),
             strerror(errno));
    }
  } else {
    g_mount_point = g_archive_innername;
    const auto n = g_mount_point.size();

    for (int i = 0;;) {
      if (mkdirat(cleanup.dirfd, g_mount_point.c_str(), 0777) == 0) {
        syslog(LOG_INFO, "Created mount point %s", redact(g_mount_point));
        cleanup.mount_point = g_mount_point;
        fuse_opt_add_arg(&args, g_mount_point.c_str());
        break;
      }

      if (errno != EEXIST) {
        syslog(LOG_ERR, "Cannot create mount point %s: %s",
               redact(g_mount_point), strerror(errno));
        return EXIT_FAILURE;
      }

      syslog(LOG_DEBUG, "Mount point %s already exists", redact(g_mount_point));
      g_mount_point.resize(n);
      g_mount_point += " (";
      g_mount_point += std::to_string(++i);
      g_mount_point += ")";
    }
  }

  TRY_EXIT_CODE(post_initialize_sync());

  return fuse_main(args.argc, args.argv, &my_operations, nullptr);
}
