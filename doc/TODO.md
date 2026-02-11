# Grebe — マイルストーン・将来拡張

## 完了マイルストーン

### Phase 0: 骨格

- Vulkan 初期化、スワップチェーン、GLFW ウィンドウ
- 固定データ（静的正弦波配列）の描画確認
- Vertex Shader（int16 → NDC 変換）
- フレームレート表示

### Phase 1: ストリーミング基盤

- データ生成スレッド + lock-free SPSC ring buffer
- Staging Buffer → Device Buffer 非同期転送
- Triple Buffering 実装
- 1 MSPS でのリアルタイム描画確認 (L0 達成)
- ImGui HUD、CSV テレメトリ

### Phase 2: 間引き実装

- MinMax decimation (scalar → SSE2 SIMD)
- LTTB 実装
- 間引きスレッド分離 + ダブルバッファ出力
- 100 MSPS でのリアルタイム描画確認 (L1 達成)

### Phase 3: ベンチマーク・最適化

- マイクロベンチマーク (BM-A, B, C, E) 実装
- 自動プロファイリングフレームワーク (`--profile`)
- Period tiling による 1 GSPS データ生成
- GPU Compute Shader 間引き実験 (TI-03)
- 1 GSPS リアルタイム描画達成 (L2 達成: 59.7 fps)

### Phase 4: 計測・文書化

- ボトルネック分析
- 技術的判断メモ TI-01〜06 (`doc/technical_judgment.md`)
- 全レートで 60 FPS 達成、V-Sync 律速を確認

### Phase 5: マルチチャンネル + 高レート安全策

- `--channels=N` (N=1,2,4,8) CLIオプション
- チャンネルごとの独立リングバッファ・間引き・描画
- Per-channel カラーパレット + 垂直レーンレイアウト
- ≥100 MSPS での LTTB 自動無効化
- 受入条件: 4ch × 100 MSPS で ≥30 FPS

### Phase 6: 実 GPU 環境再計測

- Mesa dzn ドライバ (D3D12→Vulkan) で RTX 5080 を使用
- L3 計測: 770 FPS (BM-C, V-Sync OFF, 3840 vtx)
- 全プロファイル PASS: 1ch/4ch/8ch × 全レート

### Phase 7: Windows ネイティブ MSVC ビルド

- WSL2 → MSVC 19.44 + Ninja + Vulkan SDK 1.4.341.1
- L3 更新: **2,022 FPS** (dzn 770 FPS → 2.6x 向上)
- MSVC SIMD 特性: 10.5x 高速化率 (GCC は auto-vectorize で 1.1x)
- 全プロファイル PASS

### Phase 7.5: Phase 8 事前リファクタ

- [x] exe名を `grebe` に統一、Windows スクリプト修正
- [x] Command DTO + `AppCommandQueue` 導入（key_callback/profiler の直接参照排除）
- [x] ProfileRunner を `AppCommandQueue` 経由に変更
- [x] `main.cpp` 責務分離（`cli.h/cpp` + `app_loop.h/cpp`）
- [x] Per-channel `DropCounter`（drop/backpressure メトリクス）
- [x] `RingBufferView<T>` 導入（共有メモリ対応のための raw pointer 抽象化）
- [x] CMake を `grebe_common` (STATIC) + `grebe` (exe) に再編
- [x] ImGui OpenGL backend ターゲット追加（`imgui_opengl_lib`）
- [x] Cross-platform `ProcessHandle`（fork/exec + CreateProcess）

### Phase 8: 実行バイナリ分離 (`grebe` / `grebe-sg`)

- [x] IPC 契約型定義（`FrameHeaderV2`, `IpcCommand`）を `src/ipc/contracts.h` に配置
- [x] Transport 抽象 I/F（`ITransportProducer` / `ITransportConsumer`）を `src/ipc/transport.h` に定義
- [x] Pipe transport（匿名パイプ stdin/stdout）実装（`PipeProducer` / `PipeConsumer`）
- [x] `ProcessHandle::spawn_with_pipes()` によるパイプ付きプロセス起動
- [x] `grebe-sg` 実行バイナリ（DataGenerator + sender thread + ImGui ステータス窓 + headless fallback）
- [x] `grebe` IPC 統合（receiver thread → local ring buffer → 既存 DecimationThread）
- [x] `--embedded` フラグで旧 in-process モードを温存
- [x] E2E 動作確認: IPC / embedded / multi-channel / bench / profile 全モード PASS

