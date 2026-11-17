"""Driver help routing, terminal color, command coverage and effect-free execution."""
import errno
import os
from pathlib import Path
import pty
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CORE = set("help plugins extract extension extension-replay formula-test print-report "
           "forecast quote price optimize-model estimate backtest recommend optimize demand adjust".split())
DRAGNET = set("daemon discover discover-replay discover-many capture snapshot extract-snapshot "
              "flatten find find-rule find-rule-replay collect collect-replay browse "
              "openai-account model-extract".split())
POINDEXTER = set("review reject revise ledger history replay series cohort forecast-ledger "
                 "price-ledger sheet tui gui review-collection replay-collection".split())
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def environment():
    env = dict(os.environ)
    for key in ("NO_COLOR", "FORCE_COLOR", "SHY_NATIVE_PLUGINS", "SHY_EXTENSIONS"):
        env.pop(key, None)
    env["TERM"] = "xterm-256color"
    return env


def run(*args, code=0, env=None):
    result = subprocess.run([ROOT / "shylock", *args], cwd=ROOT,
                            env=environment() if env is None else env,
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == code, (args, result.returncode, result.stdout, result.stderr)
    if code:
        assert result.stderr and not result.stdout, (args, result)
    else:
        assert not result.stderr, (args, result.stderr)
    return result.stdout


def terminal(env):
    master, slave = pty.openpty()
    try:
        result = subprocess.run([ROOT / "shylock", "adjust", "--help"], cwd=ROOT,
                                env=env, stdout=slave, stderr=subprocess.PIPE, timeout=10)
        os.close(slave)
        slave = -1
        assert result.returncode == 0 and not result.stderr, result
        output = bytearray()
        while True:
            try:
                block = os.read(master, 4096)
            except OSError as exc:
                if exc.errno != errno.EIO:
                    raise
                break
            if not block:
                break
            output.extend(block)
        return output.decode().replace("\r\n", "\n")
    finally:
        os.close(master)
        if slave >= 0:
            os.close(slave)


def main():
    overview = run("help")
    assert "\x1b" not in overview
    assert run() == run("--help") == run("-h") == overview
    names = re.findall(r"^  ([a-z][a-z-]*) {2,}", overview, re.M)
    assert len(names) == len(set(names)), names
    assert set(names) == CORE | DRAGNET | POINDEXTER, names
    fixture_examples = 0

    with tempfile.TemporaryDirectory(prefix="shylock-help-") as directory:
        # A help request must succeed even when runtime configs cannot be read,
        # including extension commands that normally bypass the main dispatcher.
        env = environment()
        env["SHY_NATIVE_PLUGINS"] = str(Path(directory) / "missing-native.json")
        env["SHY_EXTENSIONS"] = str(Path(directory) / "missing-extensions.json")
        for name in names:
            detail = run("help", name, env=env)
            assert detail.startswith(f"shylock {name}\n"), (name, detail)
            for heading in ("Usage", "Arguments", "Notes", "Examples", "Read more"):
                assert f"\n{heading}\n" in detail, (name, heading)
            examples = detail.split("\nExamples\n", 1)[1].split("\nRead more\n", 1)[0]
            assert examples.count("./shylock ") >= 3, (name, examples)
            assert f"./shylock {name}" in examples, (name, examples)
            expected = overview if name == "help" else detail
            assert run(name, "--help", env=env) == expected, name
            assert run(name, "-h", env=env) == expected, name
            assert run("--help", name, env=env) == detail
            assert run("-h", name, env=env) == detail
            # Execute examples backed entirely by shipped fixtures. Invoke argv
            # directly: redirections are documentation, never shell code. These
            # commands use saved inputs and do not modify a ledger or call models.
            if name in {"estimate", "backtest", "recommend", "adjust", "demand",
                        "optimize", "optimize-model", "forecast", "quote", "formula-test",
                        "discover", "discover-many", "snapshot", "extract", "browse"}:
                for line in examples.splitlines():
                    args = shlex.split(line)
                    if not args or args[0] != "./shylock":
                        continue
                    if ">" in args:
                        args = args[:args.index(">")]
                    paths = [a for a in args[2:] if a.endswith((".json", ".html", ".lua"))]
                    if paths and all((ROOT / p).is_file() for p in paths):
                        run(*args[1:])
                        fixture_examples += 1
        assert not list(Path(directory).iterdir())

    plain = run("help", "adjust")
    colored = run("help", "adjust", "--color=always")
    assert ANSI.sub("", colored) == plain
    assert len(set(ANSI.findall(colored))) >= 4  # Title, headings, examples, reset.
    assert run("adjust", "--color=never", "--help") == plain
    assert run("help", "--color=auto", "adjust") == plain
    env = environment()
    env["FORCE_COLOR"] = "1"
    assert run("help", "adjust", env=env) == colored
    assert run("help", "adjust", "--color=never", env=env) == plain
    env["NO_COLOR"] = "1"
    assert run("help", "adjust", env=env) == plain
    assert run("help", "adjust", "--color=always", env=env) == colored
    assert terminal(environment()) == colored
    assert terminal(env) == plain
    env = environment()
    env["TERM"] = "dumb"
    assert terminal(env) == plain
    env = environment()
    env["FORCE_COLOR"] = "0"
    assert terminal(env) == plain

    for args in (("help", "missing-command"), ("missing-command", "--help"),
                 ("help", "estimate", "extra"), ("estimate", "--help", "extra"),
                 ("help", "--color=invalid"), ("help", "--color="),
                 ("help", "--unknown"), ("help", ""),
                 ("help", "--color=always", "--color=never")):
        run(*args, code=2)
    assert fixture_examples >= 25, fixture_examples
    print(f"Help tests passed: {len(names)} commands, {fixture_examples} fixture examples, "
          "aliases, colors and effect-free routing.")


if __name__ == "__main__":
    main()
