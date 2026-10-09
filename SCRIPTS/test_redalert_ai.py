"""Build and run AI regression scenarios using the installed MSVC compiler.

The target-selection tests compile Greatest_Threat directly from TECHNO.CPP.
Tactical and harvester fixtures compile the actual controllers against controlled
worlds; native unit/harvest functions and damage responses are extracted directly.
These checks exercise game decisions without requiring installed game assets.
"""
from pathlib import Path
import argparse
import json
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
    parser.add_argument('--test', action='append', choices=['ai_policy_test', 'target_selection_test', 'house_decisions_test', 'tactics_test', 'harvester_test', 'expansion_test'], help='Run only the selected regression fixture; repeat to select multiple fixtures.')
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
    (output / 'house_decisions.inc').write_text('#include "AILOG.H"\n' + '\n'.join(implementations), encoding='ascii')
    tactics = (root / 'REDALERT/AITACTICS.CPP').read_text(encoding='ascii')
    (output / 'tactics_controller.inc').write_text(tactics.replace('#include "FUNCTION.H"', '// Controlled engine fixture supplied by tactics_test.cpp.'), encoding='ascii')
    harvest = (root / 'REDALERT/HARVESTAI.CPP').read_text(encoding='ascii')
    (output / 'harvest_controller.inc').write_text(harvest.replace('#include "FUNCTION.H"', '// Controlled engine fixture supplied by harvester_test.cpp.'), encoding='ascii')
    unit_source = (root / 'REDALERT/UNIT.CPP').read_bytes().decode('cp1252').replace('\r\n', '\n')
    harvest_methods = []
    for signature in [
        'int UnitClass::Tiberium_Check(', 'bool UnitClass::Goto_Tiberium(',
        'bool UnitClass::Harvesting(', 'int UnitClass::Mission_Harvest(',
        'BuildingClass* UnitClass::Tiberium_Unload_Refinery(',
        'void UnitClass::ReconsiderRefinery('
    ]:
        start = unit_source.index(signature)
        end = unit_source.index('\n}\n', start) + 3
        harvest_methods.append(unit_source[start:end])
    (output / 'harvest_unit_methods.inc').write_text('\n'.join(harvest_methods), encoding='ascii')
    start = unit_source.index('\t\tbool harvest_retreat = Strength < previous_strength')
    end = unit_source.rfind('\n\t\t/*', start, unit_source.index('Computer controlled harvester', start))
    (output / 'harvest_damage_response.inc').write_text(unit_source[start:end], encoding='ascii')
    start = unit_source.index('\t\tbool harvest_retreat = Strength < previous_strength')
    condition = unit_source.index('\t\tif (!harvest_retreat', start)
    end = unit_source.index(' {', condition)
    guard = unit_source[start:condition] + 'return ' + unit_source[condition:end].strip()[3:] + ';\n'
    (output / 'expansion_damage_guard.inc').write_text(guard, encoding='ascii')
    start = house.index('\t\t// Restore missing military facilities')
    end = house.index('\n\t\t// A threatened approach', start)
    essential_priority = house[start:end]
    start = house.index('\t\t// Funded mining and production infrastructure')
    end = house.index('\n\t\t// Reserve reinforcements before optional construction', start)
    (output / 'expansion_building_priority.inc').write_text(essential_priority + '\n' + house[start:end], encoding='ascii')
    start = end
    end = house.index('\n\t\t//Lets do the build stuff:', start)
    (output / 'expansion_building_budget.inc').write_text(house[start:end], encoding='ascii')
    start = house.index('\t\t//All done. Lets pick one to build:', end)
    end = house.index('\n\t}\n\treturn(TICKS_PER_SECOND);', start)
    (output / 'expansion_building_selection.inc').write_text(house[start:end], encoding='ascii')
    start = house.index('\t\t//dog house')
    end = house.index('\n\t\t//Soviet barracks', start)
    (output / 'expansion_kennel_choice.inc').write_text(house[start:end], encoding='ascii')
    start = house.index('\t\t//Build Silo if storage above x %')
    end = house.index('\n\t\t//All done. Lets pick one to build:', start)
    (output / 'expansion_storage_choice.inc').write_text(house[start:end], encoding='ascii')
    for signature, filename in [
        ('UrgencyType HouseClass::Check_Raise_Money(void) const', 'expansion_money_check.inc'),
        ('bool HouseClass::AI_Raise_Money(UrgencyType urgency) const', 'expansion_money_sale.inc')
    ]:
        start = house.index(signature)
        end = house.index('\n}\n', start) + 3
        (output / filename).write_text(house[start:end], encoding='ascii')
    expansion = (root / 'REDALERT/AIEXPANSION.CPP').read_text(encoding='ascii')
    (output / 'expansion_controller.inc').write_text(expansion.replace('#include "FUNCTION.H"', '// Controlled engine fixture supplied by expansion_test.cpp.'), encoding='ascii')
    mcv_methods = []
    for signature in ['bool UnitClass::Goto_Clear_Spot(', 'int UnitClass::Mission_Guard(']:
        start = unit_source.index(signature)
        end = unit_source.index('\n}\n', start) + 3
        mcv_methods.append(unit_source[start:end])
    (output / 'expansion_unit_methods.inc').write_text('\n'.join(mcv_methods), encoding='ascii')
    start = unit_source.index('\t\t\tMark(MARK_UP);', unit_source.index('bool UnitClass::Try_To_Deploy('))
    end = unit_source.index('\n\t\t\tif (!BuildingTypeClass', start)
    (output / 'expansion_deploy_check.inc').write_text(unit_source[start:end], encoding='ascii')
    start = text.index('\tint divisor = hptr->Factory_Count(', text.index('int TechnoTypeClass::Time_To_Build('))
    end = text.index('\n\treturn(time);', start)
    (output / 'expansion_factory_bonus.inc').write_text(text[start:end], encoding='ascii')
    factory_header = (root / 'REDALERT/FACTORY.H').read_text(encoding='ascii')
    start = factory_header.index('int Remaining_Cost(')
    end = factory_header.index('}', start) + 1
    (output / 'expansion_remaining_cost.inc').write_text(factory_header[start:end], encoding='ascii')
    environment = msvc_environment()
    environment['AIBOOST_LOG'] = '0'
    selected_tests = args.test or ['ai_policy_test', 'target_selection_test', 'house_decisions_test', 'tactics_test', 'harvester_test', 'expansion_test']
    for test in selected_tests:
        executable = output / (test + '.exe')
        subprocess.run([
            environment['AI_TEST_COMPILER'], '/nologo', '/EHsc', '/W4', '/WX', '/Od', '/MT',
            '/I' + str(output), '/I' + str(root / 'REDALERT'), '/Fo' + str(output) + os.sep,
            '/Fe' + str(executable), str(root / ('tests/' + test + '.cpp')), str(root / 'REDALERT/AILOG.CPP')
        ], cwd=output, env=environment, check=True)
        subprocess.run([str(executable)], cwd=output, env=environment, check=True, timeout=60)
        if test == 'tactics_test':
            logged = subprocess.run(
                [str(executable), '--strike-log'], cwd=output,
                env=dict(environment, AIBOOST_LOG='1'), check=True, timeout=60,
                capture_output=True, text=True, encoding='utf-8'
            )
            log_path = Path(json.loads(logged.stdout)['log_path'])
            records = [json.loads(line) for line in log_path.read_text(encoding='utf-8').splitlines()]
            cancelled = [row['data'] for row in records if row['event'] == 'strike_cancel']
            assert {'advance_timeout', 'target_destroyed_or_missing', 'economy_not_ready'} <= {
                row['reason'] for row in cancelled
            }, cancelled
            assert all(isinstance(row.get('reason'), str) and row['reason'] for row in cancelled), cancelled
            assert all({'live_members', 'power', 'ready_count', 'ready_power', 'batch_members',
                        'waypoint', 'progress_wait_ticks', 'frontline_cell', 'frontline_preserved'} <= row.keys()
                       for row in cancelled), cancelled
            assert any(row['reason'] == 'advance_timeout' and row['frontline_preserved']
                       and row['frontline_cell'] % 128 >= 60 for row in cancelled), cancelled
            assert any(row['event'] == 'strike_started' and row['data']['resumed_frontline']
                       and row['data']['home'] % 128 >= 57 for row in records), records
            assert any(row['event'] == 'strike_batch_ready' and row['data']['members'] == 40
                       and row['data']['batch_members'] == 12 for row in records), records
            print('6 native frontline cancellation log checks passed.', flush=True)


if __name__ == '__main__':
    main()