### Phase 9: SG 専用 UI と設定モデル

- [x] DataGenerator per-channel waveform 対応 (`set_channel_waveform()`)
- [x] FrameHeaderV2 に `sample_rate_hz` 追加（grebe-sg → grebe レート自動同期）
- [x] `grebe-sg` インタラクティブ UI（レート / 波形 / ブロック長 / Pause）
- [x] `grebe` IPC モードでレート/Pause キー無効化（SG が権限者）
- [x] E2E 動作確認: IPC / embedded / multi-channel / bench / profile 全モード PASS

### Phase 10: Pipe IPC 最適化

- [x] pipe バッファサイズ拡大: Windows `CreatePipe` 1MB、Linux `fcntl(F_SETPIPE_SZ)` 1MB
- [x] ブロックサイズ最適化: デフォルト 16384、`--block-size=N` CLI (1024〜65536)
- [x] sender thread の writev 最適化（Linux: header+payload を 1 syscall に統合）
- [x] TI-07 追記: 最適化前後の帯域比較（WSL2 + Windows ネイティブ）
- [x] Go/No-Go 判定: Windows ネイティブ >100 MB/s 達成 → **shm 延期**

### Phase 10-2: IPC ボトルネック再評価と次ステップ判定

- [x] **ボトルネック分析**: WSL2 Release 検証で Embedded 4ch×1G = 2.13G drops (パイプなし) を確認。根本原因は transport ではなくパイプラインスループット (cache cold data + ring drain overhead)
- [x] **shm 実装の投資対効果評価**: ボトルネック非該当のため投資対効果低。Embedded でも同水準 drops
- [x] **pipe/buffer 側改善の投資対効果評価**: マルチスレッド間引き (中), リングサイズ拡大 (低リスク), adaptive block (中)
- [x] **判定**: TI-08 に記録。shm 延期続行。実パイプライン 3.75 GSPS vs BM-B 21 GSPS の乖離解消が本質

### Phase 10-3: パイプラインボトルネック解消

- [x] リングバッファデフォルト拡大 (16M → 64M)
- [x] Debug ビルド計測警告 (`--profile`/`--bench` で `spdlog::warn`)
- [x] マルチスレッド間引き (`std::barrier` + worker threads, 4ch→4 workers, 8ch→4 workers)
- [x] 受入条件: 4ch/8ch × 1 GSPS Embedded で 0 drops 達成

### Phase 10-4: SG-side Drop 評価 (TI-09)

- [x] `FrameHeaderV2` に `sg_drops_total` フィールド追加、IPC パイプライン全体で伝搬
- [x] `--profile` レポートに SG drops 計測追加 (JSON + stdout テーブル)
- [x] HUD に SG drops 表示 (viewer drops との区別表記)
- [x] IPC vs Embedded 定量比較: 4ch×1G SG drops ~7.9G (37%), 8ch×1G ~34.3G (79%)
- [x] TI-09 執筆: 可視化品質影響なし (MinMax 3840 vtx/ch 不変)、緩和策マトリクス、PoC 許容判定

### Phase 12: E2E レイテンシ計測（NFR-02 検証）

- [x] `producer_ts_ns` → 描画完了の E2E delta を計測
- [x] HUD に E2E latency 表示追加
- [x] `--profile` JSON に E2E latency 統計追加 (avg/p50/p95/p99)
- [x] Embedded / IPC 両モードでの計測・比較
- [x] TI-11 に計測結果と分析を記録
- [x] 受入条件: 全レート NFR-02 PASS (worst p99=18.1ms, L1≤50ms/L2≤100ms)

### Phase 11e: Main Viewer UI 改善（Waveform Axis + Time Span）

- [x] Main UI に time span 設定（up/down arrow）を追加
- [x] Main UI の draw field 右側に config pane を設置し、time span 設定を配置
- [x] DecimationThread を latest history window 化し、time span > 10ms の表示長反映を修正
- [x] RingBuffer discard API 追加（古いサンプルの効率破棄）
- [x] HUD に amplitude/time 軸と raw int16 ラベルを追加
- [x] 波形描画を軸内にクリップ（viewport/scissor）
- [x] time span 上限/下限を sample rate / ring capacity から動的導出
- [x] SG UI に periodic waveform 周波数（Hz）設定フィールドを追加
- [x] TI-10 に挙動差分を追記（Phase 11e）

