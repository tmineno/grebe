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

---

## 次期マイルストーン（実装予定）

### Phase 8: 実行バイナリ分離 (`grebe` / `grebe-sg`)

- [ ] `grebe` (可視化メイン) と `grebe-sg` (信号生成) の 2 実行バイナリ化
- [ ] 共通契約（`SignalConfigV2` / `FrameHeaderV2` / transport I/F）を `src/ipc` へ分離
- [ ] `grebe` から `grebe-sg` 起動制御の基盤を実装
- [ ] Phase 8 完了時点で最小E2E動作を満たす暫定 transport（stub/pipe/最小shm のいずれか）を実装

### Phase 9: 起動モードと Shared Memory 基盤

- [ ] デフォルト: `grebe` が `grebe-sg` を自動起動
- [ ] `--attach-sg` による既存 `grebe-sg` 接続モード
- [ ] Shared memory + `memcpy` IPC を実装（初期ベースライン）
- [ ] ControlBlockV2 (`grebe-ipc-ctrl`) + generation/heartbeat による discovery/recovery
- [ ] ConsumerStatusBlockV2 (`grebe-ipc-cons`) + credit window 制御
- [ ] DataRing 固定長 v2 パラメータ（block_length=65536, slots=64）
- [ ] loss-tolerant realtime 方針 (credit 枯渇時 drop-new)
- [ ] 基本メトリクス（throughput/drop/enqueue/dequeue/inflight/credits）
- [ ] FrameHeaderV2 header CRC 検証
- [ ] `grebe-sg` 再起動時の再接続/復旧を実装
- [ ] 設定変更は generation bump 経由のみ（in-place config 書き換え禁止）
- [ ] 世代切替時の旧 shared memory segment cleanup（unlink/close + best-effort detach）

### Phase 10: SG 専用 UI と設定モデル

- [ ] `grebe-sg` 専用ウィンドウ（SG UI）を実装
- [ ] グローバル sample rate 設定
- [ ] per-channel (1-8ch) modulation/waveform 設定
- [ ] v2 は全ch共通 data length 設定（可変 per-channel は後続へ延期）
- [ ] `grebe` 側 UI を可視化専用に整理（SG 設定責務を除外）

### Phase 11: トランスポート計測とプロファイル統合

- [ ] SG timestamp → grebe render timestamp の E2E delta を計測
- [ ] `--profile` JSON/CSV に transport メトリクスを追加

### Phase 12: 外部I/F評価向け拡張点

- [ ] トランスポート差し替え可能な抽象I/Fを確定
- [ ] 帯域制限/遅延注入可能なシミュレータバックエンドを追加
- [ ] shared_mem ベースラインとの差分比較レポートを整備

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
