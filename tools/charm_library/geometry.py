"""Plate arrangement and naming extracted from the verified A1 mini conversion."""
import collections
import copy
import math
import re
import xml.etree.ElementTree as ET

NS = {'m': 'http://schemas.microsoft.com/3dmanufacturing/core/2015/02'}
P = '{http://schemas.microsoft.com/3dmanufacturing/production/2015/06}'
ET.register_namespace('', NS['m'])
ET.register_namespace('p', P[1:-1])
ET.register_namespace('BambuStudio', 'http://schemas.bambulab.com/package/2021')

def metadata(element):
    return {m.get('key'): m.get('value') for m in element.findall('metadata')}

def set_meta(element, key, value):
    item = next((m for m in element.findall('metadata') if m.get('key') == key), None)
    if item is None:
        item = ET.SubElement(element, 'metadata', key=key)
    item.set('value', str(value))

def preserve_embedded_parts(archive, settings_root):
    """Put large bases before embedded colored details for modern carving."""
    ns = {'m': 'http://schemas.microsoft.com/3dmanufacturing/core/2015/02'}
    documents = {n: ET.fromstring(archive.read(n)) for n in archive.namelist() if n.endswith('.model')}
    sizes = {}
    for document in documents.values():
        for obj in document.findall('m:resources/m:object', ns):
            vertices = [[float(v.get(k)) for k in ['x', 'y', 'z']] for v in obj.findall('m:mesh/m:vertices/m:vertex', ns)]
            if vertices:
                spans = [max((v[i] for v in vertices)) - min((v[i] for v in vertices)) for i in range(3)]
                sizes[obj.get('id')] = spans[0] * spans[1] * spans[2]
    allowed = set()
    for obj in settings_root.findall('object'):
        parts = obj.findall('part')
        if parts and all((p.get('subtype') == 'normal_part' for p in parts)):
            allowed.add(obj.get('id'))
            for part in parts:
                obj.remove(part)
            obj.extend(sorted(parts, key=lambda p: -sizes.get(p.get('id'), 0)))
    changed = {}
    for name, document in documents.items():
        touched = False
        for obj in document.findall('m:resources/m:object', ns):
            components = obj.find('m:components', ns)
            if obj.get('id') in allowed and components is not None:
                children = list(components)
                for child in children:
                    components.remove(child)
                components.extend(sorted(children, key=lambda c: -sizes.get(c.get('objectid'), 0)))
                touched = True
        if touched:
            changed[name] = ET.tostring(document, encoding='utf-8', xml_declaration=True)
    return changed

md, setmd = metadata, set_meta
original_preserve = preserve_embedded_parts

def preserve_with_cutouts(archive, settings_root):
    updates = original_preserve(archive, settings_root)
    ns = {'m': 'http://schemas.microsoft.com/3dmanufacturing/core/2015/02'}
    documents = {n: ET.fromstring(updates.get(n, archive.read(n))) for n in archive.namelist() if n.endswith('.model')}
    volumes = {}
    for document in documents.values():
        for obj in document.findall('m:resources/m:object', ns):
            vertices = [[float(v.get(k)) for k in ['x', 'y', 'z']] for v in obj.findall('m:mesh/m:vertices/m:vertex', ns)]
            if vertices:
                spans = [max((v[i] for v in vertices)) - min((v[i] for v in vertices)) for i in range(3)]
                volumes[obj.get('id')] = spans[0] * spans[1] * spans[2]
    mixed = {}
    for obj in settings_root.findall('object'):
        parts = obj.findall('part')
        normals = [p for p in parts if p.get('subtype') == 'normal_part']
        if normals and len(normals) != len(parts):
            ordered = iter(sorted(normals, key=lambda p: -volumes.get(p.get('id'), 0)))
            reordered = [next(ordered) if p.get('subtype') == 'normal_part' else p for p in parts]
            mixed[obj.get('id')] = {p.get('id') for p in normals}
            for part in parts:
                obj.remove(part)
            obj.extend(reordered)
    for name, document in documents.items():
        changed = False
        for obj in document.findall('m:resources/m:object', ns):
            normal_ids = mixed.get(obj.get('id'))
            components = obj.find('m:components', ns)
            if normal_ids and components is not None:
                children = list(components)
                ordered = iter(sorted([c for c in children if c.get('objectid') in normal_ids], key=lambda c: -volumes.get(c.get('objectid'), 0)))
                for child in children:
                    components.remove(child)
                components.extend((next(ordered) if c.get('objectid') in normal_ids else c for c in children))
                changed = True
        if changed:
            updates[name] = ET.tostring(document, encoding='utf-8', xml_declaration=True)
    return updates


