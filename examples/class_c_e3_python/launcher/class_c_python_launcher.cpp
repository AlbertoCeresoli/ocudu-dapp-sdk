// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Supervised Class-C catalog entrypoint (shared by spectrum + SRS packages).
//
// Placement: Class-C out_of_process only. Python cannot implement in-process
// Class-A/B/C MODULEs; this ELF is the catalog artifact the DU posix_spawn()s.
//
// The DU provides a *minimal* environment (PATH + LANG only) and a control
// socket on --control-fd=N. We must therefore:
//   1. Apply PR_SET_NO_NEW_PRIVS before any long work (host admission waits).
//   2. execve Python with absolute interpreter, worker script, and PYTHONPATH
//      so ocudu_dapp_class_c_process + PyTorch are importable.
//
// CMake embeds defaults (python, worker path, PYTHONPATH, LD_LIBRARY_PATH, log
// file) at build time. Override with OCUDU_DAPP_PYTHON / OCUDU_DAPP_PYTHONPATH /
// OCUDU_DAPP_PYTHON_PROCESS_WORKER / OCUDU_DAPP_LOG_FILE when needed.
//
// Research / education only: Python process dApps are not a production RAN
// performance path. Prefer native C++/CUDA Class-C packages (ref_spectrumd_*,
// ref_srs_isacd_*) for product use.

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/prctl.h>
#include <unistd.h>
#include <vector>

namespace {

std::string getenv_or(const char* key, const char* fallback)
{
  if (const char* value = std::getenv(key)) {
    if (value[0] != '\0') {
      return value;
    }
  }
  return fallback != nullptr ? fallback : "";
}

std::string worker_script()
{
  return getenv_or("OCUDU_DAPP_PYTHON_PROCESS_WORKER",
#ifdef OCUDU_DAPP_PYTHON_PROCESS_WORKER_DEFAULT
                   OCUDU_DAPP_PYTHON_PROCESS_WORKER_DEFAULT
#else
                   ""
#endif
  );
}

std::string python_interpreter()
{
  return getenv_or("OCUDU_DAPP_PYTHON",
#ifdef OCUDU_DAPP_PYTHON_DEFAULT
                   OCUDU_DAPP_PYTHON_DEFAULT
#else
                   "/usr/bin/python3"
#endif
  );
}

std::string python_path()
{
  return getenv_or("OCUDU_DAPP_PYTHONPATH",
#ifdef OCUDU_DAPP_PYTHONPATH_DEFAULT
                   OCUDU_DAPP_PYTHONPATH_DEFAULT
#else
                   ""
#endif
  );
}

std::string log_file()
{
  return getenv_or("OCUDU_DAPP_LOG_FILE",
#ifdef OCUDU_DAPP_LOG_FILE_DEFAULT
                   OCUDU_DAPP_LOG_FILE_DEFAULT
#else
                   ""
#endif
  );
}

std::string ld_library_path()
{
  return getenv_or("LD_LIBRARY_PATH",
#ifdef OCUDU_DAPP_LD_LIBRARY_PATH_DEFAULT
                   OCUDU_DAPP_LD_LIBRARY_PATH_DEFAULT
#else
                   ""
#endif
  );
}

} // namespace

int main(int argc, char** argv)
{
  const std::string python = python_interpreter();
  const std::string worker = worker_script();
  const std::string pypath = python_path();
  const std::string log    = log_file();
  const std::string ldpath = ld_library_path();

  if (python.empty() || worker.empty()) {
    std::cerr << "class_c_python_launcher: missing python or worker path "
                 "(set OCUDU_DAPP_PYTHON / OCUDU_DAPP_PYTHON_PROCESS_WORKER)\n";
    return 127;
  }

  // Host admission waits for PR_SET_NO_NEW_PRIVS before sending routes.
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    std::cerr << "class_c_python_launcher: PR_SET_NO_NEW_PRIVS failed: " << std::strerror(errno) << "\n";
    return 127;
  }

  // argv: python worker.py [du-args...] [--log-file=...] [--device cpu]
  std::vector<std::string> storage;
  storage.reserve(static_cast<std::size_t>(argc) + 6);
  storage.push_back(python);
  storage.push_back(worker);
  for (int i = 1; i < argc; ++i) {
    storage.emplace_back(argv[i]);
  }
  // Log file only — never enable high-rate --json-log on stdout by default
  // (non-drained pipes block the worker and cause heartbeat SIGKILL).
  if (!log.empty()) {
    storage.emplace_back("--log-file=" + log);
  }
  // Prefer CPU for process workers: at demo grid sizes CUDA launch overhead
  // often exceeds the math, and host densify already delivers CF32 on CPU.
  bool has_device = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strncmp(argv[i], "--device", 8) == 0) {
      has_device = true;
      break;
    }
  }
  if (!has_device) {
    storage.emplace_back("--device");
    storage.emplace_back("cpu");
  }

  std::vector<char*> args;
  args.reserve(storage.size() + 1);
  for (auto& s : storage) {
    args.push_back(s.data());
  }
  args.push_back(nullptr);

  std::vector<std::string> env_storage;
  env_storage.push_back("PATH=/usr/bin:/bin:/usr/local/bin");
  env_storage.push_back("LANG=C");
  env_storage.push_back("LC_ALL=C");
  env_storage.push_back("HOME=/tmp");
  if (!pypath.empty()) {
    env_storage.push_back("PYTHONPATH=" + pypath);
  }
  env_storage.push_back("PYTHONUNBUFFERED=1");
  env_storage.push_back("CUDA_HOME=/usr/local/cuda");
  if (!ldpath.empty()) {
    env_storage.push_back("LD_LIBRARY_PATH=" + ldpath);
  }

  std::vector<char*> envp;
  envp.reserve(env_storage.size() + 1);
  for (auto& s : env_storage) {
    envp.push_back(s.data());
  }
  envp.push_back(nullptr);

  ::execve(python.c_str(), args.data(), envp.data());
  const int err = errno;
  std::cerr << "class_c_python_launcher: execve(" << python << ", " << worker << ") failed: " << std::strerror(err)
            << "\nPYTHONPATH was: " << pypath << "\n";
  return 127;
}
