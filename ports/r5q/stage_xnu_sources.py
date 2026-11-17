"""Parent-side fetch of pinned public build inputs. Does not execute fetched code."""
import concurrent.futures
import json
import os
import pathlib
import subprocess
import sys

root = pathlib.Path(sys.argv[1]).resolve(strict=True)
workspace = pathlib.Path('/var/tmp/codex-ios-r5q/work')
cache = workspace / 'xnu-source-cache'
cache.mkdir(exist_ok=True)
task_home = cache / '.git-home'
task_home.mkdir(exist_ok=True)
env = {'PATH': '/usr/bin:/bin', 'HOME': str(task_home), 'GIT_CONFIG_NOSYSTEM': '1',
       'GIT_CONFIG_GLOBAL': '/dev/null', 'GIT_TERMINAL_PROMPT': '0', 'LC_ALL': 'C'}
sources = [
    ('AvailabilityVersions', 'apple-oss-distributions/AvailabilityVersions', '5d9baee345b3bc72b26c71adc65f94f2d5e9ced2'),
    ('libdispatch', 'apple-oss-distributions/libdispatch', 'a641558f447659d90ad002ef0ee6f0212273fbfc'),
    ('libpthread', 'apple-oss-distributions/libpthread', 'a6c70ca0e544347a0377babf748f831577ec24f9'),
    ('mm_bootstrap_cmds', 'markmentovai/bootstrap_cmds', 'f337e600741638896fb6056ce87c6479f74b89a3'),
    ('tcsh', 'tcsh-org/tcsh', 'b7db07931546178c4cb2542b5fc850c483628ed5'),
    ('unifdef', 'fanf2/unifdef', '08d12cd3059fe072929ea001ad9833f5487fd355'),
]

def git(path, *args):
    result = subprocess.run(['/usr/bin/git', '-c', 'core.hooksPath=/dev/null', '-C', str(path), *args],
                            env=env, capture_output=True, timeout=120)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors='replace')[-1500:])
    return result.stdout.decode().strip()

def fetch(source):
    name, repo, expected = source
    path = cache / name
    url = 'https://github.com/' + repo + '.git'
    try:
        if path.exists():
            actual = git(path, 'rev-parse', 'HEAD')
            if actual != expected:
                raise RuntimeError('Existing source differs; preserved without resetting it')
        else:
            path.mkdir()
            git(path, 'init', '-q')
            git(path, 'fetch', '--depth', '1', url, expected)
            git(path, '-c', 'advice.detachedHead=false', 'checkout', '--detach', '-q', 'FETCH_HEAD')
            actual = git(path, 'rev-parse', 'HEAD')
            if actual != expected:
                raise RuntimeError('Fetched commit does not match the pin')
        return {'name': name, 'url': url, 'commit': actual, 'path': str(path), 'status': 'verified'}
    except Exception as exc:
        return {'name': name, 'url': url, 'commit': expected, 'path': str(path), 'status': 'failed', 'error': str(exc)}

results = []
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
    futures = [pool.submit(fetch, source) for source in sources]
    for future in concurrent.futures.as_completed(futures):
        row = future.result()
        results.append(row)
        print(json.dumps(row), flush=True)
apple = workspace / 'apple-xnu-8796.121.2'
actual = git(apple, 'rev-parse', 'HEAD')
assert actual == 'c54f35ca767986246321eb901baf8f5ff7923f6a'
results.append({'name': 'xnu', 'url': 'https://github.com/apple-oss-distributions/xnu.git',
                'tag': 'xnu-8796.121.2', 'commit': actual, 'path': str(apple), 'status': 'verified'})
target = root / 'research/xnu-source-staging.json'
target.parent.mkdir(parents=True, exist_ok=True)
target.write_text(json.dumps({'status': 'sources only; no fetched build code executed',
                              'inputs': sorted(results, key=lambda row: row['name'])}, indent=2)+'\n')
if any(row['status'] != 'verified' for row in results):
    raise SystemExit(1)
