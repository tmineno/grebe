# Grebe — 開発マイルストーン

> PoC (v0.1.0) の成果は [TR-001](TR-001.md) に記録済み。PoC コードベースは `v0.1.0` タグで参照可能。

---

## Phase 1: プロジェクト構造再編

PoC のコードベースを libgrebe + grebe-viewer の構成に分離する。

- [ ] ディレクトリ構成: `include/grebe/` (公開ヘッダ), `src/` (内部実装), `apps/viewer/`, `apps/bench/`
- [ ] CMake 再構成: `libgrebe` (STATIC/SHARED), `grebe-viewer` (exe), `grebe-bench` (exe)
- [ ] コア公開ヘッダ定義: `grebe/grebe.h`, `grebe/data_source.h`, `grebe/render_backend.h`, `grebe/config.h`, `grebe/telemetry.h`
- [ ] 既存 PoC コードがリファクタ後もビルド・実行可能であること

**受入条件:**
- `cmake --build` で libgrebe + grebe-viewer がビルドされること
- grebe-viewer が SyntheticSource 相当のデータで波形描画できること (既存動作維持)

---

## Phase 2: IDataSource 抽象化 (FR-01)

DataGenerator を IDataSource 実装に分離し、データソースの差し替え可能な構造にする。

- [ ] IDataSource インタフェイス定義 (`data_source.h`)
- [ ] SyntheticSource 実装 (PoC DataGenerator からの移行)
- [ ] パイプラインが IDataSource 経由でデータを受け取る構成への変更
- [ ] Config 構造体によるソース設定 (FR-07 初期対応)

**受入条件:**
- SyntheticSource で 1ch × 1 GSPS, 60 FPS 動作 (PoC L2 相当)
- IDataSource を差し替えるだけで異なるデータソースに切り替え可能な構造であること

---

## Phase 3: IRenderBackend 抽象化 (FR-04)

Renderer を IRenderBackend 実装に分離し、描画バックエンドの差し替え可能な構造にする。

- [ ] IRenderBackend インタフェイス定義 (`render_backend.h`)
- [ ] VulkanRenderer 実装 (PoC Renderer からの移行)
- [ ] パイプラインが IRenderBackend 経由で描画する構成への変更

**受入条件:**
- VulkanRenderer で PoC 同等の描画性能 (L3 ≥ 500 FPS)
- per-channel プッシュ定数 (振幅スケール、オフセット、色) が動作すること

---

## Phase 4: DecimationEngine API 公開 (FR-02, FR-03)

デシメーション処理を公開 API として整理し、アルゴリズム拡張ポイントを設ける。

- [ ] DecimationEngine 公開 API 定義
- [ ] MinMax (SSE2 SIMD) + LTTB アルゴリズム移行
- [ ] ≥ 100 MSPS での自動 MinMax 切替維持
- [ ] マルチチャネルワーカープール (1–8ch) 移行
- [ ] アルゴリズム登録による拡張ポイント

**受入条件:**
- 4ch × 1 GSPS で 0 drops (Embedded 相当)
- BM-B 相当のデシメーションスループットが PoC 水準 (≥ 19,000 MSPS) を維持

---

## Phase 5: Config API + Telemetry (FR-05, FR-07)

構造体ベースの設定 API とテレメトリ公開 API を整備する。

- [ ] Config 構造体の完全定義 (チャネル数、リングサイズ、デシメーションモード、V-Sync、バックプレッシャーポリシー)
- [ ] Telemetry メトリクス構造体 (FPS, E2E レイテンシ, 充填率, デシメーション時間, ドロップ数)
- [ ] CSV / JSON シリアライズユーティリティ
- [ ] grebe-viewer が Telemetry API 経由で HUD 表示する構成

**受入条件:**
- CLI パーサーがライブラリ外 (grebe-viewer 側) にあること
- Telemetry 構造体からフレーム単位のメトリクスが取得可能であること

---

## Phase 6: FileSource (FR-01)

バイナリファイル再生用の IDataSource 実装。

- [ ] FileSource 実装 (mmap + madvise(SEQUENTIAL))
- [ ] サンプルレートに応じたペーシング
- [ ] grebe-viewer からの FileSource 利用パス

**受入条件:**
- 事前キャプチャ済みバイナリファイルを指定して波形再生できること
- 1ch × 100 MSPS 相当のファイル再生で 60 FPS 動作

---

## Phase 7: UdpSource + grebe-udp-sender (FR-01)

UDP ソケット経由のデータ受信参照実装とデモ送信プログラム。

- [ ] UdpSource 実装 (POSIX ソケット / Winsock、外部依存なし)
- [ ] grebe-udp-sender デモプログラム (SyntheticSource 相当の波形を UDP 送信)
- [ ] ローカルループバック (127.0.0.1) での E2E パイプライン検証
- [ ] Linux / Windows 双方で動作すること

**受入条件:**
- grebe-udp-sender → UdpSource → grebe-viewer でリアルタイム波形描画が動作すること
- ループバックで 1ch × 10 MSPS 以上のストリーミングが安定動作 (0 drops)

---

## Phase 8: 波形忠実度検証フレームワーク (FR-06)

エンベロープ検証器をライブラリのオプション機能として組み込む。

- [ ] EnvelopeVerifier の libgrebe 統合 (オプション有効化)
- [ ] 既知周期信号に対する ±1 LSB エンベロープ一致率算出
- [ ] カスタムバリデータ登録の拡張ポイント

**受入条件:**
- SyntheticSource (Sine) で全レート envelope 一致率 100%
- カスタムバリデータを登録して検証動作すること

---

## Phase 9: grebe-bench ベンチマークスイート

NFR 検証用の独立ベンチマークツール。

- [ ] PoC BM-A/B/C/E の libgrebe API ベース移行
- [ ] NFR-01 (描画性能 L0–L3) の自動検証
- [ ] NFR-02 (E2E レイテンシ) の自動検証
- [ ] JSON レポート出力

**受入条件:**
- grebe-bench 単体実行で NFR-01/02 の合否判定が自動出力されること
- L2 (1ch × 1 GSPS ≥ 60 FPS) PASS

---

## Phase 10: クロスプラットフォーム + パッケージング (NFR-06)

Windows MSVC ビルドと CMake パッケージ配布。

- [ ] Windows MSVC ビルド検証 (libgrebe + grebe-viewer + grebe-bench)
- [ ] Linux/Windows 性能差 ≤ 20% の確認
- [ ] `find_package(grebe)` で利用可能な CMake config 生成
- [ ] doc/API.md (公開 API リファレンス)
- [ ] doc/INTEGRATION.md (DataSource / RenderBackend 統合ガイド)

**受入条件:**
- Linux (GCC) と Windows (MSVC) で同一ソースからビルド・動作すること
- 外部プロジェクトから `find_package(grebe)` で libgrebe をリンクできること

---

## 達成状況サマリ

| Phase | 対象 FR/NFR | ステータス |
|-------|------------|-----------|
| 1. プロジェクト構造再編 | — | 未着手 |
| 2. IDataSource 抽象化 | FR-01 | 未着手 |
| 3. IRenderBackend 抽象化 | FR-04 | 未着手 |
| 4. DecimationEngine API | FR-02, FR-03 | 未着手 |
| 5. Config + Telemetry | FR-05, FR-07 | 未着手 |
| 6. FileSource | FR-01 | 未着手 |
| 7. UdpSource | FR-01 | 未着手 |
| 8. 波形忠実度検証 | FR-06 | 未着手 |
| 9. grebe-bench | NFR-01, NFR-02 | 未着手 |
| 10. クロスプラットフォーム | NFR-06 | 未着手 |
