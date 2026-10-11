"""Resolve local presets, prepare a project and call Studio's slicing CLI."""
import copy
import json
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET
import zipfile

from .geometry import arrange, metadata, names, preserve_with_cutouts, set_meta


def resolve_presets(config):
    root = Path(config['profile_root']).expanduser()
    files = {}
    for group in ('machine', 'process', 'filament'):
        for parent in (root / 'system/BBL' / group, root / 'user'):
            for path in sorted(parent.rglob('*.json')):
                if parent.name == 'user' and path.parent.name != group:
                    continue
                files[(group, path.stem)] = path
    cache, visiting = {}, set()

    def resolve(group, name):
        key = group, name
        if key in cache:
            return dict(cache[key])
        if key in visiting:
            raise ValueError('Preset inheritance cycle: ' + name)
        if key not in files:
            raise ValueError(f'Missing {group} preset: {name}. Check Settings in the console.')
        visiting.add(key)
        child = json.loads(files[key].read_text())
        result = resolve(group, child['inherits']) if child.get('inherits') else {}
        for include in child.get('include', []):
            result.update(resolve(group, include))
        result.update(child)
        result.pop('include', None)
        visiting.remove(key)
        cache[key] = result
        return dict(result)

    result = {}
    for group, name in config['presets'].items():
        value = resolve(group, name)
        value.update(type=group, name=name, **{'from': 'User'})
        result[group] = value
    if result['machine'].get('printer_model') != 'Bambu Lab A1 mini':
        raise ValueError('Select an A1 mini machine preset for this fleet.')
    if list(map(float, result['machine'].get('nozzle_diameter', []))) != [0.4]:
        raise ValueError('The library requires the established 0.4 mm nozzle.')
    return result


def prepare(source, folder, family, category, presets, overrides=None):
    folder.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(source) as archive:
        if 'Metadata/project_settings.config' not in archive.namelist():
            raise ValueError('This is a model-only 3MF. Save it as a slicer project before adding it.')
        config = json.loads(archive.read('Metadata/project_settings.config'))
        root = ET.fromstring(archive.read('Metadata/model_settings.config'))
        original = [{'metadata': metadata(p)} for p in root.findall('plate')]
        updates = preserve_with_cutouts(archive, root)
        shifts = arrange(archive, root, updates)
        for obj in root.findall('object'):
            inherited = metadata(obj).get('extruder', '1')
            for part in obj.findall('part'):
                if 'extruder' not in metadata(part):
                    set_meta(part, 'extruder', inherited)
        plates = names(family, category, original, root)
        if not plates:
            raise ValueError('Project contains no printable plates.')
        for plate in plates:
            if overrides and str(plate['index']) in overrides:
                plate['name'] = overrides[str(plate['index'])]
            element = next(p for p in root.findall('plate') if int(metadata(p)['plater_id']) == plate['index'])
            set_meta(element, 'plater_name', plate['name'])
        config.update(curr_bed_type='High Temp Plate', printable_area=['0x0', '180x0', '180x180', '0x180'],
                      printable_height='180', wipe_tower_x=['10'] * len(plates), wipe_tower_y=['140'] * len(plates))
        if 'different_settings_to_system' in config:
            config['different_settings_to_system'] = [''] * len(config['different_settings_to_system'])
        colors = config.get('filament_colour', ['#FFFFFF'])
        with zipfile.ZipFile(folder / 'input.3mf', 'w', zipfile.ZIP_DEFLATED) as target:
            for info in archive.infolist():
                data = updates.get(info.filename, archive.read(info.filename))
                if info.filename == 'Metadata/project_settings.config':
                    data = json.dumps(config).encode()
                elif info.filename == 'Metadata/model_settings.config':
                    data = ET.tostring(root, encoding='utf-8', xml_declaration=True)
                target.writestr(copy.copy(info), data)
    settings = []
    for group in ('machine', 'process'):
        path = folder / (group + '.json')
        path.write_text(json.dumps(presets[group]))
        settings.append(str(path))
    filaments = []
    for index, color in enumerate(colors):
        value = dict(presets['filament'], filament_colour=[color])
        path = folder / f'filament-{index + 1}.json'
        path.write_text(json.dumps(value))
        filaments.append(str(path))
    return dict(plates=plates, settings=settings, filaments=filaments, colors=colors, shifts=shifts)


def slice_project(config, folder, plan):
    argv = [config['slicer'], '--datadir', str(folder / 'slicer-data'), '--debug', '3',
            '--load-settings', ';'.join(plan['settings']), '--load-filaments', ';'.join(plan['filaments']),
            '--filament-colour', ';'.join(plan['colors']), '--wipe-tower-x', ','.join(['10'] * len(plan['plates'])),
            '--wipe-tower-y', ','.join(['140'] * len(plan['plates'])), '--slice', '0', '--arrange', '0',
            '--orient', '0', '--export-3mf', 'sliced.3mf', '--outputdir', str(folder), str(folder / 'input.3mf')]
    (folder / 'command.json').write_text(json.dumps(argv, indent=2))
    with (folder / 'slice.log').open('w') as log:
        process = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT)
        try:
            code = process.wait()
        except BaseException:
            if process.poll() is None:
                process.send_signal(2)
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.wait()
            raise
    if code:
        raise ValueError(f'Slicer exited {code}; see {folder / "slice.log"}')
