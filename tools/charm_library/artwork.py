"""One embedded vector preview, bound to the sliced plate's print contents."""
import hashlib
import copy
import json
import math
from pathlib import Path
import shutil
import zipfile
import xml.etree.ElementTree as ET

MEMBER = 'Metadata/charm_library/preview.json'
MAX_JSON = 8 * 1024 * 1024
MAX_MEMBER = 512 * 1024 * 1024
CONTENT_TYPES = '[Content_Types].xml'
CONTENT_NS = 'http://schemas.openxmlformats.org/package/2006/content-types'


def content_types(raw, present):
    """Declare the optional private part without adding package relationships."""
    root = ET.fromstring(raw)
    for node in list(root):
        if node.tag == '{' + CONTENT_NS + '}Override' and node.get('PartName') == '/' + MEMBER:
            root.remove(node)
    if present:
        ET.SubElement(root, '{' + CONTENT_NS + '}Override', PartName='/' + MEMBER, ContentType='application/json')
    return ET.tostring(root, encoding='utf-8', xml_declaration=True)


def fingerprint(archive, plate):
    """SHA-256 of sorted required names, NUL, uint64-be length, member bytes.

    ZIP order, compression, timestamps and the preview itself are immaterial.
    Include settings so a stale color/filament map cannot remain clickable.
    """
    if not isinstance(archive, zipfile.ZipFile):
        with zipfile.ZipFile(archive) as z:
            return fingerprint(z, plate)
    if not isinstance(plate, int) or isinstance(plate, bool) or plate < 1:
        raise ValueError('Invalid preview plate index')
    names = archive.namelist()
    if len(names) != len(set(names)):
        raise ValueError('Duplicate archive members')
    value = hashlib.sha256()
    for name in sorted((f'Metadata/plate_{plate}.gcode', 'Metadata/project_settings.config', 'Metadata/slice_info.config')):
        info = archive.getinfo(name)
        if info.file_size > MAX_MEMBER:
            raise ValueError('Print member exceeds preview validation limit')
        value.update(name.encode() + b'\0' + info.file_size.to_bytes(8, 'big'))
        with archive.open(info) as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                value.update(chunk)
    return value.hexdigest()


def validate(data):
    if not isinstance(data, dict) or data.get('version') != 2:
        raise ValueError('Unsupported embedded vector preview')
    plate = data['plate_index']
    width, height = data['width'], data['height']
    sha = data['print_sha256']
    if (not isinstance(plate, int) or isinstance(plate, bool) or plate < 1
            or not all(math.isfinite(v) and 0 < v <= 10000 for v in (width, height))
            or not isinstance(sha, str) or len(sha) != 64 or any(c not in '0123456789abcdef' for c in sha)):
        raise ValueError('Invalid vector bounds or identity')
    regions = data['regions']
    if not isinstance(regions, list) or not 0 < len(regions) <= 256:
        raise ValueError('Invalid vector regions')
    points = 0
    for region in regions:
        filament, color, contours = region['filament'], region['color'], region['contours']
        if (not isinstance(filament, int) or isinstance(filament, bool) or not 0 <= filament <= 255
                or not isinstance(color, str) or len(color) != 7 or color[0] != '#'
                or any(c not in '0123456789abcdefABCDEF' for c in color[1:])
                or not isinstance(contours, list) or not contours):
            raise ValueError('Invalid vector filament or contours')
        for ring in contours:
            if not isinstance(ring, list) or len(ring) < 3:
                raise ValueError('Invalid vector contour')
            points += len(ring)
            if points > 200000:
                raise ValueError('Too many vector points')
            for xy in ring:
                if not isinstance(xy, list) or len(xy) != 2:
                    raise ValueError('Invalid vector point')
                x, y = xy
                if not (math.isfinite(x) and math.isfinite(y) and -.01 <= x <= width + .01 and -.01 <= y <= height + .01):
                    raise ValueError('Vector point outside bounds')
    raw = json.dumps(data, separators=(',', ':'), allow_nan=False).encode() + b'\n'
    if len(raw) > MAX_JSON:
        raise ValueError('Vector preview exceeds Studio limits')
    return raw


def read(job):
    with zipfile.ZipFile(job) as z:
        if MEMBER not in z.namelist():
            return None
        if z.getinfo(MEMBER).file_size > MAX_JSON:
            raise ValueError('Vector preview exceeds Studio limits')
        data = json.loads(z.read(MEMBER))
        validate(data)
        if data['print_sha256'] != fingerprint(z, data['plate_index']):
            raise ValueError('Vector preview does not match print contents')
        return data


def write(source, target, data):
    """Stage a new archive, preserving print members and declaring our preview.

    Source and target must differ. Publication/backup belongs to the library's
    existing transaction, so a failed preview update cannot damage a print file.
    """
    if Path(source).resolve() == Path(target).resolve():
        raise ValueError('Stage embedded artwork separately from its source')
    raw = validate(data) if data is not None else None
    with zipfile.ZipFile(source) as original:
        names = original.namelist()
        if len(names) != len(set(names)):
            raise ValueError('Duplicate archive members')
        if data is not None and data['print_sha256'] != fingerprint(original, data['plate_index']):
            raise ValueError('Vector preview does not match print contents')
        existing = original.read(MEMBER) if MEMBER in names else None
        if existing == raw:
            shutil.copyfile(source, target)
            return
        with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as output:
            output.comment = original.comment
            for info in original.infolist():
                if info.filename == MEMBER:
                    continue
                if info.filename == CONTENT_TYPES:
                    output.writestr(copy.copy(info), content_types(original.read(info), raw is not None))
                    continue
                # Streaming preserves large G-code files without holding them in RAM.
                with original.open(info) as incoming, output.open(copy.copy(info), 'w') as outgoing:
                    shutil.copyfileobj(incoming, outgoing)
            if raw is not None:
                info = zipfile.ZipInfo(MEMBER, date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                output.writestr(info, raw)
    if read(target) != data:
        raise ValueError('Embedded artwork verification failed')
