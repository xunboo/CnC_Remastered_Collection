"""Build and run AI regression scenarios using the installed MSVC compiler.

The target-selection tests compile Greatest_Threat directly from TECHNO.CPP.
They use a fake map to test the real traversal code without game assets.
"""
from pathlib import Path
import argparse
import os
import subprocess


def msvc_environment():
    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    installation = subprocess.check_output([
        str(vswhere), '-latest', '-products', '*', '-requires',
        'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'
    ], text=True).strip()
    if not installation:
        raise RuntimeError('Visual Studio C++ build tools are required.')
    toolsets = sorted((Path(installation) / 'VC/Tools/MSVC').glob('*'))
    if not toolsets:
        raise RuntimeError('No MSVC toolset was found.')
    toolset = toolsets[-1]
    kits = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Windows Kits/10'
    versions = sorted(path for path in (kits / 'Include').glob('*') if (path / 'ucrt').is_dir())
    if not versions:
        raise RuntimeError('A Windows SDK is required.')
    sdk = versions[-1]
    environment = {key.upper(): value for key, value in os.environ.items()}
    environment['PATH'] = str(toolset / 'bin/Hostx64/x86') + os.pathsep + environment.get('PATH', '')
    environment['INCLUDE'] = os.pathsep.join(str(path) for path in [toolset / 'include', sdk / 'ucrt', sdk / 'um', sdk / 'shared'])
    environment['LIB'] = os.pathsep.join(str(path) for path in [toolset / 'lib/x86', kits / 'Lib' / sdk.name / 'ucrt/x86', kits / 'Lib' / sdk.name / 'um/x86'])
    environment['AI_TEST_COMPILER'] = str(toolset / 'bin/Hostx64/x86/cl.exe')
    return environment


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target-source', type=Path, help='Alternative TECHNO.CPP for demonstrating the original regression.')
    parser.add_argument('--unit-source', type=Path, help='Alternative HOUSE.CPP for demonstrating the original economic regression.')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = root / 'build/ai-tests'
    output.mkdir(parents=True, exist_ok=True)
    source = args.target_source or root / 'REDALERT/TECHNO.CPP'
    text = source.read_bytes().decode('cp1252').replace('\r\n', '\n')
    start = text.index('TARGET TechnoClass::Greatest_Threat(ThreatType method) const')
    end = text.index('\n}\n', start) + 3
    (output / 'greatest_threat.inc').write_text(text[start:end], encoding='ascii')
    house = (root / 'REDALERT/HOUSE.CPP').read_bytes().decode('ascii').replace('\r\n', '\n')
    unit = args.unit_source.read_bytes().decode('ascii').replace('\r\n', '\n') if args.unit_source else house
    implementations = []
    for signature in [
        'static bool AI_Is_Opponent(', 'static bool AI_Can_Plan(',
        'static BuildingClass * AI_Chrono_Target(', 'int HouseClass::AI_Unit_Weight(',
        'int HouseClass::AI_Unit(void)', 'void HouseClass::AI_StrategySwitcher(void)'
    ]:
        source_text = unit if signature == 'int HouseClass::AI_Unit(void)' else house
        start = source_text.index(signature)
        end = source_text.index('\n}\n', start) + 3
        implementations.append(source_text[start:end])
    (output / 'house_decisions.inc').write_text('\n'.join(implementations), encoding='ascii')
    environment = msvc_environment()
    for test in ['ai_policy_test', 'target_selection_test', 'house_decisions_test']:
        executable = output / (test + '.exe')
        subprocess.run([
            environment['AI_TEST_COMPILER'], '/nologo', '/EHsc', '/W4', '/WX', '/Od', '/MT',
            '/I' + str(output), '/Fo' + str(output / (test + '.obj')),
            '/Fe' + str(executable), str(root / ('tests/' + test + '.cpp'))
        ], cwd=output, env=environment, check=True)
        subprocess.run([str(executable)], cwd=output, env=environment, check=True, timeout=10)


if __name__ == '__main__':
    main()
