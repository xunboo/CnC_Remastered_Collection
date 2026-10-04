"""Assemble and test the actual Win32 Red Alert sprite renderer.

--object tests an existing build object, useful for reproducing a shipped crash.
--assembly-source allows testing an original or patched source independently.
"""
from pathlib import Path
import argparse
import os
import subprocess
from test_redalert_ai import msvc_environment


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group()
    inputs.add_argument('--object', type=Path)
    inputs.add_argument('--assembly-source', type=Path)
    parser.add_argument('--width', type=int, help='Run just the clipped terrain-shadow case at this width.')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = (args.output or root / 'build/renderer-tests').resolve()
    output.mkdir(parents=True, exist_ok=True)
    environment = msvc_environment()
    tools = Path(environment['AI_TEST_COMPILER']).parent
    # Match the DLL's v143 build when it is installed alongside a newer toolset.
    toolsets = tools.parents[3]
    v143 = sorted(path for path in toolsets.glob('14.4*') if path.is_dir())
    if v143:
        toolset = v143[-1]
        previous = tools.parents[2]
        tools = toolset / 'bin/Hostx64/x86'
        for variable in ['PATH', 'INCLUDE', 'LIB']:
            environment[variable] = environment[variable].replace(str(previous), str(toolset))
    compiler = tools / 'cl.exe'
    assembler = tools / 'ml.exe'
    assembly_object = args.object.resolve() if args.object else output / 'KEYFBUFF.obj'
    if not args.object:
        source = (args.assembly_source or root / 'REDALERT/KEYFBUFF.ASM').resolve()
        subprocess.run([str(assembler), '/nologo', '/c', '/coff', '/Cp', '/Zm',
                        '/Fo' + str(assembly_object), str(source)],
                       cwd=output, env=environment, check=True)
    executable = output / 'renderer_test.exe'
    subprocess.run([str(compiler), '/nologo', '/EHsc', '/W4', '/WX', '/Od', '/MT',
                    '/Fo' + str(output / 'renderer_test.obj'), '/Fe' + str(executable),
                    str(root / 'tests/renderer_test.cpp'), str(assembly_object)],
                   cwd=output, env=environment, check=True)
    command = [str(executable)]
    if args.width is not None:
        command.append(str(args.width))
    result = subprocess.run(command, cwd=output, env=environment, timeout=20)
    raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
