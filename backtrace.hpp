#ifndef __BACKTRACE_HPP_GUARD
#define __BACKTRACE_HPP_GUARD

// This file defines some utilities around glibc's backtrace() function to print
// a nice stack trace with code view and stuff. Requires glibc >= 2.1 and a
// POSIX system (uses popen(), addr2line from GNU binutils, and assumes POSIX
// path structure). Using any reasonably modern linux or BSD should work, MacOS
// with gcc+glibc might work, Windows will not work.
//
// The code is a bit of a spaghetti mess and the path parsing functions are
// criminally brittle.
//
// License: MIT
//
// Copyright 2020 Henrik Laxhuber
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>    // we require glibc >= 2.0
#include <execinfo.h> // we require glibc >= 2.1
#include <link.h>
#include <unistd.h>

#if (not defined(__GLIBC__)) || __GLIBC__ < 2 || __GLIBC_MINOR__ < 1
#error backtrace requires glic version 2.1 or greater. Using gcc on any modern linux system should work.
#endif
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
#error Windows paths not supported
#endif

namespace _nice_backtrace_detail {
struct LineReader {
  std::ifstream ifs;
  std::map<size_t, size_t> lineoffsets;
  size_t currline;

  static constexpr size_t const LINE_GRANULARITY = 16;

  struct eof_error : std::range_error {
    template <class T>
    explicit eof_error(T const &what) : std::range_error(what) {}
  };

  LineReader(std::string const &path)
      : lineoffsets({{LINE_GRANULARITY + 1, 0}}), currline(1) {
    ifs.open(path, std::ios::in);
    if (ifs.fail()) {
      std::stringstream ss;
      ss << "Failed to open source file " << path;
      throw std::invalid_argument(ss.str());
    }
  }

  std::string read_line(size_t const linenr) {
    std::string the_line;

    if (linenr != currline) {
      auto it = lineoffsets.upper_bound(linenr);
      // it->second is offset to first element in lineoffsets where the linenr
      // is _greater_ than the searched linenr. Since we store lineoffsets in
      // the format {linenr+LINE_GRANULARITY, offset} (super hacky but it
      // works...), this means we now can search forward for the line we want.
      // note that lower_bound would not work here; this complexity is needed.
      if (it != lineoffsets.end()) {
        currline = it->first - LINE_GRANULARITY;
        ifs.seekg(it->second, std::ios::beg);
      } else {
        auto rit = lineoffsets.rbegin();
        currline = rit->first - LINE_GRANULARITY;
        ifs.seekg(rit->second, std::ios::beg);
      }
      while (currline != linenr)
        this->nextline(the_line);
    }

    this->nextline(the_line);
    return the_line;
  }

private:
  void nextline(std::string &the_line) {
    // std::cerr << "reading line " << currline << "; eofbit: " << ifs.eof()
    //           << "; failbit: " << ifs.fail() << "; goodbit: " << ifs.good()
    //           << std::endl;
    std::getline(ifs, the_line);
    if (ifs.eof()) {
      ifs.clear();
      throw eof_error("EOF reached");
    }
    if (ifs.fail())
      throw std::runtime_error("read failed");

    ++currline;
    if (currline % LINE_GRANULARITY == 1)
      lineoffsets.emplace(currline + LINE_GRANULARITY, ifs.tellg());
  }
};

static constexpr size_t const CONTEXT = 1;

void print_context(LineReader &linereader, unsigned char const nlinenrdigits,
                   size_t const err_linenr) {
  for (size_t linenr = err_linenr > CONTEXT ? err_linenr - CONTEXT : 1;
       linenr <= err_linenr + CONTEXT; ++linenr) {
    try {
      std::string the_line = linereader.read_line(linenr);

      std::cerr << (linenr == err_linenr ? "\033[31;1m" : "\033[90m") << "  "
                << std::setw(nlinenrdigits) << linenr << " │\033[0m "
                << the_line << std::endl;

    } catch (LineReader::eof_error const &) {
      break;
    }
  }
}

// don't ever do this please
std::string getline_from_fd(FILE *fd) {
  std::string str;
  char line[256];

parse_line: // yuck
  if (std::fgets(line, sizeof(line), fd) == nullptr)
    return str;

  size_t linelen = std::strlen(line);
  if (linelen == 255 && line[254] != '\n') {
    // symbol longer than 255 chars
    str += line;
    goto parse_line;
  }

  line[linelen - 1] = '\0'; // remove EOL character
  str += line;

  return str;
}

// don't ever do this either
size_t split_path_filename(std::string const &path) {
  size_t pos = path.rfind('/');
  if (pos == std::string::npos)
    return -1;
  while (pos > 0 && path[pos - 1] == '\\')
    pos = path.rfind('/', pos - 2);
  return pos;
}

// this is also a brittle mess
std::string shorten_path(std::string const &path, std::string const &rel_to) {
  size_t i = 0;
  size_t same_pos = 0;
  for (; i < std::min(path.length(), rel_to.length()); ++i) {
    if (path[i] != rel_to[i])
      break;
    if (path[i] == '\\' && i + 1 < path.length() && i + 1 < rel_to.length() &&
        path[i + 1] == '/' && rel_to[i + 1] == '/') {
      ++i;
      continue;
    }
    if (path[i] == '/')
      same_pos = i;
  }

  // same_pos now indexes first '/' after which both paths differ. If we
  // matched all of rel_to, path might still have a trailing '/', so we might
  // still need to match the last component in path:
  if (i == rel_to.length() && i < path.length() && path[i] == '/') {
    ++i;
    same_pos = i;
  }

  std::string relpath;
  if (i < rel_to.length())
    relpath += "../";
  for (; i < rel_to.length(); ++i) {
    if (i + 1 < rel_to.length() && rel_to[i] == '\\' && rel_to[i] == '/') {
      ++i;
      continue;
    }
    if (rel_to[i] == '/')
      relpath += "../";
  }

  if (relpath.length() > 0 && path[same_pos] == '/')
    *relpath.rbegin() = '\0';
  relpath += path.substr(same_pos);

  if (relpath.length() < path.length())
    return relpath;
  else
    return path;
}

// holds info about a single stack frame
struct Frameinfo {
  const char *shared_obj;       // might be null if all hell broke loose
  std::string symbol;           // .length() == 0 indicates unknown
  std::string source_file_path; // .length() == 0 indicates unknown
  std::string short_source_file_path;
  size_t linenr; // 0 indicates unknown

