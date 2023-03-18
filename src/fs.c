// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "llvm/llvm.h"
#include "path.h"

#include <fcntl.h> // open
#include <err.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h> // close
#include <sys/stat.h>
#include <sys/mman.h> // mmap


err_t mmap_file(const char* filename, mem_t* data_out) {
  int fd = open(filename, O_RDONLY);
  if (fd < 0)
    return err_errno();

  struct stat st;
  if (fstat(fd, &st) != 0) {
    err_t err = err_errno();
    close(fd);
    return err;
  }

  void* p = mmap(0, (usize)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (p == MAP_FAILED)
    return ErrNoMem;

  data_out->p = p;
  data_out->size = (usize)st.st_size;
  return 0;
}


err_t mmap_unmap(mem_t m) {
  if (munmap(m.p, m.size) == 0)
    return 0;
  return err_errno();
}


err_t writefile(const char* filename, u32 mode, slice_t data) {
  if (data.len > (usize)ISIZE_MAX)
    return ErrOverflow;
  int fd = open(filename, O_WRONLY|O_CREAT|O_TRUNC, mode);
  if (fd < 0)
    return err_errno();
  err_t err = 0;
  while (data.len) {
    isize n = write(fd, data.p, data.len);
    if (n < (isize)data.len) {
      err = n < 0 ? err_errno() : ErrCanceled;
      break;
    }
    data.p += n;
    data.len -= (usize)n;
  }
  close(fd);
  return err;
}


err_t fs_touch(const char* filename, u32 mode) {
  dlog("%s '%s' 0%o", __FUNCTION__, filename, mode);
  int fd = open(filename, O_WRONLY|O_TRUNC|O_CREAT, (mode_t)mode);
  if (fd > -1) {
    close(fd);
    return 0;
  }

  if (errno == EEXIST) {
    // note: intentionally don't chmod(filename, mode) here.
    struct timespec timebuf[2]; // atime, mtime
    timebuf[0].tv_nsec = UTIME_NOW;
    timebuf[1].tv_nsec = UTIME_NOW;
    if (utimensat(AT_FDCWD, filename, timebuf, 0) == 0)
      return 0;
  }

  err_t err = err_errno();
  vlog("failed to create file '%s': %s", filename, err_str(err));
  return err;
}


static err_t _fs_mkdirs(const char* path, int perms, bool verbose) {
  // copy path into mutable storage
  usize len = strlen(path);
  if (len == 0) return ErrInvalid;
  char* buf = alloca(len + 1);
  memcpy(buf, path, len + 1);

  char* s = buf + len;
  char* end;
  struct stat st;

  // trim away trailing separators, e.g. "/a/b//" => "/a/b"
  while (*--s == PATH_SEPARATOR) {
    if (s == buf)
      return 0; // path is "/"
  }
  if (s == buf && *buf == '.')
    return 0; // path is "."
  s[1] = 0;
  end = s;

  // stat from leaf to root, e.g "/a/b/c", "/a/b", "/a"
  for (;;) {
    if (stat(buf, &st) == 0) {
      if (!S_ISDIR(st.st_mode))
        return ErrNotDir;
      break;
    }
    if (errno != ENOENT)
      goto err;

    // skip past the last component
    while (--s > buf) {
      if (*s == PATH_SEPARATOR) {
        // skip past any trailing separators
        while (*--s == PATH_SEPARATOR) {};
        break;
      }
    }
    if (s == buf)
      break;
    // replace path separator with null terminator
    s[1] = 0;
  }

  if (s < end && verbose)
    log("creating directory '%s'", relpath(path));

  // mkdir starting with the first non-existant dir, e.g "/a", "/a/b", "/a/b/c"
  while (s < end) {
    if (mkdir(buf, perms) < 0 && errno != EEXIST) {
      dlog("mkdir %s: %s", buf, err_str(err_errno()));
      goto err;
    }
    while (++s < end && *s) {}
    *s = PATH_SEPARATOR;
  }

  return 0;
err:
  return err_errno();
}


err_t fs_mkdirs(const char* path, int perms) {
  return _fs_mkdirs(path, perms, /*verbose*/false);
}


err_t fs_mkdirs_verbose(const char* path, int perms) {
  return _fs_mkdirs(path, perms, coverbose);
}


err_t fs_remove(const char* path) {
  struct stat st;

  if (lstat(path, &st) != 0)
    return err_errno();

  if (!S_ISDIR(st.st_mode)) {
    if (unlink(path) != 0) {
      warn("unlink(%s)", path);
      return err_errno();
    }
    return 0;
  }

  DIR* dp = opendir(path);
  if (dp == NULL) {
    warn("opendir(%s)", path);
    return err_errno();
  }
  struct dirent* d;

  char path2[PATH_MAX];
  usize pathlen = MIN(strlen(path), PATH_MAX - 2);
  memcpy(path2, path, pathlen);
  path2[pathlen++] = '/';

  while ((d = readdir(dp)) != NULL) {
    if (*d->d_name == '.' && (!d->d_name[1] || (d->d_name[1] == '.' && !d->d_name[2])))
      continue;
    usize namelen = MIN(strlen(d->d_name), PATH_MAX - pathlen);
    memcpy(&path2[pathlen], d->d_name, namelen);
    path2[pathlen + namelen] = 0;
    err_t err = fs_remove(path2);
    if (err)
      return err;
  }
  closedir(dp);
  if (rmdir(path) != 0) {
    warn("rmdir(%s)", path);
    return err_errno();
  }
  return 0;
}


bool fs_isfile(const char* path) {
  struct stat st;
  if (stat(path, &st) != 0)
    return false;
  return S_ISREG(st.st_mode);
}


bool fs_isdir(const char* path) {
  struct stat st;
  if (stat(path, &st) != 0)
    return false;
  return S_ISDIR(st.st_mode);
}
