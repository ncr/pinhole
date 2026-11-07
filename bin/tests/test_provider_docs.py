"""Execute the published provider against scratch files, including failure paths."""
from pathlib import Path
import base64, json, os, py_compile, re, subprocess, tempfile

import sys
import unittest


class ProviderDocumentationTests(unittest.TestCase):
    def test_published_provider_protocol_and_filesystem_cases(self):
        doc = (Path(__file__).resolve().parents[2] / 'docs/plugin-examples.md').read_text()
        example = doc.split('## Source: JPEG folder',1)[1]
        manifest = json.loads(re.search(r'```json\n(.*?)\n```', example, re.S).group(1))
        code = re.search(r'```python\n(.*?)\n```', example, re.S).group(1)
        assert manifest['api'] == 15 and manifest['id'] == 'me.jpegs'
        assert manifest['sources'][0]['kind'] == manifest['id']
        checks = []
        with tempfile.TemporaryDirectory(prefix='pinhole-provider-doc-') as temp:
            root = Path(temp)
            script = root / manifest['sources'][0]['provider']
            script.write_text(code+'\n')
            py_compile.compile(str(script), doraise=True)
            checks.append('manifest + extracted Python compile')
            def exchange(method, params, rid=7):
                raw = json.dumps({'id': rid, 'method': method, 'params': params})+'\n'
                run = subprocess.run([sys.executable,'-I',str(script),'--state-dir',str(root/'state')], input=raw, text=True, capture_output=True, timeout=10)
                assert run.returncode == 0, run.stderr
                assert run.stderr == '', run.stderr
                lines = [json.loads(line) for line in run.stdout.splitlines()]
                assert lines and all(line['id'] == rid for line in lines), lines
                assert 'ok' in lines[-1], lines
                return lines
            def success(method, params):
                lines = exchange(method, params)
                assert lines[-1]['ok'] is True, lines
                return lines
            folders = [root/'photos',root/'second']
            names = ['same.jpg', 'same size.jpeg', 'tab\tfile.jpg', 'line\nfile.JPEG']
            for folder in folders:
                folder.mkdir()
                for name in names:
                    path=folder/name; path.write_bytes(b'test')
                    os.utime(path, ns=(1700000000000000000,1700000000000000000))
                (folder/'folder.jpg').mkdir()
                os.mkfifo(folder/'pipe.jpeg')
                (folder/'linked.jpg').symlink_to(folder/'same.jpg')
                (folder/'not-a-photo.txt').write_text('test')
                (folder/'folder.jpg'/'same.jpg').write_text('nested file')
            entries=[success('add_entry',{'folder':str(f)})[-1]['result']['entry'] for f in folders]
            assert entries[0]['key'] != entries[1]['key']
            checks.append('add_entry distinguishes folders containing identical basenames')
            start=success('start', {'entry':entries[0]})
            lines=success('list', {'entry':entries[0]})
            assets=[asset for line in lines if line.get('note')=='put' for asset in line['assets']]
            assert len(assets)==len(names),assets
            assert len({a['id'] for a in assets})==len(names)
            assert len({a['renditions'][0]['key'] for a in assets})==len(names)
            for asset in assets:
                assert len(asset['renditions'])==4
                assert all(ord(c)>=32 and not 127<=ord(c)<=159 for c in asset['filename'])
                assert re.fullmatch('[A-Za-z0-9._~+/=-]{1,256}',asset['renditions'][0]['key'])
                for rendition in asset['renditions']:
                    assert all(ord(c)>=32 for c in rendition['name'])
            checks.append('regular-files only; spaces/tab/newline names; equal size+mtime keys distinct')
            wanted=[{'asset':a['id'],'kind':r['kind'],'name':r['name'],'key':r['key']} for a in assets for r in a['renditions']]
            tickets=success('prepare',{'entry':entries[0], 'wanted':wanted})[-1]['result']['tickets']
            assert len(tickets)==len(wanted)
            for want,ticket in zip(wanted,tickets):
                original=os.fsdecode(base64.urlsafe_b64decode(want['name']))
                assert ticket['path']==str(folders[0]/original)
                assert Path(ticket['path']).is_file()
            checks.append('prepare preserves rendition order and exact newline/tab paths')
            for raw in (b'../escape.jpg', b'/tmp/escape.jpg', b'', b'.', b'..', b'nul\0.jpg'):
                name=base64.urlsafe_b64encode(raw).decode('ascii')
                reply=exchange('prepare',{'entry':entries[0],'wanted':[{'name':name}]})[-1]
                assert reply['ok'] is False and reply['error']['code']=='bad_request',reply
            for name in ('%%%','not-base64', '\u2603'):
                reply=exchange('prepare',{'entry':entries[0],'wanted':[{'name':name}]})[-1]
                assert reply['ok'] is False and reply['error']['code']=='bad_request',reply
            checks.append('prepare rejects traversal, absolute paths, NUL and malformed names')
            missing={'folder':str(root/'missing')}
            for method,params in (('add_entry',missing),('start',{'entry':missing}),('list',{'entry':missing})):
                lines=exchange(method,params)
                assert not any(line.get('note')=='put' for line in lines),lines
                assert lines[-1]['ok'] is False and lines[-1]['error']['code']=='offline',lines
            checks.append('missing folder: offline, no successful empty listing or put')
            reply=exchange('not-a-method',{'entry':entries[0]})[-1]
            assert reply['ok'] is False and reply['error']['code']=='unsupported'
            checks.append('unknown method returns unsupported')
            # A successful empty directory is distinct from a failed directory scan.
            empty=root/'empty';empty.mkdir()
            lines=success('list',{'entry':{'folder':str(empty)}})
            assert len(lines)==1 and lines[0]['result']['summary']=='0 photos'
            checks.append('legitimate empty folder is successful')
            # Explicitly cross the documented listing batch boundary without decoding files.
            batch=root/'batch';batch.mkdir()
            for i in range(501): (batch/f'{i}.jpg').touch()
            lines=success('list',{'entry':{'folder':str(batch)}})
            assert [len(line['assets']) for line in lines if line.get('note')=='put']==[500,1]
            checks.append('501 files split into 500 + 1 bounded put notes')
        self.assertEqual(len(checks), 9)


if __name__ == "__main__":
    unittest.main()
