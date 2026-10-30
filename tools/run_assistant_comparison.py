#!/usr/bin/env python3
"""Six sequential, fresh Studio assistant trials. Existing ChatGPT login only.

Measures the entire agent turn (model/network plus native tools) separately from
CLI startup, account/catalog preflight, fixture setup, validation and export.
No prompts, instructions, tools, effort levels or graders change between trials.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
MODELS = {
    'Luna Max': ('gpt-6-luna', 'max'),
    'Terra High': ('gpt-6.1-terra', 'high'),
    'Sol Medium': ('gpt-6.1-sol', 'medium'),
}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('binary', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--profile', type=Path, default=Path.home()/'Library/Application Support/BambuStudioInternal')
    args = p.parse_args()
    binary = args.binary.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False, mode=0o700)
    sources = ['tools/assistant_workbench_eval.hpp', 'tools/assistant_live_client_check.cpp', 'src/slic3r/GUI/StudioAssistantTools.cpp', 'src/slic3r/GUI/StudioAssistantWorkbench.cpp']
    def hashes():
        return {name: hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in sources}
    frozen = hashes()
    binary_hash = hashlib.sha256(binary.read_bytes()).hexdigest()
    # Reverse order in round two, giving every model the same average position.
    order = ['Sol Medium', 'Terra High', 'Luna Max', 'Luna Max', 'Terra High', 'Sol Medium']
    manifest = {'scenario': 'challenge', 'runs_per_model': 2, 'order': order,
                'source_sha256': frozen, 'binary_sha256': binary_hash,
                'model_configurations': MODELS, 'parallel_trials': False,
                'time_limit_seconds': 300, 'results': []}
    def save():
        (output/'summary.json').write_text(json.dumps(manifest, indent=2)+'\n')
    save()
    counts = {name: 0 for name in MODELS}
    for index, name in enumerate(order, 1):
        assert hashes() == frozen, 'Benchmark source changed; do not mix runs.'
        assert hashlib.sha256(binary.read_bytes()).hexdigest() == binary_hash, 'Benchmark executable changed.'
        counts[name] += 1
        model, effort = MODELS[name]
        trial = output/(name.lower().replace(' ', '-')+'-'+str(counts[name]))
        print(f'START {index}/6: {name}, run {counts[name]}', flush=True)
        started = time.monotonic()
        with (output/(trial.name+'.log')).open('w') as log:
            try:
                result = subprocess.run([str(binary), str(args.profile), '--evaluate', 'challenge', str(trial), model, effort],
                                        cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=390)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'process_timeout'
        row = {'label': name, 'run': counts[name], 'process_exit': code,
               'cli_wall_seconds': time.monotonic()-started, 'directory': str(trial)}
        report = trial/'report.json'
        if report.exists():
            row['report'] = json.loads(report.read_text())
            effective = [json.loads(line) for line in (trial/'events.jsonl').read_text().splitlines()
                         if json.loads(line).get('event') == 'benchmark_request']
            row['effort_verified'] = bool(effective) and all(e['model'] == model and e['reasoning_effort'] == effort for e in effective)
            assert row['effort_verified'], 'A request used the wrong model/effort.'
            print(f'END {name}: pass={row["report"]["passed"]}, seconds={row["report"]["elapsed_seconds"]:.1f}, tools={row["report"]["tool_calls"]}', flush=True)
        else:
            row['status'] = 'no_report_infrastructure_or_process_failure'
            print(f'END {name}: no report, process={code}; inspect its log.', flush=True)
        manifest['results'].append(row)
        save()
    print('Saved benchmark:', output/'summary.json', flush=True)


if __name__ == '__main__':
    main()