def safe(text):
    return re.sub('[/:\\\\<>"|?*\\x00-\\x1f]', '-', text).strip(' .')

def transformed(points, text):
    t = list(map(float, text.split())) if text else [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]
    return [[sum((p[j] * t[j * 3 + i] for j in range(3))) + t[9 + i] for i in range(3)] for p in points]

def bounds(points):
    return ([min((p[i] for p in points)) for i in range(3)], [max((p[i] for p in points)) for i in range(3)])

def arrange(archive, settings, updates):
    docs = {n: ET.fromstring(updates.get(n, archive.read(n))) for n in archive.namelist() if n.endswith('.model')}
    objects = {(n, o.get('id')): o for n, d in docs.items() for o in d.findall('m:resources/m:object', NS)}
    nonprint = {p.get('id') for o in settings.findall('object') for p in o.findall('part') if p.get('subtype') not in ['normal_part', None]}
    cache = {}

    def corners(name, oid):
        key = (name, oid)
        if key in cache:
            return cache[key]
        obj = objects[key]
        points = [[float(v.get(k)) for k in ['x', 'y', 'z']] for v in obj.findall('m:mesh/m:vertices/m:vertex', NS)]
        for c in obj.findall('m:components/m:component', NS):
            if c.get('objectid') in nonprint:
                continue
            points.extend(transformed(corners(c.get(P + 'path', name).lstrip('/'), c.get('objectid')), c.get('transform')))
        if not points:
            return []
        lo, hi = bounds(points)
        cache[key] = [[x, y, z] for x in [lo[0], hi[0]] for y in [lo[1], hi[1]] for z in [lo[2], hi[2]]]
        return cache[key]
    root = docs['3D/3dmodel.model']
    byid = collections.defaultdict(list)
    for item in root.findall('m:build/m:item', NS):
        byid[item.get('objectid')].append(item)
    originals = list(settings.findall('plate'))
    placements = []
    for old in originals:
        instances = old.findall('model_instance')
        if not instances:
            continue
        entries = []
        for instance in instances:
            item = byid[md(instance)['object_id']][int(md(instance)['instance_id'])]
            points = transformed(corners(item.get(P + 'path', '3D/3dmodel.model').lstrip('/'), item.get('objectid')), item.get('transform'))
            lo, hi = bounds(points)
            entries.append({'instance': instance, 'item': item, 'lo': lo, 'hi': hi})
        lo, hi = bounds([p for e in entries for p in [e['lo'], e['hi']]])
        if hi[0] - lo[0] <= 166 and hi[1] - lo[1] <= 128:
            placements.append((old, entries, None))
        else:
            pages, current, x, y, rowh = ([], [], 0, 0, 0)
            for entry in sorted(entries, key=lambda e: -(e['hi'][1] - e['lo'][1])):
                w, h = (entry['hi'][0] - entry['lo'][0], entry['hi'][1] - entry['lo'][1])
                if w > 166 or h > 166:
                    raise ValueError('Individual model exceeds A1 mini area: ' + str((w, h)))
                if w <= 94 and h > 128:
                    if current:
                        pages.append(current)
                        current = []
                    pages.append([(entry, 80, 6)])
                    x = y = rowh = 0
                    continue
                if h > 128:
                    raise ValueError('Tall model needs rotation or a different tower position: ' + str((w, h)))
                if x + w > 166:
                    x = 0
                    y += rowh + 6
                    rowh = 0
                if y + h > 128:
                    pages.append(current)
                    current = []
                    x = y = rowh = 0
                current.append((entry, 6 + x, 6 + y))
                x += w + 6
                rowh = max(rowh, h)
            if current:
                pages.append(current)
            for page in pages:
                placements.append((old, [e for e, x, y in page], {id(e): (x, y) for e, x, y in page}))
    for plate in originals:
        settings.remove(plate)
    cols = math.ceil(math.sqrt(len(placements)))
    shifts = []
    for index, (old, entries, packed) in enumerate(placements):
        plate = copy.deepcopy(old)
        for inst in list(plate.findall('model_instance')):
            plate.remove(inst)
        for entry in entries:
            plate.append(copy.deepcopy(entry['instance']))
        setmd(plate, 'source_plate', md(old)['plater_id'])
        setmd(plate, 'plater_id', index + 1)
        settings.append(plate)
        lo, hi = bounds([p for e in entries for p in [e['lo'], e['hi']]])
        dx = 100 - (lo[0] + hi[0]) / 2
        dy = 70 - (lo[1] + hi[1]) / 2
        for entry in entries:
            if packed:
                x, y = packed[id(entry)]
                sx = x - entry['lo'][0]
                sy = y - entry['lo'][1]
            else:
                sx, sy = (dx, dy)
            t = list(map(float, entry['item'].get('transform', '1 0 0 0 1 0 0 0 1 0 0 0').split()))
            t[9] += sx + index % cols * 216
            t[10] += sy - index // cols * 216
            entry['item'].set('transform', ' '.join((format(v, '.12g') for v in t)))
        shifts.append({'plate': index + 1, 'source_plate': md(old)['plater_id'], 'translation': [dx, dy, 0] if packed is None else None, 'dimensions': [hi[i] - lo[i] for i in range(3)], 'repacked': packed is not None})
    updates['3D/3dmodel.model'] = ET.tostring(root, encoding='utf-8', xml_declaration=True)
    return shifts

