#pragma once
// Host stand-in: the "card" is the sdcard/ folder next to the simulator.
#include <Arduino.h>
#include <sys/stat.h>
#include <dirent.h>
#define FILE_WRITE "w"
#define FILE_READ "r"
enum { CARD_NONE, CARD_MMC, CARD_SD, CARD_SDHC };
class File {
 public:
  FILE *fp = nullptr;
  DIR *dir = nullptr;
  std::string path, base;
  File() {}
  explicit operator bool() const { return fp || dir; }
  size_t write(const uint8_t *b, size_t n) { return fwrite(b, 1, n, fp); }
  size_t read(uint8_t *b, size_t n) { return fread(b, 1, n, fp); }
  bool seek(uint32_t pos) { return fseek(fp, pos, SEEK_SET) == 0; }
  size_t size() { long at = ftell(fp); fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, at, SEEK_SET); return n < 0 ? 0 : (size_t)n; }
  bool isDirectory() { return dir != nullptr; }
  const char *name() { return base.c_str(); }
  File openNextFile() {
    File f;
    for (struct dirent *e; dir && (e = readdir(dir));) {
      if (e->d_name[0] == '.') continue;
      f.base = e->d_name;
      f.path = path + "/" + e->d_name;
      f.fp = fopen(f.path.c_str(), "rb");
      break;
    }
    return f;
  }
  void close() {
    if (fp) fclose(fp);
    if (dir) closedir(dir);
    fp = nullptr;
    dir = nullptr;
  }
};
class SDMMCFS {
 public:
  bool present = true;
  bool setPins(int, int, int) { return true; }
  bool begin(const char *, bool) { return present; }
  int cardType() { return present ? CARD_SDHC : CARD_NONE; }
  void end() {}
  std::string p(const char *path) { return std::string("sdcard") + path; }
  bool mkdir(const char *path) { ::mkdir("sdcard", 0755); return ::mkdir(p(path).c_str(), 0755) == 0; }
  bool exists(const char *path) { struct stat st; return stat(p(path).c_str(), &st) == 0; }
  File open(const char *path, const char *mode = FILE_READ) {
    File f;
    f.path = p(path);
    struct stat st;
    if (mode[0] == 'r' && stat(f.path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      f.dir = opendir(f.path.c_str());
    } else {
      f.fp = fopen(f.path.c_str(), mode[0] == 'w' ? "wb" : "rb");
    }
    return f;
  }
};
extern SDMMCFS SD_MMC;
