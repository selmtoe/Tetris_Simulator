# レガシーの封印（2026-10-26）

## 何を封印したか

`Documents\tetris ai` の作業ツリーに残っていた**古いシミュレータ/エディタ一式**。
公開版（https://selmtoe.github.io/Tetris_Simulator/）とは別物で、**origin/main より31コミット遅れ**ていた。

- 見た目: 紫がかった暗色テーマ（現行は明るいテーマ）
- 構造: `hub/js/*` の旧モジュール群、`kasane/` など
- 状態: 383ファイルの未コミット変更（削除含む）を含む

## どこに封印したか

git ブランチ **`legacy/local-20261026`**（コミット `05eb143`）。
`main` は origin/main（`23ea2c3`）に合わせてある。

## 復元したいとき

```powershell
cd "C:\Users\hirom\Documents\tetris ai"
git switch legacy/local-20261026   # 旧ツリーを見る
git switch main                    # 現行に戻る
```

## 注意

- **この旧ツリーを編集しない**。正史は `main`（origin/main）側。
- 旧ツリーのファイルを現行に混ぜると、公開版と食い違う原因になる。
- 旧ツリーは履歴として残すだけで、今後の開発対象ではない。

## 正史の構成（参考）

| 役割 | 場所 |
|---|---|
| シミュレータ本体 | `index.html` + `simulator/` + `styles/simulator.css` |
| エディタ/ビューワー | `F/` |
| ハブ（作業台） | `hub/` |
| 共通部品 | `shared/` |
| 公開ビルド | `tools/build-web.py` → `dist/pages`（GitHub Actions が push で自動デプロイ） |
