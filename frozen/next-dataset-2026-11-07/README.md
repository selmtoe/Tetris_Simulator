# NEXT/HOLD 学習データ（2026-11-07 レビュー済み）

ユーザーが全件レビューしたNEXTパネルのクロップ集。

## 中身
| ファイル | 内容 |
|---|---|
| `images.u8` | 48x48x3 の planar uint8 を連結（40,255枚 = 265MB） |
| `metadata.tsv` | 各行のラベル（image_index で images.u8 を参照） |
| `corrections.json` | ユーザーの最終修正（image_index -> label, -1=対象外） |
| `metadata-original-labels.tsv` | 参考: 修正前の元ラベル |

## ラベル分布（レビュー後）
- 対象外: 3,456
- I: 5,310 / O: 5,393 / T: 4,799 / L: 5,903 / J: 5,075 / S: 5,110 / Z: 5,209
- 学習に使える合計: 36,799

## 読み方
```python
import numpy as np
data = np.fromfile('images.u8', dtype=np.uint8).reshape(-1, 3, 48, 48)  # CHW
```
metadata.tsv の列: image_index, actual_seconds, video, target_seconds, source_id,
match, player, phase, slot, label, split, key, annotation, sample_kind,
phase_offset, phase_duration, boundary_margin, line_clear, unclear

## 学習
`tools/next-cnn/loop_next.py --images images.u8 --metadata metadata.tsv --out <dir>`
