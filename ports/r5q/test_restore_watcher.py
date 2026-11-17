"""Validate one-shot restoration with fake commands; never contacts a phone."""
import contextlib
import io
import json
import pathlib
import runpy
import subprocess
import sys
import tempfile
from unittest.mock import patch

root = pathlib.Path(__file__).resolve().parent
allowed = (root.parents[1] / 'outputs/S10-Lite-native-iOS-port').resolve(strict=True)
results = []
for case in ['success', 'log-failure', 'wrong-model', 'no-root', 'offline', 'restore-failure', 'hash-mismatch']:
    with tempfile.TemporaryDirectory(prefix='watcher-fixture-', dir=allowed) as temporary:
        bundle = pathlib.Path(temporary).resolve(strict=True)
        assert bundle.parent == allowed
        (bundle / 'validation').mkdir()
        operator = bundle / 'Manage-Fixture.ps1'
        operator.write_text('# fixture; never executed\n')
        hashes = ['1'*64, '2'*64, '3'*64]
        (bundle / 'manifest.json').write_text(json.dumps({
            'original_recovery': {'sha256': hashes[0]}, 'original_boot_sha256': hashes[1],
            'original_vbmeta_sha256': hashes[2]}))
        state = {'clock': 0, 'actions': []}

        def clock():
            state['clock'] += 100
            return state['clock']

        def fake_run(argv, **kwargs):
            code, data = 0, b''
            if argv[0] == 'pwsh':
                state['actions'].append(argv[-1])
                assert argv[-1] == 'Restore'
                code = int(case == 'restore-failure')
            else:
                assert argv[:3] == ['adb', '-s', '<SERIAL>']
                if argv[3] == 'get-state':
                    code = int(case == 'offline');data = b'device\n'
                elif argv[3] == 'exec-out':
                    code = int(case == 'log-failure');data = b'x'*1200
                elif argv[3] == 'shell' and 'getprop' in argv[4]:
                    data = (b'uid=2000(shell)' if case == 'no-root' else b'uid=0(root)')
                    data += b'\nOTHER' if case == 'wrong-model' else b'\nSM-G770F'
                elif argv[3] == 'shell' and 'sha256sum' in argv[4]:
                    values = ['0'*64]+hashes[1:] if case == 'hash-mismatch' else hashes
                    data = '\n'.join(value+'  partition' for value in values).encode()
                else:
                    raise AssertionError(argv)
            return subprocess.CompletedProcess(argv, code, data, b'fixture error' if code else b'')

        status = 0
        with patch('subprocess.run', fake_run), patch('shutil.which', lambda name: name), \
             patch('time.monotonic', clock), patch('time.sleep'), contextlib.redirect_stdout(io.StringIO()), \
             patch.object(sys, 'argv', ['restore_native_on_return.py', str(bundle),
                                       '--operator', operator.name, '--max-wait', '180']):
            try:
                runpy.run_path(str(root / 'restore_native_on_return.py'), run_name='__main__')
            except SystemExit as exc:
                status = exc.code
        receipt = json.loads((bundle / 'validation/restore-on-return-receipt.json').read_text())
        if case in ['wrong-model', 'no-root', 'offline']:
            assert status == 2 and state['actions'] == []
        else:
            assert state['actions'] == ['Restore']
        assert (status == 0) is (case in ['success', 'log-failure'])
        assert receipt['original_recovery_restored'] is (status == 0)
        results.append({'case': case, 'result': 'passed', 'actions': state['actions'], 'exit': status})
(root / 'port/build/restore-watcher-checks.json').write_text(json.dumps(results, indent=2)+'\n')
print(json.dumps(results, indent=2))
