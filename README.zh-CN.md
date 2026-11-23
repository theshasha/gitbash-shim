# gitbash-shim

[English](README.md) | 简体中文

在 Windows 上，opencode、Claude Code 这类工具通过 Git Bash 执行命令时，命令会被悄悄改坏：

- 成对的反斜杠少一半，`\\` 变成 `\`
- 超过约 8000 个字符的命令被截断

gitbash-shim 是一个放在 Git Bash 前面的 `bash.exe`，让命令原样到达。

## 安装

1. 从 [Releases](https://github.com/theshasha/gitbash-shim/releases) 下载 `bash.exe`，单独放一个目录，比如 `C:\tools\gitbash-shim\`。不要改名，也不要用它覆盖 Git 自带的 `bash.exe`。
2. 让工具用它。

   opencode，在 `opencode.jsonc` 里：

   ```jsonc
   {
     "shell": "C:/tools/gitbash-shim/bash.exe"
   }
   ```

   Claude Code，在 `~/.claude/settings.json` 里：

   ```json
   {
     "env": {
       "CLAUDE_CODE_GIT_BASH_PATH": "C:\\tools\\gitbash-shim\\bash.exe"
     }
   }
   ```

3. 重启工具，让它执行 `printf '%s\n' 'a\\b'`。输出 `a\\b` 就是装好了，输出 `a\b` 说明没生效。

## 常见问题

**报错 `Git Bash not found`。** shim 没找到你的 Git。把环境变量 `GITBASH_SHIM_TARGET` 设成 Git 的 `bin\bash.exe` 的完整路径，比如 `C:\Program Files\Git\bin\bash.exe`。

**以前能用的命令现在不对了。** 以前为了抵消“少一半”而多写的反斜杠，现在要改回正常写法。

原理、限制、自己编译等见[详细说明](docs/details.zh-CN.md)。

## 许可证

[MIT](LICENSE)。个人项目，按现状提供。欢迎提 issue，但不保证回复。

## 友链

[LINUX DO](https://linux.do)
