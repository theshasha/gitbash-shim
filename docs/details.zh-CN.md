# gitbash-shim 详细说明

[English](details.md) | 简体中文

## 问题是什么

按这些工具的方式，从 Node 调 Git Bash：

```js
import { spawnSync } from "node:child_process"
const bash = "C:/Program Files/Git/bin/bash.exe"
const run = (cmd) => spawnSync(bash, ["-c", cmd], { encoding: "utf8" }).stdout
console.log(run(String.raw`printf '%s\n' 'a\\b'`))                   // a\b, expected a\\b
console.log(run("cat <<'E' | wc -c\n" + "x".repeat(10000) + "\nE"))  // 8169, expected 10001
```

Node、Bun、Python 这类原生程序按 Microsoft C 运行库的规则给参数加引号、拼成 Windows 命令行。Git Bash 底下的 MSYS2 运行时被这类程序启动时，却按自己的规则拆分命令行：带引号的参数（`-c` 后面的命令几乎总是带引号）里，每一串反斜杠都会减半，奇数个时向上取整（紧挨在 `"` 前面的除外），参数超过 8,186 个字符的部分直接丢掉，不报错。这一步发生在 bash 解析之前，所以单引号、带引号的 heredoc 都挡不住，bash 往往照样以 0 退出。设置 `MSYS=noglob` 能保住反斜杠和长度，但 `\"` 又会出错。

