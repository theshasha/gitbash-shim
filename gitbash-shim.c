/*
 * bash.exe shim that lets native Windows programs (OpenCode, Claude Code and other tools that run
 * `bash.exe -c <command>`) run Git Bash commands exactly as written.
 *
 * Native callers such as Node and Bun build the Windows command line with the
 * standard MSVCRT quoting rules, but the MSYS2 runtime splits it with its own
 * rules: inside double quotes it turns `\\` into `\`, and its glob step works
 * in a fixed 8192-character buffer, which cuts quoted arguments at 8,186
 * characters. MSYS=noglob avoids both but breaks `\"` instead
 * (msys2/msys2-runtime#178).
 *
 * This shim is a native program, so it recovers the caller's arguments
 * exactly. It then passes the `-c` command string to bash through an
 * environment variable, which MSYS2 does not reparse, and re-quotes the
 * remaining arguments with the escaping MSYS2 decodes losslessly. The command
 * string survives MSYS=noglob as well; the other arguments need it unset.
 *
 * Build: gcc -O2 -s -static -municode -o bash.exe gitbash-shim.c -lshell32 -ladvapi32
 * Test:  node test.mjs bash.exe
 * The file must stay named bash.exe: OpenCode and Claude Code both recognise
 * the shell by its file name.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

/* Longest path Windows accepts, in characters including the terminator. */
#define PATH_CHARS 32768

static const wchar_t SCRIPT_VAR[] = L"GITBASH_SHIM_SCRIPT";
static const wchar_t TARGET_VAR[] = L"GITBASH_SHIM_TARGET";
/*
 * Replaces the -c command string; un-exporting keeps the script out of the environment of its children.
 * Written to the command line as is: single quotes and no backslash decode the same with and without
 * MSYS=noglob, so commands still run correctly when that setting is around.
 */
static const wchar_t RUNNER[] = L"'declare +x GITBASH_SHIM_SCRIPT; eval \"$GITBASH_SHIM_SCRIPT\"'";

/* Writes UTF-8, which is what the callers read; the C runtime would drop non-ASCII path characters. */
static int die(const wchar_t *format, ...) {
  wchar_t wide[1024];
  char utf8[sizeof wide / sizeof *wide * 3];
  va_list args;
  va_start(args, format);
  _vsnwprintf(wide, sizeof wide / sizeof *wide - 1, format, args);
  va_end(args);
  wide[sizeof wide / sizeof *wide - 1] = 0;
  int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, sizeof utf8, NULL, NULL);
  DWORD written;
  if (size > 1) WriteFile(GetStdHandle(STD_ERROR_HANDLE), utf8, (DWORD)size - 1, &written, NULL);
  return 127;
}

static int fail_code(const wchar_t *what, DWORD error) {
  return die(L"gitbash-shim: %ls failed (error %lu)\n", what, error);
}

static int fail(const wchar_t *what) {
  return fail_code(what, GetLastError());
}

static void *alloc(size_t size) {
  void *memory = malloc(size);
  if (!memory) exit(fail(L"malloc"));
  return memory;
}

/* The variable's value, or NULL when it is unset or empty. */
static wchar_t *env(const wchar_t *name) {
  DWORD size = GetEnvironmentVariableW(name, NULL, 0);
  if (size < 2) return NULL;
  wchar_t *value = alloc(size * sizeof *value);
  DWORD length = GetEnvironmentVariableW(name, value, size);
  if (length && length < size) return value;
  free(value);
  return NULL;
}

static int is_separator(wchar_t c) {
  return c == L'\\' || c == L'/';
}

