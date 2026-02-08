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

---

## 次期マイルストーン（実装予定）

### 現状の IPC 性能ベースライン (TI-07)

Phase 9 の anonymous pipe IPC 計測結果を Phase 10 以降の判断基準とする。

**WSL2 (dzn, GCC Debug)**

| 条件 | 実効帯域 | Drops | FPS |
|---|---|---|---|
| 1ch × ≤100 MSPS | ~195 MB/s (100% 配信) | 0 | 60.0 |
| 1ch × 1 GSPS | ~245 MB/s (パイプ飽和) | 0 | 58.7 |
| 4ch × 100 MSPS | ~325 MB/s (42% 配信) | 0 | 59.4 |
| 4ch × 1 GSPS | ~337 MB/s (パイプ飽和) | 35M | 56.7 |

**Windows ネイティブ (NVIDIA Vulkan, MSVC Release)**

| 条件 | 実効帯域 | Drops | FPS |
|---|---|---|---|
| 1ch × 10 MSPS | ~12 MB/s | 0 | 60.1 |
| 1ch × 100 MSPS | ~13 MB/s (パイプ飽和) | 0 | 60.1 |
| 4ch × 10 MSPS | ~36 MB/s | 0 | 60.0 |
| 4ch × 100 MSPS | ~9 MB/s (パイプ飽和) | 0 | 60.1 |

**重要な知見:**
- パイプ帯域が律速になっても FPS への影響は軽微（サンプル配信量が減るだけでデシメーション後の頂点数は不変）
- **Windows ネイティブのパイプ帯域は WSL2 の 1/10〜1/30** (~10-36 MB/s vs ~300 MB/s)。Windows の anonymous pipe デフォルトバッファ (4 KB) がボトルネック
- Phase 10 での最適化は特に Windows ネイティブで大きな改善が期待できる

### Phase 10: Pipe IPC 最適化

**目標:** 既存 pipe transport のバッファ・ブロックサイズ最適化で、shm 移行前にどこまで改善可能かを確認する。特に Windows ネイティブの低帯域 (~10-36 MB/s) を改善する。
**リスク:** 低（既存コードの定数変更とOS API呼び出しが主）
**Go/No-Go:** 最適化後の帯域を WSL2/Windows 両環境で計測。Windows で 100 MB/s 未満なら Phase 11 (shm) を開始。WSL2 では既に十分だが改善余地を確認。

- [ ] pipe バッファサイズ拡大: Windows `CreatePipe` のバッファサイズ指定 (4KB → 64KB+)、Linux `fcntl(F_SETPIPE_SZ)` (64KB → 1MB)
- [ ] ブロックサイズ最適化: 4096 → 16384/65536 の帯域・レイテンシ比較計測
- [ ] sender thread の write batching 改善（複数ブロックの一括 write 呼び出し）
- [ ] TI-07 追記: 最適化前後の帯域比較（WSL2 + Windows ネイティブ）、Go/No-Go 判定記録

**受入条件:** 帯域計測結果と Go/No-Go 判定を TI-07 に追記。改善が不十分な場合は Phase 11 の実施を決定。

### Phase 11: Shared Memory IPC 基盤

**目標:** Pipe transport を Shared Memory に置換し、パイプ帯域制約 (~300 MB/s) を排除する。
**リスク:** 高（OS API 依存、同期プリミティブ、障害復旧の複雑さ）
**前提:** Phase 10 の Go/No-Go で shm 移行が必要と判定されていること。

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
- トリガ機能（レベル/エッジ/パターントリガ）
- データ録画・再生
- GUI フレームワーク統合（Qt / ImGui 本格UI）
- リモート表示（WebSocket 経由ブラウザ表示）
- GPU Direct (RDMA) によるデバイス→VRAM 直接転送