上游报告：git-for-windows/git [#6517](https://github.com/git-for-windows/git/issues/6517)（反斜杠、`MSYS=noglob`）、[#6516](https://github.com/git-for-windows/git/issues/6516)（截断），msys2/msys2-runtime [#178](https://github.com/msys2/msys2-runtime/issues/178)（`MSYS=noglob`）。工具这边的报告：anthropics/claude-code [#85856](https://github.com/anthropics/claude-code/issues/85856)、[#102533](https://github.com/anthropics/claude-code/issues/102533)、[#85111](https://github.com/anthropics/claude-code/issues/85111)，anomalyco/opencode [#57278](https://github.com/anomalyco/opencode/issues/57278)。

## 原理

```
工具   --  bash.exe -c <命令>  -->  gitbash-shim（bash.exe）
shim   --  <命令> 放进环境变量 GITBASH_SHIM_SCRIPT，
           命令行上只写一段固定的启动脚本  -->  Git 的 bin\bash.exe
bash   --  执行 eval "$GITBASH_SHIM_SCRIPT"
```

shim 是原生程序，拿到的参数和工具写的一字不差。它把 `-c` 的命令通过环境变量交给 bash，MSYS2 运行时不会重新解析环境变量；命令行上只有一小段固定的启动脚本，负责执行这个变量，并把它从后续命令的环境里去掉。其余参数按 MSYS2 能无损还原的格式重新加引号。它启动的是 Git 的 `bin\bash.exe`，也就是先设好 `MSYSTEM` 和 `PATH` 再启动 bash 的那个包装程序。

另外它还会：

- 把 bash 放进一个 Windows 作业对象（Job Object）。工具杀掉 shim 时（比如超时），bash 和它启动的所有进程一起退出。bash 正常退出后仍在运行的后台进程不受影响。
- bash 运行期间把控制台代码页切成 UTF-8，结束后恢复，这样 bash 启动的 cmd、PowerShell 等原生程序输出的是 UTF-8。
- 把自己的标准句柄显式交给 bash，所以用 detached 方式启动它的工具（Claude Code 就是）也能拿到输出。

## 为什么文件名必须是 bash.exe

两个工具都按文件名判断 shell 的类型。opencode 据此决定传什么参数、用哪种语法解析命令。Claude Code 只在文件名是 `bash.exe`、`sh.exe`、`bash` 或 `sh` 时才采用 `CLAUDE_CODE_GIT_BASH_PATH`，否则不提示地改用它自己找到的 Git Bash（[文档](https://code.claude.com/docs/en/env-vars)）。

## 查找 Git Bash

shim 按下面的顺序找，用第一个合格的：

1. `GITBASH_SHIM_TARGET`（如果设了）
2. 注册表 `Software\GitForWindows` 下的 `InstallPath`，先 `HKEY_CURRENT_USER`，后 `HKEY_LOCAL_MACHINE`
3. `GIT_INSTALL_ROOT`（scoop 会设）
4. `PATH` 里 `git.exe` 所在目录的上一级或上两级（`PATH` 里的相对路径会跳过）
5. `%ProgramFiles%\Git`
6. `%LocalAppData%\Programs\Git`（Git for Windows 安装程序在默认目录不可写时改推荐这里）

第 2 到 6 步找到的目录，要同时有 `bin\bash.exe`、`git-bash.exe` 和 `usr\bin\msys-2.0.dll` 才算 Git for Windows，所以不会误用 Cygwin 或单独安装的 MSYS2。都不合格时，shim 以退出码 127 退出，并输出：

```
gitbash-shim: Git Bash not found; set GITBASH_SHIM_TARGET to the full path of Git's bin\bash.exe
```

`GITBASH_SHIM_TARGET` 必须是指向 Git 的 `bin\bash.exe` 的绝对路径，这一项不做上面三个文件的检查。设了但不是绝对路径、文件不存在、或者指向 shim 自己时，shim 直接报错并以 127 退出，不再往下找，免得设错了也没人发现。设成空值等于没设。

## 换上以后会有什么不同

命令原样到达 bash，所以原先为了抵消“减半”而写的东西，现在会按字面执行：`grep` 模式里的 `\\b` 以前到达时是 `\b`，现在就是 `\\b`。claude-code [#85856](https://github.com/anthropics/claude-code/issues/85856) 里有用户统计过，日志里单引号模式中带 `\\` 的命令，多数都依赖减半。请检查那些为绕开这个问题而写的 agent 指令、hook 和保存下来的命令。

## 管不了的

- 命令里的回车符（CR）仍会丢失：不管命令怎么送进去，bash 自己都会丢掉它。
- 工具启动 shim 时仍然走 Windows 命令行，连同命令在内最多 32,767 个字符。
- 设了 `MSYS=noglob` 时，`-c` 的命令仍能完整到达，其他参数则不一定。
- `GITBASH_SHIM_TARGET` 可以指向任何文件，但只用 Git for Windows 的 `bin\bash.exe` 测试过。

## 其他做法

- [intact-bash-mod](https://github.com/GLEAM-Lab/intact-bash-mod) 是 Claude Code 的 mod。它把长于 200 个字符、或含 heredoc、或含成对反斜杠的命令写进 `%LOCALAPPDATA%\Temp\claude-cmd\` 下的文件，让 bash 去 source 这个文件；没有哪种途径能完整送达的命令，它直接拒绝。它只在 Claude Code 里起作用，写出的文件要自己删。gitbash-shim 适用于任何能指定 `bash.exe` 的工具，不写文件。
- [gash](https://github.com/hex2f/gash) 和 [niubash](https://github.com/unixwin/niubash) 是用 Rust 从头实现的 bash 兼容 shell，用来替换 Git Bash。用 gitbash-shim 时，命令仍由 Git for Windows 自带的 bash 和工具执行，变的只是命令送过去的方式。

## 核对下载的文件

在下载目录里用 Git Bash 执行 `sha256sum -c bash.exe.sha256`，应输出 `bash.exe: OK`。在 PowerShell 里，`(Get-FileHash bash.exe).Hash.ToLower()` 的结果应与 `bash.exe.sha256` 的第一段相同。

这个程序没有代码签名，由本仓库的 GitHub Actions 按打了标签的源码编译并测试。

## 编译和测试

用 MinGW-w64 的 gcc：

```
gcc -O2 -s -static -municode -o bash.exe gitbash-shim.c -lshell32 -ladvapi32
```

测试需要 Node。它把每个用例经 shim 执行一遍，再和 Git Bash 从脚本文件执行同样内容的结果比对。第二个参数是用来对照的 Git `bin\bash.exe` 路径；不给的话，用 shim 自己找到的那个。

```
node test.mjs bash.exe [Git 的 bin\bash.exe 路径]
```

`test-locate.mjs` 用一个跳过注册表的版本测试查找顺序：

```
gcc -O2 -s -static -municode -DSKIP_REGISTRY -o shim-noreg.exe gitbash-shim.c -lshell32 -ladvapi32
node test-locate.mjs shim-noreg.exe "C:\Program Files\Git\bin\bash.exe"
```

已在 Windows 11、Git for Windows 2.52.0 上用 Claude Code 2.1.321 实测。每次推送，GitHub Actions 都会在 `windows-latest` 上跑这两套测试。
