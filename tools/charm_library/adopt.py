"""One-time migration of the verified batches into the persistent manifest."""
import copy
import json
from pathlib import Path
import shutil

from .engine import atomic_json, digest, signature
from . import package


def adopt(library, folder):
    folder = Path(folder).expanduser().resolve()
    recipe, preview_recipe, presets = library.recipes()
    vector_report = folder / 'vector-library-regeneration/report.json'
    review = {r['name'].casefold(): r for r in json.loads(vector_report.read_text())['jobs']} if vector_report.exists() else {}
    records = []
    audit_path = folder / 'remaining-projects/audit.json'
    categories = {r['row']['selected_source']: r['row']['category']
                  for r in json.loads(audit_path.read_text())} if audit_path.exists() else {}
    for batch, report in [('a1mini-batch-20', 'batch-20-report.json'), ('a1mini-batch-21-30', 'batch-21-30-report.json')]:
        for record in json.loads((folder / batch / report).read_text())['records']:
            source = folder / batch / record['project'] / 'input.3mf'
            if not source.exists():
                source = Path('/private/tmp/charm-library-batch-21-30') / record['project'] / 'input.3mf'
            records.append((source, record))
    for path in sorted((folder / 'remaining-projects').glob('*/published.json')):
        records.append((path.parent / 'input.3mf', json.loads(path.read_text())))
    state = copy.deepcopy(library.state)
    count, imported, checked = 0, library.home / 'imported-sources', {}
    for source, record in records:
        if not source.is_file():
            raise ValueError('Prepared source missing; migration stopped: ' + str(source))
        key = signature(['adopted', record['project']])[:16]
        if key in state['projects']:
            continue
        target = imported / (key + '.3mf')
        source_sha = digest(source)
        if target.exists() and digest(target) != source_sha:
            raise ValueError('Imported source already differs: ' + str(target))
        outputs, plates = {}, []
        for plate in record['plates']:
            job = Path(library.config['library']).expanduser().resolve() / plate['filename']
            if str(job) not in checked:
                if not job.is_file() or digest(job) != plate['sha256']:
                    raise ValueError('Current job differs from its publication record: ' + str(job))
                package.validate(job, library.config, presets, single=True)
                checked[str(job)] = digest(job)
            elif checked[str(job)] != plate['sha256']:
                raise ValueError('Conflicting historical job hashes: ' + str(job))
            outputs[str(job)] = checked[str(job)]
            r = review.get(job.name.casefold(), {})
            policy = r.get('reason', 'Reviewed PNG fallback') if r.get('status') == 'fallback' and 'Visual review:' in r.get('reason', '') else 'auto'
            item = dict(index=plate['plate'], name=job.name.removesuffix('.gcode.3mf'), filename=job.name,
                        preview_policy=policy, preview_fallback=r.get('reason', '') if r.get('status') != 'ready' else '')
            for suffix in ('.preview.json', '.preview.svg'):
                path = Path(str(job) + suffix)
                if path.is_file():
                    outputs[str(path)] = digest(path)
            plates.append(item)
        # Snapshot every prepared source into permanent state, including the ten
        # sources previously held in /private/tmp. No historical script is needed
        # after this one-time import.
        imported.mkdir(parents=True, exist_ok=True)
        if not target.exists():
            temp = target.with_suffix('.copying')
            shutil.copyfile(source, temp)
            if digest(temp) != source_sha:
                raise ValueError('Source copy failed verification')
            temp.replace(target)
        plan_path = source.parent / 'plan.json'
        plan = json.loads(plan_path.read_text()) if plan_path.exists() else {}
        inferred_pair = record['project'].split(' ')[0].casefold() in ('wanda', 'cedro', 'twain', 'finley', 'larkin', 'ella', 'casper', 'kirby', 'murphy', 'zuri', 'jessie')
        category = 'Pair Eyewear' if 'pair' in categories.get(record['source'], '').casefold() or inferred_pair else 'Bogg Charm'
        state['projects'][key] = dict(id=key, name=record['project'], source=str(target), source_sha256=source_sha,
                                      original_source=record['source'], category=category,
                                      recipe=signature([recipe, library.config['plate_names'].get(key, {}), category]),
                                      preview_recipe=preview_recipe, prepared_input=str(target), prepared_sha256=source_sha,
                                      plates=plates, outputs=outputs, adopted=True,
                                      source_family=plan.get('family', record['project']))
        count += 1
        if count % 25 == 0:
            print(f'  Verified {count} projects…', flush=True)
    atomic_json(library.state_path, state)
    library.state = state
    print(f'Adopted {count} projects; {len(checked)} unique sliced jobs verified. Existing print files unchanged.')
