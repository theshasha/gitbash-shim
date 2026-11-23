// Runs each command through the shim the way OpenCode and Claude Code do (spawn shell ["-c", command])
// and compares with Git Bash running the same text from a script file, which is lossless.
// Usage: node test.mjs bash.exe [path of Git's bin\bash.exe]
import { spawn, spawnSync } from "node:child_process"
import { copyFileSync, mkdirSync, mkdtempSync, realpathSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import path from "node:path"

const shim = path.resolve(process.argv[2])
const env = { ...process.env }
delete env.MSYS
delete env.GITBASH_SHIM_TARGET
// The Git installation the shim forwards to, as the shim's own bash reports it.
const root = (run) => path.resolve(spawnSync(shim, ["-c", "cygpath -w /"], { encoding: "utf8", timeout: 30000, ...run }).stdout.trim())
const gitbash = process.argv[3] ? path.resolve(process.argv[3]) : path.join(root({ env }), "bin", "bash.exe")
const codePage = () => spawnSync("chcp.com", { encoding: "latin1" }).stdout
const initialCodePage = codePage()
const dir = realpathSync.native(mkdtempSync(path.join(tmpdir(), "shim-test-")))
writeFileSync(path.join(dir, "aXb"), "")

const long = Array.from({ length: 400 }, (_, i) => `line ${i}: "q" 'sq' a\\\\b \\n $HOME \`x\` C:\\Users\\me`).join("\n")
const cases = {
  heredoc: `cat <<'EOF'\ndq: "x" bs: a\\\\b nl: a\\nb path: C:\\Users\\me\nEOF`,
  inlineDq: `echo "inline dq: \\"z\\" tail"`,
  noSpace: `cat<<<'a\\\\b'`,
  globs: `echo 'sq' "dq" a*b ~ {a,b}`,
  chinese: `printf '%s\\n' '中文 \\\\ “引号”'`,
  trailing: `printf '%s\\n' 'end\\\\' end\\\\`,
  bsBeforeQuote: `printf '%s\\n' "a\\\\\\"b" 'c\\\\\\\\"d'`,
  dollarQuote: `echo $'tab\\there' \`echo hi\` "$(printf '%s' 'a\\\\b')"`,
  percentBang: `echo 100% '!x' "%PATH%"`,
  python: `python -c "print('a\\\\\\\\b', \\"q\\")"`,
  bare: `true`,
  long: `cat <<'EOF' | md5sum\n${long}\nEOF\necho len=${long.length}`,
}

let failed = 0
const check = (name, ok, detail = "") => {
  console.log(`${ok ? "PASS" : "FAIL"} ${name}${detail ? "  " + detail : ""}`)
  if (!ok) failed++
}

// Without the second argument the expected output comes from the bash the shim found, which proves nothing
// about the search; with it, the shim has to find that same installation by itself.
const same = (a, b) => a.toLowerCase() === b.toLowerCase()
if (process.argv[3]) check("finds the given Git Bash", same(path.join(root({ env }), "bin", "bash.exe"), gitbash), root({ env }))

const target = (value) => spawnSync(shim, ["-c", "echo ran"], { env: { ...env, GITBASH_SHIM_TARGET: value }, encoding: "utf8", timeout: 30000 })
const refused = (name, value, message) => {
  const run = target(value)
  check(`GITBASH_SHIM_TARGET ${name} is refused`, run.status === 127 && run.stdout === "" && run.stderr.includes(message), JSON.stringify([run.status, run.stdout, run.stderr]))
}
// A copy of Git's launcher outside any installation names its own folder when it finds no bash to start,
// which shows that the shim started this file and not the installed one.
const copy = path.join(dir, "copy", "bin", "bash.exe")
mkdirSync(path.dirname(copy), { recursive: true })
copyFileSync(gitbash, copy)
const copied = target(copy)
check("GITBASH_SHIM_TARGET comes before the installed Git", copied.stderr.toLowerCase().includes(path.dirname(copy).toLowerCase()), JSON.stringify([copied.status, copied.stdout, copied.stderr]))
check("GITBASH_SHIM_TARGET runs commands", target(gitbash).stdout === "ran\n")
check("empty GITBASH_SHIM_TARGET counts as unset", target("").stdout === "ran\n")
refused("that does not exist", path.join(dir, "missing", "bash.exe"), "is not a file")
refused("that is a folder", dir, "is not a file")
refused("that is relative", "bin\\bash.exe", "must be an absolute path")
refused("pointing to the shim", shim, "points to the shim itself")

for (const msys of [undefined, "noglob", "winsymlinks:nativestrict noglob"]) {
  const runEnv = msys ? { ...env, MSYS: msys } : env
  for (const [name, cmd] of Object.entries(cases)) {
    const file = path.join(dir, `${name}.sh`)
    writeFileSync(file, cmd)
    const expected = spawnSync(gitbash, [file], { cwd: dir, env, encoding: "utf8" })
    const actual = spawnSync(shim, ["-c", cmd], { cwd: dir, env: runEnv, encoding: "utf8" })
    const a = actual.stdout + actual.stderr
    const e = expected.stdout + expected.stderr
    const label = `${name} [MSYS=${msys ?? "unset"}]`
    check(label, a === e && actual.status === expected.status, a === e ? "" : `\n  expected: ${JSON.stringify(e)}\n  actual:   ${JSON.stringify(a)}`)
  }
}

const exit = spawnSync(shim, ["-c", "exit 7"], { env })
check("exit code", exit.status === 7, `status=${exit.status}`)

const leak = spawnSync(shim, ["-c", "env | grep -c GITBASH_SHIM_SCRIPT; bash -c 'echo ${GITBASH_SHIM_SCRIPT-unset}'"], { env, encoding: "utf8" })
check("no env leak", leak.stdout === "0\nunset\n", JSON.stringify(leak.stdout))

// cmd and PowerShell encode redirected output with the console code page, which the shim sets to UTF-8.
const native = spawnSync(shim, ["-c", `cmd //c 'echo 中文' && powershell -NoProfile -c "Write-Output '中文'"`], { env, encoding: "utf8" })
check("native output is UTF-8", native.stdout === "中文\r\n中文\r\n", JSON.stringify(native.stdout))

// A console shared with the caller gets its code page back after every run above.
const after = codePage()
check("code page restored", after === initialCodePage, JSON.stringify({ before: initialCodePage, after }))

// A detached caller (Claude Code) leaves the shim without a console; bash must still write to the shim's handles.
const detached = await new Promise((resolve) => {
  const child = spawn(shim, ["-c", "echo out; echo err >&2"], { env, detached: true, windowsHide: true })
  let out = ""
  child.stdout.on("data", (data) => (out += data))
  child.stderr.on("data", (data) => (out += data))
  child.on("close", () => resolve(out))
})
check("detached caller gets output", detached === "out\nerr\n", JSON.stringify(detached))

const interactive = spawnSync(shim, [], { env, encoding: "utf8", input: "echo 'from stdin a\\\\b'\n" })
check("no -c (stdin)", interactive.stdout === "from stdin a\\\\b\n", JSON.stringify(interactive.stdout))

const alive = (pid) => {
  try {
    process.kill(pid, 0)
    return true
  } catch {
    return false
  }
}

// Background processes survive a normal exit.
const bg = spawnSync(shim, ["-c", "sleep 20 >/dev/null 2>&1 </dev/null & cat /proc/$!/winpid"], { env, encoding: "utf8" })
const bgPid = Number(bg.stdout.trim())
check("background survives normal exit", bgPid > 0 && alive(bgPid), `pid=${bgPid}`)
if (bgPid > 0) spawnSync("taskkill", ["/PID", String(bgPid), "/F"])

// Killing the shim ends bash and the commands it started, both the way OpenCode
// kills on timeout (taskkill /T) and by terminating the shim alone.
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms))
const killers = {
  terminate: (child) => child.kill(),
  "taskkill /T": (child) => spawnSync("taskkill", ["/PID", String(child.pid), "/T", "/F"]),
}
for (const [how, kill] of Object.entries(killers)) {
  await new Promise((resolve) => {
    const child = spawn(shim, ["-c", "sleep 60 & cat /proc/$$/winpid /proc/$!/winpid; wait"], { env })
    let out = ""
    child.stdout.on("data", async (data) => {
      out += data
      const pids = out.trim().split(/\s+/).map(Number)
      if (pids.length < 2) return
      child.stdout.removeAllListeners("data")
      kill(child)
      for (let waited = 0; waited < 5000 && pids.some(alive); waited += 100) await sleep(100)
      const left = pids.filter(alive)
      check(`killed by ${how}: bash and sleep exit`, left.length === 0, `pids=${pids} alive=${left}`)
      for (const pid of left) spawnSync("taskkill", ["/PID", String(pid), "/F"])
      resolve()
    })
  })
}

rmSync(dir, { recursive: true, force: true })
console.log(failed ? `${failed} failed` : "all passed")
process.exit(failed ? 1 : 0)