  void print() const {
    if (!shared_obj) {
      std::cerr << "\033[90m<unknown location>\033[0m" << std::endl;
      return;
    }

    if (symbol.length() == 0) {
      std::cerr << "\033[90msomewhere in\033[0m " << shared_obj << std::endl;
      return;
    }

    if (source_file_path.length() == 0) {
      std::cerr << "\033[90msomewhere in\033[0m " << symbol << std::endl;
      return;
    }

    size_t fname_pos = split_path_filename(short_source_file_path);
    std::cerr << "\033[90mat ";
    if (fname_pos > 0)
      std::cerr << short_source_file_path.substr(0, fname_pos + 1);
    std::cerr << "\033[33m" << short_source_file_path.substr(fname_pos + 1)
              << "\033[90m:\033[31m" << linenr << " \033[90min\033[33m "
              << symbol << "\033[0m" << std::endl;
  }
};

#ifdef BACKTRACE_DEBUG
struct Logger {
  bool print_preamble = true;
  template <typename T> Logger operator<<(T const &v) const {
    if (print_preamble)
      std::cerr << "[backtrace] ";
    std::cerr << v;
    return {false};
  }
  Logger operator<<(std::ostream &(*f)(std::ostream &)) const {
    if (print_preamble)
      std::cerr << "[backtrace] ";
    f(std::cerr);
    return {false};
  }
};
#else
struct Logger : std::ostream {
  template <typename T> Logger const &operator<<(T const &v) const {
    return *this;
  }
  Logger const &operator<<(std::ostream &(*f)(std::ostream &)) const {
    return *this;
  }
};
#endif

} // namespace _nice_backtrace_detail

