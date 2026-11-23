// Checks how the shim finds Git Bash when the registry has no answer. Needs a shim built with
// -DSKIP_REGISTRY, because every machine with an installed Git answers from the registry first.
// Usage: node test-locate.mjs shim-noreg.exe <path of Git's bin\bash.exe>
import { spawnSync } from "node:child_process"
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, realpathSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import path from "node:path"

const shim = path.resolve(process.argv[2])
const gitbash = path.resolve(process.argv[3])

const base = Object.fromEntries(
  Object.entries(process.env).filter(([name]) => !/^(path|msys|git_install_root|gitbash_shim_target)$/i.test(name)),
)
// Windows ignores the case of variable names, and bash hands them over in capitals.
const variable = (name) => Object.entries(base).find(([key]) => key.toLowerCase() === name.toLowerCase())[1]
const system = [path.join(variable("SystemRoot"), "System32"), variable("SystemRoot")].join(";")

// The long form of the path, which is how Git's launcher reports itself below.
const temp = realpathSync.native(mkdtempSync(path.join(tmpdir(), "shim-locate-")))
const run = (PATH, extra = {}, cwd = temp) => {
  const result = spawnSync(shim, ["-c", "cygpath -w /"], { env: { ...base, PATH, ...extra }, cwd, encoding: "utf8", timeout: 30000 })
  return { status: result.status, stdout: result.stdout, stderr: result.stderr }
}

let failed = 0
const check = (name, ok, detail) => {
  console.log(`${ok ? "PASS" : "FAIL"} ${name}${ok ? "" : "  " + JSON.stringify(detail)}`)
  if (!ok) failed++
}
const same = (a, b) => a.toLowerCase() === b.toLowerCase()

// A folder laid out like Git for Windows, so that what the shim picks does not depend on where the real Git
// is installed. Its bin\bash.exe is Git's launcher, which names its own folder when it finds no bash to start.
const fake = path.join(temp, "Git")
for (const file of ["cmd/git.exe", "mingw64/bin/git.exe", "git-bash.exe", "usr/bin/msys-2.0.dll"]) {
  mkdirSync(path.dirname(path.join(fake, file)), { recursive: true })
  writeFileSync(path.join(fake, file), "")
}
mkdirSync(path.join(fake, "bin"))
copyFileSync(gitbash, path.join(fake, "bin", "bash.exe"))
const takesFake = (name, result) => check(name, result.stderr.toLowerCase().includes(path.join(fake, "bin").toLowerCase()), result)

takesFake("git.exe in cmd on PATH", run(`${path.join(fake, "cmd")};${system}`))
takesFake("git.exe in mingw64\\bin on PATH", run(`${path.join(fake, "mingw64", "bin")};${system}`))
takesFake("quoted PATH entry", run(`"${path.join(fake, "cmd")}";${system}`))
takesFake("GIT_INSTALL_ROOT", run(system, { GIT_INSTALL_ROOT: fake }))
takesFake("GIT_INSTALL_ROOT comes before PATH", run(`${path.join(path.dirname(gitbash), "..", "cmd")};${system}`, { GIT_INSTALL_ROOT: fake }))

// With nothing else to go by, the shim takes Git from its default folders or reports that there is none.
const standard = [path.join(variable("ProgramFiles"), "Git"), path.join(variable("LOCALAPPDATA"), "Programs", "Git")].find((root) =>
  existsSync(path.join(root, "bin", "bash.exe")),
)
const fallsBack = (name, result) =>
  check(
    name,
    standard
      ? result.status === 0 && same(path.resolve(result.stdout.trim()), standard)
      : result.status === 127 && result.stderr.includes("Git Bash not found"),
    result,
  )
fallsBack(standard ? "default install folder" : "nothing found is reported", run(system))
fallsBack("git.exe in the current directory is ignored", run(system, {}, path.join(fake, "cmd")))
fallsBack("relative PATH entries are ignored", run(`.;..\\cmd;${system}`, {}, path.join(fake, "cmd")))
fallsBack("relative GIT_INSTALL_ROOT is ignored", run(system, { GIT_INSTALL_ROOT: "Git" }))

// Cygwin and a plain MSYS2 have bin\bash.exe next to git.exe but are not Git for Windows.
const other = path.join(temp, "other")
mkdirSync(path.join(other, "bin"), { recursive: true })
writeFileSync(path.join(other, "bin", "git.exe"), "")
copyFileSync(gitbash, path.join(other, "bin", "bash.exe"))
fallsBack("a bash that is not Git for Windows is ignored", run(`${path.join(other, "bin")};${system}`))

rmSync(temp, { recursive: true, force: true })
console.log(failed ? `${failed} failed` : "all passed")
process.exit(failed ? 1 : 0)
