"""Exercise the controller's return/restore/error paths using fake command results."""
import json
import pathlib
import runpy
import subprocess
import sys
import tempfile
from unittest.mock import patch

root = pathlib.Path(__file__).resolve().parent
outcomes = []
for case in ['success', 'missing-marker', 'log-failure', 'restore-failure', 'timeout', 'start-failure',
             'shared-marker-old-count', 'shared-marker-before-anchor']:
    with tempfile.TemporaryDirectory(prefix='controller-', dir=root / 'port/build') as temporary:
        bundle = pathlib.Path(temporary)
        operator = bundle / 'Manage-Fixture.ps1'
        operator.write_text('# fixture; never executed\n')
        hashes = ['1'*64, '2'*64, '3'*64]
        (bundle / 'manifest.json').write_text(json.dumps({
            'original_recovery': {'sha256': hashes[0]}, 'original_boot_sha256': hashes[1],
            'original_vbmeta_sha256': hashes[2]}))
        state = {'phase': 0, 'actions': [], 'clock': 0}

        def clock():
            state['clock'] += 100
            return state['clock']

        def fake_run(argv, **kwargs):
            code, data = 0, b''
            if argv[0] == 'pwsh':
                action = argv[-1]
                state['actions'].append(action)
                if action == 'Start':
                    code = int(case == 'start-failure')
                    if not code:
                        state['phase'] = 1
                elif action == 'Restore':
                    code = int(case == 'restore-failure')
                    if not code:
                        state['phase'] = 2
                else:
                    raise AssertionError(argv)
            elif argv[0] == 'adb':
                assert argv[1:3] == ['-s', '<SERIAL>']
                if argv[3] == 'get-state':
                    code = int(case == 'timeout' and state['phase'] == 1)
                    data = b'device\n' if not code else b''
                elif argv[3] == 'exec-out':
                    code = int(case == 'log-failure' and state['phase'] == 1)
                    marker = b'FRESH-MARKER' if state['phase'] and case != 'missing-marker' else b''
                    data = b'x'*1200 + marker
                    if case in ['shared-marker-old-count', 'shared-marker-before-anchor']:
                        data = b'x'*1200 + (marker+b'SHARED-MARKER' if case == 'shared-marker-old-count'
                                             else b'SHARED-MARKER'+marker)
                elif argv[3] == 'shell':
                    text = argv[4]
                    if 'boot_id' in text:
                        data = str(state['phase']).encode()
                    elif "su -c 'id'" == text:
                        data = b'uid=0(root)'
                    elif 'sha256sum' in text:
                        data = '\n'.join(h+'  partition' for h in hashes).encode()
                    else:
                        raise AssertionError(argv)
                else:
                    raise AssertionError(argv)
            else:
                raise AssertionError('Unexpected process: '+repr(argv))
            return subprocess.CompletedProcess(argv, code, data, b'fixture error' if code else b'')

        exit_code = 0
        extra = ['--expect', 'SHARED-MARKER'] if case.startswith('shared-marker-') else []
        with patch('subprocess.run', fake_run), patch('shutil.which', lambda name: name), \
             patch('time.monotonic', clock), patch('time.sleep'), \
             patch.object(sys, 'argv', ['run_native_test.py', str(bundle), '--operator', operator.name,
                                       '--expect', 'FRESH-MARKER'] + extra):
            try:
                runpy.run_path(str(root / 'run_native_test.py'), run_name='__main__')
            except SystemExit as exc:
                exit_code = exc.code
            except RuntimeError:
                exit_code = 1
        receipt = json.loads(next((bundle / 'validation').glob('*/receipt.json')).read_text())
        if case == 'timeout':
            assert state['actions'] == ['Start'] and exit_code == 2
            assert receipt['original_recovery_restored'] is False
        else:
            assert state['actions'] == ['Start', 'Restore'], (case, state)
            assert receipt['original_recovery_restored'] is (case != 'restore-failure')
        assert (exit_code == 0) is (case in ['success', 'shared-marker-old-count']), (case, exit_code)
        outcomes.append({'case': case, 'result': 'passed', 'actions': state['actions'], 'exit': exit_code})
(root / 'port/build/native-controller-checks.json').write_text(json.dumps(outcomes, indent=2)+'\n')
print(json.dumps(outcomes, indent=2))
