/* Host-side complement for switchVK's loaderless DRM shim.
 * The packaged SDK handles stat("/dev/dri/renderD128") but Mesa 26 also
 * fstat()s the opened synthetic render-node descriptor.  Newlib dispatches an
 * unknown descriptor through a null devoptab entry, which faults at PC=0. */

#ifdef USE_VULKAN

#include <sys/stat.h>
#include <sys/types.h>

extern int drm_shim_owns_fd(int fd);
extern int __real_fstat(int fd, struct stat *status);

int __wrap_fstat(int fd, struct stat *status) {
  if (!drm_shim_owns_fd(fd))
    return __real_fstat(fd, status);
  if (!status) return -1;
  *status = (struct stat){0};
  status->st_mode = S_IFCHR | 0666;
  status->st_rdev = (226 << 8) | 128;
  status->st_nlink = 1;
  return 0;
}

#endif /* USE_VULKAN */