/* A relative path would mean a different file in every working directory, which is the caller's project. */
static int is_absolute(const wchar_t *path) {
  int letter = (path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z');
  if (letter && path[1] == L':') return is_separator(path[2]);
  return is_separator(path[0]) && is_separator(path[1]) && path[2] && !is_separator(path[2]);
}

/* dir without its trailing separators, followed by tail. */
static wchar_t *join(const wchar_t *dir, const wchar_t *tail) {
  size_t length = wcslen(dir);
  while (length && is_separator(dir[length - 1])) length--;
  wchar_t *path = alloc((length + wcslen(tail) + 1) * sizeof *path);
  wmemcpy(path, dir, length);
  wcscpy(path + length, tail);
  return path;
}

/* Cuts the last component off path; returns 0 when there is none left. */
static int parent(wchar_t *path) {
  size_t length = wcslen(path);
  while (length && is_separator(path[length - 1])) length--;
  while (length && !is_separator(path[length - 1])) length--;
  if (!length) return 0;
  path[length - 1] = 0;
  return 1;
}

/* Identifies the file itself, so short names, junctions and hard links to the same file compare equal. */
static int file_id(const wchar_t *path, BY_HANDLE_FILE_INFORMATION *id) {
  DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
  HANDLE file = CreateFileW(path, 0, share, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) return 0;
  BOOL ok = GetFileInformationByHandle(file, id);
  CloseHandle(file);
  return ok;
}

static int same_file(const BY_HANDLE_FILE_INFORMATION *a, const BY_HANDLE_FILE_INFORMATION *b) {
  return a->dwVolumeSerialNumber == b->dwVolumeSerialNumber && a->nFileIndexHigh == b->nFileIndexHigh &&
         a->nFileIndexLow == b->nFileIndexLow;
}

static int has_file(const wchar_t *dir, const wchar_t *tail) {
  wchar_t *path = join(dir, tail);
  DWORD attributes = GetFileAttributesW(path);
  free(path);
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

/*
 * root's bin\bash.exe when root is a Git for Windows installation, otherwise NULL. bin\bash.exe is the
 * launcher that sets MSYSTEM and PATH. The two other files tell Git for Windows apart from Cygwin and a
 * plain MSYS2, whose bash.exe would otherwise be picked up silently.
 */
static wchar_t *git_bash(const wchar_t *root, const BY_HANDLE_FILE_INFORMATION *self) {
  if (!root || !is_absolute(root)) return NULL;
  if (!has_file(root, L"\\git-bash.exe") || !has_file(root, L"\\usr\\bin\\msys-2.0.dll")) return NULL;
  wchar_t *bash = join(root, L"\\bin\\bash.exe");
  BY_HANDLE_FILE_INFORMATION id;
  if (file_id(bash, &id) && !same_file(&id, self)) return bash;
  free(bash);
  return NULL;
}

#ifndef SKIP_REGISTRY
/* InstallPath as the Git for Windows installer records it, or NULL. */
static wchar_t *installed_root(HKEY hive, REGSAM view) {
  HKEY key;
  if (RegOpenKeyExW(hive, L"Software\\GitForWindows", 0, KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS) return NULL;
  wchar_t *root = NULL;
  DWORD size = 0;
  if (RegGetValueW(key, NULL, L"InstallPath", RRF_RT_REG_SZ, NULL, NULL, &size) == ERROR_SUCCESS && size) {
    root = alloc(size);
    if (RegGetValueW(key, NULL, L"InstallPath", RRF_RT_REG_SZ, NULL, root, &size) != ERROR_SUCCESS) {
      free(root);
      root = NULL;
    }
  }
  RegCloseKey(key);
  return root;
}
#endif

/*
 * Git Bash of the Git whose git.exe is on PATH: git.exe sits one level (cmd, bin) or two levels
 * (mingw64\bin) below the installation. PATH is walked here because SearchPathW's default order starts
 * with the current directory, and relative entries are skipped: both would let the caller's project decide
 * what the shim starts.
 */
static wchar_t *git_bash_on_path(const BY_HANDLE_FILE_INFORMATION *self) {
  wchar_t *path = env(L"PATH"), *bash = NULL;
  for (wchar_t *dir = path, *next; dir && !bash; dir = next) {
    next = wcschr(dir, L';');
    if (next) *next++ = 0;
    if (*dir == L'"') dir++;
    size_t length = wcslen(dir);
    if (length && dir[length - 1] == L'"') dir[length - 1] = 0;
    if (!is_absolute(dir) || !has_file(dir, L"\\git.exe")) continue;
    for (int level = 0; level < 2 && !bash && parent(dir); level++) bash = git_bash(dir, self);
  }
  free(path);
  return bash;
}

static wchar_t *git_bash_below(const wchar_t *variable, const wchar_t *tail, const BY_HANDLE_FILE_INFORMATION *self) {
  wchar_t *base = env(variable);
  if (!base) return NULL;
  wchar_t *root = join(base, tail), *bash = git_bash(root, self);
  free(root);
  free(base);
  return bash;
}

/* The Git Bash launcher every call is forwarded to. Exits with a message when there is none. */
static wchar_t *find_bash(void) {
  BY_HANDLE_FILE_INFORMATION self, id;
  wchar_t *own = alloc(PATH_CHARS * sizeof *own);
  DWORD length = GetModuleFileNameW(NULL, own, PATH_CHARS);
  if (!length || length >= PATH_CHARS) exit(fail(L"GetModuleFileNameW"));
  if (!file_id(own, &self)) exit(fail(L"CreateFileW"));
  free(own);

  /* A setting that cannot be used is an error: falling back would look as if the setting had worked. */
  wchar_t *bash = env(TARGET_VAR);
  if (bash) {
    if (!is_absolute(bash)) exit(die(L"gitbash-shim: %ls must be an absolute path: %ls\n", TARGET_VAR, bash));
    if (!file_id(bash, &id) || (id.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
      exit(die(L"gitbash-shim: %ls is not a file: %ls\n", TARGET_VAR, bash));
    if (same_file(&id, &self))
      exit(die(L"gitbash-shim: %ls points to the shim itself; set it to Git's bin\\bash.exe\n", TARGET_VAR));
    return bash;
  }

#ifndef SKIP_REGISTRY
  /* The installer writes HKCU when run without administrator rights. The 32-bit view is for a 32-bit Git. */
  static const struct { HKEY hive; REGSAM view; } keys[] = {
    {HKEY_CURRENT_USER, 0}, {HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY}, {HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY},
  };
  for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
    wchar_t *root = installed_root(keys[i].hive, keys[i].view);
    bash = git_bash(root, &self);
    free(root);
    if (bash) return bash;
  }
#endif

  /* Scoop sets GIT_INSTALL_ROOT; its git.exe on PATH is a launcher outside the installation. */
  if ((bash = git_bash_below(L"GIT_INSTALL_ROOT", L"", &self))) return bash;
  if ((bash = git_bash_on_path(&self))) return bash;
  if ((bash = git_bash_below(L"ProgramFiles", L"\\Git", &self))) return bash;
  if ((bash = git_bash_below(L"LOCALAPPDATA", L"\\Programs\\Git", &self))) return bash;
  exit(die(L"gitbash-shim: Git Bash not found; set %ls to the full path of Git's bin\\bash.exe\n", TARGET_VAR));
}

/* Keeps the shim alive on Ctrl+C so bash decides how to react and the shim reports its exit code. */
static BOOL WINAPI ignore_ctrl(DWORD type) {
  (void)type;
  return TRUE;
}

/* Inside double quotes MSYS2 only unescapes \\ and \", and treats every other character literally. */
static wchar_t *append_quoted(wchar_t *out, const wchar_t *arg) {
  *out++ = L'"';
  for (; *arg; arg++) {
    if (*arg == L'\\' || *arg == L'"') *out++ = L'\\';
    *out++ = *arg;
  }
  *out++ = L'"';
  return out;
}

/* Index of bash's command string when the arguments use -c, otherwise 0. */
static int script_index(int argc, wchar_t **argv) {
  int has_c = 0;
  for (int i = 1; i < argc; i++) {
    const wchar_t *arg = argv[i];
    if (wcscmp(arg, L"--") == 0 || wcscmp(arg, L"-") == 0) return has_c && i + 1 < argc ? i + 1 : 0;
    if (arg[0] == L'-' && arg[1] == L'-') continue;
    if ((arg[0] != L'-' && arg[0] != L'+') || arg[1] == 0) return has_c ? i : 0;
    if (arg[0] == L'-' && wcschr(arg, L'c')) has_c = 1;
    /* -o and -O take the next argument as their value. */
    if (wcschr(arg, L'o') || wcschr(arg, L'O')) i++;
  }
  return 0;
}

int wmain(void) {
  int argc;
  wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return fail(L"CommandLineToArgvW");

  const wchar_t *bash = find_bash();

  int script = script_index(argc, argv);
  if (!SetEnvironmentVariableW(SCRIPT_VAR, script ? argv[script] : NULL) && script)
    return fail(L"SetEnvironmentVariableW");

  size_t size = wcslen(bash) + 3;
  for (int i = 1; i < argc; i++) size += 2 * wcslen(i == script ? RUNNER : argv[i]) + 3;
  wchar_t *line = alloc(size * sizeof *line);

  /* MSYS2 only strips the quotes from argv[0], so the path is not escaped. */
  wchar_t *out = line;
  *out++ = L'"';
  out = wcscpy(out, bash) + wcslen(bash);
  *out++ = L'"';
  for (int i = 1; i < argc; i++) {
    *out++ = L' ';
    if (i == script) out = wcscpy(out, RUNNER) + wcslen(RUNNER);
    else out = append_quoted(out, argv[i]);
  }
  *out = 0;

  /*
   * The job kills bash and everything it started if the shim is terminated, which also covers processes
   * whose parent bash already exited and that taskkill /T can no longer find. Breakaway stays disallowed:
   * MSYS2 starts every child outside the job when the job allows it. The limit is lifted once bash exits.
   */
  HANDLE job = CreateJobObjectW(NULL, NULL);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits))
    return fail(L"CreateJobObjectW");

  SetConsoleCtrlHandler(ignore_ctrl, TRUE);

  /*
   * Native programs such as cmd and PowerShell encode redirected output with the console code page, which
   * the caller then reads as UTF-8. Switching only this console avoids the system-wide UTF-8 setting; the
   * old code pages come back once bash exits, in case the console is shared with the caller.
   */
  UINT input_cp = GetConsoleCP(), output_cp = GetConsoleOutputCP();
  SetConsoleCP(CP_UTF8);
  SetConsoleOutputCP(CP_UTF8);

  /*
   * Hand bash the shim's own standard handles. A caller that starts the shim detached (Claude Code) leaves it
   * without a console; bash would then get a new console of its own and its output would go there.
   */
  STARTUPINFOW startup = {
    .cb = sizeof startup,
    .dwFlags = STARTF_USESTDHANDLES,
    .hStdInput = GetStdHandle(STD_INPUT_HANDLE),
    .hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE),
    .hStdError = GetStdHandle(STD_ERROR_HANDLE),
  };
  /* GetConsoleCP returns 0 without a console; bash then gets a hidden one instead of a visible window. */
  DWORD flags = CREATE_SUSPENDED | (input_cp ? 0 : CREATE_NO_WINDOW);
  PROCESS_INFORMATION child;
  if (!CreateProcessW(bash, line, NULL, NULL, TRUE, flags, NULL, NULL, &startup, &child))
    return fail(L"CreateProcessW");
  if (!AssignProcessToJobObject(job, child.hProcess)) {
    DWORD error = GetLastError();
    TerminateProcess(child.hProcess, 127);
    return fail_code(L"AssignProcessToJobObject", error);
  }
  ResumeThread(child.hThread);
  CloseHandle(child.hThread);

  WaitForSingleObject(child.hProcess, INFINITE);
  DWORD code = 127;
  GetExitCodeProcess(child.hProcess, &code);
  if (input_cp) SetConsoleCP(input_cp);
  if (output_cp) SetConsoleOutputCP(output_cp);

  /* Background processes the command started keep running, as they would under bash directly. */
  limits.BasicLimitInformation.LimitFlags = 0;
  SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
  return (int)code;
}