def clean(name):
    for unused in range(3):
        name = re.sub('\\.(stl|obj|3mf)(?:_\\d+)?$', '', name, flags=re.I)
        name = re.sub('(?:(?:_upper|_lower)+|_\\d+)$', '', name, flags=re.I)
    return name.strip()

def names(family, category, original_plates, root):
    audit = {"row": {"name": family, "category": category}, "plates": original_plates}
    family = audit['row']['name']
    objects = {o.get('id'): o for o in root.findall('object')}
    plates = []
    for plate in root.findall('plate'):
        selected = [objects[md(i)['object_id']] for i in plate.findall('model_instance')]
        parts = [p for o in selected for p in o.findall('part')]
        raw = [md(o).get('name', '') for o in selected] + [md(p).get('name', '') for p in parts]
        positives = [clean(md(p).get('name', '')) for p in parts if p.get('subtype') in ['normal_part', None]]
        meaningful = []
        for n in positives:
            if not n or re.search('screw|knob|cylinder|floating.?peg|hole.?indicator|flatback.?indicator|hole.?loop|^body\\d|^frame\\d|^\\d+(?:\\.\\d+)?in (?:Scallop )?Circle$|^Assembly$|^\\d+$', n, re.I):
                continue
            if n not in meaningful:
                meaningful.append(n)
        if not meaningful:
            meaningful = list(dict.fromkeys((clean(md(o).get('name', '')) for o in selected if md(o).get('name') != 'Assembly')))
        label = meaningful[0] if meaningful else family
        source_plate = md(plate).get('source_plate', md(plate).get('plater_id'))
        source_record = next((p for p in audit.get('plates', []) if p['metadata'].get('plater_id') == source_plate), None)
        original = source_record['metadata'].get('plater_name', '') if source_record else ''
        if original and (not re.fullmatch('(plate|plater)[ _]?\\d*', original, re.I)):
            label = clean(re.sub('\\s+(Screwback|SB)$', '', original, flags=re.I))
        if family.startswith('Town Sign') or family == 'TownSignsMini':
            label = 'Town Sign ' + ' + '.join(meaningful)
            texts = [e.get('text') for o in selected for e in o.iter() if e.tag.endswith('text') and e.get('text') and re.search('[A-Za-z]', e.get('text'))]
            if texts:
                label = 'Town Sign ' + ' + '.join(dict.fromkeys(texts))
        elif family == 'christmas tree brownie':
            label = 'Christmas Tree Brownie Body and Red Details' if int(md(plate)['plater_id']) == 1 else 'Christmas Tree Brownie Colored Decoration Parts'
        elif family.startswith('Conversation Hearts'):
            label = 'Conversation Heart ' + label.title()
        elif 'Coffee Set' in family:
            brand = re.sub('^(A1M )?', '', family).replace(' Coffee Set', '')
            model = 'Frappe' if any(('Frappe' in n for n in meaningful)) else 'Iced Coffee' if any(('Iced Coffee' in n for n in meaningful)) else 'Coffee'
            label = brand + ' ' + model
        elif audit['row']['category'] == 'Pair Eyewear':
            label = next((n for n in meaningful if re.search('cartoon|brow|Elphaba|Wicked|2025', n, re.I)), family)
            slots = {md(p).get('extruder', md(o).get('extruder', '1')) for o in selected for p in o.findall('part') if p.get('subtype') == 'normal_part'} - {'0'}
            if len(slots) > 1:
                label += ' ' + str(len(slots)) + ' Color'
            if len(selected) > 1:
                label += ' Pair'
        elif family in ['Patriotic Flip Flops', 'Bitty Flip Flops', 'JackOLantern', 'WhiteHouseKey', 'Granny']:
            label = family
        elif len(meaningful) > 1:
            generic = {'Scrubs', 'Football', 'House', 'Blank Conversation heart', 'Tiny Bogg Bit', 'Floating_Peg'}
            chosen = [n for n in meaningful if n not in generic]
            label = ' + '.join(chosen or meaningful)
        screw = any((re.search('screw bit', n, re.I) and (not re.search('knob', n, re.I)) for n in raw))
        knob = any((re.search('knob', n, re.I) for n in raw))
        if knob and meaningful:
            screw = True
        shape = ' Scallop' if any(('scallop' in n.lower() for n in raw)) else ' Circle' if any((re.search('\\din Circle', n, re.I) for n in raw)) and (not 'circle' in label.lower()) else ''
        if shape and (not label.lower().endswith(shape.lower())):
            label += shape
        hole = any((p.get('subtype') == 'negative_part' for p in parts)) or any((re.search('Hole_Indicator|Hole_Loop', n, re.I) for n in raw))
        if screw and knob:
            label += ' SB'
        elif hole and (not re.search('hole', label, re.I)):
            label += ' Hanging Hole'
        label = safe(label)
        plates.append({'index': int(md(plate)['plater_id']), 'source_plate': md(plate).get('source_plate'), 'name': label, 'objects': [md(o).get('name', '') for o in selected], 'parts': [md(p).get('name', '') for p in parts], 'screw_part': screw, 'knob': knob})
    counts = collections.Counter((p['name'].casefold() for p in plates))
    numbers = collections.Counter()
    for p in plates:
        if counts[p['name'].casefold()] > 1:
            numbers[p['name'].casefold()] += 1
            suffix = ' SB' if p['name'].endswith(' SB') else ''
            base = p['name'][:-3] if suffix else p['name']
            p['name'] = base + ' Design ' + str(numbers[p['name'].casefold()]) + suffix
    return plates