---

## PoC 達成状況サマリ

| PoC 目的 | ステータス | 根拠 |
|---|---|---|
| 1 GSPS リアルタイム描画 | **達成** | L0-L3 全 PASS (L3=2,022 FPS) |
| パイプライン各段の定量計測 | **達成** | TI-01~11, BM-A/B/C/E |
| ボトルネック特定・解消 | **達成** | CPU 間引き律速 → マルチスレッドで 0-drops |
| マルチチャンネル (4ch/8ch) | **達成** | 8ch×1G PASS, 0-drops (Embedded) |
| プロセス分離 IPC | **達成** | pipe IPC + embedded 両モード動作 |
| 波形表示整合性 (NFR-02b) | **達成** | TI-10: Embedded 1ch/4ch × 全レートで envelope 100% |
| 波形整合性 高精度計測 | **達成** | Phase 11c: lazy-caching で全シナリオ envelope 100% (R-16 完了)。Phase 11d: IPC ≤100 MSPS 100%, 4ch×1G 99.2% (R-17 完了) |
| E2E レイテンシ (NFR-02) | **達成** | TI-11: 全レート PASS (worst p99=18.1ms, L1≤50ms/L2≤100ms) |
| Main UI 改善 (Phase 11e) | **達成** | 波形軸表示、time span 設定、SG 周波数設定 |

### Phase 11: 波形表示整合性検証（NFR-02b）

**目標:** 高速ストリーミング条件下で、パイプライン出力が入力信号を忠実に再現していることを定量的に検証する。
**リスク:** 低〜中（FrameHeaderV2 拡張 + profiler 拡張が主）
**優先度:** **高** — 波形整合性は PoC の KSF。「速いだけでなく正しい」ことの証明。

- [x] FrameHeaderV2 に `first_sample_index` 追加 + IPC sequence continuity check
- [x] Sequence continuity check（フレーム欠落検知 + HUD/profile 表示）
- [x] Window coverage 計測（HUD + profile レポート）
- [x] EnvelopeVerifier 実装（cyclic sliding-window min/max, ±1 LSB 許容, per-channel）
- [x] `--profile` に envelope 検証統合（Embedded: DataGenerator period buffer 直接参照）
- [x] Per-channel raw counts の atomic 取得（DecimationThread::try_get_frame overload）
- [x] マルチスレッド間引きのバリアデッドロック修正（workers_exit_ フラグ導入）
- [x] TI-10 執筆: 波形整合性検証の結果分析

**受入条件:**
- [x] Embedded 1ch × 全レート: envelope 一致率 100%（±1 LSB）
- [x] Embedded 4ch × 全レート: envelope 一致率 100%（±1 LSB）
- [x] IPC モード: envelope スキップ (-1.0) を TI-10 に記録（DataGenerator 非参照のため）
- [x] `--profile` JSON に envelope_match_rate + seq_gaps + window_coverage が含まれる
- [x] HUD に seq_gaps, window_coverage がリアルタイム表示される

### Phase 11c: Envelope 検証精度改善

**目標:** 高レート × 多チャンネル (1 GSPS × 4ch) で envelope 一致率 100% を達成する。
**リスク:** 低（profiler 内の verifier テーブル構築ロジック変更のみ）
**優先度:** 高 — Phase 11 の残課題。「正しさの証明」の完全性向上。

**背景:**
Phase 11b で build-once 最適化（verifier テーブルを初回フレームの実測 bucket size で 1 回だけ構築）を導入し、Windows 4ch 60 FPS を達成した。しかし初回フレームの実測 bucket size は定常状態と乖離するため、高レートで envelope 一致率が低下する（Linux 1ch×1G: 37%, 4ch×1G: 46%）。これは計測手法の限界であり、パイプラインのバグではない。

**実装:**
- Lazy-caching approach: `EnvelopeVerifier::set_period()` で周期バッファ参照を設定し、`verify()` に ch_raw を渡して bucket_size を算出、未キャッシュの bucket_size は on-demand で build
- Per-bucket-size キャッシュ (`std::map<size_t, vector<uint32_t>>`) で同一 bucket_size は初回のみ build
- Rate change 安定化のため warmup frame 10 以降に verifier を初期化

