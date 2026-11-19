#!/usr/bin/env python3
"""解析の「一式」を凍結する（エンジン・実行環境・実際に使ったモデル・設定・ソース・手順）。

    python tools/freeze_bundle.py --label v74c-hole-lookahead-all23

REGRESSION-LOG.jsonl の該当行（--label）から、その実行で **実際に使った**
エンジン・NEXT/シーンの上書きモデル・環境変数・git コミットを読み取り、
frozen/bundles/<日付>-<label>/ に次をまとめてコピーする。

  engine/    実行ファイル（SHA で照合）
  runtime/   bin の DLL 一式（CUDA / cuDNN / onnxruntime / ffmpeg）
  models/    実行に使ったモデル（上書き分を含む）と PPT Lab のモデル
  config/    tetris_recover.ini
  source/    エンジンのソース（記録されたコミットの git archive）
  tools/     回帰テスト・解析パイプラインのスクリプト（作業コピー）と suites.json
  results/   その実行の結果（golden トレースと軽量結果）
  env.json   環境変数・Python・GPU ドライバ・コミット
  MANIFEST.json  全ファイルの SHA-256
  verify.py  MANIFEST と照合する（python verify.py）
  README.md  復元と再実行の手順

ENGINE-SNAPSHOTS.jsonl は models/ フォルダの SHA しか残さないため、
--next-model / --scene-model で上書きした実行は再現できなかった。ここではそれも保存する。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import platform
import shutil
import subprocess
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / 'tools' / 'video-recovery-runtime' / 'bin'
MODELS = ROOT / 'tools' / 'video-recovery-runtime' / 'models'
CONFIG = ROOT / 'tools' / 'video-recovery-runtime' / 'config'
REG_LOG = ROOT / 'frozen' / 'REGRESSION-LOG.jsonl'
SUITES = ROOT / 'frozen' / 'golden' / 'suites.json'
JST = timezone(timedelta(hours=9))

PIPELINE_TOOLS = [
    'tools/regress.py', 'tools/golden_trace.py', 'tools/replay_validator.py',
    'tools/snapshot_engine.py', 'tools/compare_warnings.py', 'tools/freeze_bundle.py',
    'tools/run-ppt-stream-analysis.ps1', 'tools/ppt-winner-from-runs.py', 'tools/ppt-end3-winner.py',
    'tools/video-recovery-source/build_cuda.ps1',
]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def run(cmd: list[str]) -> str:
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=120, cwd=ROOT).stdout.strip()
    except Exception:
        return ''


def copy(source: Path, target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)


def find_record(label: str) -> dict:
    found = None
    for line in REG_LOG.read_text(encoding='utf-8').splitlines():
        if not line.strip():
            continue
        record = json.loads(line)
        if record.get('label') == label:
            found = record
    if not found:
        raise SystemExit(f'REGRESSION-LOG に {label} がありません')
    return found


def find_engine(sha: str) -> Path:
    for candidate in sorted((ROOT / 'frozen' / 'engines').glob(f'{sha[:12]}_*.exe')):
        if sha256(candidate) == sha:
            return candidate
    raise SystemExit(f'エンジン {sha[:12]} が frozen/engines にありません')


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--label', required=True, help='REGRESSION-LOG.jsonl の label')
    ap.add_argument('--out', help='出力先（既定: frozen/bundles/<日付>-<label>）')
    args = ap.parse_args()

    record = find_record(args.label)
    runtime = record.get('runtime') or {}
    stamp = datetime.now(JST)
    out = Path(args.out) if args.out else ROOT / 'frozen' / 'bundles' / f'{stamp:%Y-%m-%d}-{args.label}'
    if out.exists():
        raise SystemExit(f'既にあります（上書きしません）: {out}')
    out.mkdir(parents=True)
    notes: list[str] = []

    # 1) エンジン
    engine = find_engine(runtime['engine'])
    copy(engine, out / 'engine' / 'TetrisVideoRecovery_stream.exe')

    # 2) 実行環境（DLL 一式）
    for dll in sorted(BIN.glob('*.dll')):
        copy(dll, out / 'runtime' / dll.name)

    # 3) モデル: 実際に使ったもの（上書き分）＋既定＋スイート別＋PPT Lab
    models = out / 'models'
    used: dict[str, str] = {}

    def add_model(role: str, path: Path) -> None:
        if not path.is_file():
            notes.append(f'見つからないモデル: {role} {path}')
            return
        target = models / role / path.name
        copy(path, target)
        used[role] = str(target.relative_to(out)).replace('\\', '/')

    next_override = record.get('nextModelOverride')
    scene_override = record.get('sceneModelOverride')
    suites = json.loads(SUITES.read_text(encoding='utf-8'))
    # 上書きが無い実行は suites.json の runtime（regress.py が実際に渡したもの）を使う
    default_rt = suites.get('runtime') or {}
    add_model('next', ROOT / (next_override or default_rt.get('next') or 'tools/video-recovery-runtime/models/next_hold.onnx'))
    add_model('scene-main56', ROOT / (scene_override or default_rt.get('scene') or 'tools/video-recovery-runtime/models/scene-classifier.onnx'))
    add_model('board', ROOT / (default_rt.get('board') or 'tools/video-recovery-runtime/models/tetris.onnx'))
    suite_scenes = sorted({(s.get('runtime') or {}).get('scene') for s in suites['suites'].values()
                           if (s.get('runtime') or {}).get('scene')})
    for index, scene in enumerate(suite_scenes):
        add_model(f'scene-suite-{index + 1}', ROOT / scene)
    # そのまま実行できる組（エンジンは盤面モデルと同じフォルダの next_hold.onnx を読む）
    if 'board' in used and 'next' in used:
        copy(out / used['board'], models / 'run' / 'tetris.onnx')
        copy(out / used['next'], models / 'run' / 'next_hold.onnx')
    for folder in ('end3', 'fast320'):
        for item in sorted((ROOT / 'tools' / 'ppt-lab' / 'models' / folder).glob('*')):
            if item.is_file():
                copy(item, models / 'ppt-lab' / folder / item.name)
    # 照合: 記録された SHA と一致するか
    checks = {'next': runtime.get('next'), 'scene-main56': runtime.get('scene'), 'board': runtime.get('board')}
    for role, expected in checks.items():
        if role in used and expected:
            actual = sha256(out / used[role])
            if actual != expected:
                notes.append(f'SHA 不一致: {role} 記録 {expected[:12]} / 実物 {actual[:12]}')

    # 4) 設定
    copy(CONFIG / 'tetris_recover.ini', out / 'config' / 'tetris_recover.ini')
    if runtime.get('ini') and sha256(out / 'config' / 'tetris_recover.ini') != runtime['ini']:
        notes.append('SHA 不一致: tetris_recover.ini')

    # 5) ソース（記録されたコミットから）
    commit = record.get('git') or ''
    (out / 'source').mkdir()
    archive = out / 'source' / f'video-recovery-source-{commit[:12]}.zip'
    subprocess.run(['git', 'archive', '--format=zip', '-o', str(archive), commit,
                    'tools/video-recovery-source'], cwd=ROOT, check=True)
    app = out / 'source' / f'ppt-lab-{run(["git", "rev-parse", "--short=12", "HEAD"])}.zip'
    subprocess.run(['git', 'archive', '--format=zip', '-o', str(app), 'HEAD', 'tools/ppt-lab'],
                   cwd=ROOT, check=True)

    # 6) 回帰テスト・パイプラインのスクリプト（git に無いものもあるので作業コピー）
    for rel in PIPELINE_TOOLS:
        path = ROOT / rel
        if path.is_file():
            copy(path, out / 'tools' / rel)
    copy(SUITES, out / 'tools' / 'frozen' / 'golden' / 'suites.json')

    # 7) 結果（golden トレースと軽量結果）
    for base in (ROOT / 'frozen' / 'golden' / args.label, ROOT / 'frozen' / 'runs' / args.label):
        if base.is_dir():
            shutil.copytree(base, out / 'results' / base.parent.name / base.name)
    (out / 'results' / 'regression-record.json').write_text(
        json.dumps(record, ensure_ascii=False, indent=1), encoding='utf-8')

    # 8) 環境
    pyrt = run(['py', '-3.11', '-c', 'import onnxruntime as o, numpy as n; print(o.__version__, n.__version__)'])
    env = {
        'frozenAt': stamp.isoformat(timespec='seconds'),
        'label': args.label,
        'engineSha256': runtime.get('engine'),
        'gitCommitOfRun': commit,
        'gitDirtyAtRun': record.get('gitDirty'),
        'gitHeadAtFreeze': run(['git', 'rev-parse', 'HEAD']),
        'engineEnv': record.get('env') or {},
        'modelsUsed': used,
        'suiteSceneModels': suite_scenes,
        'python': sys.version.split()[0],
        'python311OnnxruntimeNumpy': pyrt,
        'windows': platform.platform(),
        'gpu': run(['nvidia-smi', '--query-gpu=name,driver_version', '--format=csv,noheader']),
        'notes': notes,
    }
    (out / 'env.json').write_text(json.dumps(env, ensure_ascii=False, indent=1), encoding='utf-8')

    # 9) README と verify.py
    env_lines = '\n'.join(f"$env:{k} = '{v}'" for k, v in (record.get('env') or {}).items())
    (out / 'README.md').write_text(f'''# 凍結: {args.label}

凍結日時: {env['frozenAt']}
エンジン SHA-256: `{runtime.get('engine')}`
実行時の git コミット: `{commit}`

この解析で **実際に使った** エンジン・実行環境・モデル・設定・ソース・手順の一式。
最初に `python verify.py` で全ファイルの SHA-256 を照合すること。

## 中身

| フォルダ | 内容 |
|---|---|
| engine/ | 実行ファイル |
| runtime/ | bin の DLL 一式（CUDA/cuDNN/onnxruntime/ffmpeg）。engine と同じフォルダに置いて使う |
| models/next | NEXT/HOLD モデル（この実行で使ったもの） |
| models/scene-main56 | main56（vsたわ 56試合）で使ったシーンモデル |
| models/scene-suite-* | Lab 動画のスイートで使ったシーンモデル（suites.json の指定） |
| models/board | 盤面モデル |
| models/run | そのまま実行できる組（tetris.onnx と、この実行の next_hold.onnx） |
| models/ppt-lab | PPT Lab アプリのモデル（勝敗判定 end3 / 解析 fast320）の作業コピー |
| config/ | tetris_recover.ini |
| source/ | エンジンのソース（実行時コミット）と PPT Lab のソース（凍結時 HEAD） |
| tools/ | 回帰テストと解析パイプラインのスクリプト、suites.json |
| results/ | この実行の golden トレースと軽量結果、REGRESSION-LOG の行 |

## 再実行（GPU、1本ずつ）

```powershell
{env_lines}
# runtime の DLL を engine と同じフォルダにそろえてから:
Copy-Item runtime\\*.dll engine\\
& engine\\TetrisVideoRecovery_stream.exe <動画> --detect <出力先> `
  models\\scene-main56\\<ファイル名> models\\run\\tetris.onnx config\\tetris_recover.ini
```

エンジンは盤面モデル（tetris.onnx）と同じフォルダの `next_hold.onnx` を NEXT モデルとして読む。
`models\\run\\` にはこの実行で使った2つを並べてある（regress.py の --next-model と同じ扱い）。
Lab 動画のスイートは、シーンモデルを `models\\scene-suite-*` に替えて実行する（suites.json の指定）。

リポジトリ内で回帰テストとして再実行する場合:

```powershell
python tools\\regress.py run --label recheck-{args.label} --skip-build --no-deploy `
  --exe <このフォルダ>\\engine\\TetrisVideoRecovery_stream.exe `
  --next-model <このフォルダ>\\{used.get('next', '').replace('/', chr(92))} `
  --scene-model <このフォルダ>\\{used.get('scene-main56', '').replace('/', chr(92))} `
  {' '.join(f'--env {k}={v}' for k, v in (record.get('env') or {}).items() if k != 'TR_NVDEC_QUEUE_FRAME_STRIDE')} `
  --baseline {args.label} --suites main56
```

## 注意

{chr(10).join('- ' + n for n in notes) or '- なし'}
''', encoding='utf-8')
    (out / 'verify.py').write_text('''import hashlib, json, sys
from pathlib import Path
base = Path(__file__).resolve().parent
manifest = json.loads((base / 'MANIFEST.json').read_text(encoding='utf-8'))
bad = 0
for rel, info in manifest['files'].items():
    path = base / rel
    if not path.is_file():
        print('MISSING', rel); bad += 1; continue
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    if h.hexdigest() != info['sha256']:
        print('CHANGED', rel); bad += 1
print('OK' if not bad else f'{bad} problem(s)', len(manifest['files']), 'files')
sys.exit(1 if bad else 0)
''', encoding='utf-8')

    # 10) MANIFEST
    files = {}
    for path in sorted(out.rglob('*')):
        if path.is_file() and path.name not in ('MANIFEST.json',):
            rel = str(path.relative_to(out)).replace('\\', '/')
            files[rel] = {'sha256': sha256(path), 'bytes': path.stat().st_size}
    (out / 'MANIFEST.json').write_text(json.dumps({'label': args.label, 'files': files},
                                                  ensure_ascii=False, indent=1), encoding='utf-8')
    total = sum(item['bytes'] for item in files.values())
    print(f'{out}\n{len(files)} files, {total / 1e9:.2f} GB')
    for note in notes:
        print('NOTE', note)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
