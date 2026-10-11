"""Validate and export printer-compatible single-plate jobs."""
import copy
import hashlib
import json
import re
import xml.etree.ElementTree as ET
import zipfile

from .geometry import NS, metadata, set_meta


def xml(root):
    return ET.tostring(root, encoding='utf-8', xml_declaration=True)


def validate(path, config, presets, single=False):
    with zipfile.ZipFile(path) as z:
        if z.testzip():
            raise ValueError('Corrupt sliced archive: ' + str(path))
        values = json.loads(z.read('Metadata/project_settings.config'))
        for key, expected in [('printer_settings_id', config['presets']['machine']),
                              ('print_settings_id', config['presets']['process']),
                              ('printer_model', 'Bambu Lab A1 mini'), ('curr_bed_type', 'High Temp Plate')]:
            if values.get(key) != expected:
                raise ValueError(f'{path.name}: unexpected {key}: {values.get(key)}')
        if set(values['filament_settings_id']) != {config['presets']['filament']}:
            raise ValueError('Unexpected filament profile')
        if list(map(float, values['nozzle_diameter'])) != [0.4]:
            raise ValueError('Unexpected nozzle diameter')
        if float(values['layer_height']) != float(presets['process']['layer_height']):
            raise ValueError('Unexpected layer height')
        if set(map(float, values['filament_flow_ratio'])) != set(map(float, presets['filament']['filament_flow_ratio'])):
            raise ValueError('Unexpected filament flow ratio')
        plates = ET.fromstring(z.read('Metadata/slice_info.config')).findall('plate')
        if not plates or (single and len(plates) != 1):
            raise ValueError('Unexpected sliced plate count')
        for plate in plates:
            md = metadata(plate)
            if md.get('outside') != 'false' or md.get('printer_model_id') != 'N1':
                raise ValueError('Plate is outside the bed or uses another printer')
            member = 'Metadata/plate_' + md['index'] + '.gcode'
            data = z.read(member)
            if hashlib.md5(data).hexdigest() != z.read(member + '.md5').decode().strip().lower():
                raise ValueError('G-code digest mismatch')
            if b'; printer_model = Bambu Lab A1 mini' not in data:
                raise ValueError('G-code targets another printer')
            used = {int(f.get('id')): f.get('color', '')[:7].upper() for f in plate.findall('filament') if float(f.get('used_g', 0)) > 0}
            if not used:
                raise ValueError('Empty plate')
            for slot, color in used.items():
                if color != values['filament_colour'][slot - 1][:7].upper():
                    raise ValueError('Sliced filament colors differ from project')
        if single:
            model = ET.fromstring(z.read('3D/3dmodel.model'))
            if list(model.find('m:resources', NS)) or list(model.find('m:build', NS)):
                raise ValueError('A sliced-only job must not retain editable meshes')


def export(folder, plan, config, presets):
    with zipfile.ZipFile(folder / 'sliced.3mf') as z:
        contents = {name: z.read(name) for name in z.namelist()}
    slices = ET.fromstring(contents['Metadata/slice_info.config'])
    for plate in slices.findall('plate'):
        set_meta(plate, 'printer_model_id', 'N1')
    contents['Metadata/slice_info.config'] = xml(slices)
    settings = ET.fromstring(contents['Metadata/model_settings.config'])
    for plate in settings.findall('plate'):
        match = next(p for p in plan['plates'] if p['index'] == int(metadata(plate)['plater_id']))
        set_meta(plate, 'plater_name', match['name'])
    contents['Metadata/model_settings.config'] = xml(settings)
    prepared = folder / 'prepared.3mf'
    with zipfile.ZipFile(prepared, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, data in contents.items():
            z.writestr(name, data)
    validate(prepared, config, presets)
    outputs = []
    for plate in plan['plates']:
        index = str(plate['index'])
        member = 'Metadata/plate_' + index + '.gcode'
        single = copy.deepcopy(slices)
        for element in list(single.findall('plate')):
            if metadata(element)['index'] != index:
                single.remove(element)
        root = copy.deepcopy(settings)
        for tag in ('object', 'assemble'):
            for element in list(root.findall(tag)):
                root.remove(element)
        for element in root.findall('plate'):
            for instance in list(element.findall('model_instance')):
                element.remove(instance)
            if metadata(element)['plater_id'] != index:
                set_meta(element, 'gcode_file', '')
        model = re.sub(r'<resources>.*?</resources>', '<resources/>', contents['3D/3dmodel.model'].decode(), flags=re.S)
        model = re.sub(r'<build(?:\s[^>]*)?>.*?</build>', '<build/>', model, flags=re.S)
        rels = re.sub(r'/Metadata/plate_1(?=\.png|_small\.png)', '/Metadata/plate_' + index, contents['_rels/.rels'].decode())
        overrides = {'3D/3dmodel.model': model.encode(), 'Metadata/model_settings.config': xml(root),
                     'Metadata/slice_info.config': xml(single), '_rels/.rels': rels.encode(),
                     'Metadata/cut_information.xml': b'<?xml version="1.0"?><objects/>',
                     'Metadata/_rels/model_settings.config.rels': ('<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Target="/' + member + '" Id="rel-1" Type="http://schemas.bambulab.com/package/2021/gcode"/></Relationships>').encode()}
        path = folder / f'plate-{index}.gcode.3mf'
        with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
            for name, data in contents.items():
                if name.startswith('3D/') and name != '3D/3dmodel.model':
                    continue
                if '.gcode' in name and name not in (member, member + '.md5'):
                    continue
                z.writestr(name, overrides.get(name, data))
        validate(path, config, presets, single=True)
        outputs.append((plate, path))
    return outputs