- [x] Lazy-caching envelope verifier 実装（per-bucket-size on-demand build + cache）
- [x] Profiler 統合: init_envelope_verifiers() + rate change settle
- [x] 計測実行: Embedded {1ch, 4ch} × {1M, 10M, 100M, 1G} SPS で envelope 100% 確認
- [x] TI-10 Phase 11c セクション追記

**受入条件:**
- [x] Embedded 1ch × 全レート: envelope 一致率 100%
- [x] Embedded 4ch × 全レート: envelope 一致率 100%
- [ ] Windows MSVC Release でも同等の結果 — Linux 検証完了、Windows は未検証（環境依存要素なく低リスク）

### Phase 11d: IPC モード Envelope 検証

- [x] 周波数計算 + period buffer 生成を `src/waveform_utils.h` に共通ユーティリティとして抽出
- [x] DataGenerator を `waveform_utils` 使用にリファクタ (single source of truth)
- [x] Profiler: IPC モード時に sample_rate_hz + Sine 前提で EnvelopeVerifier を初期化
- [x] 計測実行: IPC {1ch, 4ch} × {1M, 10M, 100M} SPS で envelope 100% 確認
- [x] 計測実行: IPC 1ch×1G 100%, 4ch×1G 99.2% (SG drops 影響)
- [x] TI-10 Phase 11d セクション追記、R-17 完了

**受入条件:**
- [x] IPC 1ch/4ch × ≤100 MSPS: envelope 一致率 100%
- [x] IPC 1ch × 1 GSPS: envelope 100%
- [x] IPC 4ch × 1 GSPS: envelope 99.2% (TI-10 に定量計測値記録)
- [x] `--profile` JSON に IPC モードでも `envelope_match_rate` が記録される

---

## 次期マイルストーン候補（優先度順）

### Phase 13.5: 回帰検証マトリクス [完了]

**目標:** Phase 間の回帰を防止する標準化された検証スイートを定義・運用する。
**リスク:** 低（計測スクリプト追加が主）
**優先度:** **中高** — Phase 13 の前に配置。回帰防止インフラは改善の前に整備すべき。
**出典:** TI-08-Codex-Review §Suggested Validation Matrix

- [x] 回帰検証マトリクス定義（構成 × メトリクス × 合否基準）:
  - 構成: 4ch/8ch × Embedded/IPC(16K/64K) × VSync ON/OFF (10 configs)
  - メトリクス: FPS, viewer drops, SG drops, envelope match, E2E latency
  - 合否基準: FPS ≥30, envelope ≥99% (Embedded), baseline 比較で回帰検出
- [x] `scripts/regression-test.sh` — マトリクス自動実行 + JSON 差分レポート
- [x] `--minimized` CLI フラグ追加（自動テスト時のウィンドウ最小化）
- [x] `--save-baseline` / `--dry-run` オプション対応

**受入条件:** `scripts/regression-test.sh` が全構成を自動実行し、前回結果との差分レポートを出力すること。→ **達成**

### Phase 13: IPC 堅牢性向上（選択的実装）

**目標:** 現行 pipe IPC の信頼性を向上させる。shm 移行なしで実施可能な改善。
**リスク:** 低〜中
**優先度:** 中 — PoC としては許容済み (TI-09) だが、デモ品質向上に有効。

