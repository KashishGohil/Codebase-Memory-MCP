r"""GREEN regression guard — Claude Code hook shims are .cmd wrappers on Windows.

On Windows, Cursor and Claude Code spawn hook commands without a shell. The
installer previously wrote extensionless bash scripts (`cbm-code-discovery-gate`)
into `~/.claude/hooks/`, which triggered the "Open with" dialog and blocked
workflows until the user picked an app.

The fix writes `.cmd` wrappers and points settings.json at the absolute `.cmd`
path so CreateProcess can execute them directly.

Exit code: 0 == pass, 1 == regression, 2 == setup error.

Usage:
    python test_hook_scripts.py <path-to-codebase-memory-mcp.exe>
"""
import json
import os
import subprocess
import sys
import tempfile


def run_install(binary, fake_home):
    env = dict(os.environ)
    env["USERPROFILE"] = fake_home
    env["HOME"] = fake_home
    env["APPDATA"] = os.path.join(fake_home, "AppData", "Roaming")
    env["LOCALAPPDATA"] = os.path.join(fake_home, "AppData", "Local")
    env["XDG_CONFIG_HOME"] = os.path.join(fake_home, ".config")
    env["PATH"] = os.path.dirname(binary) + os.pathsep + env.get("PATH", "")
    os.makedirs(os.path.join(fake_home, ".claude"), exist_ok=True)
    return subprocess.run([binary, "install", "-y"], capture_output=True, timeout=120, env=env)


def main():
    if len(sys.argv) < 2:
        print("usage: python test_hook_scripts.py <binary>")
        return 2
    binary = os.path.abspath(sys.argv[1])
    if not os.path.exists(binary):
        print("FAIL: binary not found: %s" % binary)
        return 2

    work = tempfile.mkdtemp(prefix="cbm_win_hooks_")
    try:
        result = run_install(binary, work)
        out = (result.stdout or b"").decode("utf-8", "replace")
        err = (result.stderr or b"").decode("utf-8", "replace")
        if result.returncode != 0:
            print("SETUP FAIL: install -y exit %d\n%s\n%s" % (result.returncode, out[:500], err[:500]))
            return 2

        gate = os.path.join(work, ".claude", "hooks", "cbm-code-discovery-gate.cmd")
        reminder = os.path.join(work, ".claude", "hooks", "cbm-session-reminder.cmd")
        settings = os.path.join(work, ".claude", "settings.json")

        for path in (gate, reminder, settings):
            if not os.path.isfile(path):
                print("FAIL: missing %s" % path)
                return 1

        with open(gate, "r", encoding="utf-8", errors="replace") as f:
            gate_body = f.read()
        if "hook-augment" not in gate_body or "@echo off" not in gate_body:
            print("FAIL: gate .cmd missing expected content")
            return 1

        with open(settings, "r", encoding="utf-8") as f:
            settings_doc = json.load(f)
        hooks = settings_doc.get("hooks", {}).get("PreToolUse", [])
        commands = []
        for entry in hooks:
            for h in entry.get("hooks", []):
                commands.append(h.get("command", ""))
        if not any("cbm-code-discovery-gate.cmd" in c for c in commands):
            print("FAIL: settings.json PreToolUse command missing .cmd wrapper: %s" % commands)
            return 1
        matchers = [entry.get("matcher", "") for entry in hooks]
        if not any(m == "Grep|Glob|Read" for m in matchers):
            print("FAIL: settings.json PreToolUse matcher not Grep|Glob|Read: %s" % matchers)
            return 1

        print("PASS: Windows hook .cmd wrappers installed and wired in settings.json")
        return 0
    finally:
        pass


if __name__ == "__main__":
    sys.exit(main())
