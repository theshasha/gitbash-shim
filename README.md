# gitbash-shim

English | [简体中文](README.zh-CN.md)

On Windows, when tools like opencode and Claude Code run commands through Git Bash, the commands are silently altered:

- pairs of backslashes are halved: `\\` becomes `\`
- commands longer than about 8,000 characters are cut off

gitbash-shim is a `bash.exe` that sits in front of Git Bash and gets commands there exactly as written.

## Install

1. Download `bash.exe` from [Releases](https://github.com/theshasha/gitbash-shim/releases) and put it in a folder of its own, for example `C:\tools\gitbash-shim\`. Don't rename it, and don't put it over Git's own `bash.exe`.
2. Point your tool at it.

   opencode, in `opencode.jsonc`:

   ```jsonc
   {
     "shell": "C:/tools/gitbash-shim/bash.exe"
   }
   ```

   Claude Code, in `~/.claude/settings.json`:

   ```json
   {
     "env": {
       "CLAUDE_CODE_GIT_BASH_PATH": "C:\\tools\\gitbash-shim\\bash.exe"
     }
   }
   ```

3. Restart the tool and have it run `printf '%s\n' 'a\\b'`. If it prints `a\\b`, the shim is working; if it prints `a\b`, the shim is not in use.

## Troubleshooting

**`Git Bash not found`.** The shim couldn't find your Git. Set the environment variable `GITBASH_SHIM_TARGET` to the full path of Git's `bin\bash.exe`, for example `C:\Program Files\Git\bin\bash.exe`.

**Commands that used to work now behave differently.** Backslashes that were doubled to make up for the halving now need to be written normally.

For how it works, its limits and building it yourself, see the [details](docs/details.md).

## License

[MIT](LICENSE). A personal project, provided as is. Issues are welcome, but replies are not guaranteed.

## Links

[LINUX DO](https://linux.do)
