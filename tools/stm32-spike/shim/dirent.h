/* spike: newlib's <dirent.h> #errors on arm-none-eabi */
#ifndef SPIKE_DIRENT_H
#define SPIKE_DIRENT_H
typedef struct spike_dir DIR;
struct dirent { char d_name[256]; };
DIR *opendir(const char *);
struct dirent *readdir(DIR *);
int closedir(DIR *);
#endif
