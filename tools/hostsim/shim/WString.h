#pragma once
#include <string>
class __FlashStringHelper;
class String {
 public:
  std::string s;
  String() {}
  String(const char *c) : s(c) {}
  String(const std::string &x) : s(x) {}
  unsigned length() const { return s.size(); }
  const char *c_str() const { return s.c_str(); }
  int indexOf(char c, unsigned from) const { auto p = s.find(c, from); return p == std::string::npos ? -1 : (int)p; }
  String substring(unsigned a, unsigned b) const { return String(s.substr(a, b - a)); }
  String &operator+=(const char *c) { s += c; return *this; }
  friend String operator+(const String &a, const String &b) { return String(a.s + b.s); }
  friend String operator+(const String &a, const char *b) { return String(a.s + b); }
};