- [ ] FrameHeaderV2 の `header_crc32c` 検証実装（FR-10.8, 現在はプレースホルダ）
- [ ] FrameHeaderV2 互換性/バージョニングポリシー策定（`header_bytes` による前方/後方互換パース、バージョン不一致時の明示的エラー）(#4)
- [ ] grebe-sg プロセス死活監視（pipe EOF 以外の異常検知強化）
- [ ] 1 時間連続実行安定性テスト (NFR-04/06)

**受入条件:** CRC 不一致フレーム破棄が動作。1 時間連続実行でリーク/ハング/クラッシュなし。

### Phase 14: DataSource 抽象化 + トランスポートシミュレータ

**目標:** データソースの pluggable 抽象化を導入し、帯域制限/遅延注入可能なシミュレータバックエンドで外部 I/F 評価を可能にする。
**リスク:** 低〜中（Transport 抽象 I/F は Phase 8 で導入済み）
**優先度:** 低 — 製品化判断に有用だが、PoC としての必要性は低い。
**出典:** TI-08-Codex-Review §Architecture Direction

- [ ] `DataSource` 抽象 I/F 導入（`Synthetic` / 将来 `PCIe` / `USB3` のプラグイン境界）
- [ ] 既存 `DataGenerator` を `DataSource` 実装として統合
- [ ] トランスポート抽象 I/F の最終化（Phase 8 の I/F をレビュー）
- [ ] `--transport=sim` シミュレータバックエンド実装（帯域制限/遅延注入）
- [ ] pipe / sim の比較プロファイルレポート

**受入条件:** `DataSource` I/F 経由でデータが供給され、`--transport=sim --sim-bandwidth=500M --sim-latency=1ms` 等で帯域/遅延を注入でき、プロファイルレポートで差分を確認できること。

### Phase 15a: Trigger 拡張 — Internal Trigger

**目標:** SG 側で internal trigger（level/edge ベース捕捉）を実装する。
**リスク:** 中（SG 側のキャプチャロジック変更）
**優先度:** 低 — 製品化フェーズで必要になった時点で実施。

- [ ] internal trigger パラメータ実装（level, edge, pre-trigger, post-trigger）
- [ ] trigger-aligned frame assembly

**受入条件:** internal trigger モードで level/edge 条件に基づく捕捉が動作すること。

### Phase 15b: Trigger 拡張 — External Trigger + 波形忠実度メトリクス（Product tier）

**目標:** external trigger と product-grade 波形忠実度メトリクスを追加する。
**リスク:** 中〜高（Embedded 基準との同期比較フレームワークが必要）
**優先度:** 低 — 製品化フェーズで必要になった時点で実施。

- [ ] external trigger 入力パス（将来デバイス接続時）
- [ ] 波形忠実度メトリクス（envelope mismatch rate, peak miss rate, extremum amplitude error p50/p95/p99）
- [ ] Embedded 基準との同期比較フレームワーク

**受入条件:** IPC/Embedded 比較で波形忠実度メトリクスによる品質判定が可能なこと。

---

## 延期マイルストーン（トリガー条件付き）

### Shared Memory IPC 基盤

**状態:** Phase 10 (TI-08) + Phase 10-4 (TI-09) の分析で **延期判定**。

**延期理由:**
- ボトルネックが pipe transport ではなく消費側 (ring drain + cache cold) にあったが、Phase 10-3 で解消済み
- Embedded モードが 0-drops のリファレンス動作として確立
- IPC mode の SG-side drops は可視化品質に影響なし (MinMax 3840 vtx/ch 不変)
- 実装コスト高 (OS API × 3 環境、同期、障害復旧) に対し PoC 価値が限定的

**再開トリガー:**
1. 実デバイス接続で pipe 帯域が律速となるユースケースが出現した場合
2. 製品化フェーズでプロセス間の確実なフロー制御が必要になった場合
3. `--attach-sg` (既存プロセス接続) モードが必要になった場合

**スコープ (再開時):**
- shm-a: 基本 Shared Memory Ring (shm_open / CreateFileMapping 抽象化、DataRingV2)
- shm-b: 制御ブロックと Discovery (ControlBlockV2, ConsumerStatusBlockV2, --attach-sg)
- shm-c: フロー制御と障害復旧 (credit window, heartbeat, generation bump)

---

## 将来拡張（PoC 後の検討事項）

### 優先度中

- AVX2 MinMax 最適化（SSE2 → AVX2, 処理幅 8→16 倍増。ただし現行 21x マージンで必要性低）
- SG-side pre-decimation（pipe 前に間引き → 帯域要求 ~1000x 削減、TI-09 施策 B）

### 優先度低

- 太線描画（Instanced Quad）
- Header CRC32C → payload CRC32C への拡張

### 製品化時

- 実デバイス接続（PCIe DMA / USB3 / 10GbE）— Phase 14 の `DataSource` 抽象 I/F に接続
- ウォーターフォール / スペクトログラム表示
- 高度トリガ機能（パターントリガ等）
- データ録画・再生
- GUI フレームワーク統合（Qt / ImGui 本格UI）
- リモート表示（WebSocket 経由ブラウザ表示）
- GPU Direct (RDMA) によるデバイス→VRAM 直接転送
- Shared Memory IPC + Credit-based flow control（延期マイルストーン参照）
