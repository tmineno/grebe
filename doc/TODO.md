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
- [x] Go/No-Go 判定: Windows ネイティブ >100 MB/s 達成 → **Phase 11 延期**

### Phase 10-2: IPC ボトルネック再評価と次ステップ判定

- [x] **ボトルネック分析**: WSL2 Release 検証で Embedded 4ch×1G = 2.13G drops (パイプなし) を確認。根本原因は transport ではなくパイプラインスループット (cache cold data + ring drain overhead)
- [x] **shm 実装の投資対効果評価**: ボトルネック非該当のため投資対効果低。Embedded でも同水準 drops
- [x] **pipe/buffer 側改善の投資対効果評価**: マルチスレッド間引き (中), リングサイズ拡大 (低リスク), adaptive block (中)
- [x] **判定**: TI-08 に記録。shm 延期続行。実パイプライン 3.75 GSPS vs BM-B 21 GSPS の乖離解消が本質

---

## 次期マイルストーン（実装予定）

### Phase 11: Shared Memory IPC 基盤（延期）

**目標:** Pipe transport を Shared Memory に置換し、パイプ帯域制約を排除する。
**リスク:** 高（OS API 依存、同期プリミティブ、障害復旧の複雑さ）
**前提:** Phase 10 の Go/No-Go で shm 移行が必要と判定されていること。
**状態:** Phase 10 で pipe 最適化が十分な帯域を達成したため延期。必要になった時点で再開。

**リスク緩和策:**
- 各サブステップ完了時に pipe fallback を維持し、shm 不具合時に `--transport=pipe` で切り戻し可能とする
- 10a 完了時点で帯域計測を実施し、pipe 比で 3x 以上の改善がなければ 10b/10c の投資対効果を再評価する
- WSL2 の shm 実装制約（POSIX shm_open のメモリ上限、Windows 側とのメモリ可視性）を事前調査する

Phase 11 は段階的に実装し、各ステップで動作確認を行う:

#### 11a: 基本 Shared Memory Ring

- [ ] WSL2 / Linux / Windows での shm API 動作確認（shm_open, CreateFileMapping の挙動差調査）
- [ ] POSIX shm_open / Win32 CreateFileMapping の抽象化
- [ ] `DataRingV2` 固定長リング実装（block_length=65536, slots=64）
- [ ] `FrameHeaderV2` header + channel-major payload 書き込み/読み出し
- [ ] FrameHeaderV2 header CRC32C 検証
- [ ] **帯域計測**: pipe IPC との A/B 比較（1ch/4ch × 全レート）、TI-07 追記
- [ ] **Go/No-Go**: pipe 比 3x 未満の改善なら 11b/11c の投資判断を再検討

**受入条件:** `--transport=shm` で 1ch×1 GSPS が無欠落動作。pipe fallback が維持されていること。

#### 11b: 制御ブロックと Discovery

- [ ] `ControlBlockV2` (`grebe-ipc-ctrl`): magic/abi_version/generation/heartbeat/config
- [ ] `ConsumerStatusBlockV2` (`grebe-ipc-cons`): read_sequence/credits/heartbeat
- [ ] `grebe` 起動時の ControlBlock 読み取り → DataRing attach フロー
- [ ] `--attach-sg` による既存 `grebe-sg` 接続モード

**受入条件:** `grebe-sg` を先行起動し、`grebe --attach-sg` で接続して波形表示できること。

#### 11c: フロー制御と障害復旧

- [ ] Credit-based window 制御（inflight < credits_granted で publish 許可）
- [ ] Credit 枯渇時 drop-new（loss-tolerant realtime）
- [ ] Producer/Consumer heartbeat 監視 + timeout 検出
- [ ] `grebe-sg` 再起動時の再接続/復旧（generation bump → reattach）
- [ ] 世代切替時の旧 shared memory segment cleanup（unlink/close + best-effort detach）
- [ ] 設定変更は generation bump 経由のみ（in-place config 書き換え禁止）
- [ ] 基本メトリクス（throughput/drop/enqueue/dequeue/inflight/credits）

**受入条件:** `grebe-sg` を kill → 再起動して `grebe` が自動復帰すること。1 時間連続実行でリーク/ハングなし。

### Phase 12: トランスポート計測とプロファイル統合

**目標:** IPC パフォーマンスを定量計測し、既存プロファイル基盤に統合する。
**リスク:** 低（計測コード追加が主、既存動作への影響なし）

- [ ] SG timestamp → grebe render timestamp の E2E delta 計測
- [ ] `--profile` JSON/CSV に transport メトリクス追加（throughput, drops, latency）
- [ ] Shared Memory baseline 計測値の記録（TI-07 に shm セクション追記）
- [ ] pipe vs shm の性能比較レポート整備

**受入条件:** `--profile` レポートに transport 指標が含まれ、pipe/shm 切替で比較可能なこと。

### Phase 13: 外部 I/F 評価向け拡張点

**目標:** Transport 差し替え可能な構造を確定し、評価用シミュレータを追加する。
**リスク:** 低〜中（抽象 I/F は Phase 8 で導入済み、シミュレータは新規）

- [ ] トランスポート差し替え可能な抽象 I/F を確定（Phase 8 の I/F を最終化）
- [ ] 帯域制限/遅延注入可能なシミュレータバックエンドを追加
- [ ] pipe / shm / シミュレータの 3 バックエンドでの比較レポート

**受入条件:** `--transport=sim --sim-bandwidth=500M --sim-latency=1ms` 等で帯域/遅延を注入でき、プロファイルレポートで差分を確認できること。

### Phase 14: Trigger 捕捉と波形整合性保証（SG/Main 同期）

**目標:** SG 側 trigger 判定と Main 側 frame validity 判定を統合し、表示フレームの時間整合性・非破損性を定量保証する。  
**リスク:** 中（データ経路に capture metadata と判定ロジックを追加）

- [ ] SG 側 trigger mode 実装（internal / external / timer fallback）
- [ ] internal trigger パラメータ実装（level, edge, pre-trigger, post-trigger）
- [ ] capture window 境界メタデータを IPC header/telemetry に追加
- [ ] Main 側 frame validity 判定を実装（sequence continuity, CRC, window coverage）
- [ ] invalid frame の HUD/ログ明示（silent success 禁止）
- [ ] 品質メトリクス実装（envelope mismatch, peak miss rate, extremum error p50/p95/p99）
- [ ] profile レポートに trigger/validity 指標を統合（viewer drops と SG drops を分離表示）

**受入条件:**
- trigger lock 後の valid frame 率を継続計測できること
- timer モードで window coverage 下限しきい値（例: 95%）を監視できること
- IPC/embedded 比較で、FPS/頂点数に加えて波形整合性メトリクスで品質判定できること

---

## 将来拡張（PoC 後の検討事項）

### 優先度中

- AVX2 MinMax 最適化（SSE2 → AVX2 で処理幅 8→16 に倍増）

### 優先度低

- E2E レイテンシ計測（NFR-02 目標の検証）
- マルチスレッド並列間引き（2 GSPS+ 向け）
- 太線描画（Instanced Quad）

### 製品化時

- 実デバイス接続（PCIe DMA / USB3 / 10GbE）
- ウォーターフォール / スペクトログラム表示
- 高度トリガ機能（パターントリガ等）
- データ録画・再生
- GUI フレームワーク統合（Qt / ImGui 本格UI）
- リモート表示（WebSocket 経由ブラウザ表示）
- GPU Direct (RDMA) によるデバイス→VRAM 直接転送