void print_backtrace(size_t const skip_frames = 1) {
  _nice_backtrace_detail::Logger const LOGGER;

  static constexpr size_t const MAX_DEPTH = 64;

  void *bt_buf[MAX_DEPTH];
  int const stack_depth = backtrace(&bt_buf[0], MAX_DEPTH);

  LOGGER << "obtained backtrace through " << stack_depth << " many frames"
         << std::endl;

  Dl_info dlinfos[MAX_DEPTH];
  struct link_map *lmaps[MAX_DEPTH];
  std::multimap<std::string, size_t> dynamic_objects;
  for (size_t i = skip_frames; i < stack_depth; ++i) {
    LOGGER << "looking up dynamic link info for frame " << i
           << " with return addr. " << std::hex << bt_buf[i] << std::dec
           << std::endl;

    if (dladdr1(bt_buf[i], &dlinfos[i], (void **)&lmaps[i], RTLD_DL_LINKMAP) ==
        0)
      std::cerr << "WARNING: failed to trace an address"
                << std::endl; // TODO: remove
    else {
      dynamic_objects.emplace(dlinfos[i].dli_fname, i);
      LOGGER << "Got dynamic link info: " << '\n'
             << "  Filename: " << dlinfos[i].dli_fname << '\n'
             << "  Load address: " << dlinfos[i].dli_fbase << '\n'
             << "  Nearest symbol: " << dlinfos[i].dli_sname << '\n'
             << "  Nearest symb. address: " << std::hex << dlinfos[i].dli_saddr
             << std::dec << std::endl;
    }
  }

  // call addr2line to get line numbers for each dynamic object

  char *cwd = getcwd(nullptr, 0);
  _nice_backtrace_detail::Frameinfo frameinfos[MAX_DEPTH] = {
      {nullptr, "", "", "", 0}};
  decltype(dynamic_objects.equal_range("")) range;
  for (auto it = dynamic_objects.begin(); it != dynamic_objects.end();
       it = range.second) {
    range = dynamic_objects.equal_range(it->first);

    // build the command string
    static char const command_base[] = "addr2line --demangle -f -e";
    size_t const command_len =
        sizeof(command_base) + 1 + it->first.length() + (3 + 16) * stack_depth +
        1; // reserve space for 64bit hex vals just to be safe
    char *command = (char *)std::calloc(command_len, sizeof(char));
    char *p = command +
              std::sprintf(command, "%s %s", command_base, it->first.c_str());

    LOGGER << "obtaining ELF offsets for " << it->first << std::endl;
    for (auto dyobj = range.first; dyobj != range.second; ++dyobj) {
      size_t const i = dyobj->second;
      p += std::sprintf(p, " %p",
                        // convert virtual address to physical ELF offset. we
                        // take -1 because backtrace() gives the return address,
                        // i.e. the address of the next instruction after the
                        // call, but we actually want to print the trace of the
                        // call instruction.
                        (char *)bt_buf[i] - 1 - (size_t)(lmaps[i]->l_addr));

      if (p - command > command_len) {
        // this should be unreachable but just to be safe
        std::cerr << "buffer overflow generating addr2line command, panicking"
                  << std::endl;
        std::exit(-1);
      }
    }

    LOGGER << "constructed addr2line command:\n  " << command << std::endl;

    // invoke and read output
    FILE *cfd = popen(command, "r"); // POSIX only
    if (cfd == nullptr) {
      std::cerr << "Could not generate backtrace (failed to launch addr2line)"
                << std::endl;
      return;
    }

    for (auto dyobj = range.first; dyobj != range.second; ++dyobj) {
      size_t const i = dyobj->second;
      _nice_backtrace_detail::Frameinfo &finfo = frameinfos[i];

      finfo.shared_obj = dlinfos[i].dli_fname;

      // addr2line outputs in the form of
      //  <symbol>
      //  <source_file_path>:<line>
      // for each address that we pass it, parse this format:
      finfo.symbol = _nice_backtrace_detail::getline_from_fd(cfd);

      std::string const &locspec = _nice_backtrace_detail::getline_from_fd(cfd);
      size_t colon_idx = locspec.rfind(':');

      finfo.source_file_path = std::string(locspec, 0, colon_idx);
      if (finfo.source_file_path == "??")
        finfo.source_file_path = "";
      else
        finfo.short_source_file_path =
            _nice_backtrace_detail::shorten_path(finfo.source_file_path, cwd);

      finfo.linenr =
          std::strtoull(locspec.c_str() + colon_idx + 1, nullptr, 10);

      LOGGER << "addr2line determined data for frame " << i << ":\n"
             << "  Symbol: " << finfo.symbol << '\n'
             << "  Source locspec: " << locspec << '\n'
             << "  Parsed&shortened source file path: "
             << finfo.source_file_path << '\n'
             << "  Parsed line number: " << finfo.linenr << std::endl;
    }

    std::free(command);
    if (pclose(cfd) != 0)
      std::exit(-1);
  }
  std::free(cwd);

  size_t max_line_digits = 1;
  for (size_t i = skip_frames; i < stack_depth; ++i) {
    if (frameinfos[i].linenr != 0)
      max_line_digits = std::max(
          max_line_digits, (size_t)std::ceil(std::log10(frameinfos[i].linenr)));
  }

  LOGGER << "Determined max. linenr digits: " << max_line_digits
         << " -- now printing trace" << std::endl;

  std::map<std::string, _nice_backtrace_detail::LineReader> linereaders;
  for (size_t i = skip_frames; i < stack_depth; ++i) {
    frameinfos[i].print();
    if (frameinfos[i].source_file_path.length() == 0)
      continue;

    try {
      auto it = linereaders.emplace(frameinfos[i].source_file_path,
                                    frameinfos[i].source_file_path);
      _nice_backtrace_detail::LineReader &reader = it.first->second;

      if (frameinfos[i].linenr != 0)
        print_context(reader, max_line_digits, frameinfos[i].linenr);
    } catch (std::invalid_argument const &ex) {
      std::cerr << "\033[31;1m  " << std::setw(max_line_digits)
                << frameinfos[i].linenr << " │\033[0;90m " << ex.what()
                << "\033[0m" << std::endl;
    }
  }
}

void backtrace_assert(bool cond, char const *msg,
                      size_t const skip_frames = 2) {
  if (!cond) {
    std::cerr << "Assertion '\033[95m" << msg
              << "\033[0m' failed:" << std::endl;
    print_backtrace(skip_frames);
    std::exit(-1);
  }
}

#endif

#ifndef NDEBUG
#include <cassert>
#undef assert
#define assert(cond) backtrace_assert((cond), #cond)
#endif
