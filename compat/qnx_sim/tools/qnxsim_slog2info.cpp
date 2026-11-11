// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// QNX API simulation on Linux, not QNX.
// qnxsim_slog2info [-b BUFFER_SET]: prints what the simulated slog2 subset wrote (<QNXSIM_ROOT>/slog2/<set>.log).
#include "qnx_sim.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>

static void dump(const std::string& file) {
  FILE* f = fopen(file.c_str(), "r");
  if (!f) return;
  char line[2048];
  while (fgets(line, sizeof line, f)) fputs(line, stdout);
  fclose(f);
}

int main(int argc, char** argv) {
  std::string only;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "-b") && i + 1 < argc) only = argv[++i];
    else { fprintf(stderr, "usage: qnxsim_slog2info [-b BUFFER_SET]\n"); return 2; }
  }
  printf("# %s (slog2 subset)\n", QNXSIM_LABEL);
  std::string dir = std::string(qnxsim_root()) + "/slog2";
  if (!only.empty()) { dump(dir + "/" + only + ".log"); return 0; }
  std::vector<std::string> names;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) { std::string n = e->d_name; if (n.size() > 4 && n.substr(n.size() - 4) == ".log") names.push_back(n); }
    closedir(d);
  }
  std::sort(names.begin(), names.end());
  for (auto& n : names) dump(dir + "/" + n);
  return 0;
}
