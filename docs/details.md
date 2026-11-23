# gitbash-shim in detail

English | [简体中文](details.zh-CN.md)

## The problem

Run Git Bash from Node the way these tools do:

```js
import { spawnSync } from "node:child_process"
const bash = "C:/Program Files/Git/bin/bash.exe"
const run = (cmd) => spawnSync(bash, ["-c", cmd], { encoding: "utf8" }).stdout
console.log(run(String.raw`printf '%s\n' 'a\\b'`))                   // a\b, expected a\\b
console.log(run("cat <<'E' | wc -c\n" + "x".repeat(10000) + "\nE"))  // 8169, expected 10001
```

Native programs such as Node, Bun and Python build the Windows command line with the Microsoft C runtime's quoting rules. When the MSYS2 runtime under Git Bash is started by such a program, it splits the command line with its own rules: in a quoted argument, which a `-c` command almost always is, each run of backslashes is halved, rounding up (except right before a `"`), and the argument is cut at 8,186 characters without an error. This happens before bash parses anything, so single quotes and quoted heredocs don't help, and bash often still exits with 0. Setting `MSYS=noglob` keeps the backslashes and the length but breaks `\"` instead.

Reported in git-for-windows/git [#6517](https://github.com/git-for-windows/git/issues/6517) (backslashes, `MSYS=noglob`) and [#6516](https://github.com/git-for-windows/git/issues/6516) (the cut), and msys2/msys2-runtime [#178](https://github.com/msys2/msys2-runtime/issues/178) (`MSYS=noglob`). Seen in anthropics/claude-code [#85856](https://github.com/anthropics/claude-code/issues/85856), [#102533](https://github.com/anthropics/claude-code/issues/102533), [#85111](https://github.com/anthropics/claude-code/issues/85111) and anomalyco/opencode [#57278](https://github.com/anomalyco/opencode/issues/57278).

## How it works

```
tool    --  bash.exe -c <command>  -->  gitbash-shim (bash.exe)
shim    --  <command> in the environment variable GITBASH_SHIM_SCRIPT,
            a fixed runner on the command line  -->  Git's bin\bash.exe
bash    --  runs eval "$GITBASH_SHIM_SCRIPT"
```

The shim is a native program, so it reads its arguments exactly as the tool wrote them. It passes the `-c` command to bash through an environment variable, which the MSYS2 runtime does not reparse; the command line only carries a short fixed runner that evaluates the variable and removes it from the environment of the commands it starts. Any other arguments are re-quoted in the form MSYS2 decodes without loss. It starts Git's `bin\bash.exe`, the launcher that sets `MSYSTEM` and `PATH` before starting bash.

It also:

- runs bash in a Windows job object, so when the tool kills the shim (on a timeout, for example), bash and everything it started are killed with it. Background processes left running after bash exits normally keep running.
- switches the console code page to UTF-8 while bash runs and restores it afterwards, so native programs that bash starts, such as cmd and PowerShell, write their output as UTF-8.
- hands bash its own standard handles, so a tool that starts it detached (Claude Code does) still gets the output.

## Why the file must be named bash.exe

Both tools decide what kind of shell they have from the file name. opencode picks the arguments and the command parser by it. Claude Code ignores `CLAUDE_CODE_GIT_BASH_PATH` unless the file is named `bash.exe`, `sh.exe`, `bash` or `sh`, and then quietly falls back to the Git Bash it detects itself ([docs](https://code.claude.com/docs/en/env-vars)).

## Finding Git Bash

The shim forwards to the first of these that qualifies:

1. `GITBASH_SHIM_TARGET`, if set
2. `InstallPath` under the registry key `Software\GitForWindows`, in `HKEY_CURRENT_USER`, then `HKEY_LOCAL_MACHINE`
3. `GIT_INSTALL_ROOT` (scoop sets it)
4. the folder one or two levels above the folder holding a `git.exe` on `PATH` (relative `PATH` entries are skipped)
5. `%ProgramFiles%\Git`
6. `%LocalAppData%\Programs\Git` (the Git for Windows installer suggests it when it can't write to the default folder)

A folder from steps 2 to 6 only counts as Git for Windows when it holds `bin\bash.exe`, `git-bash.exe` and `usr\bin\msys-2.0.dll`, so Cygwin or a standalone MSYS2 is never picked by accident. If nothing qualifies, the shim exits with 127 and prints:

```
gitbash-shim: Git Bash not found; set GITBASH_SHIM_TARGET to the full path of Git's bin\bash.exe
```

`GITBASH_SHIM_TARGET` must be an absolute path to Git's `bin\bash.exe`; the three-file check is skipped for it. If it is set but not absolute, does not exist, or points at the shim itself, the shim exits with 127 and an error instead of searching on, so a mistake in it doesn't go unnoticed. An empty value counts as unset.

## What changes for you

Commands now reach bash as written, so anything that was written to make up for the halving gets what it literally says: a `\\b` in a `grep` pattern that used to arrive as `\b` now arrives as `\\b`. In claude-code [#85856](https://github.com/anthropics/claude-code/issues/85856), one user found that most of the logged commands with `\\` inside a single-quoted pattern relied on the halving. Check agent instructions, hooks and saved commands that work around the bug.

## Limits

- Carriage returns in a command are still lost: bash drops them itself, however the command arrives.
- The tool still starts the shim through a Windows command line, which holds at most 32,767 characters, command included.
- With `MSYS=noglob` set, the `-c` command still arrives intact, but other arguments may not.
- `GITBASH_SHIM_TARGET` accepts any file, but only Git for Windows' `bin\bash.exe` has been tested.

## Other approaches

- [intact-bash-mod](https://github.com/GLEAM-Lab/intact-bash-mod) is a Claude Code mod. It writes each command that is longer than 200 characters or holds a heredoc or a backslash pair to a file under `%LOCALAPPDATA%\Temp\claude-cmd\` and has bash source that file, and refuses commands that no route carries intact. It works inside Claude Code only and leaves the files for you to delete. gitbash-shim works with any tool that lets you choose `bash.exe` and writes no files.
- [gash](https://github.com/hex2f/gash) and [niubash](https://github.com/unixwin/niubash) are bash-compatible shells written from scratch in Rust that replace Git Bash. With gitbash-shim your commands still run in Git for Windows' own bash and tools; only the way the command gets there changes.

## Checking the download

Run `sha256sum -c bash.exe.sha256` in Git Bash in the download folder; it prints `bash.exe: OK`. In PowerShell, `(Get-FileHash bash.exe).Hash.ToLower()` must equal the first field of `bash.exe.sha256`.

The binary is not code-signed. It is built and tested from the tagged source by this repository's GitHub Actions workflow.

## Build and test

With MinGW-w64 gcc:

```
gcc -O2 -s -static -municode -o bash.exe gitbash-shim.c -lshell32 -ladvapi32
```

The tests need Node. They run each case through the shim and compare with Git Bash running the same text from a script file. The second argument is the path of Git's `bin\bash.exe` to compare against; without it, the tests use the one the shim finds.

```
node test.mjs bash.exe [path of Git's bin\bash.exe]
```

`test-locate.mjs` tests the search order with a build that skips the registry:

```
gcc -O2 -s -static -municode -DSKIP_REGISTRY -o shim-noreg.exe gitbash-shim.c -lshell32 -ladvapi32
node test-locate.mjs shim-noreg.exe "C:\Program Files\Git\bin\bash.exe"
```

Tested with Claude Code 2.1.321 on Windows 11 with Git for Windows 2.52.0. The workflow runs both test files on GitHub's `windows-latest` runner on every push.
