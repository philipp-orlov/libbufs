# Copyright (c) 2026 Philipp Orlov

"""Runs the team example in every language: each language writes the message, and each language reads every message back; all
of them must print expected_output.txt. A language whose tools are missing is skipped.

    python3 run_examples.py --bufsc bufsc --cpp team [--python python3] [--node node --esbuild esbuild] [--dotnet dotnet] [--work DIR]
"""

import argparse
import os
import shutil
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
parser = argparse.ArgumentParser()
parser.add_argument("--bufsc", required=True)
parser.add_argument("--cpp", required=True)
parser.add_argument("--python", default=sys.executable)
parser.add_argument("--node")
parser.add_argument("--esbuild")
parser.add_argument("--dotnet")
parser.add_argument("--work", default=os.path.join(here, "..", "build-examples"))
args = parser.parse_args()
work = os.path.abspath(args.work)
shutil.rmtree(work, ignore_errors=True)
os.makedirs(work)
expected = open(os.path.join(here, "expected_output.txt")).read()
schema = os.path.join(here, "team.bufs")


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=300, **kw)
    if r.returncode:
        sys.exit(f"{' '.join(cmd)}\n{r.stdout}{r.stderr}")
    return r.stdout


def generate(flag, out):
    os.makedirs(out, exist_ok=True)
    run([args.bufsc, "--no-cpp", flag, "-o", out, schema])


tools = {"cpp": lambda mode, f: [args.cpp, mode, f]}

generate("--py", os.path.join(work, "python"))
tools["python"] = lambda mode, f: [args.python, os.path.join(here, "python", "team.py"), mode, f]
os.environ["PYTHONPATH"] = os.path.join(work, "python")

if args.node and args.esbuild:
    src = os.path.join(work, "ts")
    shutil.copytree(os.path.join(here, "ts"), src, ignore=shutil.ignore_patterns("gen"))
    generate("--ts", os.path.join(src, "gen"))
    bundle = os.path.join(work, "team.mjs")
    run([args.esbuild, os.path.join(src, "team.ts"), "--bundle", "--platform=node", "--format=esm", f"--outfile={bundle}",
         "--alias:@bufs/runtime=" + os.path.abspath(os.path.join(here, "..", "ts", "src", "bufs.ts")), "--log-level=warning"])
    tools["typescript"] = lambda mode, f: [args.node, bundle, mode, f]

if args.dotnet:
    project = os.path.join(work, "cs")
    shutil.copytree(os.path.join(here, "cs", "People"), project, ignore=shutil.ignore_patterns("bin", "obj", "gen"))
    generate("--cs", os.path.join(project, "gen"))
    csproj = os.path.join(project, "People.csproj")
    text = open(csproj).read().replace("../../../cs/Bufs/Bufs.csproj", os.path.abspath(os.path.join(here, "..", "cs", "Bufs", "Bufs.csproj")))
    open(csproj, "w").write(text)
    os.environ.setdefault("DOTNET_ROOT", os.path.dirname(os.path.realpath(args.dotnet)))   # the apphost of the built program needs it
    os.environ.update(DOTNET_CLI_TELEMETRY_OPTOUT="1", DOTNET_NOLOGO="1", DOTNET_SYSTEM_GLOBALIZATION_INVARIANT="1")
    run([args.dotnet, "build", project, "-o", os.path.join(work, "csbin"), "-nologo", "-v", "q"])
    tools["csharp"] = lambda mode, f: [os.path.join(work, "csbin", "People"), mode, f]

failures = 0
for writer, write in tools.items():
    path = os.path.join(work, f"{writer}.bin")
    run(write("write", path))
    for reader, read in tools.items():
        out = run(read("read", path))
        ok = out == expected
        failures += not ok
        print(f"{'ok' if ok else 'FAILED'} - {writer} writes, {reader} reads")
        if not ok:
            print(out)
sys.exit(1 if failures else 0)
