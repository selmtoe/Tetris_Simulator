"""ppt-end3-winner.py の部分実行・マージ・引き分け条件の検証。

    python tools\test-ppt-end3-winner.py -v

動画デコードもモデル推論もしない。一時フォルダに偽の winner-detection.json と
偽の classification_runs を作り、判定ロジックとマージだけを確かめる。
（解析の実行や Lab サーバの再起動は行わない。）
"""
from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import types
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# cv2 / numpy / onnxruntime は解析の実行にだけ必要。判定ロジックの単体テストは
# モデルを動かさないので、無い環境では空のモジュールで import を通す。
for _name in ('cv2', 'numpy', 'onnxruntime'):
    try:
        __import__(_name)
    except ImportError:
        sys.modules[_name] = types.ModuleType(_name)


def load(module_name: str, relative: str):
    spec = importlib.util.spec_from_file_location(module_name, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


END3 = load('ppt_end3_winner', 'tools/ppt-end3-winner.py')
RUNS = load('ppt_winner_from_runs', 'tools/ppt-winner-from-runs.py')


class DrawRule(unittest.TestCase):
    """--merge / --only-matches で使う平均確率の判定ルール。"""

    def test_draw_requires_probability_and_frames(self):
        # draw 0.95 でも採用フレームが 20 未満なら引き分けにしない。
        winner, status, confidence, reason = END3.decide_from_probabilities(
            0.95, 0.04, 0.01, 19)
        self.assertEqual((winner, status, reason), ('p1', 'review', 'draw-frames'))
        self.assertLess(confidence, END3.CONF_OK)

        winner, status, _, reason = END3.decide_from_probabilities(0.95, 0.04, 0.01, 20)
        self.assertEqual((winner, status, reason), ('draw', 'ok', 'draw'))

        winner, status, _, reason = END3.decide_from_probabilities(0.95, 0.04, 0.01, 30)
        self.assertEqual((winner, status, reason), ('draw', 'ok', 'draw'))

    def test_draw_below_threshold_uses_sides(self):
        winner, status, _, reason = END3.decide_from_probabilities(0.59, 0.30, 0.11, 30)
        self.assertEqual((winner, status, reason), ('p1', 'review', 'p1'))
        winner, status, _, reason = END3.decide_from_probabilities(0.59, 0.11, 0.30, 30)
        self.assertEqual((winner, status, reason), ('p2', 'review', 'p2'))

    def test_tie_is_undetermined(self):
        winner, status, confidence, reason = END3.decide_from_probabilities(
            0.40, 0.30, 0.30, 30)
        self.assertEqual((winner, status, reason), (None, 'undetermined', 'tie'))
        self.assertEqual(confidence, 0.0)

    def test_side_winner_needs_its_own_probability(self):
        winner, status, confidence, reason = END3.decide_from_probabilities(
            0.02, 0.95, 0.03, 30)
        self.assertEqual((winner, status, reason), ('p1', 'ok', 'p1'))
        self.assertAlmostEqual(confidence, 0.92, places=6)

        winner, status, confidence, _ = END3.decide_from_probabilities(
            0.02, 0.03, 0.95, 30)
        self.assertEqual((winner, status), ('p2', 'ok'))
        self.assertAlmostEqual(confidence, 0.92, places=6)

    def test_high_draw_with_few_frames_stays_review(self):
        # フレーム不足で引き分けを却下した場合、勝者クラスは 0.6 未満なので
        # status は ok にならない（DRAW 優勢のまま確定させない）。
        winner, status, confidence, reason = END3.decide_from_probabilities(
            0.97, 0.02, 0.01, 12)
        self.assertEqual((winner, status, reason), ('p1', 'review', 'draw-frames'))
        self.assertLess(confidence, END3.CONF_OK)

    def test_parse_only_matches(self):
        self.assertEqual(END3.parse_only_matches('17,38'), [17, 38])
        self.assertEqual(END3.parse_only_matches(' 17 38 '), [17, 38])
        self.assertEqual(END3.parse_only_matches('17, 17'), [17])
        self.assertEqual(END3.parse_only_matches(None), [])
        with self.assertRaises(SystemExit):
            END3.parse_only_matches('17,x')
        with self.assertRaises(SystemExit):
            END3.parse_only_matches('0')


class Merge(unittest.TestCase):
    """既存 winner-detection.json への部分マージ。"""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.folder = Path(self._tmp.name)
        self.target = self.folder / 'winner-detection.json'
        self.existing = {
            'schema': 'tetris-winner-detection/v1',
            'batch': 'fake batch',
            'engine': 'scene-runs-inline',
            'source': 'segments.matches.json',
            'config': {'endState': True, 'determinedRatio': 1.0,
                       'needsSecondOpinionMatches': [17, 38]},
            'matches': [
                {'match': 1, 'winner': 'p1', 'status': 'ok', 'rawWinner': 'p1',
                 'votesP1': 170, 'needsSecondOpinion': False},
                {'match': 17, 'winner': 'draw', 'status': 'ok', 'rawWinner': 'draw',
                 'votesDraw': 167, 'needsSecondOpinion': True},
                {'match': 38, 'winner': 'draw', 'status': 'ok', 'rawWinner': 'draw',
                 'votesDraw': 122, 'needsSecondOpinion': True},
            ],
        }
        self.target.write_text(
            json.dumps(self.existing, ensure_ascii=False), encoding='utf-8')

    def record(self, **overrides):
        record = {'match': 17, 'winner': 'p1', 'status': 'ok', 'confidence': 0.42,
                  'votesP1': 0.90, 'votesP2': 0.05, 'votesDraw': 0.05,
                  'selectedFrames': 27, 'decision': 'p1', 'source': 'end3-model'}
        record.update(overrides)
        return record

    def test_replaces_only_requested_rows(self):
        skipped = [{'match': 38, 'skipped': True, 'skipReason': 'no-frames',
                    'winner': None, 'status': 'undetermined'}]
        payload = END3.merge_detection(self.target, [self.record()], skipped)
        rows = {row['match']: row for row in payload['matches']}

        self.assertEqual(rows[17]['winner'], 'p1')
        self.assertEqual(rows[17]['source'], 'end3-model')
        self.assertEqual(rows[17]['previousWinner'], 'draw')
        self.assertEqual(rows[17]['rawWinner'], 'draw')
        self.assertTrue(rows[17]['secondOpinion'])
        self.assertFalse(rows[17]['needsSecondOpinion'])

        # 指定していない行はそのまま（オブジェクトごと一致）。
        self.assertEqual(rows[1], self.existing['matches'][0])
        # スキップした試合も変更しない。
        self.assertEqual(rows[38], self.existing['matches'][2])

        # schema / 既存 config は保持し、secondOpinion の記録だけ足す。
        self.assertEqual(payload['schema'], self.existing['schema'])
        self.assertEqual(payload['batch'], 'fake batch')
        self.assertEqual(payload['source'], 'segments.matches.json')
        self.assertEqual(payload['config']['endState'], True)
        self.assertEqual(payload['config']['determinedRatio'], 1.0)
        self.assertTrue(payload['config']['secondOpinion'])
        self.assertEqual(payload['config']['secondOpinionMatches'], 1)
        self.assertEqual(payload['config']['secondOpinionSkipped'], 1)
        self.assertEqual(payload['config']['needsSecondOpinionMatches'], [38])
        self.assertEqual(payload['skipped'][0]['skipReason'], 'no-frames')

        # 書き戻したファイルにも同じ内容が入っている。
        saved = json.loads(self.target.read_text(encoding='utf-8'))
        self.assertEqual(saved['config']['secondOpinionMatches'], 1)
        self.assertEqual(len(saved['matches']), 3)

    def test_unchanged_winner_has_no_previous(self):
        payload = END3.merge_detection(self.target, [self.record(match=1)], [])
        row = next(item for item in payload['matches'] if item['match'] == 1)
        self.assertEqual(row['winner'], 'p1')
        self.assertNotIn('previousWinner', row)
        self.assertEqual(row['rawWinner'], 'p1')
        self.assertEqual(payload['config']['needsSecondOpinionMatches'], [17, 38])

    def test_skipped_only_keeps_file_unchanged(self):
        skipped = [{'match': 17, 'skipped': True, 'skipReason': 'video-missing',
                    'winner': None, 'status': 'undetermined'},
                   {'match': 38, 'skipped': True, 'skipReason': 'video-missing',
                    'winner': None, 'status': 'undetermined'}]
        payload = END3.merge_detection(self.target, [], skipped)
        self.assertEqual(payload['matches'], self.existing['matches'])
        self.assertEqual(payload['config']['secondOpinionMatches'], 0)
        self.assertEqual(payload['config']['secondOpinionSkipped'], 2)
        self.assertEqual(payload['config']['needsSecondOpinionMatches'], [17, 38])
        self.assertEqual([item['skipReason'] for item in payload['skipped']],
                         ['video-missing', 'video-missing'])

    def test_missing_target_creates_new_payload(self):
        target = self.folder / 'new.json'
        payload = END3.merge_detection(target, [self.record()], [])
        self.assertEqual(payload['schema'], 'tetris-winner-detection/v1')
        self.assertEqual([row['match'] for row in payload['matches']], [17])
        self.assertTrue(target.is_file())

    def test_broken_existing_is_not_overwritten(self):
        self.target.write_text('{broken', encoding='utf-8')
        with self.assertRaises(SystemExit):
            END3.merge_detection(self.target, [self.record()], [])
        self.assertEqual(self.target.read_text(encoding='utf-8'), '{broken')


class RunsSecondOpinion(unittest.TestCase):
    """ppt-winner-from-runs.py の needsSecondOpinion の印。"""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.path = Path(self._tmp.name) / 'segments.matches.json'
        self.data = {
            'video': {'fps': 30.0},
            'settings': {'end_state_model': 'fake.onnx'},
            'classification_runs': {'items': [
                [210, 390, 6, 0.95, 0.95],    # 17: 3秒窓が全部 draw
                [810, 930, 8, 0.95, 0.95],    # 18: P1 勝ち（決定的）
                [3000, 3006, 9, 0.95, 0.95],  # 19: 短すぎる P2（判定不能）
            ]},
            'matches': [
                {'index': 17, 'end': {'frame': 300},
                 'analysis_range': {'end_seconds': 10.0}},
                {'index': 18, 'end': {'frame': 870},
                 'analysis_range': {'end_seconds': 29.0}},
                {'index': 19, 'end': {'frame': 3000},
                 'analysis_range': {'end_seconds': 100.0}},
            ],
        }
        self.path.write_text(json.dumps(self.data), encoding='utf-8')

    def test_flags(self):
        payload = RUNS.build(self.path, 0.3)
        rows = {row['match']: row for row in payload['matches']}

        self.assertEqual(rows[17]['winner'], 'draw')  # 暫定値は残す
        self.assertEqual(rows[17]['rawWinner'], 'draw')
        self.assertTrue(rows[17]['needsSecondOpinion'])

        self.assertEqual(rows[18]['winner'], 'p1')
        self.assertEqual(rows[18]['rawWinner'], 'p1')
        self.assertFalse(rows[18]['needsSecondOpinion'])

        self.assertIsNone(rows[19]['winner'])
        self.assertEqual(rows[19]['status'], 'undetermined')
        self.assertTrue(rows[19]['needsSecondOpinion'])

        self.assertEqual(payload['config']['needsSecondOpinionMatches'], [17, 19])

    def test_needs_second_opinion(self):
        self.assertTrue(RUNS.needs_second_opinion('draw', 'ok'))
        self.assertTrue(RUNS.needs_second_opinion('p1', 'review'))
        self.assertTrue(RUNS.needs_second_opinion(None, 'undetermined'))
        self.assertFalse(RUNS.needs_second_opinion('p1', 'ok'))
        self.assertFalse(RUNS.needs_second_opinion('p2', 'ok'))


if __name__ == '__main__':
    unittest.main(verbosity=2)
