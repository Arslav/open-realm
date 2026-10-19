#!/usr/bin/env python3
"""Extend a supplied Battle Simulator v0.19d archive without dropping original members."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

ROSTER = (
    (24, 'hfoo', 'ogru'), (16, 'hrif', 'ohun'), (12, 'hkni', 'orai'),
    (12, 'earc', 'ugho'), (12, 'esen', 'ucry'), (8, 'edry', 'uban'),
    (6, 'hmpr', 'oshm'), (6, 'hsor', 'unec'), (2, 'Hamg', 'Obla'), (2, 'Ewar', 'Ulic'),
)
BUILDINGS = ('hhou', 'hbar', 'hgtw', 'otrb', 'obar', 'unpl', 'uzig', 'etol')


def combat_players(data, center=False):
    """Declare both army owners in the v25 W3I; spell hostility ignores unused slots."""
    if struct.unpack_from('<I', data)[0] != 25:
        raise ValueError('Expected the Battle Simulator version-25 W3I schema')
    offset = 12
    def strings(count):
        nonlocal offset
        for _ in range(count):
            offset = data.index(b'\0', offset)+1
    strings(4)
    offset += 48+8+4+1+4  # camera, playable size, flags, tileset, loading background
    strings(4)  # loading model/text/title/subtitle
    offset += 4  # data set
    strings(4)  # prologue model/text/title/subtitle
    offset += 24  # fog and weather
    strings(1)  # sound environment
    offset += 5  # light tileset and water tint
    start = offset
    count, = struct.unpack_from('<I', data, offset)
    offset += 4
    numbers, positions = set(), {}
    for _ in range(count):
        number, = struct.unpack_from('<I', data, offset)
        numbers.add(number)
        offset += 16
        strings(1)
        positions[number] = offset
        offset += 16
    if center:
        data = bytearray(data)
        struct.pack_into('<2f', data, positions[0], 4600, -4600)
        data = bytes(data)
    additions = b''
    for number, race, x in ((2, 1, 4100), (4, 2, 5100)):
        if number in numbers:
            raise ValueError(f'Expected unused army slot {number}')
        additions += struct.pack('<4I', number, 2, race, 1)+f'Combat team {number}'.encode()+b'\0'
        additions += struct.pack('<2f2I', x, -4600, 0, 0)
    return data[:start]+struct.pack('<I', count+2)+data[start+4:offset]+additions+data[offset:]


def extend_script(original, phase=None, demo=False):
    """Inject helpers before main so retail JASS declaration order remains valid."""
    source = Path(__file__).with_name('combat_bench.j').read_text()
    _, _, source = source.partition('globals\n')
    globals_, _, functions = source.partition('endglobals\n')
    if not functions or original.count(b'endglobals') != 1 or b'function Combat_' in original:
        raise ValueError('Expected an unmodified map script with one globals block')
    config, total = [], 0
    for i, (count, left, right) in enumerate(ROSTER):
        total += count
        config.extend((f"    set combat_types[{i}] = '{left}'", f"    set combat_types[{i+10}] = '{right}'",
                       f'    set combat_limits[{i}] = {total}'))
    assert total == 100
    props = []
    for i, rawcode in enumerate(BUILDINGS):
        x, y = 3500 + (i % 2) * 2200, -5100 + (i // 2) * 330
        props.extend((f"    set u = CreateUnit(Player(PLAYER_NEUTRAL_PASSIVE), '{rawcode}', {x}.0, {y}.0, 270.0)",
                      '    call SetUnitAcquireRange(u, 0.0)'))
    functions = functions.replace('// ROSTER', '\n'.join(config)).replace('// BUILDINGS', '\n'.join(props))
    script = original.replace(b'endglobals', globals_.encode() + b'endglobals', 1)
    if demo:
        start = b'call DefineStartLocation( 0, 6464.0, 9088.0 )'
        if script.count(start) != 1:
            raise ValueError('Expected the original observer start location')
        script = script.replace(start, b'call DefineStartLocation( 0, 4600.0, -4600.0 )', 1)
    marker = b'function main takes nothing returns nothing'
    if script.count(marker) != 1:
        raise ValueError('Expected exactly one main function')
    entry = None
    if demo:
        functions += '''
function Combat_DemoMove takes nothing returns nothing
    call DestroyTimer(GetExpiredTimer())
    call Combat_Move()
    call Combat_Status()
endfunction
function Combat_DemoFight takes nothing returns nothing
    call DestroyTimer(GetExpiredTimer())
    call Combat_Fight()
    call Combat_Status()
endfunction
function Combat_Demo takes nothing returns nothing
    call DestroyTimer(GetExpiredTimer())
    call Combat_Setup()
    call TimerStart(CreateTimer(), 10.0, false, function Combat_DemoMove)
    call TimerStart(CreateTimer(), 30.0, false, function Combat_DemoFight)
    call TimerStart(CreateTimer(), 90.0, false, function Combat_Demo)
endfunction
'''
        entry = 'Combat_Demo'
    elif phase:
        verify = '''
function Combat_VerifyRoster takes nothing returns nothing
    local integer i = 0
    local integer slot = 0
    local integer kind = 0
    loop
        exitwhen i >= 200
        set slot = ModuloInteger(i, 100)
        set kind = 0
        loop
            exitwhen slot < combat_limits[kind]
            set kind = kind + 1
        endloop
        call BJassAssert(GetUnitTypeId(combat_units[i]) == combat_types[kind+(i/100)*10], "COMBAT roster identity index="+I2S(i)+" actual="+I2S(GetUnitTypeId(combat_units[i]))+" expected="+I2S(combat_types[kind+(i/100)*10]))
        call BJassAssert(GetUnitState(combat_units[i], UNIT_STATE_MAX_LIFE) > 0.0, "COMBAT authored unit health")
        set i = i + 1
    endloop
    call BJassAssert(combat_initial_hp > 10000.0, "COMBAT initial HP")
endfunction
function Combat_Verify takes nothing returns nothing
'''
        if phase == 'Fight':
            verify += '''    call BJassAssert(combat_ticks >= 39, "COMBAT simulation advanced")
    call BJassAssert(combat_deaths > 0, "COMBAT real deaths")
    call BJassAssert(combat_spends > 0, "COMBAT mana costs")
    call BJassAssert(combat_spells[0] > 0, "COMBAT Blizzard mana cost")
    call BJassAssert(combat_spells[1] > 0, "COMBAT Fan of Knives mana cost")
    call BJassAssert(combat_spells[2] > 0, "COMBAT Frost Nova mana cost level="+I2S(GetUnitAbilityLevel(combat_units[198], 'AUfn'))+" hp="+R2S(GetWidgetLife(combat_units[198]))+" mana="+R2S(GetUnitState(combat_units[198], UNIT_STATE_MANA))+" order="+I2S(GetUnitCurrentOrder(combat_units[198]))+" spellorders="+I2S(combat_orders))
    call BJassAssert(combat_spells[3] > 0, "COMBAT Bladestorm mana cost")
'''
        else:
            verify += '    call BJassAssert(combat_deaths == 0, "COMBAT allied stage stays alive")\n'
        # Map runs do not propagate timer-coroutine errors as process failures.
        # Require this exact terminal sentinel, after every assertion, in the audit log.
        verify += '    call BJassError("COMBAT_VERIFY_PASS")\nendfunction\n'
        functions += verify + f'\nfunction Combat_Auto takes nothing returns nothing\n    call Combat_{phase}()\n    call Combat_VerifyRoster()\n    call TimerStart(CreateTimer(), 40.0, false, function Combat_Verify)\nendfunction\n'
        entry = 'Combat_Auto'
    if entry:
        start = script.index(marker)
        end = script.index(b'endfunction', start)
        command = f'    call TimerStart(CreateTimer(), 1.0, false, function {entry})\n'
        if demo:
            command += '    call TimerStart(CreateTimer(), 5.0, true, function Combat_Status)\n'
        script = script[:end] + command.encode() + script[end:]
    return script.replace(marker, functions.encode() + b'\n' + marker, 1)


def build(base, output, tool, phase=None, demo=False):
    """Repack every listed member, then byte-verify the unchanged assets."""
    def mpq(archive, command, *args):
        return subprocess.check_output([str(tool), '-mpq', str(archive), command, *map(str, args)])
    members = mpq(base, 'ls').decode().splitlines()
    if not members or any(name.endswith('/') for name in members):
        raise ValueError('Expected a flat map archive listing')
    originals = {name: mpq(base, 'cat', name) for name in members}
    name = next(name for name in members if name.lower() == 'war3map.j')
    replacements = {name: extend_script(originals[name], phase, demo)}
    info = next(name for name in members if name.lower() == 'war3map.w3i')
    replacements[info] = combat_players(originals[info], demo)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='wc3-combat-') as temp:
        command = [str(tool), '-mpq', str(output), 'pack']
        for i, member in enumerate(members):
            path = Path(temp) / str(i)
            path.write_bytes(replacements.get(member, originals[member]))
            command.extend((str(path), member))
        subprocess.run(command, check=True)
    # Keep an ordinary retail-compatible map user header before the MPQ.
    header = b'HM3W' + struct.pack('<I', 0) + b'Combat Bench 200\0' + struct.pack('<II', 0x1c69, 3)
    output.write_bytes(header.ljust(512, b'\0') + output.read_bytes())
    actual = mpq(output, 'ls').decode().splitlines()
    if set(actual)-{'(listfile)'} != set(members)-{'(listfile)'}:
        raise RuntimeError('Archive member set changed')
    for member, data in originals.items():
        if mpq(output, 'cat', member) != replacements.get(member, data):
            raise RuntimeError(f'Archive verification failed: {member}')
    metadata = {'base_sha256': hashlib.sha256(base.read_bytes()).hexdigest(),
                'sha256': hashlib.sha256(output.read_bytes()).hexdigest(), 'members': len(members),
                'changed_members': sorted(replacements), 'units': 200, 'unit_types': 20, 'trees': 64, 'buildings': 8,
                'roster': [{'count_per_team': count, 'left': left, 'right': right} for count, left, right in ROSTER],
                'auto_phase': phase, 'demo': demo}
    output.with_suffix('.json').write_text(json.dumps(metadata, indent=2)+'\n')
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True, help='Unmodified BattleSimulatorV0.19d.w3x')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--tool', type=Path, default=Path('build/bin/mpqtool'))
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--auto-phase', choices=('Setup', 'Move', 'Fight'), help='Run assertions; require COMBAT_VERIFY_PASS sentinel in the audit log')
    modes.add_argument('--demo', action='store_true', help='Repeat 10s idle, 20s movement, 60s combat without console input')
    args = parser.parse_args()
    if args.base.resolve() == args.output.resolve():
        parser.error('Output must differ from the original map')
    print(json.dumps(build(args.base, args.output, args.tool, args.auto_phase, args.demo), indent=2))


if __name__ == '__main__':
    main()
